/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Driver entry points for LinuxKPI add-ons that are not network drivers
 * (Bluetooth, and later others): plain C, usable from both halves.
 *
 * A network driver (mt7921e) uses freebsd_network's HAIKU_FBSD_*_GLUE:
 * init_driver() probes PCI and publishes net/<driver>/<n>. Any other
 * driver class uses LKPI_HAIKU_DRIVER_GLUE(name) from its glue.c:
 * init_driver() brings up the freebsd_network runtime LinuxKPI sits on
 * (mutexes, callouts, taskqueues, SYSINITs: LinuxKPI's module_init()s and
 * bus attach, e.g. LinuxKPI USB), and devfs nodes come from lkpi_devfs.
 */
#ifndef _LKPI_DRIVER_H_
#define _LKPI_DRIVER_H_

#include <Drivers.h>

#ifdef __cplusplus
extern "C" {
#endif

status_t	lkpi_driver_init(void);
void		lkpi_driver_uninit(void);

/* devfs nodes (lkpi_devfs.c) */
status_t	lkpi_devfs_add(const char *name, device_hooks *hooks);
void		lkpi_devfs_remove(const char *name);
int		lkpi_devfs_count(void);
const char	**lkpi_devfs_publish(void);
device_hooks	*lkpi_devfs_find(const char *name);
void		lkpi_devfs_set_quiet(bool quiet);
void		lkpi_devfs_notify_enter(void);
void		lkpi_devfs_notify_exit(void);
void		lkpi_devfs_cleanup(void);

/*
 * The glue of a non-network LinuxKPI add-on. 'publicname' must be the
 * add-on's file name: devfs and Haiku's USB stack find the driver by it.
 * The wlan entry points are stubs, as in HAIKU_FBSD_DRIVERS_GLUE, because
 * freebsd_network refers to them.
 */
#define	LKPI_HAIKU_DRIVER_GLUE(publicname)				\
	const char *gDriverName = #publicname;				\
	int32 api_version = B_CUR_DRIVER_API_VERSION;			\
	status_t init_hardware(void) { return B_OK; }			\
	status_t init_driver(void) { return lkpi_driver_init(); }	\
	void uninit_driver(void) { lkpi_driver_uninit(); }		\
	const char **publish_devices(void)				\
		{ return lkpi_devfs_publish(); }			\
	device_hooks *find_device(const char *name)			\
		{ return lkpi_devfs_find(name); }			\
	void __haiku_init_hardware(void) {}				\
	status_t init_wlan_stack(void) { return B_OK; }			\
	void uninit_wlan_stack(void) {}					\
	status_t start_wlan(device_t dev) { return B_OK; }		\
	status_t stop_wlan(device_t dev) { return B_OK; }		\
	status_t wlan_control(void *cookie, uint32 op, void *arg,	\
		size_t length) { return B_BAD_VALUE; }			\
	status_t wlan_close(void *cookie) { return B_OK; }

#ifdef __cplusplus
}
#endif

#endif /* _LKPI_DRIVER_H_ */
