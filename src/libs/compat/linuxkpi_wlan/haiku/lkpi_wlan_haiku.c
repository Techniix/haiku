/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku attach ordering for LinuxKPI 802.11.
 *
 * freebsd_network's device_attach() calls freebsd_wlan's start_wlan() as soon
 * as the driver's attach method returns; start_wlan() looks the net80211
 * device up by the device's name and creates the station vap. Linux drivers
 * (mt7921 among them) return from probe at once and register with mac80211
 * later from a work queue (firmware load first), so we wait in attach until
 * LinuxKPI has created the ieee80211com for this device.
 *
 * Haiku holds Giant across the whole attach (driver.c), and LinuxKPI's
 * ieee80211 attach takes bus_topo_lock(), which is Giant here, so Giant is
 * released while waiting.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_var.h>
#include <net/if_media.h>
#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_proto.h>

#include <KernelExport.h>

#include "lkpi_control_kern.h"

#define	LKPI_WLAN_ATTACH_TIMEOUT_MS	20000

extern int (*lkpi_pci_attach_wait_hook)(device_t dev);	/* lkpi_pci.c */

static int
lkpi_wlan_attach_wait(device_t dev)
{
	const char *name = device_get_nameunit(dev);
	bool giant = mtx_owned(&Giant);
	int ms;

	if (giant)
		mtx_unlock(&Giant);
	for (ms = 0; ms < LKPI_WLAN_ATTACH_TIMEOUT_MS; ms += 10) {
		if (ieee80211_find_com(name) != NULL)
			break;
		snooze(10000);
	}
	if (giant)
		mtx_lock(&Giant);

	if (ms >= LKPI_WLAN_ATTACH_TIMEOUT_MS) {
		device_printf(dev, "LinuxKPI 802.11: no wireless device "
		    "registered within %d s (firmware missing?)\n",
		    LKPI_WLAN_ATTACH_TIMEOUT_MS / 1000);
		return (ETIMEDOUT);
	}
	device_printf(dev, "LinuxKPI 802.11: %s registered after %d ms\n",
	    name, ms);
	return (0);
}

/*
 * Read-only 802.11 state for LKPI_CTL_GET_STATS (lkpictl status, LinuxKPI
 * Manager): where we are (band, channel, width, mode), block-ack sessions,
 * and net80211's own drop counters, so a slow link can be explained without
 * a debug build. Values only; nothing is changed. Haiku printf: no %j/%D.
 */
