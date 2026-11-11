/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku side of the LinuxKPI PCI layer: everything that needs Haiku's
 * pci_info / PCI bus manager (freebsd_network's private device.h). Built
 * without the LinuxKPI preinclude, so E* values here are Haiku's; callers
 * on the LinuxKPI side only see 0/-1, booleans, sizes or pointers.
 *
 * FreeBSD functions provided here keep FreeBSD's prototypes
 * (sys/dev/pci/pcivar.h, sys/sys/bus.h); see include/dev/pci/pcivar.h.
 */
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <machine/resource.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>

#include <PCI.h>
#include <string.h>

#include "device.h"	/* freebsd_network private: gPci, get_device_pci_info() */

int		lkpi_haiku_pci_bar(device_t dev, int bar, uint64_t *start,
		    uint64_t *size, int *isIO, int *is64);
uint8_t		pci_get_class(device_t dev);
uint8_t		pci_get_subclass(device_t dev);
uint8_t		pci_get_progif(device_t dev);
int		pci_disable_busmaster(device_t dev);
int		pci_disable_io(device_t dev, int space);
bool		is_pci_device(device_t dev);
bool		pci_has_pme(device_t dev, int state);
void		pci_save_state(device_t dev);
void		pci_restore_state(device_t dev);
int		pci_get_max_payload(device_t dev);
device_t	pci_find_bsf(uint8_t bus, uint8_t slot, uint8_t func);
device_t	pci_find_device(uint16_t vendor, uint16_t device);
device_t	pci_find_class_from(uint8_t cls, uint8_t subcls, device_t from);
device_t	pci_find_base_class_from(uint8_t cls, device_t from);
device_t	pci_find_pcie_root_port(device_t dev);
int		lkpi_haiku_pci_is_msi(device_t dev);
uint32_t	lkpi_haiku_pci_irq_base(device_t dev);

int
lkpi_haiku_pci_bar(device_t dev, int bar, uint64_t *start, uint64_t *size,
    int *isIO, int *is64)
{
	pci_info *info = get_device_pci_info(dev);
	uint8_t flags;

	if (info == NULL || bar < 0 || bar > 5)
		return (-1);
	flags = info->u.h0.base_register_flags[bar];
	*start = info->u.h0.base_registers[bar];
	*size = info->u.h0.base_register_sizes[bar];
	*isIO = (flags & PCI_address_space) != 0;
	*is64 = !*isIO && (flags & PCI_address_type) == PCI_address_type_64;
	if (*is64 && bar < 5) {
		/* same composition as freebsd_network bus_alloc_mem_resource() */
		*start |= (uint64_t)info->u.h0.base_registers[bar + 1] << 32;
		*size |= (uint64_t)info->u.h0.base_register_sizes[bar + 1] << 32;
	}
	return (*size != 0 ? 0 : -1);
}

uint8_t
pci_get_class(device_t dev)
{
	return (get_device_pci_info(dev)->class_base);
}

uint8_t
pci_get_subclass(device_t dev)
{
	return (get_device_pci_info(dev)->class_sub);
}

uint8_t
pci_get_progif(device_t dev)
{
	return (get_device_pci_info(dev)->class_api);
}

int
pci_disable_busmaster(device_t dev)
{
	uint16_t cmd = pci_read_config(dev, PCI_command, 2);

	pci_write_config(dev, PCI_command, cmd & ~PCI_command_master, 2);
	return (0);
}

int
pci_disable_io(device_t dev, int space)
{
	uint16_t bit, cmd;

	if (space == SYS_RES_IOPORT)
		bit = PCI_command_io;
	else if (space == SYS_RES_MEMORY)
		bit = PCI_command_memory;
	else
		return (-1);
	cmd = pci_read_config(dev, PCI_command, 2);
	pci_write_config(dev, PCI_command, cmd & ~bit, 2);
	return (0);
}

bool
is_pci_device(device_t dev)
{
	struct root_device_softc *root;

	if (dev == NULL || dev->root == NULL)
		return (false);
	root = (struct root_device_softc *)dev->root->softc;
	return (root != NULL && root->bus == BUS_pci);
}

int
lkpi_haiku_pci_is_msi(device_t dev)
{
	struct root_device_softc *root =
	    (struct root_device_softc *)dev->root->softc;

	return (root->is_msi || root->is_msix);
}

/*
 * After pci_alloc_msi()/pci_alloc_msix(), freebsd_network stores the first
 * vector in pci_info.u.h0.interrupt_line and maps IRQ rid N to vector
 * (base + N - 1) (bus.cpp, bus_alloc_resource()). Before that it is the
 * legacy INTx line.
 */
uint32_t
lkpi_haiku_pci_irq_base(device_t dev)
{
	return (get_device_pci_info(dev)->u.h0.interrupt_line);
}

bool
pci_has_pme(device_t dev, int state)
{
	int cap;
	uint16_t pmc;

	if (pci_find_cap(dev, PCIY_PMG, &cap) != 0)
		return (false);
	pmc = pci_read_config(dev, cap + PCIR_POWER_CAP, 2);
	/* PME_Support is bits 15:11, one bit per D-state (D0..D3cold). */
	if (state < 0 || state > 4)
		return (false);
	return ((pmc >> (11 + state)) & 1) != 0;
}

/* Saved config space, one 256-byte block per device. */
struct lkpi_saved_cfg {
	device_t		dev;
	uint32_t		cfg[64];
	struct lkpi_saved_cfg	*next;
};
static struct lkpi_saved_cfg *sSavedCfg;

void
pci_save_state(device_t dev)
{
	struct lkpi_saved_cfg *s;
	int i;

	for (s = sSavedCfg; s != NULL && s->dev != dev; s = s->next)
		;
	if (s == NULL) {
		s = malloc(sizeof(*s));
		if (s == NULL)
			return;
		memset(s, 0, sizeof(*s));
		s->dev = dev;
		s->next = sSavedCfg;
		sSavedCfg = s;
	}
	for (i = 0; i < 64; i++)
		s->cfg[i] = pci_read_config(dev, i * 4, 4);
}

void
pci_restore_state(device_t dev)
{
	struct lkpi_saved_cfg *s;
	int i;

	for (s = sSavedCfg; s != NULL && s->dev != dev; s = s->next)
		;
	if (s == NULL)
		return;
	/* Restore the header after the BARs/caps, command register last. */
	for (i = 63; i >= 1; i--)
		pci_write_config(dev, i * 4, s->cfg[i], 4);
	pci_write_config(dev, PCI_command, s->cfg[1] & 0xffff, 2);
}

int
pci_get_max_payload(device_t dev)
{
	int cap;
	uint16_t ctl;

	if (pci_find_cap(dev, PCIY_EXPRESS, &cap) != 0)
		return (0);
	ctl = pci_read_config(dev, cap + PCIER_DEVICE_CTL, 2);
	return (128 << ((ctl & PCIEM_CTL_MAX_PAYLOAD) >> 5));
}

/*
 * freebsd_network only creates device_t's for the device(s) the driver
 * attached to; there is no tree of other PCI functions to search. These
 * return NULL, which LinuxKPI callers handle ("not found"). [stage f: build
 * on-demand device_t's from gPci->get_nth_pci_info() if a driver needs it]
 */
device_t pci_find_bsf(uint8_t b, uint8_t s, uint8_t f) { return (NULL); }
device_t pci_find_device(uint16_t v, uint16_t d) { return (NULL); }
device_t pci_find_class_from(uint8_t c, uint8_t s, device_t from) { return (NULL); }
device_t pci_find_base_class_from(uint8_t c, device_t from) { return (NULL); }
device_t pci_find_pcie_root_port(device_t dev) { return (NULL); }
