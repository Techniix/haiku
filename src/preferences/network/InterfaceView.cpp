/*
 * Copyright 2004-2015 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, <axeld@pinc-software.de>
 *		Alexander von Gluck, kallisti5@unixzen.com
 *		John Scipione, jscipione@gmail.com
 */


#include "InterfaceView.h"

#include <set>

#include <net/if_media.h>

#include <AutoDeleter.h>
#include <Button.h>
#include <Catalog.h>
#include <ControlLook.h>
#include <LayoutBuilder.h>
#include <NetworkAddress.h>
#include <StringForSize.h>
#include <StringView.h>
#include <TextControl.h>

#include "MediaTypes.h"
#include "WirelessNetworkMenuItem.h"

#include <CheckBox.h>
#include <WirelessNetworkGroups.h>


static const uint32 kMsgInterfaceToggle = 'onof';
static const uint32 kMsgInterfaceRenegotiate = 'redo';
static const uint32 kMsgJoinNetwork = 'join';
static const uint32 kMsgShowAccessPoints = 'shap';


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "IntefaceView"


// #pragma mark - InterfaceView


InterfaceView::InterfaceView()
	:
	BGroupView(B_VERTICAL),
	fPulseCount(0)
{
	SetFlags(Flags() | B_PULSE_NEEDED);

	// TODO: Small graph of throughput?

	BStringView* statusLabel = new BStringView("status label", B_TRANSLATE("Status:"));
	statusLabel->SetAlignment(B_ALIGN_RIGHT);
	fStatusField = new BStringView("status field", "");
	BStringView* macAddressLabel = new BStringView("mac address label",
		B_TRANSLATE("MAC address:"));
	macAddressLabel->SetAlignment(B_ALIGN_RIGHT);
	fMacAddressField = new BStringView("mac address field", "");
	BStringView* linkSpeedLabel = new BStringView("link speed label",
		B_TRANSLATE("Link speed:"));
	linkSpeedLabel->SetAlignment(B_ALIGN_RIGHT);
	fLinkSpeedField = new BStringView("link speed field", "");

	// TODO: These metrics may be better in a BScrollView?
	BStringView* linkTxLabel = new BStringView("tx label",
		B_TRANSLATE("Sent:"));
	linkTxLabel->SetAlignment(B_ALIGN_RIGHT);
	fLinkTxField = new BStringView("tx field", "");
	BStringView* linkRxLabel = new BStringView("rx label",
		B_TRANSLATE("Received:"));
	linkRxLabel->SetAlignment(B_ALIGN_RIGHT);
	fLinkRxField = new BStringView("rx field", "");

	fNetworkMenuField = new BMenuField(B_TRANSLATE("Network:"), new BMenu(
		B_TRANSLATE("Choose automatically")));
	fNetworkMenuField->SetAlignment(B_ALIGN_RIGHT);
	fNetworkMenuField->Menu()->SetLabelFromMarked(true);

	// Renga: networks are listed once per name and security; this shows
	// every access point (BSSID) instead (WirelessNetworkGroups.h).
	fShowAccessPoints = new BCheckBox("show access points",
		B_TRANSLATE("Show each access point"),
		new BMessage(kMsgShowAccessPoints));
	fShowAccessPoints->SetValue(BPrivate::wireless_show_access_points()
		? B_CONTROL_ON : B_CONTROL_OFF);
	fShowAccessPoints->SetToolTip(B_TRANSLATE("A network with several access "
		"points (a mesh, or a router with two radios) is shown once, and "
		"joining it lets the system choose the access point. Turn this on "
		"to see and choose each access point."));

	// Construct the BButtons
	fToggleButton = new BButton("onoff", B_TRANSLATE("Disable"),
		new BMessage(kMsgInterfaceToggle));

	fRenegotiateButton = new BButton("heal", B_TRANSLATE("Renegotiate"),
		new BMessage(kMsgInterfaceRenegotiate));
	fRenegotiateButton->SetEnabled(false);

	BLayoutBuilder::Group<>(this)
		.AddGrid()
			.Add(statusLabel, 0, 0)
			.Add(fStatusField, 1, 0)
			.AddMenuField(fNetworkMenuField, 0, 1, B_ALIGN_RIGHT, 1, 2)
			.Add(fShowAccessPoints, 1, 2)
			.Add(macAddressLabel, 0, 3)
			.Add(fMacAddressField, 1, 3)
			.Add(linkSpeedLabel, 0, 4)
			.Add(fLinkSpeedField, 1, 4)
			.Add(linkTxLabel, 0, 5)
			.Add(fLinkTxField, 1, 5)
			.Add(linkRxLabel, 0, 6)
			.Add(fLinkRxField, 1, 6)
		.End()
		.AddGlue()
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(fToggleButton)
			.Add(fRenegotiateButton)
		.End();
}


InterfaceView::~InterfaceView()
{
}


void
InterfaceView::SetTo(const char* name)
{
	fInterface.SetTo(name);
}


