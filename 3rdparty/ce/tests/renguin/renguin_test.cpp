/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: Linux programs under Renguin (roadmap F-27), the Linux
	binary personality in the Renga kernel.

	The harness image carries Renguin's Linux test programs, built on the
	build host from 3rdparty/ce/tests/renguin/linux (see the Jamfile), and
	their expected results (linux/expected.txt: program, arguments, exit
	status, standard output). Each is copied to /tmp, started like any
	program (the kernel's personality hook hands it to the renguin module,
	which loads it and answers its Linux system calls), and must give
	exactly the expected exit status and output:

	  module                   the renguin personality module is installed
	  native_after             a native program still runs normally after
	                           the Linux ones (the hook leaves native teams
	                           alone)
	  <program>                the program's exit status and output match

	The programs: hello (write and exit_group, nothing else), ebadf and
	enosys (Linux errno numbers), exit, xmm (vector registers kept across a
	system call), args (the initial stack), mem (mmap, munmap, mprotect,
	brk), fileio, ids, proc (fork, vfork, clone, execve, wait4), threads
	(clone threads, futexes), signals (Linux signal frames, rt_sigreturn),
	and static glibc programs: libc, libc-pie (static PIE), pthread,
	libc-signals, libc-fs, libc-procfs.

	Programs listed in kPending are reported as CE-INFO lines with their
	result, not as checks, until they pass (each with the reason, recorded
	in the roadmap's F-27 entry); everything else has to pass.

	Output format: see 3rdparty/ce/README.md.
*/


#include <OS.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>


#define SUITE "renguin"

static const char* kSource = "/boot/system/non-packaged/data/renguin-tests";
static const char* kWork = "/tmp/renguin-tests";
static const char* kModule
	= "/boot/system/add-ons/kernel/personalities/renguin";

// Programs that do not pass yet; see the roadmap's F-27 entry.
//  - libc-fs: its utimensat check sets the access time and reads it back,
//    but BFS stores no access time (it reports the current time), so the
//    check cannot pass on a BFS volume whatever Renguin does; the rest of
//    the program is not reached
static const char* kPending[] = { "libc-fs", NULL };

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[700];
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


static std::string
run(const std::string& command)
{
	std::string output;
	FILE* pipe = popen(command.c_str(), "r");
	if (pipe == NULL)
		return output;
	char buffer[4096];
	size_t bytes;
	while ((bytes = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
		output.append(buffer, bytes);
	pclose(pipe);
	return output;
}


//! expected.txt writes line breaks as \n, as printf %b reads them
static std::string
unescape(const std::string& text)
{
	std::string result;
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == 'n') {
			result += '\n';
			i++;
		} else
			result += text[i];
	}
	return result;
}


static std::string
one_line(std::string text)
{
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] == '\n')
			text[i] = '|';
	}
	if (text.size() > 240)
		text.resize(240);
	return text;
}


static bool
is_pending(const std::string& program)
{
	for (int i = 0; kPending[i] != NULL; i++) {
		if (program == kPending[i])
			return true;
	}
	return false;
}


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);

	if (access(kSource, F_OK) != 0) {
		printf("CE-INFO renguin: no Linux test programs on this image\n");
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}

	report("module", access(kModule, F_OK) == 0, "%s: %s", kModule,
		access(kModule, F_OK) == 0 ? "installed" : strerror(errno));

	std::string setup = std::string("rm -rf ") + kWork + " && cp -r "
		+ kSource + " " + kWork + " && chmod +x " + kWork + "/* && echo ok";
	if (run(setup).find("ok") == std::string::npos) {
		report("setup", false, "could not copy the programs to %s", kWork);
		printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
		return 1;
	}

	std::string expectedPath = std::string(kWork) + "/expected.txt";
	FILE* expected = fopen(expectedPath.c_str(), "r");
	if (expected == NULL) {
		report("setup", false, "no %s", expectedPath.c_str());
		printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
		return 1;
	}

	char line[2048];
	while (fgets(line, sizeof(line), expected) != NULL) {
		std::string text(line);
		while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
			text.pop_back();
		if (text.empty() || text[0] == '#')
			continue;
		std::vector<std::string> fields;
		size_t start = 0;
		for (int i = 0; i < 3; i++) {
			size_t bar = text.find('|', start);
			if (bar == std::string::npos)
				break;
			fields.push_back(text.substr(start, bar - start));
			start = bar + 1;
		}
		if (fields.size() != 3)
			continue;
		fields.push_back(text.substr(start));
		const std::string& program = fields[0];
		int wantStatus = atoi(fields[2].c_str());
		std::string want = unescape(fields[3]);

		// a hung program fails after a minute instead of stopping the run
		std::string command = std::string("cd ") + kWork
			+ " && { timeout 60 ./" + program + " " + fields[1]
			+ " 2>/dev/null; printf '\\nCE-RC:%d\\n' $?; }";
		bigtime_t started = system_time();
		std::string output = run(command);
		bigtime_t took = system_time() - started;
		size_t marker = output.rfind("\nCE-RC:");
		int status = -1;
		std::string got;
		if (marker != std::string::npos) {
			status = atoi(output.c_str() + marker + 7);
			got = output.substr(0, marker);
			// the shell's $(...) drops trailing line breaks; so does
			// expected.txt
			while (!got.empty() && got.back() == '\n')
				got.pop_back();
		}
		bool pass = status == wantStatus && got == want;
		if (is_pending(program)) {
			printf("CE-INFO renguin: %s (pending) %s: exit %d, output [%s]\n",
				program.c_str(), pass ? "passes now" : "fails",
				status, one_line(got).c_str());
			fflush(stdout);
			continue;
		}
		report(program.c_str(), pass,
			"exit %d (want %d), %.0f ms, output [%s]%s%s%s", status,
			wantStatus, took / 1000.0, one_line(got).c_str(),
			pass ? "" : " (want [", pass ? "" : one_line(want).c_str(),
			pass ? "" : "])");
	}
	fclose(expected);

	std::string native = run("/bin/uname -s 2>&1");
	report("native_after", !native.empty()
			&& native.find("Linux") == std::string::npos,
		"uname -s after the Linux programs: %s", one_line(native).c_str());

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	fflush(stdout);
	return sFailures != 0;
}
