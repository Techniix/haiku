/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Regression checks for the two segment violations of the batch-2
	renga-raw sweep (roadmap, batch 3 item 1):

	- media_teardown.exit_while_reading: WebPositive crashed when it quit
	  during an HTML5 clip. exit() runs the static PluginManager's
	  destructor, which unloaded every media plug-in, also those still in
	  use, while other threads of the team ran inside them. A child of
	  this test opens a WAV file with BMediaFile, keeps two threads
	  seeking and reading frames, and calls exit(0) under them; it must
	  end with status 0, not with a signal from debug_server. Repeated
	  kRuns times, since the window is short.

	- media_teardown.stale_reply: the Audio preferences crashed in
	  BMediaRoster::GetInstancesFor(). A query whose reply came after the
	  15 s timeout left that reply in its pooled reply port, and the
	  team's next query read it as its own answer. A fake server here
	  answers the first query late with a nonsense count and the second
	  one at once; the second query must get its own answer.
	  It takes about 16 s.

	- media_teardown.count_clamped: GetInstancesFor() never reports more
	  instances than the caller asked for.
*/


#include <File.h>
#include <MediaFile.h>
#include <MediaRoster.h>
#include <MediaTrack.h>
#include <OS.h>

#include <DataExchange.h>
#include <ServerInterface.h>

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>


#define SUITE "media_teardown"

using namespace BPrivate::media;
using namespace BPrivate::media::dataexchange;


static int sFailures = 0;
static const int kRuns = 8;
static const char* kWavePath = "/tmp/media_teardown_test.wav";


static void
info(const char* format, ...)
{
	char text[500];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	printf("CE-INFO " SUITE ": %s\n", text);
	fflush(stdout);
}


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[400];
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


// #pragma mark - exit while reading


/*!	Crash reports debug_server saved for this program. A thread that
	crashes while the team is already exiting does not always change the
	exit status, so the reports are counted too.
*/
static int
count_crash_reports()
{
	DIR* dir = opendir("/boot/system/var/log/crash");
	if (dir == NULL)
		return 0;
	int count = 0;
	while (dirent* entry = readdir(dir)) {
		if (strncmp(entry->d_name, "app-media_teardown_test-", 24) == 0)
			count++;
	}
	closedir(dir);
	return count;
}


static void
put16(FILE* file, uint16 value)
{
	fputc(value & 0xff, file);
	fputc(value >> 8, file);
}


static void
put32(FILE* file, uint32 value)
{
	put16(file, value & 0xffff);
	put16(file, value >> 16);
}


//!	Two seconds of 44.1 kHz stereo 16-bit PCM, a 440 Hz square wave.
static bool
write_wave()
{
	FILE* file = fopen(kWavePath, "wb");
	if (file == NULL)
		return false;
	const uint32 frames = 88200;
	const uint32 dataSize = frames * 4;
	fwrite("RIFF", 1, 4, file);
	put32(file, 36 + dataSize);
	fwrite("WAVEfmt ", 1, 8, file);
	put32(file, 16);
	put16(file, 1);
	put16(file, 2);
	put32(file, 44100);
	put32(file, 44100 * 4);
	put16(file, 4);
	put16(file, 16);
	fwrite("data", 1, 4, file);
	put32(file, dataSize);
	for (uint32 i = 0; i < frames; i++) {
		int16 sample = (i / 50) % 2 ? 8000 : -8000;
		put16(file, sample);
		put16(file, sample);
	}
	return fclose(file) == 0;
}


static status_t
reader_thread(void* data)
{
	BMediaTrack* track = (BMediaTrack*)data;
	char buffer[65536];
	while (true) {
		int64 frame = 0;
		track->SeekToFrame(&frame);
		for (int i = 0; i < 8; i++) {
			int64 count = 0;
			if (track->ReadFrames(buffer, &count) != B_OK)
				break;
		}
	}
	return B_OK;
}


//!	The child: readers running in the plug-ins, then exit(0) under them.
static int
child_main()
{
	// Two files, so that the two readers do not share a track. They are
	// left for the team's end, as a browser that quits leaves its players.
	int threads = 0;
	for (int i = 0; i < 2; i++) {
		BMediaFile* file = new BMediaFile(new BFile(kWavePath, B_READ_ONLY),
			0);
		if (file->InitCheck() != B_OK || file->CountTracks() < 1)
			return 3;
		BMediaTrack* track = file->TrackAt(0);
		if (track == NULL)
			return 4;
		media_format format;
		format.type = B_MEDIA_RAW_AUDIO;
		format.u.raw_audio = media_raw_audio_format::wildcard;
		if (track->DecodedFormat(&format) != B_OK)
			return 4;
		thread_id thread = spawn_thread(reader_thread, "reader",
			B_NORMAL_PRIORITY, track);
		if (thread < 0 || resume_thread(thread) != B_OK)
			return 5;
		threads++;
	}

	snooze(300000);
	exit(0);
}


