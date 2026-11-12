/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Passing file descriptors from an application to app_server (roadmap
	F-58, the prerequisite of the Quasar driver's request R-1): the client
	sends a descriptor with BPrivate::app_server_send_fd() (SCM_RIGHTS on
	app_server's local socket next to the link) and names it in an
	app_server message by the token it got. AS_CHECK_CLIENT_FD makes
	app_server take the descriptor and report its size and first bytes.

	Checks:
	- socket: app_server listens on APP_SERVER_FD_SOCKET
	- send: a memfd goes over, a token comes back
	- content: app_server reads the memfd's size and bytes through its own
	  descriptor (the same open file)
	- once: a token is used up when taken
	- other_team: another team cannot take this team's descriptor, and this
	  team still can afterwards; the other team's own descriptor works
	- many: 64 descriptors in a row, each taken by its token
	- bad_descriptor: -1 is refused at once
	- closed_quietly: after the child team (which connected) is gone,
	  app_server's "fd transfer" thread is idle (it once spun on the
	  closed connection: recvmsg() returning 0 was not taken as its end)
*/


#include <Application.h>
#include <OS.h>
#include <String.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <AppServerLink.h>
#include <ServerFdTransfer.h>
#include <ServerProtocol.h>


#define SUITE "appserver_fd"

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[384];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);
	printf("CE-TEST " SUITE ".%s %s %s\n", name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


//!	app_server takes the descriptor of \a token: its size and first bytes.
static status_t
check(int32 token, int64* _size, char* data, int32 size, int32* _length)
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_CHECK_CLIENT_FD);
	link.Attach<int32>(token);
	link.Attach<int32>(size);
	status_t status;
	if (link.FlushWithReply(status) != B_OK)
		return B_ERROR;
	if (status != B_OK)
		return status;
	link.Read<int64>(_size);
	link.Read<int32>(_length);
	if (*_length > size)
		return B_BAD_DATA;
	return link.Read(data, *_length);
}


static int
memfd_with(const char* text, off_t size)
{
	int fd = memfd_create("appserver_fd_test", 0);
	if (fd < 0)
		return -1;
	ftruncate(fd, size);
	pwrite(fd, text, strlen(text), 0);
	return fd;
}


//!	The other team: tries the parent's token, then sends its own.
static int
run_child(int32 parentToken)
{
	BApplication app("application/x-vnd.Renga-appserver-fd-test-child");
	int64 size = 0;
	int32 length = 0;
	char data[64];
	status_t parent = check(parentToken, &size, data, sizeof(data), &length);

	int fd = memfd_with("child", 4096);
	int32 token = 0;
	status_t sent = BPrivate::app_server_send_fd(fd, &token);
	status_t own = sent == B_OK
		? check(token, &size, data, sizeof(data), &length) : sent;
	bool ownRight = own == B_OK && size == 4096 && length >= 5
		&& memcmp(data, "child", 5) == 0;
	printf("CE-INFO " SUITE ": child: parent's token %s, own %s\n",
		strerror(parent), strerror(own));
	fflush(stdout);
	return (parent == B_ENTRY_NOT_FOUND ? 0 : 1) | (ownRight ? 0 : 2);
}


int
main(int argc, char** argv)
{
	if (argc == 3 && strcmp(argv[1], "child") == 0)
		return run_child(atoi(argv[2]));

	BApplication app("application/x-vnd.Renga-appserver-fd-test");
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	struct stat st;
	bool listening = false;
	for (int i = 0; i < 50 && !listening; i++) {
		listening = stat(APP_SERVER_FD_SOCKET, &st) == 0
			&& S_ISSOCK(st.st_mode);
		if (!listening)
			snooze(200000);
	}
	report("socket", listening, "%s %s", APP_SERVER_FD_SOCKET,
		listening ? "is a socket" : "missing");

	static const char kText[] = "Renga: a descriptor for app_server";
	int fd = memfd_with(kText, 8192);
	int32 token = 0;
	status_t status = BPrivate::app_server_send_fd(fd, &token);
	report("send", fd >= 0 && status == B_OK && token > 0,
		"memfd %d: %s, token %" B_PRId32, fd, strerror(status), token);

	int64 size = 0;
	int32 length = 0;
	char data[256];
	status = check(token, &size, data, sizeof(data), &length);
	report("content", status == B_OK && size == 8192
			&& length == (int32)sizeof(data)
			&& memcmp(data, kText, sizeof(kText) - 1) == 0,
		"%s; size %" B_PRId64 " (want 8192), %" B_PRId32 " bytes, \"%.*s\"",
		strerror(status), size, length,
		(int)min_c(length, (int32)sizeof(kText) - 1), data);

	status = check(token, &size, data, sizeof(data), &length);
	report("once", status == B_ENTRY_NOT_FOUND,
		"the same token again: %s (want %s)", strerror(status),
		strerror(B_ENTRY_NOT_FOUND));

	// Another team.
	int32 shared = 0;
	status = BPrivate::app_server_send_fd(fd, &shared);
	BString command;
	command.SetToFormat("'%s' child %" B_PRId32, argv[0], shared);
	int childStatus = system(command.String());
	status_t mine = check(shared, &size, data, sizeof(data), &length);
	report("other_team", status == B_OK && WIFEXITED(childStatus)
			&& WEXITSTATUS(childStatus) == 0 && mine == B_OK,
		"the child %s this team's token and %s its own; this team took it "
		"afterwards: %s", WIFEXITED(childStatus)
			&& (WEXITSTATUS(childStatus) & 1) == 0 ? "could not take"
			: "TOOK", WIFEXITED(childStatus)
			&& (WEXITSTATUS(childStatus) & 2) == 0 ? "used" : "could not use",
		strerror(mine));

	int good = 0;
	for (int i = 0; i < 64; i++) {
		char text[32];
		snprintf(text, sizeof(text), "descriptor %d", i);
		int memory = memfd_with(text, 4096 * (i + 1));
		int32 each = 0;
		if (BPrivate::app_server_send_fd(memory, &each) == B_OK
			&& check(each, &size, data, sizeof(data), &length) == B_OK
			&& size == 4096 * (i + 1) && length > 0
			&& strncmp(data, text, strlen(text)) == 0)
			good++;
		close(memory);
	}
	report("many", good == 64, "%d of 64 descriptors sent and taken", good);

	bigtime_t start = system_time();
	status = BPrivate::app_server_send_fd(-1, &token);
	report("bad_descriptor", status == B_BAD_VALUE
			&& system_time() - start < 100000,
		"-1: %s", strerror(status));

	// The child's connection was closed when it quit; the thread serving
	// the socket must not be busy with it.
	bigtime_t busy = -1;
	team_info appServer;
	int32 teamCookie = 0;
	while (busy < 0 && get_next_team_info(&teamCookie, &appServer) == B_OK) {
		if (strstr(appServer.args, "app_server") == NULL)
			continue;
		thread_info thread;
		int32 cookie = 0;
		while (get_next_thread_info(appServer.team, &cookie, &thread)
				== B_OK) {
			if (strcmp(thread.name, "fd transfer") != 0)
				continue;
			bigtime_t before = thread.user_time + thread.kernel_time;
			snooze(1000000);
			if (get_thread_info(thread.thread, &thread) == B_OK)
				busy = thread.user_time + thread.kernel_time - before;
			break;
		}
	}
	report("closed_quietly", busy >= 0 && busy < 100000,
		"the fd transfer thread used %" B_PRIdBIGTIME " us of CPU in 1 s "
		"after the child's connection closed (want < 100000)", busy);

	close(fd);
	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
