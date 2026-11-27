/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: packagefs rolls back a package activation that fails
	part-way, so that nothing of the failed package stays in the node tree
	(batch 14, hardware report: a kernel panic, use-after-free in
	PackageDirectory::HasPrecedenceOver(), on the activation after a failed
	one). The hardware sequence was libglvnd_devel 1.7.0 (headers in the
	directory develop/headers/os/opengl/GL) next to mesa_devel 22.0.5 (a
	symlink at that path): "Failed to change the package activation in
	packagefs: Is a directory", then later another activation touching
	those directories.

	The test builds three tiny packages with "package create" and installs
	them in the home installation location with "pkgman install -H -y -R"
	(no repository refresh, no network):

		ce_pfa  data/ce_pf/inc/GL is a symlink, data/ce_pf/a.txt
		ce_pfb  data/ce_pf/inc/GL/gl.h (GL is a directory), data/ce_pf/b.txt
		ce_pfc  data/ce_pf/inc/c.h

	  pkgfs.conflict_refused  installing ce_pfb next to ce_pfa fails, and
	                          nothing of ce_pfb shows (inc/GL stays ce_pfa's
	                          symlink)
	  pkgfs.rollback_clean    once ce_pfa is uninstalled, data/ce_pf is
	                          gone: the failed ce_pfb left no directories
	                          behind (the old code left data/ce_pf and
	                          data/ce_pf/inc, held by its freed nodes)
	  pkgfs.after_failure     ce_pfc then installs and shows inc/c.h, and
	                          uninstalls without a trace (the old code
	                          compared against the freed nodes here)
	Everything the test creates is removed again. Output format: see
	3rdparty/ce/README.md.
*/


#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>


#define SUITE "pkgfs"

static const char* kWorkDir = "/tmp/ce-pkgfs";
static const char* kData = "/boot/home/config/data/ce_pf";

