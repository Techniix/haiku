/*-
 * Copyright (c) 2015-2016 Mellanox Technologies, Ltd.
 * All rights reserved.
 * Copyright (c) 2020-2026 The FreeBSD Foundation
 *
 * Portions of this software were developed by Björn Zeeb
 * under sponsorship from the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 *
 * ---------------------------------------------------------------------------
 * LinuxKPI_HaikuOS: Haiku implementation of LinuxKPI's PCI layer.
 *
 * Derived from FreeBSD sys/compat/linuxkpi/common/src/linux_pci.c at
 * b556d9c9ce94a6c9b6e28e8ecef7cc252531e91c (license above). FreeBSD's file
 * depends on its PCI bus internals (resource lists, pci_devinfo, kobj PCI
 * interfaces, SR-IOV, cdev); this one keeps LinuxKPI's exported API and
 * logic but sits on Haiku's freebsd_network (bus.cpp, pci.cpp) and the
 * Haiku-side helpers in lkpi_haiku_pci.c. Haiku-specific changes:
 * Copyright 2026, LinuxKPI_HaikuOS contributors, same license.
 *
 * Haiku integration (see docs/ARCHITECTURE.md):
 *  - One static bridge driver_t, DRIVER_MODULE_NAME(lkpi, pci), is what
 *    Haiku's glue (HAIKU_FBSD_WLAN_DRIVER_GLUE(x, lkpi, pci)) hands to
 *    _fbsd_init_hardware_pci(). Its probe matches the MODULE_DEVICE_TABLE()
 *    tables collected in the __lkpi_pnp linker set, so no LinuxKPI code runs
 *    in init_hardware. Its attach runs in init_driver, after LinuxKPI's
 *    SYSINITs (and so module_init/pci_register_driver) have run.
 *  - DMA lives in lkpi_dma.c. Not implemented: ROM mapping, DRM
 *    registration. Not planned: SR-IOV, backlight.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/rman.h>
#include <sys/module.h>
#include <sys/linker_set.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <machine/resource.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/compat.h>

/* linux/pci.h defines Linux pci_get_class()/pci_get_slot() macros; as in
 * FreeBSD's linux_pci.c, we want the FreeBSD functions here. */
#undef pci_get_class
#undef pci_get_slot

/* lkpi_haiku_pci.c (Haiku side, returns 0/-1) */
int		lkpi_haiku_pci_bar(device_t dev, int bar, uint64_t *start,
		    uint64_t *size, int *isIO, int *is64);
int		lkpi_haiku_pci_is_msi(device_t dev);
uint32_t	lkpi_haiku_pci_irq_base(device_t dev);

/* lkpi_dma.c */
int		linux_pdev_dma_init(struct pci_dev *pdev);
int		linux_pdev_dma_uninit(struct pci_dev *pdev);

extern struct device linux_root_device;
extern const struct kobj_type linux_dev_ktype;

int pci_enable_aspm = 0;

/*
 * Optional step after a successful Linux probe, before Haiku continues the
 * attach: Haiku's freebsd_wlan start_wlan() runs right after attach returns
 * and needs the net80211 device, which Linux drivers often register later
 * from a work queue. Set by lkpi_wlan_haiku.c. Returns a FreeBSD errno.
 */
int (*lkpi_pci_attach_wait_hook)(device_t dev);

static struct list_head lkpi_pci_drivers = LINUX_LIST_HEAD_INIT(lkpi_pci_drivers);
static struct list_head lkpi_pci_devices = LINUX_LIST_HEAD_INIT(lkpi_pci_devices);
static spinlock_t lkpi_pci_lock;

#define	LKPI_PCI_LOCK()			spin_lock(&lkpi_pci_lock)
#define	LKPI_PCI_UNLOCK()		spin_unlock(&lkpi_pci_lock)

static void
lkpi_pci_lock_init(void *arg __unused)
{
	spin_lock_init(&lkpi_pci_lock);
}
SYSINIT(lkpi_pci_lock, SI_SUB_LOCK, SI_ORDER_ANY, lkpi_pci_lock_init, NULL);

const char *pci_power_names[] = {
	"UNKNOWN", "D0", "D1", "D2", "D3hot", "D3cold"
};

/* ---- ID matching ------------------------------------------------------- */

