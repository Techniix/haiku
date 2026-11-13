/*
 * Copyright 2006-2009, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 *		Dario Casalinuovo
 */
#ifndef NETWORK_STATUS_VIEW_H
#define NETWORK_STATUS_VIEW_H


#include <Notification.h>
#include <ObjectList.h>
#include <View.h>

#include <map>

#include <NetworkConnectivity.h>
#include <NetworkStatusIcon.h>


class BMessageRunner;
class BNetworkInterface;
class BPopUpMenu;


enum {
	kStatusUnknown = 0,
	kStatusNoLink,
	kStatusLinkNoConfig,
	kStatusConnecting,
	kStatusReady,

	kStatusCount
};


class NetworkStatusView : public BView {
	public:
		NetworkStatusView(BRect frame, int32 resizingMode,
			bool inDeskbar = false);
		NetworkStatusView(BMessage* archive);
		virtual	~NetworkStatusView();

		static	NetworkStatusView* Instantiate(BMessage* archive);
		virtual	status_t Archive(BMessage* archive, bool deep = true) const;

		virtual	void	AttachedToWindow();
		virtual	void	DetachedFromWindow();

		virtual	void	MessageReceived(BMessage* message);
		virtual void	FrameResized(float width, float height);
		virtual	void	MouseDown(BPoint where);
		virtual	void	Draw(BRect updateRect);

				// Renga: what the icon, its tooltip and its menu show, for
				// "NetworkStatus --state" (the tests); checks the internet
				// connection first, waiting for the answer.
				void	GetState(BMessage& state);

	private:
		void			_AboutRequested();
		void			_Quit();
		void			_Init();
		void			_UpdateBitmaps();
		void			_ShowConfiguration(BMessage* message);
		int32			_DetermineInterfaceStatus(
							const BNetworkInterface& interface);
		void			_Update(bool force = false);
		void			_OpenNetworksPreferences();

		// Renga: the dynamic icon
		void			_BuildMenu(BPopUpMenu* menu);
		void			_UpdateIcon();
		void			_LoadConnectivitySettings();
		void			_StartConnectivityCheck();
		static status_t	_ConnectivityThread(void* data);
		void			_ToggleWireless(const char* name);

		std::map<BString, int32>
						fInterfaceStatuses;
		bool			fInDeskbar;
		BBitmap*		fTrayIcons[kStatusCount];
		BBitmap*		fNotifyIcons[kStatusCount];

		BBitmap*		fIcons[BPrivate::kNetworkIconCount];
		BBitmap*		fMenuLock;
		BPrivate::network_icon
						fIcon;
		BPrivate::network_link_state
						fState;
		BPrivate::network_link_state
						fLinkState;
							// fState before the internet check
		BString			fPrimaryInterface;
		BString			fToolTip;
		int32			fFrame;
		BMessageRunner*	fAnimation;
		BMessageRunner*	fTicker;

		BPrivate::connectivity_settings
						fConnectivity;
		int32			fConnectivityResult;
							// a connectivity_result, or -1: not known
		int32			fCheckGeneration;
		thread_id		fCheckThread;
		bigtime_t		fLastCheck;
};

#endif	// NETWORK_STATUS_VIEW_H