void
InterfaceView::AttachedToWindow()
{
	_Update();
		// Populate the fields

	fToggleButton->SetTarget(this);
	fRenegotiateButton->SetTarget(this);
	fShowAccessPoints->SetTarget(this);
}


void
InterfaceView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgJoinNetwork:
		{
			const char* name;
			BNetworkAddress address;
			if (message->FindString("name", &name) == B_OK) {
				BNetworkDevice device(fInterface.Name());
				// Renga: by name unless the entry is one access point
				status_t status = message->FindFlat("address", &address)
						== B_OK
					? device.JoinNetwork(address) : device.JoinNetwork(name);
				if (status != B_OK) {
					// This does not really matter, as it's stored this way,
					// anyway.
				}
				// TODO: store value
			}
			break;
		}
		case kMsgShowAccessPoints:
		{
			BPrivate::set_wireless_show_access_points(
				fShowAccessPoints->Value() == B_CONTROL_ON);
			// rebuild the list in the new form
			BMenu* menu = fNetworkMenuField->Menu();
			while (BMenuItem* item = menu->RemoveItem((int32)0))
				delete item;
			_Update();
			break;
		}
		case kMsgInterfaceToggle:
		{
			// Disable/enable interface
			uint32 flags = fInterface.Flags();
			if ((flags & IFF_UP) != 0)
				flags &= ~IFF_UP;
			else
				flags |= IFF_UP;

			if (fInterface.SetFlags(flags) == B_OK)
				_Update();
			break;
		}

		case kMsgInterfaceRenegotiate:
		{
			// TODO: renegotiate addresses
			break;
		}

		default:
			BView::MessageReceived(message);
	}
}


void
InterfaceView::Pulse()
{
	// Update the wireless network menu every 5 seconds
	_Update((fPulseCount++ % 5) == 0);
}