static bool
lkpi_pci_id_match(const struct pci_device_id *id, uint16_t vendor,
    uint16_t device, uint16_t subvendor, uint16_t subdevice)
{
	/* Same rule as FreeBSD's linux_pci_find(). */
	return (vendor == id->vendor &&
	    (PCI_ANY_ID == id->device || device == id->device) &&
	    (PCI_ANY_ID == id->subvendor || subvendor == id->subvendor) &&
	    (PCI_ANY_ID == id->subdevice || subdevice == id->subdevice));
}

static struct pci_driver *
linux_pci_find(device_t dev, const struct pci_device_id **idp)
{
	const struct pci_device_id *id;
	struct pci_driver *pdrv;
	uint16_t vendor = pci_get_vendor(dev);
	uint16_t device = pci_get_device(dev);
	uint16_t subvendor = pci_get_subvendor(dev);
	uint16_t subdevice = pci_get_subdevice(dev);

	LKPI_PCI_LOCK();
	list_for_each_entry(pdrv, &lkpi_pci_drivers, node) {
		for (id = pdrv->id_table; id->vendor != 0; id++) {
			if (lkpi_pci_id_match(id, vendor, device, subvendor,
			    subdevice)) {
				*idp = id;
				LKPI_PCI_UNLOCK();
				return (pdrv);
			}
		}
	}
	LKPI_PCI_UNLOCK();
	return (NULL);
}

/* MODULE_DEVICE_TABLE() tables (include/sys/module.h). The dummy entry keeps
 * the linker set non-empty. */
static const struct lkpi_pnp_info lkpi_pnp_dummy = { .bus = "" };
DATA_SET(__lkpi_pnp, lkpi_pnp_dummy);
SET_DECLARE(__lkpi_pnp, const struct lkpi_pnp_info);

static const struct pci_device_id *
lkpi_pci_pnp_find(device_t dev)
{
	const struct lkpi_pnp_info * const *pp;
	const struct pci_device_id *id;
	uint16_t vendor = pci_get_vendor(dev);
	uint16_t device = pci_get_device(dev);
	uint16_t subvendor = pci_get_subvendor(dev);
	uint16_t subdevice = pci_get_subdevice(dev);
	int i;

	SET_FOREACH(pp, __lkpi_pnp) {
		if (strcmp((*pp)->bus, "pci") != 0 ||
		    (*pp)->entsize != sizeof(struct pci_device_id))
			continue;
		id = (*pp)->table;
		for (i = 0; i < (*pp)->count && id[i].vendor != 0; i++) {
			if (lkpi_pci_id_match(&id[i], vendor, device,
			    subvendor, subdevice))
				return (&id[i]);
		}
	}
	return (NULL);
}

/* ---- pci_dev life cycle ------------------------------------------------ */

struct pci_dev *
lkpi_pci_get_device(uint32_t vendor, uint32_t device, struct pci_dev *odev)
{
	struct pci_dev *pdev, *found, *odev0;

	odev0 = odev;
	found = NULL;
	LKPI_PCI_LOCK();
	list_for_each_entry(pdev, &lkpi_pci_devices, links) {
		if (odev != NULL) {
			if (pdev == odev)
				odev = NULL;
			continue;
		}
		if ((pdev->vendor == vendor || vendor == PCI_ANY_ID) &&
		    (pdev->device == device || device == PCI_ANY_ID)) {
			found = pdev;
			break;
		}
	}
	pci_dev_get(found);
	LKPI_PCI_UNLOCK();
	pci_dev_put(odev0);
	return (found);
}

static void
lkpi_pci_dev_release(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	lkpi_devres_release_free_list(dev);

	LKPI_PCI_LOCK();
	list_del(&pdev->links);
	LKPI_PCI_UNLOCK();

	if (!list_empty_careful(&pdev->dev.irqents))
		dev_warn(&pdev->dev, "%s: driver did not clean up; leaking "
		    "IRQs\n", __func__);
	if (!TAILQ_EMPTY(&pdev->mmio))
		dev_warn(&pdev->dev, "%s: driver did not clean up; leaking "
		    "mmio resources\n", __func__);

	spin_lock_destroy(&dev->devres_lock);
	spin_lock_destroy(&pdev->pcie_cap_lock);
	linux_pdev_dma_uninit(pdev);
	free(pdev->bus, M_DEVBUF);
	kfree(pdev->path_name);

	/* Signal linux_pci_detach_device() that the last reference went. */
	pdev->dev.release = NULL;
}

