/**
 * @file
 *
 * @brief Thread Queue Operations
 * @ingroup ScoreThreadQ
 */

/*
 *  COPYRIGHT (c) 1989-2014.
 *  On-Line Applications Research Corporation (OAR).
 *
 *  Copyright (c) 2015, 2016 embedded brains GmbH.
 *
 *  The license and distribution terms for this file may be
 *  found in the file LICENSE in this distribution or at
 *  http://www.rtems.org/license/LICENSE.
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif

#include <rtems/score/threadqimpl.h>
#include <rtems/score/assert.h>
#include <rtems/score/threaddispatch.h>
#include <rtems/score/threadimpl.h>
#include <rtems/score/status.h>
#include <rtems/score/watchdogimpl.h>

#define THREAD_QUEUE_INTEND_TO_BLOCK \
  (THREAD_WAIT_CLASS_OBJECT | THREAD_WAIT_STATE_INTEND_TO_BLOCK)

#define THREAD_QUEUE_BLOCKED \
  (THREAD_WAIT_CLASS_OBJECT | THREAD_WAIT_STATE_BLOCKED)

#define THREAD_QUEUE_READY_AGAIN \
  (THREAD_WAIT_CLASS_OBJECT | THREAD_WAIT_STATE_READY_AGAIN)

#if defined(RTEMS_SMP)
/*
 * A global registry of active thread queue links is used to provide deadlock
 * detection on SMP configurations.  This is simple to implement and no
 * additional storage is required for the thread queues.  The disadvantage is
 * the global registry is not scalable and may lead to lock contention.
 * However, the registry is only used in case of nested resource conflicts.  In
 * this case, the application is already in trouble.
 */

typedef struct {
  ISR_lock_Control Lock;

  RBTree_Control Links;
} Thread_queue_Links;

static Thread_queue_Links _Thread_queue_Links = {
  ISR_LOCK_INITIALIZER( "Thread Queue Links" ),
  RBTREE_INITIALIZER_EMPTY( _Thread_queue_Links.Links )
};

static bool _Thread_queue_Link_equal(
  const void        *left,
  const RBTree_Node *right
)
{
  const Thread_queue_Queue *the_left;
  const Thread_queue_Link  *the_right;

  the_left = left;
  the_right = (Thread_queue_Link *) right;

  return the_left == the_right->source;
}

static bool _Thread_queue_Link_less(
  const void        *left,
  const RBTree_Node *right
)
{
  const Thread_queue_Queue *the_left;
  const Thread_queue_Link  *the_right;

  the_left = left;
  the_right = (Thread_queue_Link *) right;

  return (uintptr_t) the_left < (uintptr_t) the_right->source;
}

static void *_Thread_queue_Link_map( RBTree_Node *node )
{
  return node;
}

static Thread_queue_Link *_Thread_queue_Link_find(
  Thread_queue_Links *links,
  Thread_queue_Queue *source
)
{
  return _RBTree_Find_inline(
    &links->Links,
    source,
    _Thread_queue_Link_equal,
    _Thread_queue_Link_less,
    _Thread_queue_Link_map
  );
}

static bool _Thread_queue_Link_add(
  Thread_queue_Link  *link,
  Thread_queue_Queue *source,
  Thread_queue_Queue *target
)
{
  Thread_queue_Links *links;
  Thread_queue_Queue *recursive_target;
  ISR_lock_Context    lock_context;

  links = &_Thread_queue_Links;
  recursive_target = target;

  _ISR_lock_Acquire( &links->Lock, &lock_context );

  while ( true ) {
    Thread_queue_Link *recursive_link;

    recursive_link = _Thread_queue_Link_find( links, recursive_target );

    if ( recursive_link == NULL ) {
      break;
    }

    recursive_target = recursive_link->target;

    if ( recursive_target == source ) {
      _ISR_lock_Release( &links->Lock, &lock_context );
      return false;
    }
  }

  link->source = source;
  link->target = target;
  _RBTree_Insert_inline(
    &links->Links,
    &link->Registry_node,
    source,
    _Thread_queue_Link_less
  );

  _ISR_lock_Release( &links->Lock, &lock_context );
  return true;
}

