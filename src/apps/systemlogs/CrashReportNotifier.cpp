/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * crash_report_notifier (roadmap F-51): after login, a notification for
 * each kernel crash report crash_collector saved and nobody has seen yet;
 * clicking it opens the report in System logs. The user's launch script
 * starts it (data/launch/user_crash_reports); it quits when it is done.
 *
 * A background server (B_BACKGROUND_APP, never in Deskbar) that runs once
 * per session (B_EXCLUSIVE_LAUNCH). It used to be System logs started with
 * --notify, a normal application that showed in Deskbar for about a minute
 * after every login while it waited for the notification server (found on
 * the maintainer's PC, 2026-11-13). "SystemLogs --notify" still starts it.
 */


#include <Application.h>
#include <Catalog.h>
#include <Directory.h>
#include <Entry.h>
#include <Node.h>
#include <Notification.h>
#include <OS.h>
#include <Path.h>
#include <Roster.h>
#include <String.h>

#include <string.h>
#include <syslog.h>

#include <crash_report_format.h>

#include "CrashReportCommon.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "SystemLogs"


static const char* kSignature = "application/x-vnd.Renga-SystemLogs";


static int
notify_new_crashes()
{
	BApplication app(kNotifierSignature);

	// notification_server is a system service; at login it may still be
	// starting.
	for (int32 i = 0; i < 60
			&& !be_roster->IsRunning("application/x-vnd.Haiku-notification_server");
			i++) {
		snooze(500000);
	}

	int32 shown = 0;
	BDirectory directory(CRASH_REPORT_DIRECTORY);
	BEntry entry;
	while (directory.GetNextEntry(&entry) == B_OK) {
		BPath path;
		if (entry.GetPath(&path) != B_OK
			|| strncmp(path.Leaf(), "kernel-", 7) != 0) {
			continue;
		}
		BNode node(&entry);
		int32 isNew = 0;
		if (node.ReadAttr(CRASH_REPORT_NEW_ATTRIBUTE, B_INT32_TYPE, 0, &isNew,
				sizeof(isNew)) != sizeof(isNew) || isNew == 0) {
			continue;
		}

		std::string text;
		read_file(path.Path(), text);
		text = "\n" + text;
		std::string when = report_field(text, "crash time");
		std::string message = report_field(text, "message");

		BNotification notification(B_IMPORTANT_NOTIFICATION);
		notification.SetGroup(B_TRANSLATE_SYSTEM_NAME("System logs"));
		notification.SetTitle(B_TRANSLATE("The system crashed"));
		BString content;
		content.SetToFormat(B_TRANSLATE("The kernel stopped on %s: %s\n"
			"Click to view the crash report."), when.c_str(),
			message.c_str());
		std::string driver = report_field(text, "driver");
		if (!driver.empty()) {
			BString line;
			line.SetToFormat(B_TRANSLATE("\nIt happened in the driver %s."),
				driver.c_str());
			content << line;
		}
		notification.SetContent(content);
		notification.SetMessageID(path.Path());
		notification.SetOnClickApp(kSignature);
		entry_ref ref;
		if (entry.GetRef(&ref) == B_OK)
			notification.SetOnClickFile(&ref);

		status_t status = B_ERROR;
		for (int32 attempt = 0; attempt < 20; attempt++) {
			status = notification.Send(60 * 1000000LL);
			if (status == B_OK)
				break;
			snooze(500000);
		}
		if (status != B_OK) {
			syslog(LOG_ERR, "SystemLogs: could not show the notification for "
				"%s: %s", path.Path(), strerror(status));
			continue;
		}
		node.RemoveAttr(CRASH_REPORT_NEW_ATTRIBUTE);
		syslog(LOG_NOTICE, "SystemLogs: crash report notification shown for "
			"%s", path.Path());
		printf("SystemLogs: notification shown for %s\n", path.Path());
		shown++;
	}
	return 0;
}


int
main()
{
	openlog("crash_report_notifier", 0, LOG_USER);
	return notify_new_crashes();
}
