/*
 * Copyright 2006-2013, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Dario Casalinuovo
 *		Axel Dörfler, axeld@pinc-software.de
 *		Rene Gollent, rene@gollent.com
 *		Hugo Santos, hugosantos@gmail.com
 */


#include "NetworkStatusView.h"

#include <algorithm>
#include <set>
#include <vector>

#include <arpa/inet.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <unistd.h>

#include <AboutWindow.h>
#include <Alert.h>
#include <Application.h>
#include <Catalog.h>
#include <ControlLook.h>
#include <Bitmap.h>
#include <Deskbar.h>
#include <Dragger.h>
#include <Drivers.h>
#include <IconUtils.h>
#include <Locale.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <NetworkInterface.h>
#include <NetworkRoster.h>
#include <NetworkRoute.h>
#include <Path.h>
#include <PathMonitor.h>
#include <PopUpMenu.h>
#include <Resources.h>
#include <Roster.h>
#include <String.h>
#include <TextView.h>

#include "NetworkStatus.h"
#include "NetworkStatusIcons.h"
#include "RadioView.h"
#include "WirelessFrequency.h"
#include "WirelessNetworkMenuItem.h"

#include <WirelessNetworkGroups.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "NetworkStatusView"


static const char *kStatusDescriptions[] = {
	B_TRANSLATE("Unknown"),
	B_TRANSLATE("No link"),
	B_TRANSLATE("No stateful configuration"),
	B_TRANSLATE("Configuring"),
	B_TRANSLATE("Connected")
};

extern "C" _EXPORT BView *instantiate_deskbar_item(float maxWidth, float maxHeight);


const uint32 kMsgShowConfiguration = 'shcf';
const uint32 kMsgOpenNetworkPreferences = 'onwp';
const uint32 kMsgJoinNetwork = 'join';
const uint32 kMsgAnimate = 'anim';
const uint32 kMsgTick = 'tick';
const uint32 kMsgConnectivityResult = 'cnrs';
const uint32 kMsgToggleWireless = 'twif';

// Renga: how often the icon follows the signal, and the animation speed
static const bigtime_t kTickInterval = 3000000;
static const bigtime_t kAnimationInterval = 350000;

using namespace BPrivate;

const uint32 kMinIconWidth = 16;
const uint32 kMinIconHeight = 16;


//	#pragma mark - NetworkStatusView


NetworkStatusView::NetworkStatusView(BRect frame, int32 resizingMode,
		bool inDeskbar)
	: BView(frame, kDeskbarItemName, resizingMode,
		B_WILL_DRAW | B_TRANSPARENT_BACKGROUND | B_FRAME_EVENTS),
	fInDeskbar(inDeskbar)
{
	_Init();

	if (!inDeskbar) {
		// we were obviously added to a standard window - let's add a dragger
		frame.OffsetTo(B_ORIGIN);
		frame.top = frame.bottom - 7;
		frame.left = frame.right - 7;
		BDragger* dragger = new BDragger(frame, this,
			B_FOLLOW_RIGHT | B_FOLLOW_BOTTOM);
		AddChild(dragger);
	} else
		_Update();
}


NetworkStatusView::NetworkStatusView(BMessage* archive)
	: BView(archive),
	fInDeskbar(false)
{
	app_info info;
	if (be_app->GetAppInfo(&info) == B_OK
		&& !strcasecmp(info.signature, "application/x-vnd.Be-TSKB"))
		fInDeskbar = true;

	_Init();
}


NetworkStatusView::~NetworkStatusView()
{
	if (fCheckThread >= 0) {
		status_t result;
		wait_for_thread(fCheckThread, &result);
	}
	delete fAnimation;
	delete fTicker;
	for (int i = 0; i < kStatusCount; i++) {
		delete fTrayIcons[i];
		delete fNotifyIcons[i];
	}
	for (int i = 0; i < kNetworkIconCount; i++)
		delete fIcons[i];
	delete fMenuLock;
}


