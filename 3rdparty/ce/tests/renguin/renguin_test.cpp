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

	Then dynamic Linux programs from the harness image's Linux root
	(/boot/system/non-packaged/compat/linux: Alpine's minirootfs with GNU
	coreutils and the dyn/ test programs, see root/build-root.sh), each run
	by its Haiku path from a native shell and compared with its expected
	output and exit status:

	  root.present             the Linux root and its loaders are there
	  dyn.hello_musl/glibc     a dynamic program through ld-musl/ld-linux
	  dyn.busybox              busybox's shell: arithmetic, the root's files
	  dyn.busybox_applets      a pipeline of applets, files in Haiku's /tmp
	  dyn.coreutils            GNU coreutils (ls, sort, head, sha256sum)
	  dyn.net_musl/glibc       sockets, SCM_RIGHTS, getaddrinfo, netlink
	                           (getifaddrs, if_nameindex, RTM_GETROUTE)
	  dyn.fds_musl/glibc       epoll, eventfd, timerfd, signalfd, inotify,
	                           memfd with sealing, on Renga's own
	  dyn.wget                 busybox wget from a busybox nc server on
	                           localhost (/etc/hosts), Renguin's sockets
	  dyn.ip                   busybox ip addr/route (netlink) and ifconfig
	                           (SIOCGIF* ioctls) show the loopback "lo"
	  dyn.resolv_conf          /etc/resolv.conf in the root is Haiku's own

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

// Programs that do not pass yet; see the roadmap's F-27 entry. (libc-fs
// was here until its utimensat check accepted Renguin's noatime-style
// access times on BFS, 2026-11-14.)
static const char* kPending[] = { NULL };

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


static const char* kRoot = "/boot/system/non-packaged/compat/linux";


struct RootCheck {
	const char*	name;
	const char*	command;	// run by a native sh; $R is the Linux root
	int			status;
	const char*	output;		// exact, trailing line breaks dropped
};

#define BB "$R/bin/busybox"

static const RootCheck kRootChecks[] = {
	{ "dyn.hello_musl", "RENGUIN_TEST_ENV=1 $R/opt/renguin-tests/hello-musl x", 0,
		"hello dynamic argc=2 arg=x tls=7 env=yes" },
	{ "dyn.hello_glibc", "RENGUIN_TEST_ENV=1 $R/opt/renguin-tests/hello-glibc x", 0,
		"hello dynamic argc=2 arg=x tls=7 env=yes" },
	{ "dyn.busybox", BB " sh -c 'echo $((6 * 7)); cat /etc/alpine-release'",
		0, "42\n3.24.4" },
	{ "dyn.busybox_applets", BB " sh -c 'rm -rf /tmp/rg-bb && mkdir -p "
		"/tmp/rg-bb && cd /tmp/rg-bb && printf \"b\\na\\nc\\n\" > f && "
		"cp f g && sort g | head -n 2 | tr \"\\n\" \" \" && wc -l < f "
		"&& cd / && rm -r /tmp/rg-bb && test ! -e /tmp/rg-bb && echo gone'",
		0, "a b 3\ngone" },
	{ "dyn.coreutils", BB " sh -c '/bin/ls /opt/renguin-tests | /usr/bin/sort "
		"| /usr/bin/head -n 1; printf \"x\\n\" | /usr/bin/sha256sum; "
		"/bin/ls --version | /usr/bin/head -n 1'", 0,
		"fds-glibc\n73cb3858a687a8494ca3323053016282f3dad39d42cf62ca4e79dda2"
		"aac7d9ac  -\nls (GNU coreutils) 9.11" },
	{ "dyn.net_musl", "$R/opt/renguin-tests/net-musl", 0,
		"tcp4 ok\ntcp6 ok\nudp4 ok\nunix ok\nsocketpair ok\nfdpass ok\n"
		"timeouts ok\nnonblock ok\ngai ok\nifaddrs ok\nifindex ok\n"
		"route ok\nok" },
	{ "dyn.net_glibc", "$R/opt/renguin-tests/net-glibc", 0,
		"tcp4 ok\ntcp6 ok\nudp4 ok\nunix ok\nsocketpair ok\nfdpass ok\n"
		"timeouts ok\nnonblock ok\ngai ok\nifaddrs ok\nifindex ok\n"
		"route ok\nok" },
	{ "dyn.fds_musl", "$R/opt/renguin-tests/fds-musl", 0,
		"epoll ok\neventfd ok\ntimerfd ok\nsignalfd ok\ninotify ok\n"
		"memfd ok\nok" },
	{ "dyn.fds_glibc", "$R/opt/renguin-tests/fds-glibc", 0,
		"epoll ok\neventfd ok\ntimerfd ok\nsignalfd ok\ninotify ok\n"
		"memfd ok\nok" },
	// Alpine's busybox has no httpd: busybox nc answers one request with a
	// canned HTTP response (a Linux TCP server, on 127.0.0.1: Haiku's IPv6
	// has no IPv4-mapped addresses, so nc's default [::] listener would
	// see no IPv4 client), busybox wget fetches it (a Linux TCP client, by
	// address: localhost resolves to ::1 first, as on Linux; getaddrinfo()
	// is checked by the net programs).
	{ "dyn.wget", "printf 'HTTP/1.0 200 OK\\r\\nContent-Length: 12\\r\\n"
		"Content-Type: text/plain\\r\\n\\r\\nrenguin-www\\n' > /tmp/rg-response"
		"\n" BB " nc -l -p 8088 -s 127.0.0.1 < /tmp/rg-response > /dev/null &"
		"\nserver=$!\nsleep 2\n" BB " wget -q -T 20 -O - "
		"http://127.0.0.1:8088/index.html\nstatus=$?\nkill $server "
		"2>/dev/null\nwait $server 2>/dev/null\nrm -f /tmp/rg-response\n"
		"exit $status", 0, "renguin-www" },
	{ "dyn.ip", BB " sh -c 'ip -o addr show lo | grep -c \"inet 127.0.0.1/8\"; "
		"ip route | grep -c \" dev \" | grep -qv \"^0$\" && echo routes; "
		"ifconfig lo | grep -c \"127.0.0.1\"'", 0, "1\nroutes\n1" },
	{ NULL, NULL, 0, NULL }
};


