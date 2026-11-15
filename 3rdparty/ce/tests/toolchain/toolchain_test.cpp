/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: the Chromium build toolchain from HaikuPorts (the
	Chromium build readiness gate, 2026-11-15). Runs only in images built
	with "ce-test.py --toolchain", which adds HaikuPorts' nodejs24,
	python3.14, gn, ninja, llvm23_clang and llvm23_lld (haikuports.pins);
	elsewhere it reports that and passes.

	  node.eval        node -e prints 2 (node crashed in
	                   v8::Isolate::Initialize on the maintainer's machine)
	  node.repeat      40 starts that allocate a large heap
	  node.script      a script with worker_threads, fs and child_process
	  python3          python3 -c with json, hashlib and subprocess
	  gn, ninja        --version
	  clang_lld        clang -fuse-ld=lld builds a program that runs
	  renga_probe      Chromium-HaikuOS' tools/renga-probe.c (R-1 to R-6
	                   interfaces as Chromium calls them) passes
	  native           node, python3, gn, ninja, clang and ld.lld are
	                   native Haiku programs (no Linux brand that Renguin
	                   would take: EI_OSABI with PT_INTERP/DT_NEEDED)

	Output format: see 3rdparty/ce/README.md.
*/

#include <OS.h>

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>


#define SUITE "toolchain"

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[900];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);
	for (char* c = detail; *c != '\0'; c++) {
		if (*c == '\n')
			*c = '|';
	}
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
	while (!output.empty() && output.back() == '\n')
		output.pop_back();
	if (output.size() > 700)
		output = output.substr(output.size() - 700);
	return output;
}


static bool
write_file(const char* path, const char* text)
{
	FILE* file = fopen(path, "w");
	if (file == NULL)
		return false;
	fputs(text, file);
	fclose(file);
	return true;
}


static void
check(const char* name, const std::string& command, const std::string& want)
{
	bigtime_t start = system_time();
	std::string got = run("cd /tmp && { " + command + " ; } 2>&1; echo rc=$?");
	bool pass = got == want + "\nrc=0";
	report(name, pass, "%.1f s, output [%s]%s%s%s",
		(system_time() - start) / 1000000.0, got.c_str(),
		pass ? "" : " (want [", pass ? "" : want.c_str(), pass ? "" : "])");
}


