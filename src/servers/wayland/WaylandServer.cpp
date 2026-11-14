#include "WaylandServer.h"
#include "Wayland.h"
#include "HaikuShm.h"
#include "HaikuCompositor.h"
#include "HaikuSubcompositor.h"
#include "HaikuViewporter.h"
#include "HaikuXdgShell.h"
#include "HaikuXdgSurface.h"
#include "HaikuXdgToplevel.h"
#include "HaikuOutput.h"
#include "HaikuDataDeviceManager.h"
#include "HaikuSeat.h"
#include "HaikuTextInput.h"
#include "HaikuServerDecoration.h"
#include "WaylandEnv.h"

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <xdg-shell-protocol.h>
#include <stdio.h>
#include <new>

#include <Application.h>
#include <String.h>
#include <File.h>
#include <OS.h>
#include <AppFileInfo.h>
#include <kernel/image.h>

#include "AppKitPtrs.h"


static void Assert(bool cond) {if (!cond) abort();}


static struct wl_display *sDisplay;

ServerHandler gServerHandler;
BMessenger gServerMessenger;
BLooper *gWaylandLooper;


ServerHandler::ServerHandler(): BHandler("server")
{}

void ServerHandler::MessageReceived(BMessage *msg)
{
	switch (msg->what) {
	case dispatchMsg:
		DispatchEvents();
		return;
	default:
		break;
	}
	BHandler::MessageReceived(msg);
}


class Application: public BApplication {
public:
	Application(const char *signature);
	virtual ~Application() = default;

	void MessageReceived(BMessage *msg) override;
	bool QuitRequested() override;
};

Application::Application(const char *signature): BApplication(signature)
{
}

void Application::MessageReceived(BMessage *msg)
{
	switch (msg->what) {
	case B_KEY_MAP_LOADED: {
		// Renga: every client gets the new keymap.
		if (sDisplay == NULL) return;
		WaylandEnv env(this);
		struct wl_list *clients = wl_display_get_client_list(sDisplay);
		struct wl_client *client;
		wl_client_for_each(client, clients) {
			HaikuSeatGlobal *seat = HaikuGetSeat(client);
			if (seat != NULL)
				seat->UpdateKeymap();
		}
		return;
	}
	}
	return BApplication::MessageReceived(msg);
}

bool Application::QuitRequested()
{
	return true;
}


//#pragma mark - Renga: the compositor process
//
// Renga runs HaiWay as one compositor process with a Wayland socket instead
// of X547's in-process server: libwayland (src/libs/wayland, built with
// WL_RENGA_SOCKETS) listens on $XDG_RUNTIME_DIR/wayland-0 and every client,
// native or Linux (under Renguin), connects there. HaiWay's objects keep
// their threading model: all Wayland state belongs to the "wayland" looper
// (gWaylandLooper), and window threads enter it through WaylandEnv. A poll
// thread watches libwayland's event loop and asks the looper to dispatch;
// events go out at the end of each dispatch and whenever a window thread
// leaves WaylandEnv (WaylandFlushClients()).

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <Roster.h>


static const char* kCompositorSignature
	= "application/x-vnd.Renga-WaylandCompositor";
static const char* kDefaultRuntimeDir = "/tmp/xdg-runtime";

static sem_id sDispatchDone = -1;


/*!	The looper all Wayland state belongs to. Handlers in it (the server
	handler, the clipboard watchers, the text input) send events outside a
	dispatch too; whatever any message produced goes out when it is done.
*/
class WaylandLooper: public BLooper {
public:
	WaylandLooper()
		:
		BLooper("wayland", B_DISPLAY_PRIORITY)
	{
	}

	void DispatchMessage(BMessage* message, BHandler* handler) override
	{
		BLooper::DispatchMessage(message, handler);
		WaylandFlushClients();
	}
};


void
WaylandFlushClients()
{
	if (sDisplay != NULL)
		wl_display_flush_clients(sDisplay);
}


