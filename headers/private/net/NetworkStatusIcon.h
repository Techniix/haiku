/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _NETWORK_STATUS_ICON_H
#define _NETWORK_STATUS_ICON_H


//!	Which Deskbar network icon (NetworkStatus) fits a connection: the kind
//	of link (Wi-Fi, wired, USB tethering), its state and its signal. Kept
//	apart from the view so that it can be unit tested (tests/wifi).


#include <string.h>

#include <SupportDefs.h>


namespace BPrivate {


// The HVIF icons of NetworkStatusIconsRenga.rdef (written by
// 3rdparty/ce/tools/make_network_icons.py); the numbers are the resource IDs.
enum {
	kNetworkIconWifi0		= 1100,	// + 0..4 lit bars
	kNetworkIconWifiOff		= 1105,
	kNetworkIconWired		= 1106,
	kNetworkIconWiredDim	= 1107,
	kNetworkIconTether		= 1108,
	kNetworkIconTetherDim	= 1109,
	kNetworkIconNoInternet	= 1110,
	kNetworkIconLock		= 1111,
	kNetworkIconCross		= 1112,
	kNetworkIconMenuLock	= 1113,	// full size, for the menu

	kNetworkIconFirst		= kNetworkIconWifi0,
	kNetworkIconCount		= 14,
	kNetworkIconNone		= 0
};


enum network_link_kind {
	NETWORK_LINK_WIRED = 0,
	NETWORK_LINK_WIRELESS,
	NETWORK_LINK_TETHER
};


enum network_link_state {
	NETWORK_STATE_DISCONNECTED = 0,	// no link, or no device
	NETWORK_STATE_CONNECTING,		// link up, being configured
	NETWORK_STATE_CONNECTED,		// configured; internet reachable or unknown
	NETWORK_STATE_NO_INTERNET,		// configured, the internet check failed
	NETWORK_STATE_RADIO_OFF			// Wi-Fi switched off
};


struct network_icon {
	int32	base;
	int32	badge;	// kNetworkIconNone for none

	bool operator==(const network_icon& other) const
	{
		return base == other.base && badge == other.badge;
	}
	bool operator!=(const network_icon& other) const
	{
		return !(*this == other);
	}
};


//!	Frames of the "connecting" animation: the Wi-Fi bars fill one by one.
static const int32 kNetworkConnectingFrames = 5;


/*!	Lit bars (0-4) for a signal in percent, as the Haiku network UI shows
	signal_strength (RadioView). A negative value means unknown: full bars.
*/
static inline int32
network_signal_bars(int32 percent)
{
	if (percent < 0 || percent >= 75)
		return 4;
	if (percent >= 50)
		return 3;
	if (percent >= 25)
		return 2;
	if (percent > 0)
		return 1;
	return 0;
}


/*!	USB tethering shows a phone. Phones share their connection over RNDIS
	(Android) or NCM (newer Android and iOS); CDC ECM is mostly USB Ethernet
	adapters, so it stays "wired".
*/
static inline network_link_kind
network_link_kind_for(const char* interfaceName, bool wireless)
{
	if (wireless)
		return NETWORK_LINK_WIRELESS;
	if (interfaceName != NULL && (strstr(interfaceName, "usb_rndis") != NULL
			|| strstr(interfaceName, "usb_ncm") != NULL))
		return NETWORK_LINK_TETHER;
	return NETWORK_LINK_WIRED;
}


//!	tun/N and tap/N (the tunnel device): VPNs and similar.
static inline bool
network_is_tunnel(const char* interfaceName)
{
	return interfaceName != NULL && (strncmp(interfaceName, "tun/", 4) == 0
		|| strncmp(interfaceName, "tap/", 4) == 0);
}


/*!	The icon for a link. \a frame counts the animation ticks while
	connecting; \a tunnel adds the lock when a VPN carries the traffic.
*/
static inline network_icon
network_icon_for(network_link_kind kind, network_link_state state,
	int32 signalPercent, int32 frame = 0, bool tunnel = false)
{
	network_icon icon = { kNetworkIconNone, kNetworkIconNone };
	if (frame < 0)
		frame = 0;

	switch (kind) {
		case NETWORK_LINK_WIRELESS:
			switch (state) {
				case NETWORK_STATE_RADIO_OFF:
					icon.base = kNetworkIconWifiOff;
					return icon;
				case NETWORK_STATE_DISCONNECTED:
					icon.base = kNetworkIconWifi0;
					icon.badge = kNetworkIconCross;
					return icon;
				case NETWORK_STATE_CONNECTING:
					icon.base = kNetworkIconWifi0
						+ frame % kNetworkConnectingFrames;
					return icon;
				default:
					icon.base = kNetworkIconWifi0
						+ network_signal_bars(signalPercent);
					break;
			}
			break;

		case NETWORK_LINK_WIRED:
		case NETWORK_LINK_TETHER:
		{
			bool tether = kind == NETWORK_LINK_TETHER;
			int32 lit = tether ? kNetworkIconTether : kNetworkIconWired;
			int32 dim = tether ? kNetworkIconTetherDim : kNetworkIconWiredDim;
			switch (state) {
				case NETWORK_STATE_DISCONNECTED:
				case NETWORK_STATE_RADIO_OFF:
					icon.base = dim;
					icon.badge = kNetworkIconCross;
					return icon;
				case NETWORK_STATE_CONNECTING:
					// blink
					icon.base = frame % 2 == 0 ? dim : lit;
					return icon;
				default:
					icon.base = lit;
					break;
			}
			break;
		}
	}

	if (state == NETWORK_STATE_NO_INTERNET)
		icon.badge = kNetworkIconNoInternet;
	else if (tunnel)
		icon.badge = kNetworkIconLock;
	return icon;
}


//!	"2.4 GHz", "5 GHz" or "6 GHz" for a channel's centre frequency in MHz.
static inline const char*
network_band_name(uint32 megahertz)
{
	if (megahertz >= 2400 && megahertz < 2500)
		return "2.4 GHz";
	if (megahertz >= 5925 && megahertz < 7125)
		return "6 GHz";
	if (megahertz >= 4900 && megahertz < 5925)
		return "5 GHz";
	return NULL;
}


}	// namespace BPrivate


#endif	// _NETWORK_STATUS_ICON_H