void
NetworkStatusView::_Init()
{
	for (int i = 0; i < kStatusCount; i++) {
		fTrayIcons[i] = NULL;
		fNotifyIcons[i] = NULL;
	}
	for (int i = 0; i < kNetworkIconCount; i++)
		fIcons[i] = NULL;
	fMenuLock = NULL;
	fIcon.base = kNetworkIconNone;
	fIcon.badge = kNetworkIconNone;
	fState = NETWORK_STATE_DISCONNECTED;
	fLinkState = NETWORK_STATE_DISCONNECTED;
	fFrame = 0;
	fAnimation = NULL;
	fTicker = NULL;
	fConnectivityResult = -1;
	fCheckGeneration = 0;
	fCheckThread = -1;
	fLastCheck = 0;
	_LoadConnectivitySettings();

	_UpdateBitmaps();
}


void
NetworkStatusView::_UpdateBitmaps()
{
	for (int i = 0; i < kStatusCount; i++) {
		delete fTrayIcons[i];
		delete fNotifyIcons[i];
		fTrayIcons[i] = NULL;
		fNotifyIcons[i] = NULL;
	}
	for (int i = 0; i < kNetworkIconCount; i++) {
		delete fIcons[i];
		fIcons[i] = NULL;
	}
	delete fMenuLock;
	fMenuLock = NULL;

	image_info info;
	if (our_image(info) != B_OK)
		return;

	BFile file(info.name, B_READ_ONLY);
	if (file.InitCheck() < B_OK)
		return;

	BResources resources(&file);
#ifdef HAIKU_TARGET_PLATFORM_HAIKU
	if (resources.InitCheck() < B_OK)
		return;
#endif

	for (int i = 0; i < kStatusCount; i++) {
		const void* data = NULL;
		size_t size;
		data = resources.LoadResource(B_VECTOR_ICON_TYPE,
			kNetworkStatusNoDevice + i, &size);
		if (data != NULL) {
			// Scale main tray icon
			BBitmap* trayIcon = new BBitmap(Bounds(), B_RGBA32);
			if (trayIcon->InitCheck() == B_OK
				&& BIconUtils::GetVectorIcon((const uint8 *)data,
					size, trayIcon) == B_OK) {
				fTrayIcons[i] = trayIcon;
			} else
				delete trayIcon;

			// Scale notification icon
			BBitmap* notifyIcon
				= new BBitmap(BRect(B_ORIGIN, be_control_look->ComposeIconSize(32)), B_RGBA32);
			if (notifyIcon->InitCheck() == B_OK
				&& BIconUtils::GetVectorIcon((const uint8 *)data,
					size, notifyIcon) == B_OK) {
				fNotifyIcons[i] = notifyIcon;
			} else
				delete notifyIcon;
		}
	}

	// Renga: the dynamic icon (NetworkStatusIconsRenga.rdef), drawn in
	// layers: the link, then a badge
	for (int i = 0; i < kNetworkIconCount; i++) {
		size_t size;
		const void* data = resources.LoadResource(B_VECTOR_ICON_TYPE,
			kNetworkIconFirst + i, &size);
		if (data == NULL)
			continue;
		BRect bounds = i + kNetworkIconFirst == kNetworkIconMenuLock
			? BRect(B_ORIGIN, be_control_look->ComposeIconSize(B_MINI_ICON))
			: Bounds();
		BBitmap* icon = new BBitmap(bounds, B_RGBA32);
		if (icon->InitCheck() == B_OK && BIconUtils::GetVectorIcon(
				(const uint8*)data, size, icon) == B_OK) {
			if (i + kNetworkIconFirst == kNetworkIconMenuLock)
				fMenuLock = icon;
			else
				fIcons[i] = icon;
		} else
			delete icon;
	}
}


void
NetworkStatusView::_Quit()
{
	if (fInDeskbar) {
		BDeskbar deskbar;
		deskbar.RemoveItem(kDeskbarItemName);
	} else
		be_app->PostMessage(B_QUIT_REQUESTED);
}


NetworkStatusView*
NetworkStatusView::Instantiate(BMessage* archive)
{
	if (!validate_instantiation(archive, "NetworkStatusView"))
		return NULL;

	return new NetworkStatusView(archive);
}


status_t
NetworkStatusView::Archive(BMessage* archive, bool deep) const
{
	status_t status = BView::Archive(archive, deep);
	if (status == B_OK)
		status = archive->AddString("add_on", kSignature);
	if (status == B_OK)
		status = archive->AddString("class", "NetworkStatusView");

	return status;
}


