/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test: when two package repositories offer a package with the
	same name, the one whose repository configuration has the better
	(lower) priority must be chosen by the package solver, as pkgman and
	HaikuDepot use it.

	The test builds two tiny repositories on the spot with the system's own
	tools ("package create", "package_repo create") and configures both:

		CETestPreferred  config priority 1, offers cetestpkg-1-1
		CETestOther      config priority 2, offers cetestpkg-2-1

	Both repositories' own repo.info say priority 1, as the HaikuPorts and
	Techniix servers do, so only the configured priority can tell them
	apart. The preferred repository also has the older version, so a
	solver that sees a tie and picks the highest version fails the test.
	"pkgman install -R" (no refresh, no network needed) prints the planned
	change ("install package cetestpkg-1-1 from repository CETestPreferred")
	and is then answered "no", so nothing is installed. Everything the test
	creates is removed again. Output format: see 3rdparty/ce/README.md.
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


#define SUITE "pkg_priority"

static const char* kWorkDir = "/tmp/ce-pkg-priority";
static const char* kConfigDir = "/boot/system/settings/package-repositories";
static const char* kCacheDir = "/boot/system/cache/package-repositories";


static int sFailures = 0;


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[256];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);

	// One line per check, whatever the detail holds.
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


/*!	Runs \a command with sh, returns its exit status and its combined
	stdout/stderr in \a output.
*/
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


//! The first line of \a text that contains \a needle, or "".
static std::string
line_with(const std::string& text, const char* needle)
{
	size_t at = text.find(needle);
	if (at == std::string::npos)
		return "";
	size_t start = text.rfind('\n', at);
	start = start == std::string::npos ? 0 : start + 1;
	size_t end = text.find('\n', at);
	std::string line = text.substr(start,
		end == std::string::npos ? std::string::npos : end - start);
	while (!line.empty() && (line[0] == ' ' || line[0] == '\t'))
		line.erase(0, 1);
	return line;
}


/*!	Creates the repository \a name (config priority \a configPriority,
	offering cetestpkg-\a version-1): the package, the repository cache in
	the system cache directory and the configuration.
*/
static bool
make_repository(const char* name, int configPriority, int version,
	std::string& error)
{
	std::string dir = std::string(kWorkDir) + "/" + name;
	std::string content = dir + "/content";
	std::string output;
	if (run("mkdir -p " + content, output) != 0) {
		error = "mkdir: " + output;
		return false;
	}

	char text[1024];
	snprintf(text, sizeof(text),
		"name\t\t\tcetestpkg\n"
		"version\t\t\t%d-1\n"
		"architecture\tany\n"
		"summary\t\t\t\"Renga harness test package\"\n"
		"description\t\t\"Only used by the pkg_priority test.\"\n"
		"packager\t\t\"Renga harness <harness@renga.invalid>\"\n"
		"vendor\t\t\t\"Renga harness\"\n"
		"copyrights\t\t\"2026 Renga contributors\"\n"
		"licenses\t\t\"MIT\"\n"
		"provides {\n\tcetestpkg = %d\n}\n"
		"requires {\n}\n",
		version, version);
	if (!write_file(content + "/.PackageInfo", text)) {
		error = "cannot write .PackageInfo";
		return false;
	}

	char packageName[64];
	snprintf(packageName, sizeof(packageName), "cetestpkg-%d-1-any.hpkg",
		version);
	std::string package = dir + "/" + packageName;
	if (run("package create -q -C " + content + " " + package, output)
			!= 0) {
		error = "package create: " + output;
		return false;
	}

	// The repository's own info says priority 1 in both repositories.
	snprintf(text, sizeof(text),
		"name %s\n"
		"vendor \"Renga harness\"\n"
		"summary \"pkg_priority test repository\"\n"
		"priority 1\n"
		"baseurl file://%s\n"
		"identifier tag:renga.invalid,2026:ce-test/%s\n"
		"architecture x86_64\n",
		name, dir.c_str(), name);
	if (!write_file(dir + "/repo.info", text)) {
		error = "cannot write repo.info";
		return false;
	}
	if (run("cd " + dir + " && package_repo create -q repo.info " + package,
			output) != 0) {
		error = "package_repo create: " + output;
		return false;
	}
	if (run(std::string("mkdir -p ") + kCacheDir + " && cp " + dir + "/repo "
			+ kCacheDir + "/" + name, output) != 0) {
		error = "cache: " + output;
		return false;
	}

	// The configuration, as "pkgman add-repo" would write it, but with its
	// own priority.
	snprintf(text, sizeof(text),
		"cfgversion=2\n"
		"baseurl=file://%s\n"
		"identifier=tag:renga.invalid,2026:ce-test/%s\n"
		"url=tag:renga.invalid,2026:ce-test/%s\n"
		"priority=%d\n",
		dir.c_str(), name, name, configPriority);
	if (!write_file(std::string(kConfigDir) + "/" + name, text)) {
		error = "cannot write config";
		return false;
	}
	return true;
}


static void
clean_up()
{
	std::string output;
	run(std::string("rm -rf ") + kWorkDir
		+ " " + kConfigDir + "/CETestPreferred"
		+ " " + kConfigDir + "/CETestOther"
		+ " " + kCacheDir + "/CETestPreferred"
		+ " " + kCacheDir + "/CETestOther", output);
}


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);

	clean_up();

	std::string error;
	bool made = make_repository("CETestPreferred", 1, 1, error)
		&& make_repository("CETestOther", 2, 2, error);
	report("setup", made, "%s", made ? "two test repositories configured"
		: error.c_str());

	if (made) {
		// list-repos shows the configured priorities.
		std::string repos;
		run("pkgman list-repos", repos);
		report("configs_read", repos.find("CETestOther") != std::string::npos
				&& repos.find("CETestPreferred") != std::string::npos,
			"pkgman list-repos lists both test repositories");

		std::string output;
		int status = run("echo no | pkgman install -R cetestpkg", output);
		std::string plan = line_with(output, "install package cetestpkg");
		bool pass = plan.find("cetestpkg-1-1 from repository CETestPreferred")
			!= std::string::npos;
		report("config_priority_wins", pass, "pkgman plans \"%s\" (want "
			"cetestpkg-1-1 from CETestPreferred, config priority 1 over 2;"
			" exit %d)", plan.empty() ? output.substr(0, 160).c_str()
				: plan.c_str(), status);

		bool notInstalled
			= access("/boot/system/packages/cetestpkg-1-1-any.hpkg", F_OK) != 0
			&& access("/boot/system/packages/cetestpkg-2-1-any.hpkg", F_OK)
				!= 0;
		report("nothing_installed", notInstalled,
			"answering \"no\" left /boot/system/packages unchanged");
	}

	clean_up();

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
