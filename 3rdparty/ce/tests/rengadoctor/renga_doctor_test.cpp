/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Renga Doctor (batch 5): the one-click hardware report.

	  renga_doctor.overview     the overview has a line for each area
	                            (system, graphics, Linux drivers, boot,
	                            restore points, crash reports, boot volume),
	                            each marked "ok" or "check"
	  renga_doctor.collect      --collect writes the archive
	  renga_doctor.archive      it holds overview.txt first, the logs, the
	                            hardware listings, LinuxKPI, the boot state
	                            and restore points, packages and README.txt
	  renga_doctor.window       the window opens; screenshot renga_doctor
*/


#include <Application.h>
#include <Messenger.h>
#include <OS.h>
#include <Roster.h>
#include <String.h>

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>


#define SUITE "renga_doctor"

static const char* kApp = "/boot/system/apps/RengaDoctor";
static const char* kSignature = "application/x-vnd.Renga-Doctor";
static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[600];
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


static BString
run(const char* command, int* _status = NULL)
{
	BString output;
	FILE* pipe = popen(command, "r");
	if (pipe == NULL)
		return output;
	char buffer[1024];
	size_t bytes;
	while ((bytes = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
		output.Append(buffer, bytes);
	int status = pclose(pipe);
	if (_status != NULL)
		*_status = status;
	return output;
}


int
main()
{
	BApplication app("application/x-vnd.Renga-doctor-test");
	printf("CE-SUITE %s START\n", SUITE);
	if (access(kApp, X_OK) != 0) {
		printf("CE-INFO no %s on this image, nothing to test\n", kApp);
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}

	int status;
	BString overview = run("/boot/system/apps/RengaDoctor --overview 2>&1",
		&status);
	const char* areas[] = { "System", "Graphics", "Linux drivers", "Boot ",
		"Restore points", "Crash reports", "Boot volume" };
	BString missing;
	for (size_t i = 0; i < sizeof(areas) / sizeof(areas[0]); i++) {
		if (overview.FindFirst(areas[i]) < 0)
			missing << areas[i] << " ";
	}
	BString shown(overview);
	shown.ReplaceAll("\n", " | ");
	report("overview", status == 0 && missing.IsEmpty()
			&& (overview.FindFirst("\nok ") >= 0
				|| overview.FindFirst("\ncheck ") >= 0),
		"missing: %s; %s", missing.IsEmpty() ? "none" : missing.String(),
		shown.String());

	const char* archive = "/boot/home/ce-renga-report.zip";
	unlink(archive);
	BString output = run("/boot/system/apps/RengaDoctor --collect "
		"/boot/home/ce-renga-report.zip 2>&1", &status);
	report("collect", status == 0 && access(archive, F_OK) == 0, "%s",
		output.String());

	BString listing = run("unzip -l /boot/home/ce-renga-report.zip 2>&1");
	const char* entries[] = { "overview.txt", "logs/syslog",
		"hardware/listdev.txt", "hardware/linuxkpi.txt",
		"hardware/screen.txt", "system/boot-state.txt",
		"system/restore-points.txt", "system/packages.txt",
		"network/ifconfig.txt", "README.txt" };
	missing = "";
	for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
		if (listing.FindFirst(entries[i]) < 0)
			missing << entries[i] << " ";
	}
	int32 first = listing.FindFirst("overview.txt");
	int32 logs = listing.FindFirst("logs/");
	report("archive", missing.IsEmpty() && first >= 0
			&& (logs < 0 || first < logs),
		"missing: %s", missing.IsEmpty() ? "none" : missing.String());
	unlink(archive);

	team_id team = -1;
	be_roster->Launch(kSignature, (BMessage*)NULL, &team);
	BMessenger messenger;
	for (int i = 0; i < 100 && !messenger.IsValid(); i++) {
		snooze(100000);
		messenger = BMessenger(kSignature);
	}
	snooze(2500000);
	report("window", messenger.IsValid(), "team %" B_PRId32, team);
	printf("CE-HOST screenshot renga_doctor\n");
	fflush(stdout);
	snooze(1500000);
	messenger.SendMessage(B_QUIT_REQUESTED);
	snooze(500000);
	if (team >= 0)
		kill(team, SIGKILL);

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