/* Port of lkpi_pci_dev_init_common(): pci_devinfo replaced by Haiku's
 * pci_info (via freebsd_network's pci_get_*()). */
static int
lkpi_pci_dev_init_common(device_t dev, struct pci_dev *pdev)
{
	int error, cap;

	error = kobject_init_and_add(&pdev->dev.kobj, &linux_dev_ktype,
	    &linux_root_device.kobj, device_get_nameunit(dev));
	if (error != 0)
		return (error);

	pdev->devfn = PCI_DEVFN(pci_get_slot(dev), pci_get_function(dev));
	pdev->vendor = pci_get_vendor(dev);
	pdev->device = pci_get_device(dev);
	pdev->subsystem_vendor = pci_get_subvendor(dev);
	pdev->subsystem_device = pci_get_subdevice(dev);
	pdev->class = (pci_get_class(dev) << 16) | (pci_get_subclass(dev) << 8) |
	    pci_get_progif(dev);
	pdev->revision = pci_get_revid(dev);
	pdev->path_name = kasprintf(GFP_KERNEL, "%04d:%02d:%02d.%d",
	    pci_get_domain(dev), pci_get_bus(dev), pci_get_slot(dev),
	    pci_get_function(dev));

	pdev->bus = malloc(sizeof(*pdev->bus), M_DEVBUF, M_WAITOK | M_ZERO);
	pdev->bus->number = pci_get_bus(dev);
	pdev->bus->domain = pci_get_domain(dev);
	/* No PCI tree on Haiku's side: treat each device as its own upstream
	 * (pci_upstream_bridge() then stops here). */
	pdev->bus->self = pdev;

	if (pci_find_cap(dev, PCIY_MSI, &cap) == 0)
		pdev->msi_cap = cap;
	if (pci_find_cap(dev, PCIY_MSIX, &cap) == 0)
		pdev->msix_cap = cap;
	pdev->dev.bsddev = dev;
	pdev->dev.parent = &linux_root_device;

	TAILQ_INIT(&pdev->mmio);
	spin_lock_init(&pdev->pcie_cap_lock);
	spin_lock_init(&pdev->dev.devres_lock);
	INIT_LIST_HEAD(&pdev->dev.devres_head);
	INIT_LIST_HEAD(&pdev->dev.irqents);
	INIT_LIST_HEAD(&pdev->links);
	/* DMA state (lkpi_dma.c); FreeBSD does this here as well. */
	return (linux_pdev_dma_init(pdev));
}

struct pci_dev *
lkpinew_pci_dev(device_t dev)
{
	struct pci_dev *pdev;

	pdev = malloc(sizeof(*pdev), M_DEVBUF, M_WAITOK | M_ZERO);
	if (lkpi_pci_dev_init_common(dev, pdev) != 0) {
		free(pdev, M_DEVBUF);
		return (NULL);
	}
	return (pdev);
}

struct pci_dev *
lkpi_pci_get_class(unsigned int class, struct pci_dev *from)
{
	device_t dev = pci_find_class_from(class >> 16, (class >> 8) & 0xff,
	    from != NULL ? from->dev.bsddev : NULL);

	return (dev != NULL ? lkpinew_pci_dev(dev) : NULL);
}

struct pci_dev *
lkpi_pci_get_base_class(unsigned int baseclass, struct pci_dev *from)
{
	device_t dev = pci_find_base_class_from(baseclass,
	    from != NULL ? from->dev.bsddev : NULL);

	return (dev != NULL ? lkpinew_pci_dev(dev) : NULL);
}

struct pci_dev *
lkpi_pci_get_domain_bus_and_slot(int domain, unsigned int bus,
    unsigned int devfn)
{
	device_t dev = pci_find_dbsf(domain, bus, PCI_SLOT(devfn),
	    PCI_FUNC(devfn));

	return (dev != NULL ? lkpinew_pci_dev(dev) : NULL);
}

struct pci_dev *
lkpi_pci_get_slot(struct pci_bus *pbus, unsigned int devfn)
{
	device_t dev = pci_find_bsf(pbus->number, PCI_SLOT(devfn),
	    PCI_FUNC(devfn));

	return (dev != NULL ? lkpinew_pci_dev(dev) : NULL);
}