/*!	Populate fields with current settings.
*/
status_t
InterfaceView::_Update(bool updateWirelessNetworks)
{
	BNetworkDevice device(fInterface.Name());
	bool isWireless = device.IsWireless();
	bool disabled = (fInterface.Flags() & IFF_UP) == 0;

	uint32 flags = fInterface.Flags();

	if ((flags & IFF_LINK) == 0)
		fStatusField->SetText(B_TRANSLATE("no link"));
	else if ((flags & (IFF_UP | IFF_CONFIGURING)) == 0)
		fStatusField->SetText(B_TRANSLATE("no stateful configuration"));
	else if ((flags & IFF_CONFIGURING) == IFF_CONFIGURING)
		fStatusField->SetText(B_TRANSLATE("configuring"));
	else if ((flags & IFF_UP) == IFF_UP)
		fStatusField->SetText(B_TRANSLATE("connected"));
	else
		fStatusField->SetText(B_TRANSLATE("unknown"));

	BNetworkAddress hardwareAddress;
	if (device.GetHardwareAddress(hardwareAddress) == B_OK)
		fMacAddressField->SetText(hardwareAddress.ToString());
	else
		fMacAddressField->SetText("-");

	int media = fInterface.Media();
	if ((media & IFM_ACTIVE) != 0)
		fLinkSpeedField->SetText(media_type_to_string(media));
	else
		fLinkSpeedField->SetText("-");

	// Update Link stats
	ifreq_stats stats;
	if (fInterface.GetStats(stats) == B_OK) {
		char buffer[100];

		string_for_size(stats.send.bytes, buffer, sizeof(buffer));
		fLinkTxField->SetText(buffer);

		string_for_size(stats.receive.bytes, buffer, sizeof(buffer));
		fLinkRxField->SetText(buffer);
	}

	// TODO: move the wireless info to a separate tab. We should have a
	// BListView of available networks, rather than a menu, to make them more
	// readable and easier to browse and select.
	if (fNetworkMenuField->IsHidden(fNetworkMenuField) && isWireless) {
		fNetworkMenuField->Show();
		fShowAccessPoints->Show();
	} else if (!fNetworkMenuField->IsHidden(fNetworkMenuField)
		&& !isWireless) {
		fNetworkMenuField->Hide();
		fShowAccessPoints->Hide();
	}

	if (isWireless && updateWirelessNetworks) {
		// Rebuild network menu
		BMenu* menu = fNetworkMenuField->Menu();
		int32 count = menu->CountItems();

		// remove non-network items from menu and save them for later
		BMenuItem* chooseItem = NULL;
		BSeparatorItem* separatorItem = NULL;
		if (count > 0 && strcmp(menu->ItemAt(0)->Label(),
				B_TRANSLATE("Choose automatically")) == 0) {
			// remove Choose automatically item
			chooseItem = menu->RemoveItem((int32)0);
			// remove separator item too
			separatorItem = (BSeparatorItem*)menu->RemoveItem((int32)0);
			count -= 2;
		}

		BMenuItem* noNetworksFoundItem = NULL;
		if (menu->CountItems() > 0 && strcmp(menu->ItemAt(0)->Label(),
				B_TRANSLATE("<no wireless networks found>")) == 0) {
			// remove <no wireless networks found> item
			noNetworksFoundItem = menu->RemoveItem((int32)0);
			count--;
		}

		std::set<BNetworkAddress> associated;
		BNetworkAddress address;
		uint32 cookie = 0;
		while (device.GetNextAssociatedNetwork(cookie, address) == B_OK)
			associated.insert(address);

		wireless_network* networks = NULL;
		uint32 networksCount = 0;
		device.GetNetworks(networks, networksCount);
		uint32 scanned = networksCount;

		if ((fPulseCount % 15) == 0 && networksCount == 0) {
			// We don't seem to know of any networks, and it's been long
			// enough since the last scan, so trigger one to try and
			// find some networks.
			device.Scan(false, false);

			// We don't want to block for the full length of the scan, but
			// 50ms is often more than enough to find at least one network,
			// and the increase in perceived QoS to the user of not seeing
			// "no wireless networks" if we can avoid it is great enough
			// to merit such a wait. It's only just over ~4 vertical
			// retraces, anyway.
			snooze(50 * 1000);

			device.GetNetworks(networks, networksCount);
		}

		ArrayDeleter<wireless_network> networksDeleter(networks);

		// Renga: one entry per network name and security, unless "Show
		// each access point" is on (WirelessNetworkGroups.h)
		bool eachAccessPoint = fShowAccessPoints->Value() == B_CONTROL_ON;
		std::vector<BPrivate::wireless_network_group> entries;
		if (eachAccessPoint) {
			BPrivate::ungrouped_wireless_networks(networks, scanned,
				associated, entries);
		} else {
			BPrivate::group_wireless_networks(networks, scanned, associated,
				entries);
		}
		std::vector<wireless_network> shown;
		for (size_t i = 0; i < entries.size(); i++)
			shown.push_back(entries[i].network);
		networksCount = shown.size();
		const wireless_network* list = shown.empty() ? NULL : &shown[0];

		// go through menu items and remove networks that have dropped out
		for (int32 index = 0; index < count; index++) {
			WirelessNetworkMenuItem* networkItem =
				dynamic_cast<WirelessNetworkMenuItem*>(
					menu->ItemAt(index));
			if (networkItem == NULL)
				break;

			bool networkFound = false;
			for (uint32 i = 0; i < networksCount; i++) {
				if (networkItem->Network() == list[i]) {
					networkFound = true;
					break;
				}
			}

			if (!networkFound) {
				menu->RemoveItem(networkItem);
				count--;
			}
		}

		// go through networks and add new ones to menu
		for (uint32 i = 0; i < networksCount; i++) {
			const wireless_network& network = list[i];
			bool marked = entries[i].associated;

			bool networkFound = false;
			for (int32 index = 0; index < count; index++) {
				WirelessNetworkMenuItem* networkItem =
					dynamic_cast<WirelessNetworkMenuItem*>(
						menu->ItemAt(index));
				if (networkItem == NULL)
					break;

				if (networkItem->Network() == network) {
					// found it
					networkFound = true;
					if (marked)
						networkItem->SetMarked(true);
					break;
				}
			}

			if (!networkFound) {
				BMessage* message = new BMessage(kMsgJoinNetwork);
				message->AddString("device", fInterface.Name());
				message->AddString("name", network.name);
				if (eachAccessPoint || network.name[0] == '\0')
					message->AddFlat("address", &network.address);
				BMenuItem* item = new WirelessNetworkMenuItem(network,
					message);
				menu->AddItem(item);
				if (marked)
					item->SetMarked(true);
			}

			count++;
		}

		if (count == 0) {
			// no networks found
			if (noNetworksFoundItem != NULL)
				menu->AddItem(noNetworksFoundItem);
			else {
				BMenuItem* item = new BMenuItem(
					B_TRANSLATE("<no wireless networks found>"), NULL);
				item->SetEnabled(false);
				menu->AddItem(item);
			}
		} else {
			// sort items by signal strength
			menu->SortItems(WirelessNetworkMenuItem::CompareSignalStrength);

			// add Choose automatically item to start
			if (chooseItem != NULL) {
				menu->AddItem(chooseItem, 0);
				menu->AddItem(separatorItem, 1);
			} else {
				BMenuItem* item = new BMenuItem(
					B_TRANSLATE("Choose automatically"), NULL);
				if (menu->FindMarked() == NULL)
					item->SetMarked(true);
				menu->AddItem(item, 0);
				menu->AddItem(new BSeparatorItem(), 1);
			}
		}

		menu->SetTargetForItems(this);
	}

	//fRenegotiateButton->SetEnabled(!disabled);
	fToggleButton->SetLabel(disabled
		? B_TRANSLATE("Enable") : B_TRANSLATE("Disable"));

	return B_OK;
}