#define	LKPI_STAT(_r, _st, _f)						\
	lkpi_control_add((_r), "wlan." #_f, "%u", (unsigned)(_st)->is_##_f)

static void
lkpi_wlan_stats_vap(struct lkpi_reply *r, struct ieee80211vap *vap)
{
	struct ieee80211com *ic = vap->iv_ic;
	const struct ieee80211_channel *c;
	const struct ieee80211_stats *st = &vap->iv_stats;
	struct ieee80211_node *ni;
	char key[48];
	int tid;

	lkpi_control_add(r, "wlan.vap", "%s", vap->iv_ifp != NULL ?
	    if_name(vap->iv_ifp) : "?");
	lkpi_control_add(r, "wlan.state", "%s",
	    vap->iv_state < IEEE80211_S_MAX ?
	    ieee80211_state_name[vap->iv_state] : "?");

	IEEE80211_LOCK(ic);
	c = ic->ic_curchan;
	if (c != NULL && c != IEEE80211_CHAN_ANYC) {
		lkpi_control_add(r, "wlan.channel", "%u", c->ic_ieee);
		lkpi_control_add(r, "wlan.freq_mhz", "%u", c->ic_freq);
		lkpi_control_add(r, "wlan.band", "%s",
		    IEEE80211_IS_CHAN_2GHZ(c) ? "2.4GHz" :
		    IEEE80211_IS_CHAN_5GHZ(c) ? "5GHz" : "other");
		lkpi_control_add(r, "wlan.width_mhz", "%u",
		    IEEE80211_IS_CHAN_VHT160(c) ? 160 :
		    IEEE80211_IS_CHAN_VHT80(c) ? 80 :
		    IEEE80211_IS_CHAN_HT40(c) ? 40 : 20);
		lkpi_control_add(r, "wlan.channel_mode", "%s",
		    IEEE80211_IS_CHAN_VHT(c) ? "VHT (802.11ac)" :
		    IEEE80211_IS_CHAN_HT(c) ? "HT (802.11n)" : "legacy");
	}
	ni = (vap->iv_bss != NULL) ? ieee80211_ref_node(vap->iv_bss) : NULL;
	IEEE80211_UNLOCK(ic);

	if (ni != NULL) {
		const uint8_t *a = ni->ni_macaddr;

		lkpi_control_add(r, "wlan.bssid", "%02x:%02x:%02x:%02x:%02x:%02x",
		    a[0], a[1], a[2], a[3], a[4], a[5]);
		lkpi_control_add(r, "wlan.node_flags", "%s%s%s%s%s",
		    (ni->ni_flags & IEEE80211_NODE_HT) ? "HT " : "",
		    (ni->ni_flags & IEEE80211_NODE_VHT) ? "VHT " : "",
		    (ni->ni_flags & IEEE80211_NODE_AMPDU_RX) ? "AMPDU-RX " : "",
		    (ni->ni_flags & IEEE80211_NODE_AMPDU_TX) ? "AMPDU-TX " : "",
		    (ni->ni_flags & IEEE80211_NODE_AMSDU_RX) ? "AMSDU-RX" : "");
		lkpi_control_add(r, "wlan.node_chw", "%d", (int)ni->ni_chw);
		lkpi_control_add(r, "wlan.node_vht_chanwidth", "%u",
		    ni->ni_vht_chanwidth);
		if (ic->ic_node_getrssi != NULL)
			lkpi_control_add(r, "wlan.rssi_half_db", "%d",
			    (int)ic->ic_node_getrssi(ni));
		for (tid = 0; tid < WME_NUM_TID; tid++) {
			const struct ieee80211_rx_ampdu *rap =
			    &ni->ni_rx_ampdu[tid];
			const struct ieee80211_tx_ampdu *tap =
			    &ni->ni_tx_ampdu[tid];

			if (rap->rxa_flags & IEEE80211_AGGR_RUNNING) {
				snprintf(key, sizeof(key), "wlan.rx_ba.tid%d", tid);
				lkpi_control_add(r, key, "window %u",
				    rap->rxa_wnd);
			}
			if ((tap->txa_flags & IEEE80211_AGGR_RUNNING) ||
			    tap->txa_attempts != 0) {
				snprintf(key, sizeof(key), "wlan.tx_ba.tid%d", tid);
				lkpi_control_add(r, key, "%s window %u attempts %u",
				    (tap->txa_flags & IEEE80211_AGGR_RUNNING) ?
				    "running" : "pending", tap->txa_wnd,
				    tap->txa_attempts);
			}
		}
		ieee80211_free_node(ni);
	}

	lkpi_control_add(r, "wlan.ierrors", "%llu",
	    (unsigned long long)counter_u64_fetch(ic->ic_ierrors));
	lkpi_control_add(r, "wlan.oerrors", "%llu",
	    (unsigned long long)counter_u64_fetch(ic->ic_oerrors));
	/* receive drops by reason (net80211 ieee80211_stats) */
	LKPI_STAT(r, st, rx_dup);
	LKPI_STAT(r, st, rx_tooshort);
	LKPI_STAT(r, st, rx_badversion);
	LKPI_STAT(r, st, rx_wrongbss);
	LKPI_STAT(r, st, rx_wrongdir);
	LKPI_STAT(r, st, rx_unencrypted);
	LKPI_STAT(r, st, rx_wepfail);
	LKPI_STAT(r, st, rx_decap);
	LKPI_STAT(r, st, rx_decryptcrc);
	LKPI_STAT(r, st, rx_ccmpreplay);
	LKPI_STAT(r, st, rx_ccmpformat);
	LKPI_STAT(r, st, rx_ccmpmic);
	LKPI_STAT(r, st, rx_badkeyid);
	LKPI_STAT(r, st, rx_nobuf);
	LKPI_STAT(r, st, rx_mgtdiscard);
	LKPI_STAT(r, st, rx_ctl);
	LKPI_STAT(r, st, amsdu_decap);
	LKPI_STAT(r, st, amsdu_tooshort);
	LKPI_STAT(r, st, amsdu_split);
	LKPI_STAT(r, st, ampdu_rx_oor);
	LKPI_STAT(r, st, ampdu_rx_drop);
	LKPI_STAT(r, st, ampdu_rx_move);
	LKPI_STAT(r, st, ampdu_rx_age);
	LKPI_STAT(r, st, addba_reject);
	LKPI_STAT(r, st, ampdu_stop);
	LKPI_STAT(r, st, ampdu_stop_failed);
	LKPI_STAT(r, st, ampdu_bar_tx);
	LKPI_STAT(r, st, ampdu_bar_oow);
	LKPI_STAT(r, st, ampdu_bar_rx);
	/* transmit */
	LKPI_STAT(r, st, tx_nobuf);
	LKPI_STAT(r, st, tx_nonode);
	LKPI_STAT(r, st, tx_unknownmgt);
	LKPI_STAT(r, st, tx_badcipher);
	LKPI_STAT(r, st, tx_nodefkey);
	LKPI_STAT(r, st, tx_noheadroom);
}

static void
lkpi_wlan_stats_com(void *arg, struct ieee80211com *ic)
{
	struct lkpi_reply *r = arg;
	struct ieee80211vap *vap;

	lkpi_control_add(r, "wlan.device", "%s", ic->ic_name);
	/* vaps are created/destroyed under Giant + the ic lock in our
	 * driver; the list itself is read under the ic lock */
	IEEE80211_LOCK(ic);
	vap = TAILQ_FIRST(&ic->ic_vaps);
	IEEE80211_UNLOCK(ic);
	if (vap != NULL)
		lkpi_wlan_stats_vap(r, vap);	/* station mode: one vap */
}

static void
lkpi_wlan_stats(struct lkpi_reply *r)
{
	ieee80211_iterate_coms(lkpi_wlan_stats_com, r);
}

static void
lkpi_wlan_haiku_init(void *arg __unused)
{
	lkpi_pci_attach_wait_hook = lkpi_wlan_attach_wait;
	lkpi_control_set_stats_provider(lkpi_wlan_stats);
}
SYSINIT(lkpi_wlan_haiku, SI_SUB_DRIVERS, SI_ORDER_FIRST, lkpi_wlan_haiku_init,
    NULL);