int
linux_pci_attach_device(device_t dev, struct pci_driver *pdrv,
    const struct pci_device_id *id, struct pci_dev *pdev)
{
	int error;

	linux_set_current(curthread);

	/* Return convention as FreeBSD: positive errno (it is public API). */
	error = lkpi_pci_dev_init_common(dev, pdev);
	if (error != 0)
		return (error);
	pdev->dev.release = lkpi_pci_dev_release;
	pdev->pdrv = pdrv;

	/* INTx line as the Linux "irq" (rid 0); see lkpi_haiku_pci_irq_base() */
	pdev->dev.irq = lkpi_haiku_pci_irq_base(dev);
	if (pdev->dev.irq == 0 || pdev->dev.irq == 0xff)
		pdev->dev.irq = LINUX_IRQ_INVALID;
	pdev->irq = pdev->dev.irq;

	LKPI_PCI_LOCK();
	list_add(&pdev->links, &lkpi_pci_devices);
	LKPI_PCI_UNLOCK();

	if (pdrv != NULL) {
		error = pdrv->probe(pdev, id);
		if (error)
			goto out_err;
	}
	return (0);

out_err:
	put_device(&pdev->dev);
	return (-error);
}

int
linux_pci_detach_device(struct pci_dev *pdev)
{
	linux_set_current(curthread);

	if (pdev->pdrv != NULL)
		pdev->pdrv->remove(pdev);
	lkpi_devres_release_free_list(&pdev->dev);
	pci_dev_put(pdev);

	if (pdev->dev.release != lkpi_pci_dev_release)
		return (0);
	/* Someone still holds a reference; FreeBSD waits here too. */
	dev_warn(&pdev->dev, "%s: references remain after remove()\n",
	    __func__);
	return (0);
}

/* ---- the bridge driver_t ------------------------------------------------ */

static int
lkpi_pci_bridge_probe(device_t dev)
{
	const struct pci_device_id *id = lkpi_pci_pnp_find(dev);

	/* Haiku's newbus: >= 0 is a match, higher wins; BUS_PROBE_DEFAULT is
	 * +20 there (freebsd_network compat/sys/bus.h). E* values are FreeBSD
	 * positive ones in this file, so "no match" must be spelled -1. */
	if (id == NULL)
		return (-1);
	device_set_desc(dev, "LinuxKPI PCI device");
	return (BUS_PROBE_DEFAULT);
}

static int
lkpi_pci_bridge_attach(device_t dev)
{
	const struct pci_device_id *id;
	struct pci_driver *pdrv;
	struct pci_dev *pdev;
	bool giant;
	int error;

	pdrv = linux_pci_find(dev, &id);
	if (pdrv == NULL) {
		device_printf(dev, "LinuxKPI: no registered pci_driver matches "
		    "%04x:%04x (module_init not run?)\n", pci_get_vendor(dev),
		    pci_get_device(dev));
		return (ENXIO);
	}
	pdev = device_get_softc(dev);
	/*
	 * freebsd_network holds Giant across the whole attach (driver.c).
	 * Linux probe routines may wait for work they queued (FreeBSD's mt76
	 * waits for mt7921_init_work()), and that work ends in LinuxKPI 802.11
	 * attach, which takes bus_topo_lock() == Giant: a deadlock (stage (c),
	 * stuck right after the firmware booted). Run the Linux probe without
	 * Giant; nothing in it touches freebsd_network's device tree directly.
	 */
	giant = mtx_owned(&Giant);
	if (giant)
		mtx_unlock(&Giant);
	error = linux_pci_attach_device(dev, pdrv, id, pdev);
	if (error == 0 && lkpi_pci_attach_wait_hook != NULL) {
		/* e.g. LinuxKPI 802.11: see lkpi_wlan_haiku.c */
		error = lkpi_pci_attach_wait_hook(dev);
		if (error != 0) {
			device_printf(dev, "LinuxKPI: driver did not finish "
			    "attaching (%d); detaching it again\n", error);
			linux_pci_detach_device(pdev);
		}
	}
	if (giant)
		mtx_lock(&Giant);
	return (lkpi_errno_to_haiku(error));
}

static int
lkpi_pci_bridge_detach(device_t dev)
{
	struct pci_dev *pdev = device_get_softc(dev);
	bool giant;
	int error;

	/* As attach: Linux remove() may wait for work that takes Giant. */
	giant = mtx_owned(&Giant);
	if (giant)
		mtx_unlock(&Giant);
	error = linux_pci_detach_device(pdev);
	if (giant)
		mtx_lock(&Giant);
	return (lkpi_errno_to_haiku(error));
}