static void _Thread_queue_Link_remove( Thread_queue_Link *link )
{
  Thread_queue_Links *links;
  ISR_lock_Context    lock_context;

  links = &_Thread_queue_Links;

  _ISR_lock_Acquire( &links->Lock, &lock_context );
  _RBTree_Extract( &links->Links, &link->Registry_node );
  _ISR_lock_Release( &links->Lock, &lock_context );
}
#endif

#define THREAD_QUEUE_LINK_OF_PATH_NODE( node ) \
  RTEMS_CONTAINER_OF( node, Thread_queue_Link, Path_node );

static void _Thread_queue_Path_release( Thread_queue_Path *path )
{
#if defined(RTEMS_SMP)
  Chain_Node *head;
  Chain_Node *node;

  head = _Chain_Head( &path->Links );
  node = _Chain_Last( &path->Links );

  if ( head != node ) {
    Thread_queue_Link *link;

    /*
     * The terminal link may have an owner which does not wait on a thread
     * queue.
     */

    link = THREAD_QUEUE_LINK_OF_PATH_NODE( node );

    if ( link->Queue_context.Wait.queue == NULL ) {
      _Thread_Wait_release_default_critical(
        link->owner,
        &link->Queue_context.Lock_context
      );

      node = _Chain_Previous( node );
#if defined(RTEMS_DEBUG)
      _Chain_Set_off_chain( &link->Path_node );
#endif
    }

    while ( head != node ) {
      /* The other links have an owner which waits on a thread queue */
      link = THREAD_QUEUE_LINK_OF_PATH_NODE( node );
      _Assert( link->Queue_context.Wait.queue != NULL );

      _Thread_queue_Link_remove( link );
      _Thread_Wait_release_queue_critical(
        link->Queue_context.Wait.queue,
        &link->Queue_context
      );
      _Thread_Wait_remove_request( link->owner, &link->Queue_context );

      node = _Chain_Previous( node );
#if defined(RTEMS_DEBUG)
      _Chain_Set_off_chain( &link->Path_node );
#endif
    }
  }
#else
  (void) path;
#endif
}

static bool _Thread_queue_Path_acquire(
  Thread_Control     *the_thread,
  Thread_queue_Queue *queue,
  Thread_queue_Path  *path
)
{
  Thread_Control     *owner;

#if defined(RTEMS_SMP)
  Thread_queue_Link  *link;
  Thread_queue_Queue *target;

  /*
   * For an overview please look at the non-SMP part below.  We basically do
   * the same on SMP configurations.  The fact that we may have more than one
   * executing thread and each thread queue has its own SMP lock makes the task
   * a bit more difficult.  We have to avoid deadlocks at SMP lock level, since
   * this would result in an unrecoverable deadlock of the overall system.
   */

  _Chain_Initialize_empty( &path->Links );

  owner = queue->owner;

  if ( owner == NULL ) {
    return true;
  }

  if ( owner == the_thread ) {
    return false;
  }

  _RBTree_Initialize_node( &path->Start.Registry_node );
  _Chain_Initialize_node( &path->Start.Path_node );
  _Thread_queue_Context_initialize( &path->Start.Queue_context );
  link = &path->Start;

  do {
    _Chain_Append_unprotected( &path->Links, &link->Path_node );
    link->owner = owner;

    _Thread_Wait_acquire_default_critical(
      owner,
      &link->Queue_context.Lock_context
    );

    target = owner->Wait.queue;
    link->Queue_context.Wait.queue = target;

    if ( target != NULL ) {
      if ( _Thread_queue_Link_add( link, queue, target ) ) {
        _Thread_queue_Gate_add(
          &owner->Wait.Lock.Pending_requests,
          &link->Queue_context.Wait.Gate
        );
        _Thread_Wait_release_default_critical(
          owner,
          &link->Queue_context.Lock_context
        );
        _Thread_Wait_acquire_queue_critical( target, &link->Queue_context );

        if ( link->Queue_context.Wait.queue == NULL ) {
          _Thread_queue_Link_remove( link );
          _Thread_Wait_release_queue_critical( target, &link->Queue_context );
          _Thread_Wait_acquire_default_critical(
            owner,
            &link->Queue_context.Lock_context
          );
          _Thread_Wait_remove_request_locked( owner, &link->Queue_context );
          _Assert( owner->Wait.queue == NULL );
          return true;
        }
      } else {
        link->Queue_context.Wait.queue = NULL;
        _Thread_queue_Path_release( path );
        return false;
      }
    } else {
      return true;
    }

    link = &owner->Wait.Link;
    queue = target;
    owner = queue->owner;
  } while ( owner != NULL );
#else
  do {
    owner = queue->owner;

    if ( owner == NULL ) {
      return true;
    }

    if ( owner == the_thread ) {
      return false;
    }

    queue = owner->Wait.queue;
  } while ( queue != NULL );
#endif

  return true;
}