void
NetworkStatusView::AttachedToWindow()
{
	BView::AttachedToWindow();

	SetViewColor(B_TRANSPARENT_COLOR);

	start_watching_network(
		B_WATCH_NETWORK_INTERFACE_CHANGES | B_WATCH_NETWORK_LINK_CHANGES, this);

	// Renga: follow the signal, the routes and the internet check; and the
	// Network preferences' switch for the check
	BMessage tick(kMsgTick);
	fTicker = new BMessageRunner(BMessenger(this), &tick, kTickInterval);
	BPath path;
	if (connectivity_settings_path(path)) {
		BPrivate::BPathMonitor::StartWatching(path.Path(),
			B_WATCH_STAT | B_WATCH_FILES_ONLY, BMessenger(this));
	}

	_Update();
}


void
NetworkStatusView::DetachedFromWindow()
{
	stop_watching_network(this);
	BPrivate::BPathMonitor::StopWatching(BMessenger(this));
	delete fTicker;
	fTicker = NULL;
	delete fAnimation;
	fAnimation = NULL;
}


void
NetworkStatusView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case B_NETWORK_MONITOR:
			_Update();
			break;

		case kMsgTick:
			_UpdateIcon();
			break;

		case kMsgAnimate:
			fFrame++;
			_UpdateIcon();
			break;

		case kMsgConnectivityResult:
		{
			fCheckThread = -1;
			if (message->GetInt32("generation", -1) == fCheckGeneration) {
				fConnectivityResult = message->GetInt32("result",
					CONNECTIVITY_ONLINE);
				_UpdateIcon();
			}
			break;
		}

		case B_PATH_MONITOR:
		{
			// the Network preferences changed the internet check
			bool wasEnabled = fConnectivity.enabled;
			BString oldURL = fConnectivity.url;
			_LoadConnectivitySettings();
			if (fConnectivity.enabled != wasEnabled
				|| fConnectivity.url != oldURL) {
				fCheckGeneration++;
				fConnectivityResult = -1;
				fLastCheck = 0;
				_UpdateIcon();
			}
			break;
		}

		case kMsgToggleWireless:
		{
			const char* name;
			if (message->FindString("interface", &name) == B_OK)
				_ToggleWireless(name);
			break;
		}

		case kMsgShowConfiguration:
			_ShowConfiguration(message);
			break;

		case kMsgOpenNetworkPreferences:
			_OpenNetworksPreferences();
			break;

		case kMsgJoinNetwork:
		{
			const char* deviceName;
			const char* name;
			BNetworkAddress address;
			if (message->FindString("device", &deviceName) == B_OK
				&& message->FindString("name", &name) == B_OK) {
				BNetworkDevice device(deviceName);
				// Renga: a network shown once for all its access points is
				// joined by name; wpa_supplicant chooses the access point
				// (WirelessNetworkGroups.h). With "Show each access point"
				// the entry carries that access point's address.
				status_t status = message->FindFlat("address", &address)
						== B_OK
					? device.JoinNetwork(address) : device.JoinNetwork(name);
				if (status != B_OK) {
					BString text
						= B_TRANSLATE("Could not join wireless network:\n");
					text << strerror(status);
					BAlert* alert = new BAlert(name, text.String(),
						B_TRANSLATE("OK"), NULL, NULL, B_WIDTH_AS_USUAL,
						B_STOP_ALERT);
					alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
					alert->Go(NULL);
				}
			}
			break;
		}

		case B_ABOUT_REQUESTED:
			_AboutRequested();
			break;

		case B_QUIT_REQUESTED:
			_Quit();
			break;

		default:
			BView::MessageReceived(message);
			break;
	}
}


void
NetworkStatusView::FrameResized(float width, float height)
{
	_UpdateBitmaps();
	Invalidate();
}