static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[400];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);

	// one line per check, whatever the detail holds
	for (char* c = detail; *c != '\0'; c++) {
		if (*c == '\n' || *c == '\r')
			*c = ' ';
	}
	printf("CE-TEST %s.%s %s %s\n", SUITE, name, pass ? "PASS" : "FAIL",
		detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


static bool
write_file(const std::string& path, const std::string& content)
{
	FILE* file = fopen(path.c_str(), "w");
	if (file == NULL)
		return false;
	bool ok = fwrite(content.data(), 1, content.size(), file)
		== content.size();
	return fclose(file) == 0 && ok;
}


//!	Runs \a command with sh; its exit status, its output in \a output.
static int
run(const std::string& command, std::string& output)
{
	output.clear();
	FILE* pipe = popen((command + " 2>&1").c_str(), "r");
	if (pipe == NULL)
		return -1;
	char buffer[512];
	size_t bytes;
	while ((bytes = fread(buffer, 1, sizeof(buffer), pipe)) > 0)
		output.append(buffer, bytes);
	int status = pclose(pipe);
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}


//!	The last line of \a text that is not empty, at most 160 characters.
static std::string
last_line(const std::string& text)
{
	size_t end = text.find_last_not_of("\n\r ");
	if (end == std::string::npos)
		return "";
	size_t start = text.rfind('\n', end);
	start = start == std::string::npos ? 0 : start + 1;
	std::string line = text.substr(start, end + 1 - start);
	if (line.size() > 160)
		line = line.substr(0, 160);
	return line;
}


/*!	Creates \a name-1-1-any.hpkg in the work directory from the content
	directory "<work>/<name>" (made by the caller); its path in \a path.
*/
static bool
make_package(const char* name, std::string& path, std::string& error)
{
	std::string content = std::string(kWorkDir) + "/" + name;
	char text[1024];
	snprintf(text, sizeof(text),
		"name\t\t\t%s\n"
		"version\t\t\t1-1\n"
		"architecture\tany\n"
		"summary\t\t\t\"Renga harness test package\"\n"
		"description\t\t\"Only used by the pkgfs test.\"\n"
		"packager\t\t\"Renga harness <harness@renga.invalid>\"\n"
		"vendor\t\t\t\"Renga\"\n"
		"copyrights\t\t\"2026 Renga contributors\"\n"
		"licenses\t\t\"MIT\"\n"
		"provides {\n\t%s = 1\n}\n"
		"requires {\n}\n", name, name);
	if (!write_file(content + "/.PackageInfo", text)) {
		error = "cannot write .PackageInfo";
		return false;
	}
	path = std::string(kWorkDir) + "/" + name + "-1-1-any.hpkg";
	std::string output;
	if (run("package create -q -C " + content + " " + path, output) != 0) {
		error = "package create: " + last_line(output);
		return false;
	}
	return true;
}


static int
install(const std::string& package, std::string& output)
{
	return run("pkgman install -H -y -R " + package, output);
}


static int
uninstall(const char* name, std::string& output)
{
	return run(std::string("pkgman uninstall -H -y ") + name, output);
}


static bool
is_symlink(const std::string& path)
{
	struct stat st;
	return lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}


static bool
exists(const std::string& path)
{
	struct stat st;
	return lstat(path.c_str(), &st) == 0;
}


static void
cleanup()
{
	std::string output;
	// whatever is still installed from an earlier, interrupted run
	const char* names[] = { "ce_pfc", "ce_pfb", "ce_pfa", NULL };
	for (int i = 0; names[i] != NULL; i++) {
		std::string path = std::string("/boot/home/config/packages/")
			+ names[i] + "-1-1-any.hpkg";
		if (exists(path))
			uninstall(names[i], output);
	}
	run(std::string("rm -rf ") + kWorkDir, output);
}


int
main()
{
	cleanup();

	std::string output;
	std::string error;
	std::string work(kWorkDir);
	bool made = run("mkdir -p " + work + "/ce_pfa/data/ce_pf/inc "
			+ work + "/ce_pfb/data/ce_pf/inc/GL "
			+ work + "/ce_pfc/data/ce_pf/inc", output) == 0
		&& symlink("../a.txt", (work + "/ce_pfa/data/ce_pf/inc/GL").c_str())
			== 0
		&& write_file(work + "/ce_pfa/data/ce_pf/a.txt", "a\n")
		&& write_file(work + "/ce_pfb/data/ce_pf/b.txt", "b\n")
		&& write_file(work + "/ce_pfb/data/ce_pf/inc/GL/gl.h", "gl\n")
		&& write_file(work + "/ce_pfc/data/ce_pf/inc/c.h", "c\n");
	std::string packageA;
	std::string packageB;
	std::string packageC;
	made = made && make_package("ce_pfa", packageA, error)
		&& make_package("ce_pfb", packageB, error)
		&& make_package("ce_pfc", packageC, error);
	if (!made) {
		report("conflict_refused", false, "setup: %s %s", error.c_str(),
			last_line(output).c_str());
		cleanup();
		return 1;
	}

	// 1. ce_pfa, then ce_pfb on top: a directory where ce_pfa has a symlink
	int statusA = install(packageA, output);
	std::string outputA = last_line(output);
	bool aActive = is_symlink(std::string(kData) + "/inc/GL");
	int statusB = install(packageB, output);
	std::string outputB = last_line(output);
	bool bShows = exists(std::string(kData) + "/b.txt")
		|| exists(std::string(kData) + "/inc/GL/gl.h");
	bool stillLink = is_symlink(std::string(kData) + "/inc/GL");
	report("conflict_refused", statusA == 0 && aActive && statusB != 0
			&& !bShows && stillLink,
		"ce_pfa: exit %d, inc/GL symlink %d (%s); ce_pfb: exit %d (want "
		"failure: %s); b.txt or gl.h shown %d, inc/GL still a symlink %d",
		statusA, aActive, outputA.c_str(), statusB, outputB.c_str(), bShows,
		stillLink);

	// 2. without ce_pfa, nothing is left of either package
	int statusUninstallA = uninstall("ce_pfa", output);
	std::string outputUninstallA = last_line(output);
	bool dataLeft = exists(kData);
	bool incLeft = exists(std::string(kData) + "/inc");
	report("rollback_clean", statusUninstallA == 0 && !dataLeft,
		"uninstall ce_pfa: exit %d (%s); data/ce_pf left %d, data/ce_pf/inc "
		"left %d (want neither)", statusUninstallA, outputUninstallA.c_str(),
		dataLeft, incLeft);

	// 3. another package in the same directories
	int statusC = install(packageC, output);
	std::string outputC = last_line(output);
	bool cShows = exists(std::string(kData) + "/inc/c.h");
	int statusUninstallC = uninstall("ce_pfc", output);
	bool cGone = !exists(kData);
	report("after_failure", statusC == 0 && cShows && statusUninstallC == 0
			&& cGone,
		"install ce_pfc: exit %d (%s), inc/c.h shown %d; uninstall: exit %d, "
		"data/ce_pf gone %d", statusC, outputC.c_str(), cShows,
		statusUninstallC, cGone);

	cleanup();
	return sFailures == 0 ? 0 : 1;
}
