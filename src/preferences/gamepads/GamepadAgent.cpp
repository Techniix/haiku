/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * gamepad_agent (roadmap F-42): the small background server behind two
 * settings of the Gamepads preferences, the guide-button overlay and the
 * low-battery notification. The user's launch script starts it
 * (data/launch/user_gamepads); it is a background application
 * (B_BACKGROUND_APP), so it never shows in Deskbar, and exclusive
 * (B_EXCLUSIVE_LAUNCH), so it runs once per session. It used to be the
 * Gamepads preferences started with --agent, which made a normal
 * application that stayed in Deskbar for good with no windows to show
 * (found on the maintainer's PC, 2026-11-13). Gamepads --agent still
 * starts this server, for old launch scripts.
 *
 * The agent opens the pads only to watch the guide button and battery; it
 * is idle (blocked in poll()) when nothing happens, and does not watch at
 * all when both features are off at start or when pads come and go. It
 * reads the settings file again whenever it is about to act (a held guide
 * button, the battery check), so changes apply at once without any
 * message between the processes.
 */


#include "GamepadsCommon.h"


static const char* kAgentSignature = "application/x-vnd.Renga-GamepadAgent";


// #pragma mark - agent


class OverlayWindow : public BWindow {
public:
	OverlayWindow()
		:
		BWindow(BRect(0, 0, 320, 70), "Gamepad", B_BORDERED_WINDOW_LOOK,
			B_FLOATING_ALL_WINDOW_FEEL, B_NOT_MOVABLE | B_NOT_RESIZABLE
				| B_AVOID_FOCUS | B_NOT_CLOSABLE | B_NOT_ZOOMABLE,
			B_ALL_WORKSPACES),
		fHideRunner(NULL)
	{
		fName = new BStringView("name", "");
		BFont font(be_bold_font);
		font.SetSize(font.Size() * 1.3f);
		fName->SetFont(&font);
		fDetails = new BStringView("details", "");
		BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_SMALL_SPACING)
			.SetInsets(B_USE_DEFAULT_SPACING)
			.Add(fName)
			.Add(fDetails);
	}

	void ShowFor(const char* name, int32 player, int32 battery,
		bool charging, bool wired)
	{
		fName->SetText(name);
		BString details;
		if (player > 0)
			details << "Player " << player;
		else
			details << "No player number";
		if (wired)
			details << "  ·  Wired";
		else if (battery >= 0) {
			details << "  ·  Battery " << battery << " %";
			if (charging)
				details << " (charging)";
		}
		fDetails->SetText(details);

		BRect frame = BScreen(this).Frame();
		ResizeToPreferred();
		MoveTo(frame.left + (frame.Width() - Bounds().Width()) / 2,
			frame.top + frame.Height() / 8);
		if (IsHidden())
			Show();
		delete fHideRunner;
		BMessage hide(MSG_HIDE_OVERLAY);
		fHideRunner = new BMessageRunner(BMessenger(this), &hide, 3000000, 1);
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == MSG_HIDE_OVERLAY) {
			if (!IsHidden())
				Hide();
			return;
		}
		BWindow::MessageReceived(message);
	}

private:
	BStringView*	fName;
	BStringView*	fDetails;
	BMessageRunner*	fHideRunner;
};


struct WatchedPad {
	BString		path;
	int			fd;
	BString		name;
	bigtime_t	guideDown;
	bool		overlayShown;
	bool		lowBatteryNotified;
};


class AgentApp : public BApplication {
public:
	AgentApp()
		:
		BApplication(kAgentSignature),
		fOverlay(NULL),
		fThread(-1),
		fQuit(false),
		fPads(8)
	{
		fWakeUp[0] = fWakeUp[1] = -1;
	}

	virtual void ReadyToRun()
	{
		fSettings.Load();
		fOverlay = new OverlayWindow;
		fOverlay->Hide();
		fOverlay->Show();
		BGamepad::StartWatching(BMessenger(this));
		_Restart();

		// low battery checks once a minute (cheap: one ioctl per pad)
		BMessage check('bchk');
		new BMessageRunner(BMessenger(this), &check, 60000000);
	}

	virtual bool QuitRequested()
	{
		_Stop();
		return true;
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case B_GAMEPAD_ADDED:
			case B_GAMEPAD_REMOVED:
				fSettings.Load();
				_Restart();
				break;
			case 'bchk':
			{
				// a feature switched on while nothing was watched
				fSettings.Load();
				bool idle;
				{
					BAutolock locker(fLock);
					idle = (fSettings.overlay && fThread < 0)
						|| (fSettings.lowBattery && fPads.IsEmpty());
				}
				if (idle && BGamepad::CountGamepads() > 0)
					_Restart();
				_CheckBatteries();
				break;
			}
			case MSG_SHOW_OVERLAY:
			{
				fSettings.Load();
				if (!fSettings.overlay)
					break;
				const char* path;
				if (message->FindString("path", &path) != B_OK)
					break;
				BGamepad pad;
				if (pad.Open(path) != B_OK)
					break;
				int32 level = -1;
				bool charging = false, wired = false;
				pad.GetBattery(&level, &charging, &wired);
				if (fOverlay->LockLooper()) {
					fOverlay->ShowFor(pad.Name(), pad.Player(), level,
						charging, wired);
					fOverlay->UnlockLooper();
				}
				break;
			}
			default:
				BApplication::MessageReceived(message);
		}
	}