void
NetworkStatusView::Draw(BRect updateRect)
{
	// Renga: the link, then its badge
	if (fIcon.base >= kNetworkIconFirst
		&& fIcons[fIcon.base - kNetworkIconFirst] != NULL) {
		SetDrawingMode(B_OP_ALPHA);
		DrawBitmap(fIcons[fIcon.base - kNetworkIconFirst]);
		if (fIcon.badge >= kNetworkIconFirst
			&& fIcons[fIcon.badge - kNetworkIconFirst] != NULL)
			DrawBitmap(fIcons[fIcon.badge - kNetworkIconFirst]);
		SetDrawingMode(B_OP_COPY);
		return;
	}

	int32 status = kStatusUnknown;
	for (std::map<BString, int32>::const_iterator it
		= fInterfaceStatuses.begin(); it != fInterfaceStatuses.end(); ++it) {
		if (it->second > status)
			status = it->second;
	}

	if (fTrayIcons[status] == NULL)
		return;

	SetDrawingMode(B_OP_ALPHA);
	DrawBitmap(fTrayIcons[status]);
	SetDrawingMode(B_OP_COPY);
}


void
NetworkStatusView::_ShowConfiguration(BMessage* message)
{
	const char* name;
	if (message->FindString("interface", &name) != B_OK)
		return;

	BNetworkInterface networkInterface(name);
	if (!networkInterface.Exists())
		return;

	BString text(B_TRANSLATE("%ifaceName information:\n"));
	text.ReplaceFirst("%ifaceName", name);

	size_t boldLength = text.Length();

	int32 numAddrs = networkInterface.CountAddresses();
	for (int32 i = 0; i < numAddrs; i++) {
		BNetworkInterfaceAddress address;
		networkInterface.GetAddressAt(i, address);
		switch (address.Address().Family()) {
			case AF_INET:
				text << "\n" << B_TRANSLATE("IPv4 address:") << " "
					<< address.Address().ToString()
					<< "\n" << B_TRANSLATE("Broadcast:") << " "
					<< address.Broadcast().ToString()
					<< "\n" << B_TRANSLATE("Netmask:") << " "
					<< address.Mask().ToString()
					<< "\n";
				break;
			case AF_INET6:
				text << "\n" << B_TRANSLATE("IPv6 address:") << " "
					<< address.Address().ToString()
					<< "/" << address.Mask().PrefixLength()
					<< "\n";
				break;
			default:
				break;
		}
	}

	BAlert* alert = new BAlert(name, text.String(), B_TRANSLATE("OK"));
	alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
	BTextView* view = alert->TextView();
	BFont font;

	view->SetStylable(true);
	view->GetFont(&font);
	font.SetFace(B_BOLD_FACE);
	view->SetFontAndColor(0, boldLength, &font);

	alert->Go(NULL);
}


void
NetworkStatusView::MouseDown(BPoint point)
{
	BPopUpMenu* menu = new BPopUpMenu(B_EMPTY_STRING, false, false);
	menu->SetAsyncAutoDestruct(true);
	menu->SetFont(be_plain_font);
	_BuildMenu(menu);
	menu->SetTargetForItems(this);

	ConvertToScreen(&point);
	menu->Go(point, true, true, true);
}