void
ServerHandler::DispatchEvents()
{
	struct wl_event_loop* loop = wl_display_get_event_loop(sDisplay);
	wl_event_loop_dispatch(loop, 0);
	wl_display_flush_clients(sDisplay);
	release_sem(sDispatchDone);
}


static status_t
poll_thread(void*)
{
	struct wl_event_loop* loop = wl_display_get_event_loop(sDisplay);
	struct pollfd pollFD = { wl_event_loop_get_fd(loop), POLLIN, 0 };
	while (true) {
		if (poll(&pollFD, 1, -1) < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "wayland: poll failed: %s\n", strerror(errno));
			return B_ERROR;
		}
		BMessage message(ServerHandler::dispatchMsg);
		if (gServerMessenger.SendMessage(&message) != B_OK)
			return B_OK;
		// One dispatch at a time: the looper reads everything that is
		// ready, then the next poll() waits for more.
		while (acquire_sem(sDispatchDone) == B_INTERRUPTED)
			;
	}
}


static void
create_globals(struct wl_display* display)
{
	HaikuSeatGlobal* seat {};
	Assert(HaikuShmGlobal::Create(display) != NULL);
	Assert(HaikuCompositorGlobal::Create(display) != NULL);
	Assert(HaikuSubcompositorGlobal::Create(display) != NULL);
	Assert(HaikuViewporterGlobal::Create(display) != NULL);
	Assert(HaikuOutputGlobal::Create(display) != NULL);
	Assert(HaikuDataDeviceManagerGlobal::Create(display) != NULL);
	Assert((seat = HaikuSeatGlobal::Create(display)) != NULL);
	Assert(HaikuTextInputGlobal::Create(display, seat) != NULL);
	Assert(HaikuXdgShell::Create(display) != NULL);
	Assert(HaikuServerDecorationManagerGlobal::Create(display) != NULL);
}


int
main(int argc, char** argv)
{
	// The socket's directory, as XDG_RUNTIME_DIR names it for clients
	// (SetupEnvironment sets it); private to the user.
	const char* runtimeDir = getenv("XDG_RUNTIME_DIR");
	if (runtimeDir == NULL || runtimeDir[0] == '\0') {
		runtimeDir = kDefaultRuntimeDir;
		setenv("XDG_RUNTIME_DIR", runtimeDir, 1);
	}
	if (mkdir(runtimeDir, 0700) != 0 && errno != EEXIST) {
		fprintf(stderr, "wayland: cannot create %s: %s\n", runtimeDir,
			strerror(errno));
		return 1;
	}
	chmod(runtimeDir, 0700);

	// A client that disappears must not end the compositor.
	signal(SIGPIPE, SIG_IGN);

	Application* application = new Application(kCompositorSignature);

	gWaylandLooper = new WaylandLooper();
	gWaylandLooper->AddHandler(&gServerHandler);
	gServerMessenger.SetTo(&gServerHandler);

	sDisplay = wl_display_create();
	if (sDisplay == NULL) {
		fprintf(stderr, "wayland: cannot create the display\n");
		return 1;
	}
	const char* socketName = argc > 1 ? argv[1] : "wayland-0";
	if (wl_display_add_socket(sDisplay, socketName) != 0) {
		fprintf(stderr, "wayland: cannot listen on %s/%s: %s\n", runtimeDir,
			socketName, strerror(errno));
		return 1;
	}
	create_globals(sDisplay);
	fprintf(stderr, "wayland: listening on %s/%s\n", runtimeDir,
		socketName);

	gWaylandLooper->Run();

	sDispatchDone = create_sem(0, "wayland dispatch");
	thread_id poller = spawn_thread(poll_thread, "wayland poll",
		B_DISPLAY_PRIORITY, NULL);
	resume_thread(poller);

	application->Run();

	// Quit: no more clients; the socket and its lock file go away.
	gWaylandLooper->Lock();
	wl_display_destroy_clients(sDisplay);
	wl_display_destroy(sDisplay);
	sDisplay = NULL;
	gWaylandLooper->Quit();
	delete application;
	return 0;
}