static int
lkpi_pci_bridge_suspend(device_t dev)
{
	struct pci_dev *pdev = device_get_softc(dev);
	pm_message_t pm = { .event = PM_EVENT_SUSPEND };
	int error = 0;

	linux_set_current(curthread);
	if (pdev->pdrv != NULL && pdev->pdrv->suspend != NULL)
		error = -pdev->pdrv->suspend(pdev, pm);
	return (lkpi_errno_to_haiku(error));
}

static int
lkpi_pci_bridge_resume(device_t dev)
{
	struct pci_dev *pdev = device_get_softc(dev);
	int error = 0;

	linux_set_current(curthread);
	if (pdev->pdrv != NULL && pdev->pdrv->resume != NULL)
		error = -pdev->pdrv->resume(pdev);
	return (lkpi_errno_to_haiku(error));
}

static void
lkpi_pci_bridge_shutdown(device_t dev)
{
	struct pci_dev *pdev = device_get_softc(dev);

	linux_set_current(curthread);
	if (pdev->pdrv != NULL && pdev->pdrv->shutdown != NULL)
		pdev->pdrv->shutdown(pdev);
}

static device_method_t lkpi_pci_bridge_methods[] = {
	DEVMETHOD(device_probe,		lkpi_pci_bridge_probe),
	DEVMETHOD(device_attach,	lkpi_pci_bridge_attach),
	DEVMETHOD(device_detach,	lkpi_pci_bridge_detach),
	DEVMETHOD(device_suspend,	lkpi_pci_bridge_suspend),
	DEVMETHOD(device_resume,	lkpi_pci_bridge_resume),
	DEVMETHOD(device_shutdown,	lkpi_pci_bridge_shutdown),
	DEVMETHOD_END
};

static driver_t lkpi_pci_bridge_driver = {
	"lkpi",
	lkpi_pci_bridge_methods,
	sizeof(struct pci_dev)
};

/* What HAIKU_FBSD_[WLAN_]DRIVER_GLUE(<public>, lkpi, pci) refers to. */
driver_t *DRIVER_MODULE_NAME(lkpi, pci) = &lkpi_pci_bridge_driver;

/* ---- driver (un)registration -------------------------------------------- */

int
linux_pci_register_driver(struct pci_driver *pdrv)
{
	linux_set_current(curthread);
	pdrv->isdrm = false;
	LKPI_PCI_LOCK();
	list_add(&pdrv->node, &lkpi_pci_drivers);
	LKPI_PCI_UNLOCK();
	if (pdrv->bsddriver.name == NULL)
		pdrv->bsddriver.name = pdrv->name;
	return (0);
}

int
linux_pci_register_drm_driver(struct pci_driver *pdrv)
{
	return (-ENXIO);	/* DRM: later project stage */
}

void
linux_pci_unregister_driver(struct pci_driver *pdrv)
{
	LKPI_PCI_LOCK();
	list_del(&pdrv->node);
	LKPI_PCI_UNLOCK();
}

void
linux_pci_unregister_drm_driver(struct pci_driver *pdrv)
{
}

/* ---- BARs, regions, iomap ----------------------------------------------- */

static bool
lkpi_pci_bar_id_valid(int bar)
{
	return (bar >= 0 && bar <= PCIR_MAX_BAR_0);
}

static struct pci_map lkpi_pci_maps[PCIR_MAX_BAR_0 + 1];	/* scratch */

struct pci_map *
pci_find_bar(device_t dev, int reg)
{
	uint64_t start, size;
	int isIO, is64, bar;
	struct pci_map *pm;

	bar = (reg - PCIR_BAR(0)) / 4;
	if (!lkpi_pci_bar_id_valid(bar) ||
	    lkpi_haiku_pci_bar(dev, bar, &start, &size, &isIO, &is64) != 0)
		return (NULL);
	/* Returned map is only read immediately by linux/pci.h
	 * (pci_resource_type, pci_resource_flags). */
	pm = &lkpi_pci_maps[bar];
	pm->pm_value = start | (isIO ? PCIM_BAR_IO_SPACE : PCIM_BAR_MEM_SPACE) |
	    (is64 ? PCIM_BAR_MEM_64 : 0);
	pm->pm_size = size;
	pm->pm_reg = reg;
	pm->pm_bar = bar;
	return (pm);
}

