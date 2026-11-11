/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * The dynamic-newbus calls LinuxKPI makes (devclass_*, device_get_driver,
 * device_probe_and_attach, bus_topo_lock), on Haiku's freebsd_network
 * device.c. Haiku's newbus is static (drivers are known at build time), so
 * devclasses are just names here. PCI drivers do not go through this at all:
 * see lkpi_pci.c. Built without the LinuxKPI preinclude (Haiku errno).
 */
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/mutex.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "device.h"

int lkpi_errno_from_haiku(int haikuError);

struct devclass {
	char	name[32];
};

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

#define LKPI_MAX_DEVCLASSES	16
static struct devclass sDevclasses[LKPI_MAX_DEVCLASSES];

driver_t *
device_get_driver(device_t dev)
{
	return (dev->driver);
}

devclass_t
devclass_find(const char *name)
{
	int i;

	for (i = 0; i < LKPI_MAX_DEVCLASSES; i++) {
		if (strcmp(sDevclasses[i].name, name) == 0)
			return (&sDevclasses[i]);
	}
	return (NULL);
}

devclass_t
devclass_create(const char *name)
{
	devclass_t dc = devclass_find(name);
	int i;

	if (dc != NULL)
		return (dc);
	for (i = 0; i < LKPI_MAX_DEVCLASSES; i++) {
		if (sDevclasses[i].name[0] == '\0') {
			strlcpy(sDevclasses[i].name, name, sizeof(sDevclasses[i].name));
			return (&sDevclasses[i]);
		}
	}
	return (NULL);
}

int
devclass_add_driver(devclass_t dc, driver_t *driver, int pass, devclass_t *dcp)
{
	if (dcp != NULL)
		*dcp = devclass_create(driver->name);
	return (0);
}

int
devclass_delete_driver(devclass_t busclass, driver_t *driver)
{
	return (0);
}

int
device_probe_and_attach(device_t dev)
{
	int error;

	/* freebsd_network's device_attach() does not probe; FreeBSD's
	 * device_probe_and_attach() does (probe returns <= 0 on a match). */
	if (dev->methods.device_probe != NULL) {
		error = dev->methods.device_probe(dev);
		if (error > 0)
			return (lkpi_errno_from_haiku(error));
	}
	return (lkpi_errno_from_haiku(device_attach(dev)));
}

device_t
devclass_get_device(devclass_t dc, int unit)
{
	/* Devclasses carry no device lists here (see top of file); LinuxKPI
	 * then creates its own child device (device_initialize()). */
	return (NULL);
}

void
bus_topo_lock(void)
{
	mtx_lock(&Giant);
}

void
bus_topo_unlock(void)
{
	mtx_unlock(&Giant);
}

int
lkpi_bus_rescan(device_t dev)
{
	return (lkpi_errno_from_haiku(ENODEV));
}

/* No identify methods on Haiku: drivers are matched by probe only. */
void
bus_identify_children(device_t dev)
{
}

void
bus_attach_children(device_t dev)
{
	bus_generic_attach(dev);
}

device_t
device_find_child(device_t dev, const char *classname, int unit)
{
	device_t *children, child = NULL;
	int count, i;

	if (device_get_children(dev, &children, &count) != 0)
		return (NULL);
	for (i = 0; i < count; i++) {
		if (strcmp(device_get_name(children[i]), classname) == 0 &&
		    (unit == DEVICE_UNIT_ANY ||
		    device_get_unit(children[i]) == unit)) {
			child = children[i];
			break;
		}
	}
	free(children);	/* plain malloc() in freebsd_network device.c */
	return (child);
}

void
device_set_descf(device_t dev, const char *fmt, ...)
{
	char buf[128];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	device_set_desc_copy(dev, buf);
}