void
NetworkStatusView::_BuildMenu(BPopUpMenu* menu)
{
	BString wifiInterface;
	BNetworkDevice device;

	if (!fInterfaceStatuses.empty()) {
		for (std::map<BString, int32>::const_iterator it
				= fInterfaceStatuses.begin(); it != fInterfaceStatuses.end();
				++it) {
			const BString& name = it->first;

			// we only show network of the first wireless device we find
			if (wifiInterface.IsEmpty()) {
				device.SetTo(name);
				if (device.IsWireless())
					wifiInterface = name;
			}
		}
	}

	// Add wireless networks, if any, first so that we can sort the menu

	if (!wifiInterface.IsEmpty()) {
		// Renga: Wi-Fi on and off (as the Network preferences' Disable
		// button does: the interface goes down)
		bool wifiOn = (BNetworkInterface(wifiInterface).Flags() & IFF_UP) != 0;

		if (wifiOn) {
			std::set<BNetworkAddress> associated;
			BNetworkAddress address;
			uint32 cookie = 0;
			while (device.GetNextAssociatedNetwork(cookie, address) == B_OK)
				associated.insert(address);

			uint32 networksCount = 0;
			wireless_network* networks = NULL;
			device.GetNetworks(networks, networksCount);
			// Renga: one entry per network name and security, not per
			// access point, unless "Show each access point" is on (Network
			// preferences; WirelessNetworkGroups.h).
			bool eachAccessPoint = wireless_show_access_points();
			std::vector<wireless_network_group> entries;
			if (eachAccessPoint) {
				ungrouped_wireless_networks(networks, networksCount,
					associated, entries);
			} else {
				group_wireless_networks(networks, networksCount, associated,
					entries);
			}
			for (size_t i = 0; i < entries.size(); i++) {
				const wireless_network& network = entries[i].network;
				BMessage* message = new BMessage(kMsgJoinNetwork);
				message->AddString("device", wifiInterface);
				message->AddString("name", network.name);
				if (eachAccessPoint || network.name[0] == '\0')
					message->AddFlat("address", &network.address);

				BMenuItem* item = new WirelessNetworkMenuItem(network,
					message, fMenuLock);
				menu->AddItem(item);
				if (entries[i].associated)
					item->SetMarked(true);
			}
			delete[] networks;

			if (networksCount == 0) {
				BMenuItem* item = new BMenuItem(
					B_TRANSLATE("<no wireless networks found>"), NULL);
				item->SetEnabled(false);
				menu->AddItem(item);
			} else
				menu->SortItems(WirelessNetworkMenuItem::CompareSignalStrength);
		}

		BMessage* toggle = new BMessage(kMsgToggleWireless);
		toggle->AddString("interface", wifiInterface);
		menu->AddItem(new BMenuItem(wifiOn ? B_TRANSLATE("Turn Wi-Fi off")
			: B_TRANSLATE("Turn Wi-Fi on"), toggle));

		menu->AddSeparatorItem();
	}

	// add action menu items

	menu->AddItem(new BMenuItem(B_TRANSLATE(
		"Network preferences" B_UTF8_ELLIPSIS),
		new BMessage(kMsgOpenNetworkPreferences)));

	if (fInDeskbar) {
		menu->AddItem(new BMenuItem(B_TRANSLATE("Quit"),
			new BMessage(B_QUIT_REQUESTED)));
	}

	// Add wired interfaces to top of menu
	if (!fInterfaceStatuses.empty()) {
		int32 wiredCount = 0;
		for (std::map<BString, int32>::const_iterator it
				= fInterfaceStatuses.begin(); it != fInterfaceStatuses.end();
				++it) {
			const BString& name = it->first;

			BString label = name;
			label += ": ";
			label += kStatusDescriptions[
				_DetermineInterfaceStatus(name.String())];

			BMessage* info = new BMessage(kMsgShowConfiguration);
			info->AddString("interface", name.String());
			menu->AddItem(new BMenuItem(label.String(), info), wiredCount);
			wiredCount++;
		}

		// add separator item between wired and wireless networks
		// (or between wired networks and actions if no wireless found)
		if (wiredCount > 0)
			menu->AddItem(new BSeparatorItem(), wiredCount);
	}
}


void
NetworkStatusView::_AboutRequested()
{
	BAboutWindow* window = new BAboutWindow(
		B_TRANSLATE_SYSTEM_NAME("NetworkStatus"), kSignature);

	const char* authors[] = {
		"Axel Dörfler",
		"Hugo Santos",
		NULL
	};

	window->AddCopyright(2007, "Haiku, Inc.");
	window->AddAuthors(authors);

	window->Show();
}


int32
NetworkStatusView::_DetermineInterfaceStatus(
	const BNetworkInterface& interface)
{
	uint32 flags = interface.Flags();

	if ((flags & IFF_LINK) == 0)
		return kStatusNoLink;
	if ((flags & (IFF_UP | IFF_LINK | IFF_CONFIGURING)) == IFF_LINK)
		return kStatusLinkNoConfig;
	if ((flags & IFF_CONFIGURING) == IFF_CONFIGURING)
		return kStatusConnecting;
	if ((flags & (IFF_UP | IFF_LINK)) == (IFF_UP | IFF_LINK))
		return kStatusReady;

	return kStatusUnknown;
}


