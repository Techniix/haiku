/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Renga's development files on Renga images (roadmap: Renga development
	packaging; Quasar R-17): haiku_devel is installed and active and
	provides renga_devel, the system package provides renga, both with the
	system's version, and both have the vendor "Techniix" (the Renga
	repository's); Renga's own headers are installed (epoll, inotify,
	eventfd, the capture and virtual camera headers, GpuPresent.h,
	HardwareCursor.h); and the compiler on the image builds and links
	small programs: one with epoll (gnu headers, -lgnu), one with eventfd
	and the two graphics headers from the default include path only,
	linked without -lgnu (eventfd is in libroot), which then runs.

	Minimum images have no development files active; they only report
	that.
*/


#include <dirent.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <String.h>


#define SUITE "devel"

static int sFailures = 0;
static const char* kPackages = "/boot/system/packages";
static const char* kHeaders = "/boot/system/develop/headers";
// By its path: started as plain "gcc" from the harness runner's
// environment, gcc does not find cc1 (its search for itself along PATH
// fails there); from a full path it does. Whether a Terminal's "gcc" works
// is on the maintainer's hardware list.
#define kGcc "/boot/system/bin/gcc"


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[512];
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
run(const char* command, int* status = NULL)
{
	BString output;
	// a login shell's PATH: the harness runner's minimal one (/bin first)
	// makes gcc look for cc1 next to /bin/gcc, a symlink it cannot resolve
	// back to develop/tools
	BString full("PATH=/boot/home/config/non-packaged/bin:/boot/system/"
		"non-packaged/bin:/boot/home/config/bin:/boot/system/bin:$PATH; ");
	full << command << " 2>&1";
	FILE* pipe = popen(full.String(), "r");
	if (pipe == NULL)
		return output;
	char buffer[512];
	while (fgets(buffer, sizeof(buffer), pipe) != NULL)
		output << buffer;
	int result = pclose(pipe);
	if (status != NULL)
		*status = result;
	output.Trim();
	return output;
}