static void
check_exit_while_reading(const char* self)
{
	if (!write_wave()) {
		report("exit_while_reading", false, "cannot write %s: %s",
			kWavePath, strerror(errno));
		return;
	}

	int reportsBefore = count_crash_reports();
	int clean = 0;
	int signalled = 0;
	int other = 0;
	int lastStatus = 0;
	for (int run = 0; run < kRuns; run++) {
		pid_t child = fork();
		if (child == 0) {
			execl(self, self, "--child", (char*)NULL);
			_exit(127);
		}
		if (child < 0) {
			other++;
			continue;
		}
		int status = 0;
		bool done = false;
		for (int i = 0; i < 300 && !done; i++) {
			if (waitpid(child, &status, WNOHANG) == child)
				done = true;
			else
				snooze(100000);
		}
		if (!done) {
			kill(child, SIGKILL);
			waitpid(child, &status, 0);
			other++;
			lastStatus = -1;
			continue;
		}
		lastStatus = status;
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
			clean++;
		else if (WIFSIGNALED(status))
			signalled++;
		else
			other++;
	}
	unlink(kWavePath);
	snooze(1000000);
		// debug_server writes its reports after the team is gone
	int reports = count_crash_reports() - reportsBefore;

	report("exit_while_reading", clean == kRuns && reports == 0,
		"%d of %d children exited cleanly with plug-in readers running, "
		"%d killed by a signal, %d other (last status 0x%x); %d crash "
		"reports", clean, kRuns, signalled, other, lastStatus, reports);
}


// #pragma mark - stale reply


struct FakeServer {
	port_id	port;
	int		requests;
};


static status_t
fake_server(void* data)
{
	FakeServer* server = (FakeServer*)data;
	server_get_instances_for_request request;
	while (true) {
		int32 code;
		ssize_t size = read_port(server->port, &code, &request,
			sizeof(request));
		if (size < 0)
			return B_OK;
		server->requests++;

		server_get_instances_for_reply* reply
			= new server_get_instances_for_reply;
		memset(reply, 0, sizeof(*reply));
		reply->result = B_OK;
		if (server->requests == 1) {
			// later than the client's 15 s, with a count nobody asked for
			snooze(15500000);
			reply->count = 1000000;
			reply->node_id[0] = 7;
		} else {
			reply->count = 1;
			reply->node_id[0] = 42;
		}
		write_port(request.reply_port, 0, reply, sizeof(*reply));
			// fails with B_BAD_PORT_ID when the client retired the port
		delete reply;
	}
}


static void
check_stale_reply()
{
	FakeServer server;
	server.port = create_port(4, "fake media server");
	server.requests = 0;
	thread_id thread = spawn_thread(fake_server, "fake media server",
		B_NORMAL_PRIORITY, &server);
	resume_thread(thread);

	server_get_instances_for_reply* reply
		= new server_get_instances_for_reply;

	status_t first;
	{
		server_get_instances_for_request request;
		request.max_count = 1;
		request.add_on_id = 1;
		request.flavor_id = 1;
		first = QueryPort(server.port, SERVER_GET_INSTANCES_FOR, &request,
			sizeof(request), reply, sizeof(*reply));
	}
	// let the late reply arrive
	snooze(1500000);

	status_t second;
	{
		server_get_instances_for_request request;
		request.max_count = 1;
		request.add_on_id = 1;
		request.flavor_id = 1;
		memset(reply, 0, sizeof(*reply));
		second = QueryPort(server.port, SERVER_GET_INSTANCES_FOR, &request,
			sizeof(request), reply, sizeof(*reply));
	}

	report("stale_reply", first != B_OK && second == B_OK
			&& reply->count == 1 && reply->node_id[0] == 42,
		"first query: %s (want a timeout); second: %s, count %" B_PRId32
		" node %" B_PRId32 " (want 1 and 42, the late answer had 1000000 "
		"and 7)", strerror(first), strerror(second), reply->count,
		reply->node_id[0]);

	delete reply;
	delete_port(server.port);
	status_t exitValue;
	wait_for_thread(thread, &exitValue);
}


static void
check_count_clamped()
{
	if (!BMediaRoster::IsRunning()) {
		info("count_clamped skipped: no media_server");
		return;
	}
	media_node_id ids[2] = { -1, -1 };
	int32 count = 1;
	// the mixer's add-on and flavour are not known here: ask for any
	// node the roster lists and check the count stays within one
	live_node_info nodes[8];
	int32 nodeCount = 8;
	BMediaRoster* roster = BMediaRoster::Roster();
	status_t status = roster->GetLiveNodes(nodes, &nodeCount);
	dormant_node_info dormant;
	bool found = false;
	for (int32 i = 0; status == B_OK && i < nodeCount && !found; i++) {
		if (roster->GetDormantNodeFor(nodes[i].node, &dormant) == B_OK
			&& dormant.addon >= 0)
			found = true;
	}
	if (!found) {
		info("count_clamped skipped: no node from an add-on");
		return;
	}
	status = roster->GetInstancesFor(dormant.addon, dormant.flavor_id, ids,
		&count);
	report("count_clamped", status == B_OK && count >= 0 && count <= 1
			&& ids[1] == -1,
		"add-on %" B_PRId32 " flavour %" B_PRId32 ": %s, count %" B_PRId32
		" (asked for 1), the slot after it %s", dormant.addon,
		dormant.flavor_id, strerror(status), count,
		ids[1] == -1 ? "untouched" : "written");
}


int
main(int argc, char** argv)
{
	if (argc > 1 && strcmp(argv[1], "--child") == 0)
		return child_main();

	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	check_exit_while_reading(argv[0]);
	check_count_clamped();
	check_stale_reply();

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
