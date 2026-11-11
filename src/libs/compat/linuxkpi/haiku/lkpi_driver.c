/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * init_driver()/uninit_driver() of non-network LinuxKPI add-ons (see
 * lkpi_driver.h). Same runtime bring-up as freebsd_network's
 * _fbsd_init_drivers() (driver.c), without its PCI probing and network
 * device publishing: LinuxKPI's SYSINITs attach the drivers to their
 * buses, and the drivers publish device nodes through lkpi_devfs.
 *
 * Built WITHOUT the LinuxKPI preinclude (Haiku side).
 */
#include <KernelExport.h>
#include <Drivers.h>

#include <compat/sys/haiku-module.h>	/* HAIKU_DRIVER_REQUIRES */

#include "device.h"			/* freebsd_network: init_mutexes() ... */
#include "sysinit.h"

#include "lkpi_driver.h"

status_t
lkpi_driver_init(void)
{
	status_t s;

	/* devfs asks for our nodes right after we return */
	lkpi_devfs_set_quiet(true);

	s = init_mutexes();
	if (s < B_OK)
		goto err0;
	s = init_mbufs();
	if (s < B_OK)
		goto err1;
	s = init_callout();
	if (s < B_OK)
		goto err2;
	if (HAIKU_DRIVER_REQUIRES(FBSD_TASKQUEUES)) {
		s = init_taskqueues();
		if (s < B_OK)
			goto err3;
	}

	/* LinuxKPI: module_init()s, then bus layers start delivering devices
	 * (LinuxKPI USB reports present devices synchronously) */
	init_sysinit();

	if (lkpi_devfs_count() > 0) {
		lkpi_devfs_set_quiet(false);
		return (B_OK);
	}
	/* Nothing to drive: unload. A bus layer that matches by device ID
	 * (LinuxKPI USB) has Haiku load the add-on again on hotplug. */
	s = ENODEV;
	uninit_sysinit();
	if (HAIKU_DRIVER_REQUIRES(FBSD_TASKQUEUES))
		uninit_taskqueues();
err3:
	uninit_callout();
err2:
	uninit_mbufs();
err1:
	uninit_mutexes();
err0:
	lkpi_devfs_cleanup();
	lkpi_devfs_set_quiet(false);
	return (s);
}

void
lkpi_driver_uninit(void)
{
	lkpi_devfs_set_quiet(true);
	uninit_sysinit();	/* module_exit()s: drivers unbind, nodes go */
	if (HAIKU_DRIVER_REQUIRES(FBSD_TASKQUEUES))
		uninit_taskqueues();
	uninit_callout();
	uninit_mbufs();
	uninit_mutexes();
	lkpi_devfs_cleanup();
	lkpi_devfs_set_quiet(false);
}
