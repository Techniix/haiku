/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay: extends Haiku's compat/dev/pci/pcivar.h with the pieces of
 * FreeBSD's pcivar.h that LinuxKPI's linux/pci.h names (prototypes as in FreeBSD
 * sys/dev/pci/pcivar.h). Implementations are in
 * haiku/lkpi_pci.c, on top of Haiku's freebsd_network pci.cpp and the PCI
 * bus manager.
 */
#ifndef _LKPI_HAIKU_DEV_PCI_PCIVAR_H_
#define _LKPI_HAIKU_DEV_PCI_PCIVAR_H_

#include_next <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>

typedef uint64_t	pci_addr_t;

struct pci_map {
	pci_addr_t	pm_value;	/* raw BAR value */
	pci_addr_t	pm_size;	/* BAR size */
	uint16_t	pm_reg;		/* config space offset */
	uint8_t		pm_bar;		/* BAR index, Haiku-private */
};

#ifndef PCIV_INVALID
#define PCIV_INVALID	0xffff
#endif
#ifndef PCI_BAR_IO
#define PCI_BAR_IO(x)	(((x) & PCIM_BAR_SPACE) == PCIM_BAR_IO_SPACE)
#define PCI_BAR_MEM(x)	(((x) & PCIM_BAR_SPACE) == PCIM_BAR_MEM_SPACE)
#endif

#ifdef __cplusplus
extern "C" {
#endif
struct pci_map	*pci_find_bar(device_t dev, int reg);
extern int pci_enable_aspm;	/* FreeBSD tunable; 0 on Haiku */
uint8_t	pci_get_subclass(device_t dev);
uint8_t	pci_get_class(device_t dev);
uint8_t	pci_get_progif(device_t dev);
int	pci_disable_io(device_t dev, int space);
device_t pci_find_bsf(uint8_t bus, uint8_t slot, uint8_t func);
device_t pci_find_dbsf(uint32_t domain, uint8_t bus, uint8_t slot, uint8_t func);	/* freebsd_network pci.cpp */
device_t pci_find_device(uint16_t vendor, uint16_t device);
device_t pci_find_class_from(uint8_t cls, uint8_t subcls, device_t from);
device_t pci_find_base_class_from(uint8_t cls, device_t from);
device_t pci_find_pcie_root_port(device_t dev);
bool	is_pci_device(device_t dev);
int	pci_disable_busmaster(device_t dev);
bool	pci_has_pme(device_t dev, int state);
void	pci_save_state(device_t dev);
void	pci_restore_state(device_t dev);
int	pci_get_max_payload(device_t dev);
#ifdef __cplusplus
}
#endif

#endif
