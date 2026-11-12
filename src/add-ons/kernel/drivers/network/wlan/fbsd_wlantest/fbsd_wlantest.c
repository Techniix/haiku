/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*
 * A FreeBSD net80211 driver without a radio, for the harness (roadmap
 * F-26, F-34). It matches QEMU's PCI test device (1b36:0005), which no
 * other driver claims, and goes through the life cycle of a FreeBSD or
 * LinuxKPI Wi-Fi driver on Haiku:
 *
 * Only when /boot/system/non-packaged/data/ce-harness/fbsd_wlantest exists
 * (see wlantest_probe()).
 *
 * - attach: ieee80211_ifattach(), then one extra station vap created and
 *   deleted at once (ic_vap_create, ic_vap_delete: ieee80211_vap_setup()/
 *   _attach(), ieee80211_vap_detach(), whose ether_ifdetach() and
 *   if_free() tear the vap's ifnet down);
 * - freebsd_wlan's start_wlan() creates the vap that is published as
 *   /dev/net/fbsd_wlantest/<n>;
 * - detach (when devfs unloads or reloads the driver): stop_wlan() deletes
 *   that vap, then ieee80211_ifdetach().
 *
 * Each step prints "fbsd_wlantest: <step>" into the syslog, with a load
 * counter, for wlan_reload_test. Nothing is ever transmitted.
 */


#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <dev/pci/pcivar.h>

#include <net/if.h>
#include <net/if_media.h>
#include <net/ethernet.h>

#include <net80211/ieee80211_var.h>


HAIKU_FBSD_WLAN_DRIVER_GLUE(fbsd_wlantest, fbsd_wlantest, pci)
HAIKU_DRIVER_REQUIREMENTS(FBSD_WLAN);
HAIKU_FIRMWARE_VERSION(0);
NO_HAIKU_FBSD_MII_DRIVER();
NO_HAIKU_CHECK_DISABLE_INTERRUPTS();
NO_HAIKU_REENABLE_INTERRUPTS();


struct wlantest_softc {
	struct ieee80211com	sc_ic;
	device_t			sc_dev;
};

struct wlantest_vap {
	struct ieee80211vap	vap;
};


static bigtime_t sLoaded;
	/* when this image was loaded: tells the loads apart in the syslog */


static struct ieee80211vap *
wlantest_vap_create(struct ieee80211com *ic, const char name[IFNAMSIZ],
	int unit, enum ieee80211_opmode opmode, int flags,
	const uint8_t bssid[IEEE80211_ADDR_LEN],
	const uint8_t mac[IEEE80211_ADDR_LEN])
{
	struct wlantest_vap *wvp;
	struct ieee80211vap *vap;

	if (opmode != IEEE80211_M_STA)
		return NULL;

	wvp = malloc(sizeof(struct wlantest_vap), M_80211_VAP,
		M_WAITOK | M_ZERO);
	vap = &wvp->vap;
	if (ieee80211_vap_setup(ic, vap, name, unit, opmode,
			flags | IEEE80211_CLONE_NOBEACONS, bssid) != 0) {
		free(wvp, M_80211_VAP);
		return NULL;
	}
	ieee80211_vap_attach(vap, ieee80211_media_change,
		ieee80211_media_status, mac);
	ic->ic_opmode = opmode;
	return vap;
}


static void
wlantest_vap_delete(struct ieee80211vap *vap)
{
	struct wlantest_vap *wvp = (struct wlantest_vap *)vap;

	ieee80211_vap_detach(vap);
	free(wvp, M_80211_VAP);
	printf("fbsd_wlantest: vap deleted (load %" B_PRId64 ")\n", sLoaded);
}


static int
wlantest_raw_xmit(struct ieee80211_node *ni, struct mbuf *m,
	const struct ieee80211_bpf_params *params)
{
	m_freem(m);
	return ENETDOWN;
}


static int
wlantest_transmit(struct ieee80211com *ic, struct mbuf *m)
{
	return ENXIO;
}


static void
wlantest_parent(struct ieee80211com *ic)
{
}


static void
wlantest_nothing(struct ieee80211com *ic)
{
}


/*
 * The driver attaches only once wlan_reload_test asks for it by creating
 * this file: attached from boot on, its net80211 and mbuf allocations
 * took physical memory below 16 MiB that linuxkpi_test's 24-bit DMA
 * checks need under UEFI (they failed in two of two runs with it, passed
 * without it).
 */
#define WLANTEST_ENABLE_FILE \
	"/boot/system/non-packaged/data/ce-harness/fbsd_wlantest"


static int
wlantest_probe(device_t dev)
{
	struct stat st;
	if (pci_get_vendor(dev) != 0x1b36 || pci_get_device(dev) != 0x0005)
		return ENXIO;
	if (stat(WLANTEST_ENABLE_FILE, &st) != 0)
		return ENXIO;
	device_set_desc(dev, "Renga net80211 test device (QEMU pci-testdev)");
	return BUS_PROBE_DEFAULT;
}


static int
wlantest_attach(device_t dev)
{
	struct wlantest_softc *sc = device_get_softc(dev);
	struct ieee80211com *ic = &sc->sc_ic;
	struct ieee80211vap *vap;
	uint8_t bands[IEEE80211_MODE_BYTES];
	static const uint8_t kAddress[IEEE80211_ADDR_LEN]
		= { 0x02, 0x52, 0x47, 0x00, 0x00, 0x01 };

	sLoaded = system_time();
	sc->sc_dev = dev;

	ic->ic_softc = sc;
	ic->ic_name = device_get_nameunit(dev);
	ic->ic_opmode = IEEE80211_M_STA;
	ic->ic_phytype = IEEE80211_T_OFDM;
	ic->ic_caps = IEEE80211_C_STA;
	IEEE80211_ADDR_COPY(ic->ic_macaddr, kAddress);

	memset(bands, 0, sizeof(bands));
	setbit(bands, IEEE80211_MODE_11B);
	setbit(bands, IEEE80211_MODE_11G);
	ieee80211_init_channels(ic, NULL, bands);

	ieee80211_ifattach(ic);
	ic->ic_raw_xmit = wlantest_raw_xmit;
	ic->ic_scan_start = wlantest_nothing;
	ic->ic_scan_end = wlantest_nothing;
	ic->ic_set_channel = wlantest_nothing;
	ic->ic_vap_create = wlantest_vap_create;
	ic->ic_vap_delete = wlantest_vap_delete;
	ic->ic_parent = wlantest_parent;
	ic->ic_transmit = wlantest_transmit;

	printf("fbsd_wlantest: attached (load %" B_PRId64 ", %d channels)\n",
		sLoaded, ic->ic_nchans);

	/* A vap made and torn down at once: the ifnet's whole life. */
	vap = ic->ic_vap_create(ic, "wlan", device_get_unit(dev) + 8,
		IEEE80211_M_STA, 0, NULL, ic->ic_macaddr);
	if (vap == NULL) {
		printf("fbsd_wlantest: FAIL extra vap not created\n");
		return 0;
	}
	ic->ic_vap_delete(vap);
	printf("fbsd_wlantest: extra vap done (load %" B_PRId64 ")\n", sLoaded);
	return 0;
}


static int
wlantest_detach(device_t dev)
{
	struct wlantest_softc *sc = device_get_softc(dev);

	ieee80211_ifdetach(&sc->sc_ic);
	printf("fbsd_wlantest: detached (load %" B_PRId64 ")\n", sLoaded);
	return 0;
}


static device_method_t wlantest_methods[] = {
	DEVMETHOD(device_probe,		wlantest_probe),
	DEVMETHOD(device_attach,	wlantest_attach),
	DEVMETHOD(device_detach,	wlantest_detach),
	DEVMETHOD_END
};

static driver_t wlantest_driver = {
	"fbsd_wlantest",
	wlantest_methods,
	sizeof(struct wlantest_softc)
};

DRIVER_MODULE(fbsd_wlantest, pci, wlantest_driver, NULL, NULL);
