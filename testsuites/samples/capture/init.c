/*
 *  COPYRIGHT (c) 1989-2012.
 *  On-Line Applications Research Corporation (OAR).
 *
 *  The license and distribution terms for this file may in
 *  the file LICENSE in this distribution or at
 *  http://www.rtems.org/license/LICENSE.
 */

#define CONFIGURE_INIT

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "system.h"
#include <stdio.h>
#include <stdlib.h>

#include <rtems.h>
#include <rtems/capture-cli.h>
#include <rtems/monitor.h>
#include <rtems/shell.h>

/* forward declarations to avoid warnings */
rtems_task Init(rtems_task_argument argument);
static void notification(int fd, int seconds_remaining, void *arg);


#define USE_LIBBSD

#ifdef USE_LIBBSD
#include <sysexits.h>

#include <machine/rtems-bsd-commands.h>
#include <rtems/bsd/bsd.h>

#include <rtems/libio.h>
#include <librtemsNfs.h>

#include <bsp.h>

#if defined(LIBBSP_ARM_ALTERA_CYCLONE_V_BSP_H)
  #define NET_CFG_INTERFACE_0 "dwc0"
#elif defined(LIBBSP_ARM_REALVIEW_PBX_A9_BSP_H)
  #define NET_CFG_INTERFACE_0 "smc0"
#elif defined(LIBBSP_ARM_XILINX_ZYNQ_BSP_H)
  #define NET_CFG_INTERFACE_0 "cgem0"
#elif defined(LIBBSP_M68K_GENMCF548X_BSP_H)
  #define NET_CFG_INTERFACE_0 "fec0"
#else
  #define NET_CFG_INTERFACE_0 "lo0"
#endif

#if defined(LIBBSP_I386_PC386_BSP_H)
#define RTEMS_BSD_CONFIG_DOMAIN_PAGE_MBUFS_SIZE (64 * 1024 * 1024)
#endif

#define NET_CFG_SELF_IP "192.168.2.2"
#define NET_CFG_NETMASK "255.255.255.0"
#define NET_CFG_PEER_IP "192.168.0.100"
#define NET_CFG_GATEWAY_IP "192.168.2.1"

#endif

const char rtems_test_name[] = "CAPTURE ENGINE";
rtems_printer rtems_test_printer;

volatile int can_proceed = 1;

static void notification(int fd, int seconds_remaining, void *arg)
{
  printf(
    "Press any key to start capture engine (%is remaining)\n",
    seconds_remaining
  );
}

#ifdef USE_LIBBSD
static void
default_network_ifconfig_hwif0(char *ifname)
{
	int exit_code;
	char *ifcfg[] = {
		"ifconfig",
		ifname,
		"inet",
		NET_CFG_SELF_IP,
		"netmask",
		NET_CFG_NETMASK,
		NULL
	};

	exit_code = rtems_bsd_command_ifconfig(RTEMS_BSD_ARGC(ifcfg), ifcfg);
	assert(exit_code == EX_OK);
}

static void
default_network_route_hwif0(char *ifname)
{
	int exit_code;
	char *dflt_route[] = {
		"route",
		"add",
		"-host",
		NET_CFG_GATEWAY_IP,
		"-iface",
		ifname,
		NULL
	};
	char *dflt_route2[] = {
		"route",
		"add",
		"default",
		NET_CFG_GATEWAY_IP,
		NULL
	};

	exit_code = rtems_bsd_command_route(RTEMS_BSD_ARGC(dflt_route), dflt_route);
	assert(exit_code == EXIT_SUCCESS);

	exit_code = rtems_bsd_command_route(RTEMS_BSD_ARGC(dflt_route2), dflt_route2);
	assert(exit_code == EXIT_SUCCESS);
}
#endif

rtems_task Init(
  rtems_task_argument ignored
)
{
#ifdef USE_LIBBSD
	char *ifname;
  rtems_bsd_initialize();
	ifname = NET_CFG_INTERFACE_0;

  default_network_ifconfig_hwif0(ifname);
	default_network_route_hwif0(ifname);

  static const char remote_target[] = "1000.100@" NET_CFG_PEER_IP " :/var/nfs";
	int rv;

	do {
    sleep (1);
		rv = mount_and_make_target_path(&remote_target[0], "/nfs",
		                                RTEMS_FILESYSTEM_TYPE_NFS, RTEMS_FILESYSTEM_READ_WRITE,
		                                NULL);
		} while (rv != 0);

  mkdir("/nfs/new", 0777);
  printf ("error: dir open failed: %s\n", strerror (errno));
#endif

  rtems_status_code   status;
  rtems_task_priority old_priority;
  rtems_mode          old_mode;

  rtems_print_printer_printf(&rtems_test_printer);
  rtems_test_begin();

  status = rtems_shell_wait_for_input(
    STDIN_FILENO,
    20,
    notification,
    NULL
  );
  if (status == RTEMS_SUCCESSFUL) {
    /* lower the task priority to allow created tasks to execute */

    rtems_task_set_priority(RTEMS_SELF, 20, &old_priority);
    rtems_task_mode(RTEMS_PREEMPT,  RTEMS_PREEMPT_MASK, &old_mode);

    while (!can_proceed)
    {
      printf ("Sleeping\n");
      usleep (1000000);
    }

    rtems_monitor_init (0);
    rtems_capture_cli_init (0);

    setup_tasks_to_watch ();

    rtems_task_delete (RTEMS_SELF);
  } else {
    rtems_test_end();

    exit( 0 );
  }
}


#ifdef USE_LIBBSD
/*
 * Configure LibBSD.
 */
#define RTEMS_BSD_CONFIG_BSP_CONFIG
#define RTEMS_BSD_CONFIG_INIT

#include <machine/rtems-bsd-config.h>
#endif