void _Thread_queue_Deadlock_status( Thread_Control *the_thread )
{
  the_thread->Wait.return_code = STATUS_DEADLOCK;
}

void _Thread_queue_Deadlock_fatal( Thread_Control *the_thread )
{
  _Terminate(
    INTERNAL_ERROR_CORE,
    false,
    INTERNAL_ERROR_THREAD_QUEUE_DEADLOCK
  );
}

void _Thread_queue_Enqueue_critical(
  Thread_queue_Queue            *queue,
  const Thread_queue_Operations *operations,
  Thread_Control                *the_thread,
  States_Control                 state,
  Thread_queue_Context          *queue_context
)
{
  Thread_queue_Path  path;
  Per_CPU_Control   *cpu_self;
  bool               success;

#if defined(RTEMS_MULTIPROCESSING)
  if ( _Thread_MP_Is_receive( the_thread ) && the_thread->receive_packet ) {
    the_thread = _Thread_MP_Allocate_proxy( state );
  }
#endif

  _Thread_Wait_claim( the_thread, queue, operations );

  if ( !_Thread_queue_Path_acquire( the_thread, queue, &path ) ) {
    _Thread_Wait_restore_default( the_thread );
    _Thread_queue_Queue_release( queue, &queue_context->Lock_context );
    _Thread_Wait_tranquilize( the_thread );
    ( *queue_context->deadlock_callout )( the_thread );
    return;
  }

  ( *operations->enqueue )( queue, the_thread, &path );

  _Thread_queue_Path_release( &path );

  the_thread->Wait.return_code = STATUS_SUCCESSFUL;
  _Thread_Wait_flags_set( the_thread, THREAD_QUEUE_INTEND_TO_BLOCK );
  cpu_self = _Thread_Dispatch_disable_critical( &queue_context->Lock_context );
  _Thread_queue_Queue_release( queue, &queue_context->Lock_context );

  if (
    cpu_self->thread_dispatch_disable_level
      != queue_context->expected_thread_dispatch_disable_level
  ) {
    _Terminate(
      INTERNAL_ERROR_CORE,
      false,
      INTERNAL_ERROR_THREAD_QUEUE_ENQUEUE_FROM_BAD_STATE
    );
  }

  /*
   *  Set the blocking state for this thread queue in the thread.
   */
  _Thread_Set_state( the_thread, state );

  /*
   *  If the thread wants to timeout, then schedule its timer.
   */
  switch ( queue_context->timeout_discipline ) {
    case WATCHDOG_RELATIVE:
      /* A relative timeout of 0 is a special case indefinite (no) timeout */
      if ( queue_context->timeout != 0 ) {
        _Thread_Timer_insert_relative(
          the_thread,
          cpu_self,
          _Thread_Timeout,
          (Watchdog_Interval) queue_context->timeout
        );
      }
      break;
    case WATCHDOG_ABSOLUTE:
      _Thread_Timer_insert_absolute(
        the_thread,
        cpu_self,
        _Thread_Timeout,
        queue_context->timeout
      );
      break;
    default:
      break;
  }

  /*
   * At this point thread dispatching is disabled, however, we already released
   * the thread queue lock.  Thus, interrupts or threads on other processors
   * may already changed our state with respect to the thread queue object.
   * The request could be satisfied or timed out.  This situation is indicated
   * by the thread wait flags.  Other parties must not modify our thread state
   * as long as we are in the THREAD_QUEUE_INTEND_TO_BLOCK thread wait state,
   * thus we have to cancel the blocking operation ourself if necessary.
   */
  success = _Thread_Wait_flags_try_change_acquire(
    the_thread,
    THREAD_QUEUE_INTEND_TO_BLOCK,
    THREAD_QUEUE_BLOCKED
  );
  if ( !success ) {
    _Thread_Remove_timer_and_unblock( the_thread, queue );
  }

  _Thread_Update_priority( path.update_priority );
  _Thread_Dispatch_enable( cpu_self );
}

