rtems_tests_PROGRAMS = fileio
fileio_SOURCES = init.c system.h

dist_rtems_tests_DATA = fileio.doc

include $(RTEMS_ROOT)/make/custom/@RTEMS_BSP@.cfg
include $(top_srcdir)/../automake/compile.am
include $(top_srcdir)/../automake/leaf.am

AM_CPPFLAGS += -I$(top_srcdir)/../support/include
AM_CPPFLAGS += -I/home/vivek/devel/rtems-libbsdlatest/rtemsbsd/include
AM_CPPFLAGS += -I/home/vivek/devel/rtems-libbsdlatest/freebsd/include
AM_CPPFLAGS += -I/home/vivek/devel/rtems-libbsdlatest/freebsd/sys

LINK_OBJS = $(fileio_OBJECTS)
LINK_LIBS = $(fileio_LDLIBS)
LINK_LIBS += /home/vivek/devel/rtems-libbsd/build/arm-rtems4.12-xilinx_zynq_a9_qemu/libbsd.a

fileio$(EXEEXT): $(fileio_OBJECTS) $(fileio_DEPENDENCIES)
	@rm -f fileio$(EXEEXT)
	$(make-exe)

include $(top_srcdir)/../automake/local.am
