/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay: extends Haiku's compat/sys/bus.h with the newbus calls
 * LinuxKPI's headers make. Implemented in haiku/lkpi_newbus.c on top of
 * Haiku's freebsd_network device.c (device_attach, device_add_child, ...).
 */
#ifndef _LKPI_HAIKU_SYS_BUS_H_
#define _LKPI_HAIKU_SYS_BUS_H_

#include_next <sys/bus.h>

#ifdef __cplusplus
extern "C" {
#endif
driver_t	*device_get_driver(device_t dev);
devclass_t	devclass_create(const char *classname);
devclass_t	devclass_find(const char *classname);
int		devclass_add_driver(devclass_t dc, driver_t *driver, int pass,
		    devclass_t *dcp);
int		devclass_delete_driver(devclass_t busclass, driver_t *driver);
int		device_probe_and_attach(device_t dev);
device_t	devclass_get_device(devclass_t dc, int unit);
void		bus_topo_lock(void);
void		bus_topo_unlock(void);
int		lkpi_bus_rescan(device_t dev);
/* Newer FreeBSD (15) newbus helpers LinuxKPI 802.11 uses. */
void		bus_identify_children(device_t dev);
void		bus_attach_children(device_t dev);
device_t	device_find_child(device_t dev, const char *classname,
		    int unit);
void		device_set_descf(device_t dev, const char *fmt, ...)
		    __printflike(2, 3);
#ifdef __cplusplus
}
#endif

#define BUS_RESCAN(dev)		lkpi_bus_rescan(dev)
#define BUS_PASS_DEFAULT	__INT_MAX__
#ifndef DEVICE_UNIT_ANY
#define DEVICE_UNIT_ANY		(-1)
#endif

#endif