//! EI_OSABI, and whether the file has PT_INTERP or DT_NEEDED (via readelf
//! is not available; parse the headers).
static std::string
elf_brand(const char* path)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return "missing";
	unsigned char header[64];
	if (read(fd, header, sizeof(header)) != sizeof(header)) {
		close(fd);
		return "short";
	}
	if (memcmp(header, "\177ELF", 4) != 0) {
		close(fd);
		return "script";
	}
	uint64 phoff;
	uint16 phentsize, phnum;
	memcpy(&phoff, header + 32, 8);
	memcpy(&phentsize, header + 54, 2);
	memcpy(&phnum, header + 56, 2);
	bool interp = false, dynamic = false;
	for (int i = 0; i < phnum; i++) {
		uint32 type = 0;
		pread(fd, &type, 4, phoff + (uint64)i * phentsize);
		if (type == 3)
			interp = true;
		if (type == 2)
			dynamic = true;
	}
	close(fd);
	char text[80];
	snprintf(text, sizeof(text), "osabi %d%s%s", header[7],
		interp ? " interp" : "", dynamic ? " dynamic" : "");
	return text;
}


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);

	if (access("/boot/system/bin/node", X_OK) != 0) {
		printf("CE-INFO toolchain: no HaikuPorts toolchain in this image "
			"(build with ce-test.py --toolchain); not checked\n");
		printf("CE-SUITE %s DONE failures=0\n", SUITE);
		return 0;
	}

	// a Terminal's environment: the test runner does not export PATH, and
	// clang finds itself (and so its headers and ld.lld) through it
	setenv("PATH", "/boot/home/config/non-packaged/bin:/boot/home/config/bin:"
		"/boot/system/non-packaged/bin:/bin:/boot/system/apps:"
		"/boot/system/preferences", 1);

	check("node.eval", "node -e 'console.log(1 + 1)'", "2");

	// node crashed in v8::Isolate::Initialize on a 12-thread machine:
	// start it many times, with its worker threads busy
	{
		int failures = 0;
		std::string first;
		bigtime_t start = system_time();
		for (int i = 0; i < 40; i++) {
			std::string got = run("node -e 'let a = []; for (let i = 0; "
				"i < 200000; i++) a.push({i}); console.log(a.length)' 2>&1; "
				"echo rc=$?");
			if (got != "200000\nrc=0") {
				if (failures++ == 0)
					first = got;
			}
		}
		report("node.repeat", failures == 0, "40 starts with a 200000-object "
			"heap: %d failed, %.1f s%s%s", failures,
			(system_time() - start) / 1000000.0,
			failures > 0 ? "; first: " : "", first.c_str());
	}

	write_file("/tmp/ce-node.js",
		"const { Worker, isMainThread, parentPort } = "
			"require('worker_threads');\n"
		"const fs = require('fs');\n"
		"const cp = require('child_process');\n"
		"if (isMainThread) {\n"
		"  fs.writeFileSync('/tmp/ce-node.txt', 'fs ok');\n"
		"  const text = fs.readFileSync('/tmp/ce-node.txt', 'utf8');\n"
		"  const child = cp.spawnSync('/bin/sh', ['-c', 'echo child ok'])"
			".stdout.toString().trim();\n"
		"  const worker = new Worker(__filename);\n"
		"  worker.on('message', (n) => {\n"
		"    console.log('worker ' + n + ' ' + text + ' ' + child);\n"
		"    fs.unlinkSync('/tmp/ce-node.txt');\n"
		"  });\n"
		"} else {\n"
		"  let sum = 0; for (let i = 0; i < 7; i++) sum += i * 2;\n"
		"  parentPort.postMessage(sum);\n"
		"}\n");
	check("node.script", "node /tmp/ce-node.js", "worker 42 fs ok child ok");

	check("python3", "python3 -c 'import json, hashlib, subprocess; "
		"print(json.dumps([hashlib.sha256(b\"x\").hexdigest()[:8], "
		"subprocess.run([\"/bin/sh\", \"-c\", \"echo sub\"], "
		"capture_output=True, text=True).stdout.strip()]))'",
		"[\"2d711642\", \"sub\"]");

	std::string gn = run("gn --version 2>&1; echo rc=$?");
	report("gn", gn.find("rc=0") != std::string::npos
		&& gn.find("2385") != std::string::npos, "[%s]", gn.c_str());
	std::string ninja = run("ninja --version 2>&1; echo rc=$?");
	report("ninja", ninja.find("1.13") == 0
		&& ninja.find("rc=0") != std::string::npos, "[%s]", ninja.c_str());

	// clang with lld, the port's way (-fuse-ld=lld). HaikuPorts' clang
	// ignores the ifunc attribute for the Haiku target ("unknown attribute
	// 'ifunc' ignored"), so native IFUNCs are covered by renguin_test's
	// brand.native_ifunc (built by the build host's clang and ld.lld).
	write_file("/tmp/ce-clang.c",
		"#include <stdio.h>\n"
		"int main(void) { printf(\"clang %d\\n\", 6 * 7); return 0; }\n");
	check("clang_lld", "rm -f /tmp/ce-clang && clang -O2 -fuse-ld=lld "
		"-o /tmp/ce-clang /tmp/ce-clang.c && /tmp/ce-clang", "clang 42");
	unlink("/tmp/ce-clang.c");

	// Chromium-HaikuOS' own probe (tools/renga-probe.c, branch renga),
	// built and run as its scripts/renga-tests.sh probe does: every check
	// passes and every R-n interface is available
	{
		std::string out = run("cd /tmp && clang -D_GNU_SOURCE "
			"-D_DEFAULT_SOURCE -O0 -g -fuse-ld=lld -o /tmp/ce-renga-probe "
			"/boot/system/non-packaged/data/ce-toolchain/renga-probe.c "
			"-lgnu -lbsd -lnetwork 2>&1 && /tmp/ce-renga-probe 2>&1; "
			"echo rc=$?");
		bool pass = out.find("0 check(s) failed") != std::string::npos
			&& out.find("rc=0") != std::string::npos
			&& out.find(": missing") == std::string::npos
			&& out.find("FAIL ") == std::string::npos;
		report("renga_probe", pass, "%s", out.c_str());
		unlink("/tmp/ce-renga-probe");
	}
	fflush(stdout);

	// none of them may look like a static Linux program to Renguin
	const char* programs[] = { "node", "python3", "gn", "ninja", "clang",
		"ld.lld", NULL };
	std::string detail;
	bool native = true;
	for (int i = 0; programs[i] != NULL; i++) {
		std::string path = run(std::string("command -v ") + programs[i]);
		std::string real = run("readlink -f '" + path + "'");
		std::string brand = elf_brand(real.c_str());
		detail += std::string(programs[i]) + ": " + brand + "; ";
		if (brand.find("dynamic") == std::string::npos)
			native = false;
	}
	std::string clangBrand = elf_brand("/tmp/ce-clang");
	detail += "ce-clang: " + clangBrand;
	if (clangBrand.find("dynamic") == std::string::npos)
		native = false;
	unlink("/tmp/ce-clang");
	report("native", native, "%s", detail.c_str());

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	fflush(stdout);
	return sFailures != 0;
}