static void
run_root_checks()
{
	std::string loader = std::string(kRoot) + "/lib/ld-musl-x86_64.so.1";
	std::string glibcLoader = std::string(kRoot) + "/lib64/ld-linux-x86-64.so.2";
	bool present = access(loader.c_str(), F_OK) == 0
		&& access(glibcLoader.c_str(), F_OK) == 0;
	report("root.present", present, "%s: %s", kRoot,
		present ? "ld-musl and ld-linux present" : "missing");
	if (!present)
		return;

	for (int i = 0; kRootChecks[i].name != NULL; i++) {
		const RootCheck& check = kRootChecks[i];
		// The command runs in its own shell so that its jobs and quoting
		// stay its own; single quotes in it are passed through a file.
		std::string script = std::string("/tmp/renguin-check-")
			+ std::to_string(i) + ".sh";
		FILE* file = fopen(script.c_str(), "w");
		if (file == NULL) {
			report(check.name, false, "cannot write %s", script.c_str());
			continue;
		}
		fprintf(file, "R=%s\n%s\n", kRoot, check.command);
		fclose(file);
		std::string command = "cd /tmp && { timeout 90 /bin/sh " + script
			+ " 2>/tmp/renguin-check.err; printf '\\nCE-RC:%d\\n' $?; }";

		bigtime_t started = system_time();
		std::string output = run(command);
		bigtime_t took = system_time() - started;
		unlink(script.c_str());
		size_t marker = output.rfind("\nCE-RC:");
		int status = -1;
		std::string got;
		if (marker != std::string::npos) {
			status = atoi(output.c_str() + marker + 7);
			got = output.substr(0, marker);
			while (!got.empty() && got.back() == '\n')
				got.pop_back();
		}
		std::string want = check.output;
		bool pass = status == check.status && got == want;
		std::string errors;
		if (!pass) {
			errors = run("head -c 300 /tmp/renguin-check.err");
		}
		report(check.name, pass,
			"exit %d (want %d), %.0f ms, output [%s]%s%s%s%s%s", status,
			check.status, took / 1000.0, one_line(got).c_str(),
			pass ? "" : " (want [", pass ? "" : one_line(want).c_str(),
			pass ? "" : "])", errors.empty() ? "" : " stderr: ",
			one_line(errors).c_str());
	}

	// Name servers: the root's /etc/resolv.conf is Haiku's own file.
	const char* haikuResolv = "/boot/system/settings/network/resolv.conf";
	if (access(haikuResolv, F_OK) == 0) {
		std::string haiku = run(std::string("cat ") + haikuResolv);
		std::string linux = run(std::string(kRoot)
			+ "/bin/busybox cat /etc/resolv.conf 2>&1");
		report("dyn.resolv_conf", !haiku.empty() && haiku == linux,
			"Linux view %zu bytes, Haiku's %zu bytes", linux.size(),
			haiku.size());
	} else {
		printf("CE-INFO renguin: no %s on this machine (no DHCP name "
			"server); dyn.resolv_conf not checked\n", haikuResolv);
	}
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

	run_root_checks();

	std::string native = run("/bin/uname -s 2>&1");
	report("native_after", !native.empty()
			&& native.find("Linux") == std::string::npos,
		"uname -s after the Linux programs: %s", one_line(native).c_str());

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	fflush(stdout);
	return sFailures != 0;
}
