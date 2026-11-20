/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	session_bus: Renga's D-Bus session bus (roadmap F-43 phase D). Listens
	on $XDG_RUNTIME_DIR/bus (/tmp/xdg-runtime/bus), the address programs
	find in DBUS_SESSION_BUS_ADDRESS (SetupEnvironment; the linux command
	sets it for Linux programs). Started at login by
	data/launch/user_session_bus unless ~/config/settings/Renga/dbus says
	"disabled true". A background application (no Deskbar entry) because
	the notification clicks come back to it as launch arguments.

	session_bus --address prints the address; --socket <path> listens
	there instead (tests).
*/


#include "Services.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <Application.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <OS.h>
#include <ScrollView.h>
#include <StringView.h>
#include <Window.h>


using namespace DBus;


static const char* kDefaultSocket = "/tmp/xdg-runtime/bus";
static const uint32 kShowSecretAccess = 'mngs';
	// from Notifications preferences: the "Always allow" list
static const uint32 kRemoveProgram = 'rmpg';
static const uint32 kSelectionChanged = 'slch';


/*!	The programs allowed to use saved passwords without asking (the Secret
	Service's "Always allow" list), with a way to take them off it.
*/
class SecretAccessWindow : public BWindow {
public:
	SecretAccessWindow(SecretService* service)
		:
		BWindow(BRect(100, 100, 560, 380), "Saved passwords: allowed programs",
			B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS
				| B_ASYNCHRONOUS_CONTROLS | B_CLOSE_ON_ESCAPE),
		fService(service)
	{
		fList = new BListView("programs");
		fList->SetSelectionMessage(new BMessage(kSelectionChanged));
		fRemove = new BButton("remove", "Remove", new BMessage(kRemoveProgram));
		fRemove->SetEnabled(false);
		BStringView* note = new BStringView("note",
			"Linux and ported programs use these without asking. Others are "
			"asked about the first time.");
		BLayoutBuilder::Group<>(this, B_VERTICAL)
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(note)
			.Add(new BScrollView("scroll", fList, 0, false, true))
			.AddGroup(B_HORIZONTAL)
				.AddGlue()
				.Add(fRemove)
			.End();
		_Fill();
		CenterOnScreen();
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kSelectionChanged:
				fRemove->SetEnabled(fList->CurrentSelection() >= 0);
				break;
			case kRemoveProgram:
			{
				BStringItem* item = dynamic_cast<BStringItem*>(
					fList->ItemAt(fList->CurrentSelection()));
				if (item != NULL)
					fService->RemoveAllowedProgram(item->Text());
				_Fill();
				break;
			}
			default:
				BWindow::MessageReceived(message);
		}
	}

private:
	void _Fill()
	{
		for (int32 i = fList->CountItems() - 1; i >= 0; i--)
			delete fList->RemoveItem(i);
		BStringList programs;
		fService->GetAllowedPrograms(programs);
		for (int32 i = 0; i < programs.CountStrings(); i++)
			fList->AddItem(new BStringItem(programs.StringAt(i)));
		if (programs.IsEmpty())
			fList->AddItem(new BStringItem("(none)"));
		fRemove->SetEnabled(false);
	}

	SecretService*	fService;
	BListView*		fList;
	BButton*		fRemove;
};


class BusApplication : public BApplication {
public:
	BusApplication(const char* socketPath)
		:
		BApplication(kSessionBusSignature),
		fSocketPath(socketPath),
		fThread(-1),
		fNotifications(NULL),
		fSecrets(NULL)
	{
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == kShowSecretAccess && fSecrets != NULL) {
			(new SecretAccessWindow(fSecrets))->Show();
			return;
		}
		BApplication::MessageReceived(message);
	}

	virtual void ReadyToRun()
	{
		mkdir("/tmp/xdg-runtime", 0700);
		status_t status = fBus.Listen(fSocketPath);
		if (status != B_OK) {
			fprintf(stderr, "session_bus: cannot listen on %s: %s\n",
				fSocketPath, strerror(status));
			Quit();
			return;
		}
		fNotifications = new NotificationsService(&fBus);
		fBus.AddService(fNotifications);
		fBus.AddService(new UPowerService(&fBus));
		fBus.AddService(new PortalService(&fBus));
		fSecrets = new SecretService(&fBus);
		fBus.AddService(fSecrets);
		fThread = spawn_thread(_Run, "session bus", B_NORMAL_PRIORITY, this);
		resume_thread(fThread);
		debug_printf("session_bus: listening on %s\n", fSocketPath);
	}

	virtual void ArgvReceived(int32 argc, char** argv)
	{
		// a notification's click: --notification-clicked <id> <sender>
		for (int32 i = 1; i + 1 < argc; i++) {
			if (strcmp(argv[i], "--notification-clicked") == 0
				&& fNotifications != NULL)
				fNotifications->Clicked(strtoul(argv[i + 1], NULL, 10));
		}
	}

	virtual bool QuitRequested()
	{
		fBus.Quit();
		if (fThread >= 0) {
			status_t result;
			wait_for_thread(fThread, &result);
		}
		return true;
	}

private:
	static status_t _Run(void* data)
	{
		((BusApplication*)data)->fBus.Run();
		return B_OK;
	}

	const char*				fSocketPath;
	SessionBus				fBus;
	thread_id				fThread;
	NotificationsService*	fNotifications;
	SecretService*			fSecrets;
};


int
main(int argc, char** argv)
{
	const char* socketPath = kDefaultSocket;
	if (argc == 2 && strcmp(argv[1], "--address") == 0) {
		printf("unix:path=%s\n", kDefaultSocket);
		return 0;
	}
	if (argc == 3 && strcmp(argv[1], "--socket") == 0)
		socketPath = argv[2];

	BusApplication app(socketPath);
	app.Run();
	return 0;
}