void
NetworkStatusView::_Update(bool force)
{
	BNetworkRoster& roster = BNetworkRoster::Default();
	BNetworkInterface interface;
	uint32 cookie = 0;
	std::set<BString> currentInterfaces;

	while (roster.GetNextInterface(&cookie, interface) == B_OK) {
		if ((interface.Flags() & IFF_LOOPBACK) == 0) {
			currentInterfaces.insert((BString)interface.Name());
			int32 oldStatus = kStatusUnknown;
			if (fInterfaceStatuses.find(interface.Name())
				!= fInterfaceStatuses.end()) {
				oldStatus = fInterfaceStatuses[interface.Name()];
			}
			int32 status = _DetermineInterfaceStatus(interface);
			if (oldStatus != status) {
				BNotification notification(B_INFORMATION_NOTIFICATION);
				notification.SetGroup(B_TRANSLATE("Network Status"));
				notification.SetTitle(interface.Name());
				notification.SetMessageID(interface.Name());
				notification.SetIcon(fNotifyIcons[status]);
				if (status == kStatusConnecting
					|| (status == kStatusReady
						&& oldStatus == kStatusConnecting)
					|| (status == kStatusNoLink
						&& oldStatus == kStatusReady)
					|| (status == kStatusNoLink
						&& oldStatus == kStatusConnecting)) {
					// A significant state change, raise notification.
					notification.SetContent(kStatusDescriptions[status]);
					notification.Send();
				}
				Invalidate();
			}
			fInterfaceStatuses[interface.Name()] = status;
		}
	}

	// Check every element in fInterfaceStatuses against our current interface
	// list. If it's not there, then the interface is not present anymore and
	// should be removed from fInterfaceStatuses.
	// (Renga: erase() returns the next element; the old code stepped on
	// from the erased one)
	std::map<BString, int32>::iterator it = fInterfaceStatuses.begin();
	while (it != fInterfaceStatuses.end()) {
		if (currentInterfaces.find(it->first) == currentInterfaces.end())
			fInterfaceStatuses.erase(it++);
		else
			++it;
	}

	_UpdateIcon();
}


void
NetworkStatusView::_OpenNetworksPreferences()
{
	status_t status = be_roster->Launch("application/x-vnd.Haiku-Network");
	if (status != B_OK && status != B_ALREADY_RUNNING) {
		BString errorMessage(B_TRANSLATE("Launching the network preflet "
			"failed.\n\nError: "));
		errorMessage << strerror(status);
		BAlert* alert = new BAlert("launch error", errorMessage.String(),
			B_TRANSLATE("OK"));
		alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);

		// asynchronous alert in order to not block replicant host application
		alert->Go(NULL);
	}
}


//	#pragma mark - Renga: the dynamic icon


