/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku glue for the LinuxKPI stage (a)/(b) PCI test driver. The FreeBSD
 * driver_t handed to Haiku is LinuxKPI's PCI bridge (lkpi_pci.c), which
 * matches devices against the Linux driver's MODULE_DEVICE_TABLE.
 */
#include <sys/bus.h>
#include <sys/kernel.h>

HAIKU_FBSD_DRIVER_GLUE(lkpi_pcitest, lkpi, pci);
HAIKU_DRIVER_REQUIREMENTS(FBSD_TASKQUEUES);
NO_HAIKU_FBSD_MII_DRIVER();

/* Linux "mediatek/<file>" -> <file> in
 * /boot/system/[non-packaged/]data/firmware/lkpi_pcitest/ */
HAIKU_FIRMWARE_VERSION(0);
HAIKU_FIRMWARE_NAME_MAP({
	{"mediatek/WIFI_MT7961_patch_mcu_1_2_hdr.bin",
		"WIFI_MT7961_patch_mcu_1_2_hdr.bin"},
	{"mediatek/WIFI_RAM_CODE_MT7961_1.bin", "WIFI_RAM_CODE_MT7961_1.bin"},
	{"mediatek/WIFI_MT7922_patch_mcu_1_1_hdr.bin",
		"WIFI_MT7922_patch_mcu_1_1_hdr.bin"},
	{"mediatek/WIFI_RAM_CODE_MT7922_1.bin", "WIFI_RAM_CODE_MT7922_1.bin"},
});

/*
 * Called by freebsd_network in interrupt context before the (threaded)
 * handler is woken. With MSI the vector is exclusively ours, so claim it;
 * the Linux handler runs in the handler thread and deals with the device.
 */
int
HAIKU_CHECK_DISABLE_INTERRUPTS(device_t dev)
{
	return 1;
}

NO_HAIKU_REENABLE_INTERRUPTS();