#if defined(RTEMS_MULTIPROCESSING)
static bool _Thread_queue_MP_set_callout(
  Thread_Control             *the_thread,
  const Thread_queue_Context *queue_context
)
{
  Thread_Proxy_control    *the_proxy;
  Thread_queue_MP_callout  mp_callout;

  if ( _Objects_Is_local_id( the_thread->Object.id ) ) {
    return false;
  }

  the_proxy = (Thread_Proxy_control *) the_thread;
  mp_callout = queue_context->mp_callout;
  _Assert( mp_callout != NULL );
  the_proxy->thread_queue_callout = queue_context->mp_callout;
  return true;
}
#endif

static bool _Thread_queue_Make_ready_again( Thread_Control *the_thread )
{
  bool success;
  bool unblock;

  /*
   * We must update the wait flags under protection of the current thread lock,
   * otherwise a _Thread_Timeout() running on another processor may interfere.
   */
  success = _Thread_Wait_flags_try_change_release(
    the_thread,
    THREAD_QUEUE_INTEND_TO_BLOCK,
    THREAD_QUEUE_READY_AGAIN
  );
  if ( success ) {
    unblock = false;
  } else {
    _Assert( _Thread_Wait_flags_get( the_thread ) == THREAD_QUEUE_BLOCKED );
    _Thread_Wait_flags_set( the_thread, THREAD_QUEUE_READY_AGAIN );
    unblock = true;
  }

  _Thread_Wait_restore_default( the_thread );
  return unblock;
}

bool _Thread_queue_Do_extract_locked(
  Thread_queue_Queue            *queue,
  const Thread_queue_Operations *operations,
  Thread_Control                *the_thread
#if defined(RTEMS_MULTIPROCESSING)
  ,
  const Thread_queue_Context    *queue_context
#endif
)
{
#if defined(RTEMS_MULTIPROCESSING)
  _Thread_queue_MP_set_callout( the_thread, queue_context );
#endif
  ( *operations->extract )( queue, the_thread );
  return _Thread_queue_Make_ready_again( the_thread );
}

void _Thread_queue_Unblock_critical(
  bool                unblock,
  Thread_queue_Queue *queue,
  Thread_Control     *the_thread,
  ISR_lock_Context   *lock_context
)
{
  if ( unblock ) {
    Per_CPU_Control *cpu_self;

    cpu_self = _Thread_Dispatch_disable_critical( lock_context );
    _Thread_queue_Queue_release( queue, lock_context );

    _Thread_Remove_timer_and_unblock( the_thread, queue );

    _Thread_Dispatch_enable( cpu_self );
  } else {
    _Thread_queue_Queue_release( queue, lock_context );
  }
}

void _Thread_queue_Extract_critical(
  Thread_queue_Queue            *queue,
  const Thread_queue_Operations *operations,
  Thread_Control                *the_thread,
  Thread_queue_Context          *queue_context
)
{
  bool unblock;

  unblock = _Thread_queue_Extract_locked(
    queue,
    operations,
    the_thread,
    queue_context
  );

  _Thread_queue_Unblock_critical(
    unblock,
    queue,
    the_thread,
    &queue_context->Lock_context
  );
}

