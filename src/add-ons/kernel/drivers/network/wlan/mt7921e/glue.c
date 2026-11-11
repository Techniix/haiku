/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku glue for mt7921e (Linux mt76 on LinuxKPI). The FreeBSD driver_t
 * handed to Haiku is LinuxKPI's PCI bridge (lkpi_pci.c), which matches
 * devices against mt7921's MODULE_DEVICE_TABLE.
 */
#include <sys/bus.h>
#include <sys/kernel.h>

HAIKU_FBSD_WLAN_DRIVER_GLUE(mt7921e, lkpi, pci);
HAIKU_DRIVER_REQUIREMENTS(FBSD_TASKQUEUES | FBSD_WLAN);
NO_HAIKU_FBSD_MII_DRIVER();

/* Linux "mediatek/<file>" -> <file> in
 * /boot/system/[non-packaged/]data/firmware/mt7921e/ */
HAIKU_FIRMWARE_VERSION(0);
HAIKU_FIRMWARE_NAME_MAP({
	{"mediatek/WIFI_MT7961_patch_mcu_1_2_hdr.bin",
		"WIFI_MT7961_patch_mcu_1_2_hdr.bin"},
	{"mediatek/WIFI_RAM_CODE_MT7961_1.bin", "WIFI_RAM_CODE_MT7961_1.bin"},
	{"mediatek/WIFI_MT7961_patch_mcu_1a_2_hdr.bin",
		"WIFI_MT7961_patch_mcu_1a_2_hdr.bin"},
	{"mediatek/WIFI_RAM_CODE_MT7961_1a.bin", "WIFI_RAM_CODE_MT7961_1a.bin"},
	{"mediatek/WIFI_MT7922_patch_mcu_1_1_hdr.bin",
		"WIFI_MT7922_patch_mcu_1_1_hdr.bin"},
	{"mediatek/WIFI_RAM_CODE_MT7922_1.bin", "WIFI_RAM_CODE_MT7922_1.bin"},
});

/*
 * Called by freebsd_network in interrupt context before the (threaded)
 * handler is woken. mt7921e uses MSI only when available; with MSI the
 * vector is exclusively ours, so claim it and let the Linux handler run in
 * the handler thread.
 */
static int32 sInterruptCount;

int
HAIKU_CHECK_DISABLE_INTERRUPTS(device_t dev)
{
	/* Stage (c) diagnostics: log the first interrupts, then every 1024th. */
	int32 n = atomic_add(&sInterruptCount, 1) + 1;
	if (n <= 8 || (n & 1023) == 0)
		dprintf("mt7921e: interrupt #%" B_PRId32 "\n", n);
	return 1;
}

NO_HAIKU_REENABLE_INTERRUPTS();