/*!	Chooses the link the icon shows: the one carrying the default route
	(the first such, as the stack uses it), else the most connected one,
	Wi-Fi before wired on a tie. A tunnel (tun/N, tap/N) carrying the
	default route adds the lock to the link beneath it. Then sets the icon,
	the tooltip, the animation and, when due, starts the internet check.
*/
void
NetworkStatusView::_UpdateIcon()
{
	BNetworkRoster& roster = BNetworkRoster::Default();
	BNetworkInterface interface;
	uint32 cookie = 0;
	BString primary, routed, tunnel;
	int32 primaryStatus = -1;
	bool primaryWireless = false;
	while (roster.GetNextInterface(&cookie, interface) == B_OK) {
		uint32 flags = interface.Flags();
		if ((flags & IFF_LOOPBACK) != 0)
			continue;
		const char* name = interface.Name();
		BNetworkRoute route;
		bool hasDefault = (flags & IFF_UP) != 0
			&& (BNetworkRoute::GetDefaultRoute(AF_INET, name, route) == B_OK
				|| BNetworkRoute::GetDefaultRoute(AF_INET6, name, route)
					== B_OK);
		if (network_is_tunnel(name)) {
			if (hasDefault && tunnel.IsEmpty())
				tunnel = name;
			continue;
		}
		int32 status = _DetermineInterfaceStatus(interface);
		bool wireless = BNetworkDevice(name).IsWireless();
		if (hasDefault && status == kStatusReady && routed.IsEmpty())
			routed = name;
		if (status > primaryStatus
			|| (status == primaryStatus && wireless && !primaryWireless)) {
			primaryStatus = status;
			primary = name;
			primaryWireless = wireless;
		}
	}
	if (!routed.IsEmpty()) {
		primary = routed;
		primaryWireless = BNetworkDevice(primary).IsWireless();
	}

	network_link_kind kind = NETWORK_LINK_WIRED;
	network_link_state state = NETWORK_STATE_DISCONNECTED;
	int32 signal = -1;
	BString toolTip;
	if (primary.IsEmpty())
		toolTip = B_TRANSLATE("No network device");
	else {
		BNetworkInterface link(primary);
		uint32 flags = link.Flags();
		kind = network_link_kind_for(primary, primaryWireless);
		switch (_DetermineInterfaceStatus(link)) {
			case kStatusConnecting:
				state = NETWORK_STATE_CONNECTING;
				break;
			case kStatusReady:
				state = NETWORK_STATE_CONNECTED;
				break;
			default:
				state = primaryWireless && (flags & IFF_UP) == 0
					? NETWORK_STATE_RADIO_OFF : NETWORK_STATE_DISCONNECTED;
				break;
		}

		// the tooltip: name, address, signal, band, access point
		wireless_network network;
		bool associated = false;
		if (primaryWireless && state != NETWORK_STATE_RADIO_OFF) {
			BNetworkDevice device(primary);
			uint32 networkCookie = 0;
			associated = device.GetNextAssociatedNetwork(networkCookie,
				network) == B_OK;
			if (associated)
				signal = std::min((int32)network.signal_strength, (int32)100);
		}
		if (primaryWireless) {
			if (associated && network.name[0] != '\0') {
				toolTip = B_TRANSLATE("Wi-Fi: %name%");
				toolTip.ReplaceFirst("%name%", network.name);
			} else
				toolTip = B_TRANSLATE("Wi-Fi");
		} else if (kind == NETWORK_LINK_TETHER)
			toolTip = B_TRANSLATE("USB tethering");
		else
			toolTip = B_TRANSLATE("Wired");
		toolTip << " (" << primary << ")";

		for (int32 i = 0; i < link.CountAddresses(); i++) {
			BNetworkInterfaceAddress address;
			if (link.GetAddressAt(i, address) != B_OK
				|| address.Address().IsEmpty()
				|| address.Address().IsLinkLocal())
				continue;
			toolTip << "\n" << B_TRANSLATE("IP address:") << " "
				<< address.Address().ToString(false);
		}
		if (associated) {
			toolTip << "\n" << B_TRANSLATE("Signal:") << " " << signal << "%";
			const char* band = network_band_name(wireless_frequency(primary));
			if (band != NULL)
				toolTip << ", " << band;
			toolTip << "\n" << B_TRANSLATE("Access point:") << " "
				<< network.address.ToString();
		}
	}

	// the internet check: again whenever the link changes, then every few
	// minutes
	if (primary != fPrimaryInterface || state != fLinkState) {
		fCheckGeneration++;
		fConnectivityResult = -1;
		fLastCheck = 0;
	}
	fPrimaryInterface = primary;
	fLinkState = state;
	if (state == NETWORK_STATE_CONNECTED && fConnectivity.enabled) {
		if (fLastCheck == 0 || system_time() - fLastCheck
				> (bigtime_t)fConnectivity.interval * 1000000)
			_StartConnectivityCheck();
		if (fConnectivityResult > CONNECTIVITY_ONLINE)
			state = NETWORK_STATE_NO_INTERNET;
	}
	fState = state;

	switch (state) {
		case NETWORK_STATE_DISCONNECTED:
			if (!primary.IsEmpty())
				toolTip << "\n" << B_TRANSLATE("Disconnected");
			break;
		case NETWORK_STATE_CONNECTING:
			toolTip << "\n" << B_TRANSLATE("Connecting" B_UTF8_ELLIPSIS);
			break;
		case NETWORK_STATE_RADIO_OFF:
			toolTip << "\n" << B_TRANSLATE("Wi-Fi is off");
			break;
		case NETWORK_STATE_NO_INTERNET:
			toolTip << "\n" << (fConnectivityResult == CONNECTIVITY_CAPTIVE
				? B_TRANSLATE("No internet access: the network may want you "
					"to sign in")
				: B_TRANSLATE("No internet access"));
			break;
		case NETWORK_STATE_CONNECTED:
			break;
	}
	bool tunneled = !tunnel.IsEmpty() && (state == NETWORK_STATE_CONNECTED
		|| state == NETWORK_STATE_NO_INTERNET);
	if (tunneled)
		toolTip << "\n" << B_TRANSLATE("VPN:") << " " << tunnel;

	// connecting: animate
	if (state == NETWORK_STATE_CONNECTING) {
		if (fAnimation == NULL && Window() != NULL) {
			BMessage animate(kMsgAnimate);
			fAnimation = new BMessageRunner(BMessenger(this), &animate,
				kAnimationInterval);
		}
	} else if (fAnimation != NULL || fFrame != 0) {
		delete fAnimation;
		fAnimation = NULL;
		fFrame = 0;
	}

	network_icon icon = network_icon_for(kind, state, signal, fFrame,
		tunneled);
	if (icon != fIcon) {
		fIcon = icon;
		Invalidate();
	}
	if (toolTip != fToolTip) {
		fToolTip = toolTip;
		SetToolTip(fToolTip.String());
	}
}


