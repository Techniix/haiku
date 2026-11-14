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
make_repository(const char* name, int configPriority, const char* package,
	const char* version, const char* vendor, std::string& error,
	std::string* packagePath = NULL)
{
	std::string dir = std::string(kWorkDir) + "/" + name;
	std::string content = dir + "/content";
	std::string output;
	if (run("mkdir -p " + content, output) != 0) {
		error = "mkdir: " + output;
		return false;
	}

	char text[1024];
	std::string providedVersion(version);
	providedVersion = providedVersion.substr(0, providedVersion.find('-'));
	snprintf(text, sizeof(text),
		"name\t\t\t%s\n"
		"version\t\t\t%s\n"
		"architecture\tany\n"
		"summary\t\t\t\"Renga harness test package\"\n"
		"description\t\t\"Only used by the pkg_priority test.\"\n"
		"packager\t\t\"Renga harness <harness@renga.invalid>\"\n"
		"vendor\t\t\t\"%s\"\n"
		"copyrights\t\t\"2026 Renga contributors\"\n"
		"licenses\t\t\"MIT\"\n"
		"provides {\n\t%s = %s\n}\n"
		"requires {\n}\n",
		package, version, vendor, package, providedVersion.c_str());
	if (!write_file(content + "/.PackageInfo", text)) {
		error = "cannot write .PackageInfo";
		return false;
	}

	char packageName[128];
	snprintf(packageName, sizeof(packageName), "%s-%s-any.hpkg", package,
		version);
	std::string packageFile = dir + "/" + packageName;
	if (packagePath != NULL)
		*packagePath = packageFile;
	if (run("package create -q -C " + content + " " + packageFile, output)
			!= 0) {
		error = "package create: " + output;
		return false;
	}

	// The repository's own info says priority 1 in both repositories.
	snprintf(text, sizeof(text),
		"name %s\n"
		"vendor \"%s\"\n"
		"summary \"pkg_priority test repository\"\n"
		"priority 1\n"
		"baseurl file://%s\n"
		"identifier tag:renga.invalid,2026:ce-test/%s\n"
		"architecture x86_64\n",
		name, vendor, dir.c_str(), name);
	if (!write_file(dir + "/repo.info", text)) {
		error = "cannot write repo.info";
		return false;
	}
	if (run("cd " + dir + " && package_repo create -q repo.info "
			+ packageFile, output) != 0) {
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


static const char* kRepositories[] = { "CETestPreferred", "CETestOther",
	"CETestHaikuPorts", "CETestRenga", "CETestElsewhere",
	"CETestHaikuPorts2" };


static void
clean_up()
{
	std::string output;
	std::string command = std::string("rm -rf ") + kWorkDir;
	for (size_t i = 0; i < sizeof(kRepositories) / sizeof(kRepositories[0]);
			i++) {
		command += std::string(" ") + kConfigDir + "/" + kRepositories[i]
			+ " " + kCacheDir + "/" + kRepositories[i];
	}
	run(command, output);
}


/*!	Vendor change on update (2026-11-14): a Renga build of a HaikuPorts
	package (vendor "Techniix", revision 100, in the repository with the
	better priority) must be offered by "pkgman update" in place of the
	installed HaikuPorts build (vendor "Haiku Project"). libsolv refuses a
	vendor change on update unless the vendors are in one class; before,
	"pkgman update libsdl3" said "Nothing to do" on the maintainer's
	machine. A package of another vendor still does not replace it.
*/
static void
test_vendor_change()
{
	std::string error, installed;
	bool made = make_repository("CETestHaikuPorts", 2, "cetestvnd", "1-1",
			"Haiku Project", error, &installed)
		&& make_repository("CETestRenga", 1, "cetestvnd", "1-100",
			"Techniix", error)
		&& make_repository("CETestElsewhere", 1, "cetestother", "1-100",
			"Somebody Else", error)
		&& make_repository("CETestHaikuPorts2", 2, "cetestother", "1-1",
			"Haiku Project", error, NULL);
	std::string other = std::string(kWorkDir)
		+ "/CETestHaikuPorts2/cetestother-1-1-any.hpkg";
	report("vendor.setup", made, "%s", made ? "repositories for the vendor "
		"change configured" : error.c_str());
	if (!made)
		return;

	// The HaikuPorts builds, installed from their files.
	std::string output;
	int status = run("pkgman install -y -R " + installed + " " + other,
		output);
	bool active = access("/boot/system/packages/cetestvnd-1-1-any.hpkg",
		F_OK) == 0;
	report("vendor.installed", status == 0 && active, "cetestvnd-1-1 "
		"(Haiku Project) installed from its file: exit %d %s", status,
		active ? "" : output.substr(0, 160).c_str());
	if (!active)
		return;

	status = run("echo no | pkgman update -R cetestvnd", output);
	// pkgman prints "upgrade package cetestvnd-1-1 to 1-100 from repository
	// CETestRenga"
	std::string plan = line_with(output, "upgrade package cetestvnd");
	report("vendor.renga_update_offered", plan.find("1-100")
			!= std::string::npos && plan.find("CETestRenga")
			!= std::string::npos,
		"pkgman update plans \"%s\" (want cetestvnd-1-100, vendor Techniix, "
		"from CETestRenga in place of 1-1 from Haiku Project; exit %d)",
		plan.empty() ? output.substr(0, 160).c_str() : plan.c_str(), status);

	status = run("echo no | pkgman update -R cetestother", output);
	plan = line_with(output, "upgrade package cetestother");
	report("vendor.other_vendor_kept", plan.empty(),
		"a newer cetestother from vendor \"Somebody Else\" is not offered "
		"for the Haiku Project build: %s (exit %d)",
		plan.empty() ? "not offered" : plan.c_str(), status);

	run("pkgman uninstall -y cetestvnd cetestother", output);
	bool gone = access("/boot/system/packages/cetestvnd-1-1-any.hpkg", F_OK)
		!= 0;
	report("vendor.cleaned_up", gone, "test packages uninstalled%s",
		gone ? "" : (": " + output.substr(0, 120)).c_str());
}


int
main()
{
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);

	clean_up();

	std::string error;
	bool made = make_repository("CETestPreferred", 1, "cetestpkg", "1-1",
			"Renga harness", error)
		&& make_repository("CETestOther", 2, "cetestpkg", "2-1",
			"Renga harness", error);
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
	test_vendor_change();
	clean_up();

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	fflush(stdout);
	return sFailures == 0 ? 0 : 1;
}
