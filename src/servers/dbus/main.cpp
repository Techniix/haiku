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
#include <OS.h>


using namespace DBus;


static const char* kDefaultSocket = "/tmp/xdg-runtime/bus";


class BusApplication : public BApplication {
public:
	BusApplication(const char* socketPath)
		:
		BApplication(kSessionBusSignature),
		fSocketPath(socketPath),
		fThread(-1),
		fNotifications(NULL)
	{
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
		fBus.AddService(new SecretService(&fBus));
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