private:
	void _Stop()
	{
		if (fThread >= 0) {
			fQuit = true;
			char byte = 0;
			write(fWakeUp[1], &byte, 1);
			status_t result;
			wait_for_thread(fThread, &result);
			fThread = -1;
		}
		if (fWakeUp[0] >= 0) {
			close(fWakeUp[0]);
			close(fWakeUp[1]);
			fWakeUp[0] = fWakeUp[1] = -1;
		}
		BAutolock locker(fLock);
		for (int32 i = 0; i < fPads.CountItems(); i++)
			close(fPads.ItemAt(i)->fd);
		fPads.MakeEmpty();
	}

	void _Restart()
	{
		_Stop();
		if (!fSettings.overlay && !fSettings.lowBattery)
			return;

		BAutolock locker(fLock);
		int32 count = BGamepad::CountGamepads();
		for (int32 i = 0; i < count; i++) {
			BString path;
			if (BGamepad::GetGamepadPath(i, &path) != B_OK)
				continue;
			int fd = open(path.String(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
			if (fd < 0)
				continue;
			WatchedPad* pad = new WatchedPad;
			pad->path = path;
			pad->fd = fd;
			char name[128] = "";
			ioctl(fd, EVIOCGNAME(sizeof(name)), name);
			pad->name = name;
			pad->guideDown = 0;
			pad->overlayShown = false;
			pad->lowBatteryNotified = false;
			fPads.AddItem(pad);
		}
		if (fPads.IsEmpty() || !fSettings.overlay)
			return;

		if (pipe(fWakeUp) != 0)
			return;
		fQuit = false;
		fThread = spawn_thread(_Watch, "gamepad guide watcher",
			B_DISPLAY_PRIORITY, this);
		if (fThread >= 0)
			resume_thread(fThread);
	}

	static status_t _Watch(void* data)
	{
		AgentApp* app = (AgentApp*)data;
		while (!app->fQuit) {
			struct pollfd fds[17];
			int count = 0;
			{
				BAutolock locker(app->fLock);
				for (int32 i = 0; i < app->fPads.CountItems() && count < 16;
						i++) {
					fds[count].fd = app->fPads.ItemAt(i)->fd;
					fds[count].events = POLLIN;
					fds[count].revents = 0;
					count++;
				}
			}
			fds[count].fd = app->fWakeUp[0];
			fds[count].events = POLLIN;
			fds[count].revents = 0;

			// while the guide button is down, wake up for the hold time
			int timeout = -1;
			{
				BAutolock locker(app->fLock);
				for (int32 i = 0; i < app->fPads.CountItems(); i++) {
					WatchedPad* pad = app->fPads.ItemAt(i);
					if (pad->guideDown != 0 && !pad->overlayShown)
						timeout = kGuideHold / 1000 + 10;
				}
			}
			int ready = poll(fds, count + 1, timeout);
			if (app->fQuit)
				break;
			if (ready < 0 && errno != EINTR)
				break;

			BAutolock locker(app->fLock);
			for (int i = 0; i < count; i++) {
				WatchedPad* pad = app->fPads.ItemAt(i);
				if (pad == NULL)
					continue;
				struct input_event events[32];
				ssize_t bytes;
				while ((bytes = read(pad->fd, events, sizeof(events))) > 0) {
					int n = bytes / sizeof(struct input_event);
					for (int e = 0; e < n; e++) {
						if (events[e].type != EV_KEY
							|| events[e].code != BTN_MODE)
							continue;
						if (events[e].value != 0) {
							pad->guideDown = system_time();
							pad->overlayShown = false;
						} else
							pad->guideDown = 0;
					}
				}
				if (pad->guideDown != 0 && !pad->overlayShown
					&& system_time() - pad->guideDown >= kGuideHold) {
					pad->overlayShown = true;
					BMessage show(MSG_SHOW_OVERLAY);
					show.AddString("path", pad->path);
					be_app_messenger.SendMessage(&show);
				}
			}
		}
		return B_OK;
	}

	void _CheckBatteries()
	{
		fSettings.Load();
		if (!fSettings.lowBattery)
			return;
		BAutolock locker(fLock);
		for (int32 i = 0; i < fPads.CountItems(); i++) {
			WatchedPad* pad = fPads.ItemAt(i);
			struct renga_input_battery battery;
			if (ioctl(pad->fd, EVIOC_RENGA_GBATTERY, &battery) < 0
				|| battery.level < 0 || battery.wired)
				continue;
			if (battery.level >= kBatteryRecovered || battery.charging)
				pad->lowBatteryNotified = false;
			else if (battery.level < kLowBattery
				&& !pad->lowBatteryNotified) {
				pad->lowBatteryNotified = true;
				BNotification notification(B_IMPORTANT_NOTIFICATION);
				notification.SetGroup("Gamepads");
				notification.SetTitle("Gamepad battery low");
				BString content;
				content.SetToFormat("%s: %d %% left.", pad->name.String(),
					(int)battery.level);
				notification.SetContent(content);
				notification.SetMessageID(pad->path);
				notification.Send();
			}
		}
	}

	Settings					fSettings;
	OverlayWindow*				fOverlay;
	thread_id					fThread;
	volatile bool				fQuit;
	int							fWakeUp[2];
	BLocker						fLock;
	BObjectList<WatchedPad, true> fPads;
};


int
main()
{
	AgentApp app;
	app.Run();
	return 0;
}
