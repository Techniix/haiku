/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _WIRELESS_NETWORK_GROUPS_H
#define _WIRELESS_NETWORK_GROUPS_H


/*!	Renga: wireless networks as people know them, one per name.

	A mesh network, or a router with a 2.4 and a 5 GHz radio, has several
	access points that send the same network name (SSID). A scan lists each
	of them (each BSSID) once, so the NetworkStatus menu and the Network
	preferences showed the same network several times, in pairs by signal,
	where other systems show it once and choose the access point themselves.

	group_wireless_networks() folds the access points of one network
	together: same name and same security (authentication mode). The group
	keeps the strongest access point's entry, so its signal is shown, and
	is "associated" when any of its access points is. Networks with no name
	(hidden ones) are never folded, there is nothing to tell them apart by.
	The order of first appearance is kept; the callers sort afterwards.

	Joining a group joins by name (BNetworkDevice::JoinNetwork(const char*)):
	wpa_supplicant is given the SSID only and chooses the access point
	itself, as other systems do. (It was never given the BSSID: Haiku's
	wpa_supplicant front end sets "ssid" and "scan_ssid" only, so choosing
	one copy of the network never chose that access point either.)

	The per-access-point view stays available: "Show each access point" in
	the Network preferences (off by default), read here through
	wireless_show_access_points() from ~/config/settings/network_wireless.

	Header-only, so that NetworkStatus, the Network preferences and the
	harness's unit test share the same code without a new library.
*/


#include <FindDirectory.h>
#include <NetworkAddress.h>
#include <NetworkDevice.h>
#include <Path.h>

#include <stdio.h>
#include <string.h>

#include <set>
#include <vector>


namespace BPrivate {


struct wireless_network_group {
	wireless_network	network;		// the strongest access point's entry
	int32				access_points;
	bool				associated;
};


static inline bool
wireless_same_network(const wireless_network& a, const wireless_network& b)
{
	return a.name[0] != '\0'
		&& strncmp(a.name, b.name, sizeof(a.name)) == 0
		&& a.authentication_mode == b.authentication_mode;
}


static inline void
group_wireless_networks(const wireless_network* networks, uint32 count,
	const std::set<BNetworkAddress>& associated,
	std::vector<wireless_network_group>& groups)
{
	groups.clear();
	for (uint32 i = 0; i < count; i++) {
		const wireless_network& network = networks[i];
		bool isAssociated = associated.find(network.address)
			!= associated.end();
		size_t group = 0;
		for (; group < groups.size(); group++) {
			if (wireless_same_network(groups[group].network, network))
				break;
		}
		if (group == groups.size()) {
			wireless_network_group added;
			added.network = network;
			added.access_points = 1;
			added.associated = isAssociated;
			groups.push_back(added);
			continue;
		}
		wireless_network_group& existing = groups[group];
		existing.access_points++;
		existing.associated = existing.associated || isAssociated;
		if (network.signal_strength > existing.network.signal_strength)
			existing.network = network;
	}
}


//!	Every access point as its own entry (the option), in the same form.
static inline void
ungrouped_wireless_networks(const wireless_network* networks, uint32 count,
	const std::set<BNetworkAddress>& associated,
	std::vector<wireless_network_group>& groups)
{
	groups.clear();
	for (uint32 i = 0; i < count; i++) {
		wireless_network_group entry;
		entry.network = networks[i];
		entry.access_points = 1;
		entry.associated = associated.find(networks[i].address)
			!= associated.end();
		groups.push_back(entry);
	}
}


static inline bool
wireless_settings_path(BPath& path)
{
	return find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK
		&& path.Append("network_wireless") == B_OK;
}


//!	"Show each access point" (Network preferences); off by default.
static inline bool
wireless_show_access_points()
{
	BPath path;
	if (!wireless_settings_path(path))
		return false;
	FILE* file = fopen(path.Path(), "r");
	if (file == NULL)
		return false;
	char line[128];
	bool show = false;
	while (fgets(line, sizeof(line), file) != NULL) {
		if (strncmp(line, "show_access_points", 18) == 0)
			show = strstr(line + 18, "true") != NULL;
	}
	fclose(file);
	return show;
}


static inline status_t
set_wireless_show_access_points(bool show)
{
	BPath path;
	if (!wireless_settings_path(path))
		return B_ERROR;
	FILE* file = fopen(path.Path(), "w");
	if (file == NULL)
		return B_ERROR;
	fprintf(file, "# Network preferences: wireless networks\n"
		"show_access_points %s\n", show ? "true" : "false");
	fclose(file);
	return B_OK;
}


}	// namespace BPrivate


#endif	// _WIRELESS_NETWORK_GROUPS_H