static BString
find_package(const char* name)
{
	BString found;
	DIR* dir = opendir(kPackages);
	if (dir == NULL)
		return found;
	size_t length = strlen(name);
	while (dirent* entry = readdir(dir)) {
		if (strncmp(entry->d_name, name, length) == 0
			&& entry->d_name[length] == '-') {
			found.SetToFormat("%s/%s", kPackages, entry->d_name);
			break;
		}
	}
	closedir(dir);
	return found;
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


int
main()
{
	printf("CE-SUITE " SUITE " START\n");
	fflush(stdout);

	BString devel = find_package("haiku_devel");
	BString system = find_package("haiku");
	if (devel.IsEmpty() && access("/boot/system/bin/gcc", X_OK) != 0) {
		printf("CE-INFO devel: no development files active in this image\n");
		printf("CE-SUITE " SUITE " DONE failures=0\n");
		return 0;
	}

	// installed, active, and providing renga_devel / renga
	BString develInfo = run(BString("package list -i ") << devel);
	BString systemInfo = run(BString("package list -i ") << system);
	bool develProvides = develInfo.FindFirst("renga_devel") >= 0;
	bool systemProvides = systemInfo.FindFirst("provides: renga ") >= 0
		|| systemInfo.FindFirst("renga =") >= 0
		|| systemInfo.FindFirst("renga=") >= 0
		|| systemInfo.FindFirst("\trenga") >= 0;
	report("haiku_devel_installed", !devel.IsEmpty() && develProvides,
		"%s, provides renga_devel: %s", devel.IsEmpty() ? "missing"
			: devel.String(), develProvides ? "yes" : "no");
	report("system_provides_renga", systemProvides, "%s",
		systemProvides ? "yes" : "no");

	// Renga's own system packages carry the Renga repository's vendor,
	// "Techniix" (maintainer decision 2026-11-13), or package_repo refuses
	// them for that repository
	BString vendor;
	int32 vendorAt = systemInfo.FindFirst("\tvendor: ");
	if (vendorAt >= 0) {
		systemInfo.CopyInto(vendor, vendorAt + 9,
			systemInfo.Length() - vendorAt - 9);
		int32 end = vendor.FindFirst('\n');
		if (end >= 0)
			vendor.Truncate(end);
	}
	BString develVendor;
	vendorAt = develInfo.FindFirst("\tvendor: ");
	if (vendorAt >= 0) {
		develInfo.CopyInto(develVendor, vendorAt + 9,
			develInfo.Length() - vendorAt - 9);
		int32 end = develVendor.FindFirst('\n');
		if (end >= 0)
			develVendor.Truncate(end);
	}
	report("system_vendor", vendor == "Techniix" && develVendor == "Techniix",
		"haiku: \"%s\", haiku_devel: \"%s\" (want \"Techniix\")",
		vendor.String(), develVendor.String());

	int status = 0;

	// the package daemon resolves both provides: a package that requires
	// them activates when dropped into the user's packages folder (made
	// here with "package create", removed again). Not "pkgman install",
	// which refreshes the repositories over the network first.
	mkdir("/tmp/devel_pkg", 0755);
	mkdir("/tmp/devel_pkg/data", 0755);
	mkdir("/tmp/devel_pkg/data/renga_devel_check", 0755);
	write_file("/tmp/devel_pkg/data/renga_devel_check/ok", "ok\n");
	write_file("/tmp/devel_pkg/.PackageInfo",
		"name			renga_devel_check\n"
		"version			1-1\n"
		"architecture	any\n"
		"summary			\"Harness check: requires renga_devel\"\n"
		"description		\"Renga harness check package.\"\n"
		"packager		\"Renga harness\"\n"
		"vendor			\"Renga\"\n"
		"copyrights		\"2026 Renga contributors\"\n"
		"licenses		\"MIT\"\n"
		"provides {\n	renga_devel_check = 1\n}\n"
		"requires {\n	renga\n	renga_devel\n}\n");
	const char* package = "/tmp/renga_devel_check-1-1-any.hpkg";
	const char* dropped
		= "/boot/home/config/packages/renga_devel_check-1-1-any.hpkg";
	const char* marker = "/boot/home/config/data/renga_devel_check/ok";
	unlink(package);
	BString created = run(BString("package create -C /tmp/devel_pkg ")
		<< package, &status);
	int createStatus = status;
	run(BString("cp ") << package << " " << dropped);
	bool active = false;
	// the package daemon may still be doing the image's first-boot
	// processing (100-odd packages) when this runs: wait up to 3 minutes
	for (int i = 0; i < 720 && !active; i++) {
		usleep(250000);
		if (i % 80 == 79) {
			// keep the harness' stall detection quiet
			printf("CE-INFO devel: waiting for the package daemon (%d s)\n",
				(i + 1) / 4);
			fflush(stdout);
		}
		active = access(marker, R_OK) == 0;
	}
	if (!active) {
		BString log = run("tail -n 40 /boot/system/var/log/syslog "
			"| grep -i 'package\\|daemon\\|renga_devel'");
		printf("CE-INFO devel: package daemon log:\n%s\n", log.String());
		printf("CE-INFO devel: %s\n",
			run(BString("package list ") << package).String());
	}
	unlink(dropped);
	report("renga_devel_resolves", createStatus == 0 && active,
		"package create %d (%.120s); activation with renga and renga_devel "
		"required: %s", createStatus, created.String(),
		active ? "done" : "not within 3 minutes");

	// Renga's headers
	const char* headers[] = {
		"gnu/sys/epoll.h", "gnu/sys/inotify.h", "posix/sys/eventfd.h",
		"gnu/linux/input.h", "os/device/Gamepad.h",
		"os/add-ons/graphics/GpuPresent.h",
		"os/add-ons/graphics/HardwareCursor.h",
		"private/interface/ScreenCapture.h",
		"private/media/CaptureSession.h", "private/media/VirtualCamera.h",
		NULL
	};
	BString missing;
	for (int i = 0; headers[i] != NULL; i++) {
		BString path;
		path.SetToFormat("%s/%s", kHeaders, headers[i]);
		if (access(path.String(), R_OK) != 0)
			missing << " " << headers[i];
	}
	report("renga_headers", missing.IsEmpty(), "missing:%s",
		missing.IsEmpty() ? " none" : missing.String());

	// epoll with the gnu headers and libgnu
	write_file("/tmp/devel_epoll.c",
		"#include <sys/epoll.h>\n"
		"#include <stdio.h>\n"
		"int main(void) {\n"
		"	int fd = epoll_create1(EPOLL_CLOEXEC);\n"
		"	printf(\"epoll %d\\n\", fd >= 0);\n"
		"	return fd >= 0 ? 0 : 1;\n"
		"}\n");
	BString epollBuild = run(kGcc " -o /tmp/devel_epoll /tmp/devel_epoll.c "
		"-I/boot/system/develop/headers/gnu -lgnu", &status);
	int built = status;
	BString epollRun = run("/tmp/devel_epoll", &status);
	report("epoll_compiles", built == 0 && status == 0
		&& epollRun == "epoll 1", "build: %.150s; run: %s",
		built == 0 ? "ok" : epollBuild.String(), epollRun.String());

	// R-17: eventfd and the graphics headers from the default path, no
	// -lgnu
	write_file("/tmp/devel_r17.c",
		"#include <sys/eventfd.h>\n"
		"#include <add-ons/graphics/GpuPresent.h>\n"
		"#include <add-ons/graphics/HardwareCursor.h>\n"
		"#include <stdio.h>\n"
		"#include <unistd.h>\n"
		"int main(void) {\n"
		"	gpu_flip_params params;\n"
		"	params.acquire.buffer = -1;\n"
		"	int fd = eventfd(0, EFD_CLOEXEC);\n"
		"	eventfd_t value = 0;\n"
		"	eventfd_write(fd, 3);\n"
		"	eventfd_read(fd, &value);\n"
		"	printf(\"eventfd %d %d\\n\", (int)value,\n"
		"		B_GPU_PRESENT_SEMAPHORES);\n"
		"	return value == 3 ? 0 : 1;\n"
		"}\n");
	BString r17Build = run(kGcc " -o /tmp/devel_r17 /tmp/devel_r17.c",
		&status);
	built = status;
	BString r17Run = run("/tmp/devel_r17", &status);
	report("default_path_headers", built == 0 && status == 0
		&& r17Run == "eventfd 3 16", "build: %.200s; run: %s",
		built == 0 ? "ok" : r17Build.String(), r17Run.String());

	// Renga's ffmpeg6 (RengaPorts, --enable-vulkan, for Quasar): only once
	// it is installed; HaikuPorts' build has no Vulkan
	BString ffmpegPackage = find_package("ffmpeg6");
	BString ffmpegInfo = ffmpegPackage.IsEmpty() ? BString()
		: run(BString("package list -i ") << ffmpegPackage);
	if (ffmpegInfo.FindFirst("requires: renga") < 0
		|| access("/boot/system/bin/ffmpeg", X_OK) != 0) {
		printf("CE-INFO devel: Renga's ffmpeg6 with Vulkan is not installed "
			"in this image (%s); ffmpeg_vulkan not checked\n",
			ffmpegPackage.IsEmpty() ? "no ffmpeg6" : ffmpegPackage.String());
	} else {
		BString hwaccels = run("/boot/system/bin/ffmpeg -hide_banner "
			"-hwaccels");
		report("ffmpeg_vulkan", hwaccels.FindFirst("vulkan") >= 0,
			"ffmpeg -hwaccels: %.200s", hwaccels.String());
	}

	printf("CE-SUITE " SUITE " DONE failures=%d\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
