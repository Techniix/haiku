/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	crash_collector (roadmap F-51): stores the kernel's crash reports.

	When the kernel debugger is entered, it writes a crash report into the
	syslog buffer (src/system/kernel/debug/crash_report.cpp). After a warm
	reboot, the boot loader finds that buffer again and the kernel saves it as
	/var/log/previous_syslog. Run once per boot by launch_daemon (data/launch/
	crash_collector), this program looks for reports in that file and saves
	each one it has not saved before as /boot/system/var/log/crash/
	kernel-<date>.txt: the report, then the last 64 KiB of the syslog before
	it. A new file gets the attribute Renga:crash-report-new, which the System
	logs application's notification looks for at login.

	Usage: crash_collector [--previous-syslog <file>] [--directory <dir>]
	(the options are for tests). Exit status 0 also when there was nothing to
	collect.
*/


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <string>

#include <StorageDefs.h>
#include <TypeConstants.h>
#include <fs_attr.h>

#include <crash_report_format.h>


static bool
read_file(const char* path, std::string& contents)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return false;
	char buffer[65536];
	ssize_t bytesRead;
	while ((bytesRead = read(fd, buffer, sizeof(buffer))) > 0)
		contents.append(buffer, bytesRead);
	close(fd);
	return true;
}


static std::string
field(const std::string& block, const char* name)
{
	std::string key = std::string("\n") + name + ": ";
	size_t start = block.find(key);
	if (start == std::string::npos)
		return "";
	start += key.length();
	size_t end = block.find('\n', start);
	return block.substr(start, end == std::string::npos
		? std::string::npos : end - start);
}


/*!	Removes what the serial/syslog output adds that is not text: carriage
	returns and NUL bytes (the ring buffer can contain them).
*/
static std::string
clean(const std::string& text)
{
	std::string result;
	result.reserve(text.length());
	for (size_t i = 0; i < text.length(); i++) {
		if (text[i] != '\r' && text[i] != '\0')
			result += text[i];
	}
	return result;
}


static int
collect(const char* previousSyslog, const char* directory)
{
	std::string syslogText;
	if (!read_file(previousSyslog, syslogText)) {
		printf("crash_collector: no %s, nothing to collect\n", previousSyslog);
		return 0;
	}

	int saved = 0;
	size_t searchFrom = 0;
	while (true) {
		size_t begin = syslogText.find(CRASH_REPORT_BEGIN, searchFrom);
		if (begin == std::string::npos)
			break;
		size_t end = syslogText.find(CRASH_REPORT_END, begin);
		size_t nextBegin = syslogText.find(CRASH_REPORT_BEGIN, begin + 1);
		bool complete = end != std::string::npos
			&& (nextBegin == std::string::npos || end < nextBegin);
		size_t blockEnd = complete ? end + strlen(CRASH_REPORT_END)
			: (nextBegin != std::string::npos ? nextBegin : syslogText.length());
		std::string block = clean(syslogText.substr(begin, blockEnd - begin));
		searchFrom = blockEnd;

		// The name comes from the time of the crash, so a report is saved
		// only once even if the collector runs again in the same session.
		time_t crashTime = strtoul(field(block, "time").c_str(), NULL, 10);
		if (crashTime == 0)
			crashTime = time(NULL);
		struct tm crashTm;
		localtime_r(&crashTime, &crashTm);
		char date[32];
		strftime(date, sizeof(date), "%Y-%m-%d_%H-%M-%S", &crashTm);
		char timeString[32];
		strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &crashTm);

		mkdir(directory, 0755);
		char path[B_PATH_NAME_LENGTH];
		snprintf(path, sizeof(path), "%s/kernel-%s.txt", directory, date);
		// two reports in the same second (the debugger entered twice)
		std::string uptime = field(block, "uptime_us");
		for (int i = 2; access(path, F_OK) == 0; i++) {
			std::string existing;
			if (read_file(path, existing)
				&& existing.find("uptime_us: " + uptime + "\n")
					!= std::string::npos) {
				break;
			}
			snprintf(path, sizeof(path), "%s/kernel-%s-%d.txt", directory,
				date, i);
		}
		if (access(path, F_OK) == 0)
			continue;

		size_t tailStart = begin > CRASH_REPORT_SYSLOG_TAIL
			? begin - CRASH_REPORT_SYSLOG_TAIL : 0;
		// start the tail at the beginning of a line
		size_t lineStart = syslogText.find('\n', tailStart);
		if (tailStart > 0 && lineStart != std::string::npos && lineStart < begin)
			tailStart = lineStart + 1;
		std::string tail = clean(syslogText.substr(tailStart,
			begin - tailStart));

		time_t now = time(NULL);
		struct tm nowTm;
		localtime_r(&now, &nowTm);
		char collected[32];
		strftime(collected, sizeof(collected), "%Y-%m-%d %H:%M:%S", &nowTm);

		std::string text = "Renga kernel crash report\n";
		text += "crash time: " + std::string(timeString) + "\n";
		text += "collected: " + std::string(collected)
			+ " from the previous session's syslog\n";
		if (!complete) {
			text += "note: the report is incomplete (the debugger stopped "
				"writing it, or the buffer was overwritten)\n";
		}
		text += "\n" + block + "\n\n==== syslog before the crash (last "
			+ std::to_string(CRASH_REPORT_SYSLOG_TAIL / 1024) + " KiB) ====\n"
			+ tail;

		int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
		if (fd < 0) {
			fprintf(stderr, "crash_collector: cannot create %s: %s\n", path,
				strerror(errno));
			syslog(LOG_ERR, "crash_collector: cannot create %s: %s", path,
				strerror(errno));
			return 1;
		}
		if (write(fd, text.data(), text.length()) != (ssize_t)text.length()) {
			fprintf(stderr, "crash_collector: cannot write %s: %s\n", path,
				strerror(errno));
			close(fd);
			unlink(path);
			return 1;
		}
		static const char kType[] = "text/plain";
		fs_write_attr(fd, "BEOS:TYPE", B_MIME_STRING_TYPE, 0, kType,
			sizeof(kType));
		int32 isNew = 1;
		fs_write_attr(fd, CRASH_REPORT_NEW_ATTRIBUTE, B_INT32_TYPE, 0, &isNew,
			sizeof(isNew));
		fsync(fd);
		close(fd);

		printf("crash_collector: saved %s (%s)\n", path,
			field(block, "reason").c_str());
		syslog(LOG_NOTICE, "crash_collector: saved the kernel crash report "
			"of %s as %s", timeString, path);
		saved++;
	}

	if (saved == 0)
		printf("crash_collector: no new crash report in %s\n", previousSyslog);
	return 0;
}


int
main(int argc, char** argv)
{
	const char* previousSyslog = CRASH_REPORT_PREVIOUS_SYSLOG;
	const char* directory = CRASH_REPORT_DIRECTORY;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--previous-syslog") == 0 && i + 1 < argc)
			previousSyslog = argv[++i];
		else if (strcmp(argv[i], "--directory") == 0 && i + 1 < argc)
			directory = argv[++i];
		else {
			fprintf(stderr, "usage: %s [--previous-syslog <file>] "
				"[--directory <dir>]\n", argv[0]);
			return 2;
		}
	}

	openlog("crash_collector", 0, LOG_DAEMON);
	return collect(previousSyslog, directory);
}