unsigned long
pci_resource_start(struct pci_dev *pdev, int bar)
{
	uint64_t start, size;
	int isIO, is64;

	if (!lkpi_pci_bar_id_valid(bar) || lkpi_haiku_pci_bar(pdev->dev.bsddev,
	    bar, &start, &size, &isIO, &is64) != 0)
		return (0);
	return (start);
}

unsigned long
pci_resource_len(struct pci_dev *pdev, int bar)
{
	uint64_t start, size;
	int isIO, is64;

	if (!lkpi_pci_bar_id_valid(bar) || lkpi_haiku_pci_bar(pdev->dev.bsddev,
	    bar, &start, &size, &isIO, &is64) != 0)
		return (0);
	return (size);
}

static struct pci_mmio_region *
lkpi_pci_mmio_find(struct pci_dev *pdev, int bar)
{
	struct pci_mmio_region *mmio;

	TAILQ_FOREACH(mmio, &pdev->mmio, next) {
		if (mmio->rid == PCIR_BAR(bar))
			return (mmio);
	}
	return (NULL);
}

/*
 * As in FreeBSD, a requested region is allocated (and, for memory BARs,
 * mapped by freebsd_network's bus_alloc_resource()) once and reused by
 * pci_iomap().
 */
static struct resource *
lkpi_pci_alloc_bar(struct pci_dev *pdev, int bar)
{
	struct pci_mmio_region *mmio;
	int type;

	type = pci_resource_type(pdev, bar);
	if (type < 0)
		return (NULL);
	if ((mmio = lkpi_pci_mmio_find(pdev, bar)) != NULL)
		return (mmio->res);

	mmio = malloc(sizeof(*mmio), M_DEVBUF, M_WAITOK | M_ZERO);
	mmio->rid = PCIR_BAR(bar);
	mmio->type = type;
	mmio->res = bus_alloc_resource_any(pdev->dev.bsddev, type, &mmio->rid,
	    RF_ACTIVE | RF_SHAREABLE);
	if (mmio->res == NULL) {
		device_printf(pdev->dev.bsddev, "%s: failed to alloc bar %d "
		    "type %d\n", __func__, bar, type);
		free(mmio, M_DEVBUF);
		return (NULL);
	}
	TAILQ_INSERT_TAIL(&pdev->mmio, mmio, next);
	return (mmio->res);
}

int
linuxkpi_pci_request_region(struct pci_dev *pdev, int bar, const char *name)
{
	if (!lkpi_pci_bar_id_valid(bar))
		return (-EINVAL);
	if (pci_resource_type(pdev, bar) < 0)
		return (0);
	return (lkpi_pci_alloc_bar(pdev, bar) != NULL ? 0 : -EBUSY);
}

int
linuxkpi_pci_request_regions(struct pci_dev *pdev, const char *name)
{
	int error, i;

	for (i = 0; i <= PCIR_MAX_BAR_0; i++) {
		error = linuxkpi_pci_request_region(pdev, i, name);
		if (error && error != -EBUSY) {
			linuxkpi_pci_release_regions(pdev);
			return (error);
		}
	}
	return (0);
}

int
linuxkpi_pcim_request_all_regions(struct pci_dev *pdev, const char *name)
{
	return (linuxkpi_pci_request_regions(pdev, name));
}

void
linuxkpi_pci_release_region(struct pci_dev *pdev, int bar)
{
	struct pci_mmio_region *mmio;

	if (!lkpi_pci_bar_id_valid(bar) ||
	    (mmio = lkpi_pci_mmio_find(pdev, bar)) == NULL)
		return;
	bus_release_resource(pdev->dev.bsddev, mmio->type, mmio->rid,
	    mmio->res);
	TAILQ_REMOVE(&pdev->mmio, mmio, next);
	free(mmio, M_DEVBUF);
}

void
linuxkpi_pci_release_regions(struct pci_dev *pdev)
{
	int i;

	for (i = 0; i <= PCIR_MAX_BAR_0; i++)
		linuxkpi_pci_release_region(pdev, i);
}

void *
linuxkpi_pci_iomap_range(struct pci_dev *pdev, int bar, unsigned long off,
    unsigned long maxlen)
{
	struct resource *res;

	if (!lkpi_pci_bar_id_valid(bar))
		return (NULL);
	res = lkpi_pci_alloc_bar(pdev, bar);
	if (res == NULL)
		return (NULL);
	if (pdev->want_iomap_res)
		return (res);
	return ((void *)(rman_get_bushandle(res) + off));
}