void _Thread_queue_Extract( Thread_Control *the_thread )
{
  Thread_queue_Context  queue_context;
  Thread_queue_Queue   *queue;

  _Thread_queue_Context_initialize( &queue_context );
  _Thread_Wait_acquire( the_thread, &queue_context );

  queue = the_thread->Wait.queue;

  if ( queue != NULL ) {
    bool unblock;

    _Thread_Wait_remove_request( the_thread, &queue_context );
    _Thread_queue_Context_set_MP_callout(
      &queue_context,
      _Thread_queue_MP_callout_do_nothing
    );
    unblock = _Thread_queue_Extract_locked(
      queue,
      the_thread->Wait.operations,
      the_thread,
      &queue_context
    );
    _Thread_queue_Unblock_critical(
      unblock,
      queue,
      the_thread,
      &queue_context.Lock_context
    );
  } else {
    _Thread_Wait_release( the_thread, &queue_context );
  }
}

void _Thread_queue_Surrender(
  Thread_queue_Queue            *queue,
  const Thread_queue_Operations *operations,
  Thread_queue_Heads            *heads,
  Thread_Control                *previous_owner,
  bool                           keep_priority,
  Thread_queue_Context          *queue_context
)
{
  if ( heads != NULL ) {
    Thread_Control *new_owner;
    bool            unblock;

    new_owner = ( *operations->surrender )( queue, heads, previous_owner );
    queue->owner = new_owner;

#if defined(RTEMS_MULTIPROCESSING)
    if ( !_Thread_queue_MP_set_callout( new_owner, queue_context ) )
#endif
    {
      ++new_owner->resource_count;
    }

    unblock = _Thread_queue_Make_ready_again( new_owner );

    _Thread_queue_Unblock_critical(
      unblock,
      queue,
      new_owner,
      &queue_context->Lock_context
    );
  } else {
    _Thread_queue_Queue_release( queue, &queue_context->Lock_context );
  }

  if ( !keep_priority ) {
    Per_CPU_Control *cpu_self;

    cpu_self = _Thread_Dispatch_disable();
    _Thread_Restore_priority( previous_owner );
    _Thread_Dispatch_enable( cpu_self );
  }
}

Thread_Control *_Thread_queue_Do_dequeue(
  Thread_queue_Control          *the_thread_queue,
  const Thread_queue_Operations *operations
#if defined(RTEMS_MULTIPROCESSING)
  ,
  Thread_queue_MP_callout        mp_callout
#endif
)
{
  Thread_queue_Context  queue_context;
  Thread_Control       *the_thread;

  _Thread_queue_Context_initialize( &queue_context );
  _Thread_queue_Context_set_MP_callout( &queue_context, mp_callout );
  _Thread_queue_Acquire( the_thread_queue, &queue_context.Lock_context );

  the_thread = _Thread_queue_First_locked( the_thread_queue, operations );

  if ( the_thread != NULL ) {
    _Thread_queue_Extract_critical(
      &the_thread_queue->Queue,
      operations,
      the_thread,
      &queue_context
    );
  } else {
    _Thread_queue_Release( the_thread_queue, &queue_context.Lock_context );
  }

  return the_thread;
}

#if defined(RTEMS_MULTIPROCESSING)
void _Thread_queue_Unblock_proxy(
  Thread_queue_Queue *queue,
  Thread_Control     *the_thread
)
{
  const Thread_queue_Object *the_queue_object;
  Thread_Proxy_control      *the_proxy;
  Thread_queue_MP_callout    mp_callout;

  the_queue_object = THREAD_QUEUE_QUEUE_TO_OBJECT( queue );
  the_proxy = (Thread_Proxy_control *) the_thread;
  mp_callout = the_proxy->thread_queue_callout;
  ( *mp_callout )( the_thread, the_queue_object->Object.id );

  _Thread_MP_Free_proxy( the_thread );
}
#endif
