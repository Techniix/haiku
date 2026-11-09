/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test for how TCP connections end, over loopback:

	  - an orderly close() gives the peer end-of-file (recv() returns 0);
	  - an abortive close (SO_LINGER with a zero timeout) sends a reset: a
	    peer blocked in recv() wakes at once with ECONNRESET, and writes
	    fail afterwards. Before the fix no reset was sent at all, and the
	    peer kept a connection that no longer existed;
	  - an abortive close discards data that was still queued for sending,
	    instead of delivering it.

	Output format: see 3rdparty/ce/README.md.
*/


#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <OS.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>


#define SUITE "tcp_close"


static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[300];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);

	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


static void
set_receive_timeout(int fd, bigtime_t timeout)
{
	struct timeval tv;
	tv.tv_sec = timeout / 1000000;
	tv.tv_usec = timeout % 1000000;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}


static bool
make_pair(int& client, int& server)
{
	client = server = -1;
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0)
		return false;

	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_len = sizeof(address);
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t length = sizeof(address);
	if (bind(listener, (sockaddr*)&address, sizeof(address)) != 0
		|| listen(listener, 1) != 0
		|| getsockname(listener, (sockaddr*)&address, &length) != 0) {
		close(listener);
		return false;
	}

	client = socket(AF_INET, SOCK_STREAM, 0);
	if (client < 0
		|| connect(client, (sockaddr*)&address, sizeof(address)) != 0) {
		close(listener);
		return false;
	}
	server = accept(listener, NULL, NULL);
	close(listener);
	return server >= 0;
}


struct close_job {
	int			fd;
	bool		abortive;
};


static status_t
close_after_delay(void* data)
{
	close_job* job = (close_job*)data;
	snooze(500000);
	if (job->abortive) {
		struct linger linger = { 1, 0 };
		setsockopt(job->fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
	}
	close(job->fd);
	return B_OK;
}


/*!	Blocks in recv() on one end while another thread closes the other end
	half a second later. Returns the recv() result; \a _error and
	\a _elapsed get errno and the time spent blocked.
*/
static ssize_t
recv_while_peer_closes(bool abortive, int& _error, bigtime_t& _elapsed,
	int& _client)
{
	int client, server;
	if (!make_pair(client, server)) {
		_error = errno;
		_elapsed = 0;
		_client = -1;
		return -2;
	}

	close_job job = { server, abortive };
	thread_id thread = spawn_thread(close_after_delay, "close peer",
		B_NORMAL_PRIORITY, &job);
	resume_thread(thread);

	set_receive_timeout(client, 5000000);
	char byte;
	bigtime_t start = system_time();
	ssize_t result = recv(client, &byte, 1, 0);
	_error = errno;
	_elapsed = system_time() - start;

	status_t exitValue;
	wait_for_thread(thread, &exitValue);
	_client = client;
	return result;
}


static void
test_orderly_close()
{
	int error, client;
	bigtime_t elapsed;
	ssize_t result = recv_while_peer_closes(false, error, elapsed, client);
	report("orderly_eof", result == 0 && elapsed < 3000000,
		"recv -> %zd (%s) after %" B_PRId64 " ms (want 0, end of file)",
		result, result < 0 ? strerror(error) : "ok", elapsed / 1000);
	if (client >= 0)
		close(client);
}


static void
test_abortive_close()
{
	int error, client;
	bigtime_t elapsed;
	ssize_t result = recv_while_peer_closes(true, error, elapsed, client);
	report("abortive_reset", result < 0 && error == ECONNRESET
			&& elapsed < 3000000,
		"recv -> %zd (%s) after %" B_PRId64 " ms (want ECONNRESET, well "
		"before the 5 s timeout)", result,
		result < 0 ? strerror(error) : "data", elapsed / 1000);

	if (client >= 0) {
		ssize_t sent = send(client, "x", 1, MSG_NOSIGNAL);
		int sendError = errno;
		report("abortive_send_fails", sent < 0,
			"send after reset -> %zd (%s)", sent,
			sent < 0 ? strerror(sendError) : "accepted");
		close(client);
	}
}


static void
test_abortive_discards()
{
	int client, server;
	if (!make_pair(client, server)) {
		report("discard_setup", false, "could not connect: %s",
			strerror(errno));
		return;
	}

	// Data written right before an abortive close may already be in the
	// peer's receive queue (loopback is fast), so only the outcome after
	// it matters: the connection must end in a reset, not in a clean
	// end of file.
	send(server, "dropped", 7, 0);
	struct linger linger = { 1, 0 };
	setsockopt(server, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
	close(server);

	set_receive_timeout(client, 2000000);
	char buffer[16];
	ssize_t total = 0;
	ssize_t result;
	int error = 0;
	while ((result = recv(client, buffer, sizeof(buffer), 0)) > 0)
		total += result;
	error = errno;
	report("abortive_ends_in_reset", result < 0 && error == ECONNRESET,
		"after %zd byte(s): recv -> %zd (%s) (want ECONNRESET, not end "
		"of file)", total, result, result < 0 ? strerror(error) : "eof");
	close(client);
}


int
main()
{
	test_orderly_close();
	test_abortive_close();
	test_abortive_discards();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