void
NetworkStatusView::_LoadConnectivitySettings()
{
	load_connectivity_settings(fConnectivity);
}


struct connectivity_check {
	BMessenger	target;
	BString		url;
	int32		generation;
};


void
NetworkStatusView::_StartConnectivityCheck()
{
	if (fCheckThread >= 0 || Window() == NULL)
		return;

	fLastCheck = system_time();
	connectivity_check* check = new connectivity_check;
	check->target = BMessenger(this);
	check->url = fConnectivity.url;
	check->generation = fCheckGeneration;
	fCheckThread = spawn_thread(&_ConnectivityThread, "internet check",
		B_LOW_PRIORITY, check);
	if (fCheckThread < 0 || resume_thread(fCheckThread) != B_OK) {
		delete check;
		fCheckThread = -1;
	}
}


/*static*/ status_t
NetworkStatusView::_ConnectivityThread(void* data)
{
	connectivity_check* check = (connectivity_check*)data;
	BMessage result(kMsgConnectivityResult);
	result.AddInt32("result", check_connectivity(check->url.String()));
	result.AddInt32("generation", check->generation);
	check->target.SendMessage(&result);
	delete check;
	return B_OK;
}


void
NetworkStatusView::_ToggleWireless(const char* name)
{
	BNetworkInterface interface(name);
	uint32 flags = interface.Flags();
	interface.SetFlags((flags & IFF_UP) != 0 ? flags & ~IFF_UP
		: flags | IFF_UP);
	_Update();
}


void
NetworkStatusView::GetState(BMessage& state)
{
	_Update();
	if (fState == NETWORK_STATE_CONNECTED && fConnectivity.enabled) {
		// no window: run the check here
		fConnectivityResult = check_connectivity(fConnectivity.url.String());
		fLastCheck = system_time();
		_UpdateIcon();
	}

	static const char* kStates[] = { "disconnected", "connecting",
		"connected", "no_internet", "radio_off" };
	state.AddInt32("base", fIcon.base);
	state.AddInt32("badge", fIcon.badge);
	state.AddString("state", kStates[fState]);
	state.AddString("interface", fPrimaryInterface);
	state.AddString("tooltip", fToolTip);
	state.AddBool("check", fConnectivity.enabled);
	state.AddString("url", fConnectivity.url);

	BPopUpMenu menu("state");
	_BuildMenu(&menu);
	for (int32 i = 0; i < menu.CountItems(); i++) {
		BMenuItem* item = menu.ItemAt(i);
		if (dynamic_cast<BSeparatorItem*>(item) != NULL)
			state.AddString("menu", "-");
		else
			state.AddString("menu", item->Label());
	}
}


//	#pragma mark -


extern "C" _EXPORT BView *
instantiate_deskbar_item(float maxWidth, float maxHeight)
{
	return new NetworkStatusView(BRect(0, 0, maxHeight - 1, maxHeight - 1),
		B_FOLLOW_LEFT | B_FOLLOW_TOP, true);
}