void *
linuxkpi_pci_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen)
{
	return (linuxkpi_pci_iomap_range(pdev, bar, 0, maxlen));
}

void
linuxkpi_pci_iounmap(struct pci_dev *pdev, void *res)
{
	struct pci_mmio_region *mmio, *p;
	bus_space_handle_t bh = (bus_space_handle_t)res;

	TAILQ_FOREACH_SAFE(mmio, &pdev->mmio, next, p) {
		if (pdev->want_iomap_res) {
			if (res != mmio->res)
				continue;
		} else if (bh < rman_get_bushandle(mmio->res) ||
		    bh >= rman_get_bushandle(mmio->res) +
		    rman_get_size(mmio->res))
			continue;
		bus_release_resource(pdev->dev.bsddev, mmio->type, mmio->rid,
		    mmio->res);
		TAILQ_REMOVE(&pdev->mmio, mmio, next);
		free(mmio, M_DEVBUF);
		return;
	}
}

/* pcim_* (managed) variants: devres tracking as in FreeBSD. */
struct pcim_iomap_devres {
	void		*mmio_table[PCIR_MAX_BAR_0 + 1];
	struct resource	*res_table[PCIR_MAX_BAR_0 + 1];
};

static void
lkpi_pcim_iomap_table_release(struct device *dev, void *p)
{
	struct pcim_iomap_devres *dr = p;
	struct pci_dev *pdev = to_pci_dev(dev);
	int bar;

	for (bar = PCIR_MAX_BAR_0; bar >= 0; bar--) {
		if (dr->res_table[bar] == NULL)
			continue;
		linuxkpi_pci_release_region(pdev, bar);
	}
}

static struct pcim_iomap_devres *
lkpi_pcim_iomap_devres_find(struct pci_dev *pdev)
{
	struct pcim_iomap_devres *dr;

	dr = lkpi_devres_find(&pdev->dev, lkpi_pcim_iomap_table_release,
	    NULL, NULL);
	if (dr == NULL) {
		dr = lkpi_devres_alloc(lkpi_pcim_iomap_table_release,
		    sizeof(*dr), GFP_KERNEL | __GFP_ZERO);
		if (dr != NULL)
			lkpi_devres_add(&pdev->dev, dr);
	}
	return (dr);
}

void __iomem **
linuxkpi_pcim_iomap_table(struct pci_dev *pdev)
{
	struct pcim_iomap_devres *dr = lkpi_pcim_iomap_devres_find(pdev);

	if (dr == NULL)
		return (NULL);
	return (pdev->want_iomap_res ? (void **)dr->res_table : dr->mmio_table);
}

void *
linuxkpi_pcim_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen)
{
	struct pcim_iomap_devres *dr;
	struct resource *res;

	if (!lkpi_pci_bar_id_valid(bar))
		return (NULL);
	if ((dr = lkpi_pcim_iomap_devres_find(pdev)) == NULL)
		return (NULL);
	if (dr->res_table[bar] != NULL)
		return (pdev->want_iomap_res ? (void *)dr->res_table[bar] :
		    dr->mmio_table[bar]);
	if ((res = lkpi_pci_alloc_bar(pdev, bar)) == NULL)
		return (NULL);
	dr->res_table[bar] = res;
	dr->mmio_table[bar] = (void *)rman_get_bushandle(res);
	return (pdev->want_iomap_res ? (void *)res : dr->mmio_table[bar]);
}

int
linuxkpi_pcim_iomap_regions(struct pci_dev *pdev, uint32_t mask,
    const char *name)
{
	int bar;

	for (bar = 0; bar <= PCIR_MAX_BAR_0; bar++) {
		if ((mask & (1 << bar)) == 0)
			continue;
		if (linuxkpi_pcim_iomap(pdev, bar, 0) == NULL)
			return (-ENOMEM);
	}
	return (0);
}

void
lkpi_pci_devres_release(struct device *dev, void *p)
{
	pci_disable_device(to_pci_dev(dev));
}

int
linuxkpi_pcim_enable_device(struct pci_dev *pdev)
{
	void *dr;
	int error;

	error = pci_enable_device(pdev);
	if (error != 0)
		return (error);
	if (lkpi_devres_find(&pdev->dev, lkpi_pci_devres_release, NULL,
	    NULL) == NULL) {
		dr = lkpi_devres_alloc(lkpi_pci_devres_release, sizeof(int),
		    GFP_KERNEL | __GFP_ZERO);
		if (dr != NULL)
			lkpi_devres_add(&pdev->dev, dr);
	}
	pdev->managed = true;
	return (0);
}

bool
pci_device_is_present(struct pci_dev *pdev)
{
	return (pci_read_config(pdev->dev.bsddev, PCIR_VENDOR, 2) !=
	    PCIV_INVALID);
}

/* ---- interrupts ----------------------------------------------------------- */

struct device *
lkpi_pci_find_irq_dev(unsigned int irq)
{
	struct pci_dev *pdev;
	struct device *found = NULL;

	LKPI_PCI_LOCK();
	list_for_each_entry(pdev, &lkpi_pci_devices, links) {
		if (irq == pdev->dev.irq ||
		    (irq >= pdev->dev.irq_start && irq < pdev->dev.irq_end)) {
			found = &pdev->dev;
			break;
		}
	}
	LKPI_PCI_UNLOCK();
	return (found);
}

/*
 * After pci_alloc_msi[x]() freebsd_network maps IRQ rid N to Haiku vector
 * base + N - 1; LinuxKPI maps Linux irq (irq_start + i) to rid (i + 1)
 * (linux_interrupt.c, lkpi_irq_rid()). So irq_start = base is consistent.
 */
int
linuxkpi_pci_enable_msix(struct pci_dev *pdev, struct msix_entry *entries,
    int nreq)
{
	int avail, i;

	avail = pci_msix_count(pdev->dev.bsddev);
	if (avail < nreq)
		return (avail == 0 ? -EINVAL : avail);
	avail = nreq;
	if (pci_alloc_msix(pdev->dev.bsddev, &avail) != 0)
		return (-ENXIO);
	pdev->dev.irq_start = lkpi_haiku_pci_irq_base(pdev->dev.bsddev);
	pdev->dev.irq_end = pdev->dev.irq_start + avail;
	for (i = 0; i < nreq; i++)
		entries[i].vector = pdev->dev.irq_start + i;
	pdev->msix_enabled = true;
	return (0);
}

int
_lkpi_pci_enable_msi_range(struct pci_dev *pdev, int minvec, int maxvec)
{
	int nvec;

	if (maxvec < minvec)
		return (-EINVAL);
	nvec = pci_msi_count(pdev->dev.bsddev);
	if (nvec < 1 || nvec < minvec)
		return (-ENOSPC);
	nvec = min(nvec, maxvec);
	if (pci_alloc_msi(pdev->dev.bsddev, &nvec) != 0)
		return (-ENXIO);
	if (nvec < minvec) {
		pci_release_msi(pdev->dev.bsddev);
		return (-ENOSPC);
	}
	pdev->dev.irq_start = lkpi_haiku_pci_irq_base(pdev->dev.bsddev);
	pdev->dev.irq_end = pdev->dev.irq_start + nvec;
	pdev->irq = pdev->dev.irq_start;
	pdev->msi_enabled = true;
	return (0);
}

int
pci_alloc_irq_vectors(struct pci_dev *pdev, int minv, int maxv,
    unsigned int flags)
{
	int error;

	if (flags & PCI_IRQ_MSIX) {
		struct msix_entry *entries;
		int i;

		entries = kcalloc(maxv, sizeof(*entries), GFP_KERNEL);
		if (entries == NULL)
			return (-ENOMEM);
		for (i = 0; i < maxv; ++i)
			entries[i].entry = i;
		error = linuxkpi_pci_enable_msix(pdev, entries, maxv);
		kfree(entries);
		if (error == 0 && pdev->msix_enabled)
			return (pdev->dev.irq_end - pdev->dev.irq_start);
	}
	if (flags & PCI_IRQ_MSI) {
		if (pci_msi_count(pdev->dev.bsddev) < minv)
			return (-ENOSPC);
		error = _lkpi_pci_enable_msi_range(pdev, minv, maxv);
		if (error == 0 && pdev->msi_enabled)
			return (pdev->dev.irq_end - pdev->dev.irq_start);
	}
	if (flags & PCI_IRQ_INTX) {
		if (pdev->irq != LINUX_IRQ_INVALID)
			return (1);
	}
	return (-EINVAL);
}

struct msi_desc *
lkpi_pci_msi_desc_alloc(unsigned int irq)
{
	return (NULL);	/* stage c: only used for msi_desc->msi_attrib */
}
