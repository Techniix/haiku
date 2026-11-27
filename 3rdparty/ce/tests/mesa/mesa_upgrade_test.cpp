/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	In-guest test of the Mesa upgrade for the batch 14 image (roadmap
	decisions log, 2026-11-27, option C): the image keeps mesa 22.0.5, and
	users move to libglvnd 1.7.0-100 and mesa 26.3.0-101 from the Renga
	repository (Techniix/RengaPorts) in one pkgman transaction that also
	removes mesa_swpipe. It needs the network and an external repository, so
	it only runs in a "ce-test.py --mesa-upgrade" run (the flag file below);
	in every other run it prints one CE-INFO line and exits.

	A --mesa-upgrade run boots one disk twice, its writes kept:

	upgrade  waits for DNS, runs the transaction, checks the installed
	         versions and that mesa_swpipe is gone, then checks OpenGL in
	         fresh processes (below), GLInfo and GLTeapot
	after    the second boot of the upgraded system: the desktop is up
	         (Deskbar runs), the versions are still active, and the same
	         OpenGL checks again

	The transaction is "pkgman install libglvnd mesa mesa_devel mesa_eglvnd
	mesa_lavapipe". pkgman has no way to name a package to remove in an
	install, and "-y" fails on any solver problem; but the solver reports
	mesa_swpipe (which needs mesa 22.0.5) as a problem whose solutions
	include "allow deinstallation of mesa_swpipe-...". So the test answers
	pkgman as a user would: it runs it interactively with its answers on
	standard input, first ending each try with "q"/"no" (nothing is
	changed) to read the next problem and pick its solution, then once
	more with "yes" for the planned changes. A solution is only taken if
	each of its elements is the deinstallation of mesa_swpipe or a vendor
	change or replacement; anything else fails the check. The planned
	changes must remove no package but mesa_swpipe.

	The OpenGL check (a child process per setup, "--gl <setup>"): a BGLView
	(RGB, double-buffered, depth) in a window, GL_RENDERER, GL_VERSION and
	GL_SHADING_LANGUAGE_VERSION (llvmpipe, 4.x, 4.x), a frame cleared to a
	known colour with a red square in the middle, read back with
	glReadPixels before SwapBuffers. Two setups of the Vulkan loader:
	"no_icd" (VK_ICD_FILENAMES/VK_DRIVER_FILES name a file that does not
	exist, so no Vulkan driver is found) and "lavapipe" (only lavapipe's
	ICD file). Mesa 26.3.0-101 must pick llvmpipe in both: Zink only with a
	GPU Vulkan driver.

	Output format: see 3rdparty/ce/README.md.
*/


#include <errno.h>
#include <glob.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <map>
#include <string>
#include <vector>

#include <Application.h>
#include <Bitmap.h>
#include <Entry.h>
#include <GLView.h>
#include <Messenger.h>
#include <OS.h>
#include <Roster.h>
#include <Screen.h>
#include <Window.h>

#include <package/PackageInfo.h>
#include <package/PackageInfoSet.h>
#include <package/PackageRoster.h>

#include <GL/gl.h>


#define SUITE "mesa_upgrade"

static const char* kFlagFile
	= "/boot/system/non-packaged/data/ce-harness/mesa-upgrade";
static const char* kPhaseFile
	= "/boot/system/non-packaged/data/ce-harness/mesa-upgrade-phase";
static const char* kRepositoryHost = "techniix.github.io";
static const char* kPackages
	= "libglvnd mesa mesa_devel mesa_eglvnd mesa_lavapipe";
static const char* kRemoved = "mesa_swpipe";

struct Expected {
	const char* name;
	const char* version;
};

static const Expected kExpected[] = {
	{ "libglvnd", "1.7.0-100" },
	{ "mesa", "26.3.0-101" },
	{ "mesa_devel", "26.3.0-101" },
	{ "mesa_eglvnd", "26.3.0-101" },
	{ "mesa_lavapipe", "26.3.0-101" },
};

// the frame the GL check draws (0..255 per channel)
static const float kClear[3] = { 0.25f, 0.5f, 0.75f };
static const int kViewSize = 256;

static int sFailures = 0;
static std::string sPrefix;	// "<boot>." or "<boot>.<setup>."


static void
report(const char* name, bool pass, const char* format, ...)
{
	char detail[400];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);

	// One line per check, whatever the detail holds.
	for (char* c = detail; *c != '\0'; c++) {
		if (*c == '\n' || *c == '\r' || *c == '\t')
			*c = ' ';
	}
	printf("CE-TEST %s.%s%s %s %s\n", SUITE, sPrefix.c_str(), name,
		pass ? "PASS" : "FAIL", detail);
	fflush(stdout);
	if (!pass)
		sFailures++;
}


/*!	A CE-INFO line every 15 s of a wait: the harness takes 90 s without
	serial output for a hang and enters the kernel debugger (--stall).
*/
static void
heartbeat(const char* what, bigtime_t start, bigtime_t& last)
{
	bigtime_t now = system_time();
	if (now - last < 15000000)
		return;
	last = now;
	printf("CE-INFO mesa_upgrade_test: %s, %.0f s\n", what,
		(now - start) / 1e6);
	fflush(stdout);
}


static std::string
read_text(const char* path)
{
	std::string text;
	FILE* file = fopen(path, "r");
	if (file == NULL)
		return text;
	char buffer[4096];
	size_t length;
	while ((length = fread(buffer, 1, sizeof(buffer), file)) > 0)
		text.append(buffer, length);
	fclose(file);
	return text;
}


static void
write_text(const char* path, const std::string& text)
{
	FILE* file = fopen(path, "w");
	if (file == NULL)
		return;
	fwrite(text.data(), 1, text.size(), file);
	fclose(file);
	sync();
}


static std::string
trimmed(std::string text)
{
	while (!text.empty() && strchr(" \n\r\t", text.back()) != NULL)
		text.pop_back();
	size_t start = 0;
	while (start < text.size() && strchr(" \n\r\t", text[start]) != NULL)
		start++;
	return text.substr(start);
}


/*!	Runs \a command with sh, stdout and stderr to \a outputPath, stdin from
	\a inputPath (or /dev/null). Kills it after \a timeout seconds. Returns
	its exit status, 128 + the signal, or -1 when it was killed for the
	timeout.
*/
static int
run_command(const std::string& command, const char* inputPath,
	const char* outputPath, int timeout)
{
	pid_t child = fork();
	if (child < 0)
		return -2;
	if (child == 0) {
		setpgid(0, 0);
		freopen(inputPath != NULL ? inputPath : "/dev/null", "r", stdin);
		freopen(outputPath, "w", stdout);
		dup2(fileno(stdout), fileno(stderr));
		execl("/bin/sh", "sh", "-c", command.c_str(), (char*)NULL);
		_exit(127);
	}
	bigtime_t start = system_time();
	bigtime_t last = start;
	bigtime_t deadline = start + (bigtime_t)timeout * 1000000;
	std::string what = "running \"" + command.substr(0, 60) + "\"";
	int status = 0;
	while (true) {
		pid_t done = waitpid(child, &status, WNOHANG);
		if (done == child)
			break;
		heartbeat(what.c_str(), start, last);
		if (system_time() > deadline) {
			kill(-child, SIGKILL);
			kill(child, SIGKILL);
			waitpid(child, &status, 0);
			return -1;
		}
		snooze(200000);
	}
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return -3;
}


/*!	Copies \a text to the serial log as CE-INFO lines (at most \a maxLines).
*/
static void
log_lines(const char* label, const std::string& text, int maxLines)
{
	size_t start = 0;
	int count = 0;
	while (start < text.size() && count < maxLines) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(start, end - start);
		if (!trimmed(line).empty()) {
			printf("CE-INFO %s: %s\n", label, line.c_str());
			count++;
		}
		start = end + 1;
	}
	if (start < text.size())
		printf("CE-INFO %s: ... (%d lines shown)\n", label, maxLines);
	fflush(stdout);
}


// #pragma mark - the transaction


/*!	Whether a solution element may be taken: the deinstallation of
	mesa_swpipe, or a vendor change or a replacement (HaikuPorts' packages
	by the Renga repository's).
*/
static bool
acceptable_element(const std::string& element)
{
	if (element.find("allow deinstallation of ") == 0)
		return element.find(std::string(kRemoved) + "-") != std::string::npos;
	return element.find("allow vendor change") == 0
		|| element.find("allow replacement of ") == 0;
}


/*!	Picks the solution of the problem in \a block (from "problem N:" up to
	its prompt): the first one all of whose elements are acceptable,
	preferring one that removes mesa_swpipe. Returns its number, or 0.
*/
static int
pick_solution(const std::string& block, std::string& _choice)
{
	int best = 0;
	bool bestRemoves = false;
	size_t position = 0;
	while (true) {
		size_t solution = block.find("  solution ", position);
		if (solution == std::string::npos)
			break;
		int number = atoi(block.c_str() + solution + 11);
		size_t next = block.find("  solution ", solution + 11);
		std::string body = block.substr(solution,
			next == std::string::npos ? std::string::npos : next - solution);
		bool ok = true;
		bool removes = false;
		int elements = 0;
		size_t line = 0;
		while ((line = body.find("    - ", line)) != std::string::npos) {
			size_t end = body.find('\n', line);
			std::string element = body.substr(line + 6,
				end == std::string::npos ? std::string::npos : end - line - 6);
			element = trimmed(element);
			elements++;
			if (!acceptable_element(element))
				ok = false;
			if (element.find("allow deinstallation of ") == 0)
				removes = true;
			line += 6;
		}
		if (ok && elements > 0 && number > 0
			&& (best == 0 || (removes && !bestRemoves))) {
			best = number;
			bestRemoves = removes;
			_choice = trimmed(body);
		}
		if (next == std::string::npos)
			break;
		position = next;
	}
	return best;
}


static std::vector<size_t>
find_all(const std::string& text, const char* what)
{
	std::vector<size_t> positions;
	size_t position = 0;
	while ((position = text.find(what, position)) != std::string::npos) {
		positions.push_back(position);
		position += strlen(what);
	}
	return positions;
}


static bool
run_transaction()
{
	const char* answersPath = "/tmp/ce-mesa-upgrade.answers";
	const char* outputPath = "/tmp/ce-mesa-upgrade.out";
	std::string command = std::string("pkgman install ") + kPackages;
	std::string answers;
	std::string output;
	bool planned = false;

	for (int round = 0; round < 8; round++) {
		// "q" ends pkgman at a problem; at the confirmation it is not an
		// answer and "no" follows. Nothing changes either way.
		write_text(answersPath, answers + "q\nno\n");
		int status = run_command(command, answersPath, outputPath, 900);
		output = read_text(outputPath);
		std::vector<size_t> prompts
			= find_all(output, "Please select a solution");
		size_t answered = 0;
		for (size_t i = 0; i < answers.size(); i++) {
			if (answers[i] == '\n')
				answered++;
		}
		if (prompts.size() > answered) {
			size_t prompt = prompts[answered];
			size_t problem = output.rfind("problem ", prompt);
			if (problem == std::string::npos) {
				report("transaction", false, "a prompt without a problem");
				log_lines("pkgman", output, 120);
				return false;
			}
			std::string block = output.substr(problem, prompt - problem);
			std::string choice;
			int number = pick_solution(block, choice);
			log_lines("pkgman problem", block, 30);
			if (number == 0) {
				report("transaction", false, "no acceptable solution for: %s",
					trimmed(block.substr(0, block.find('\n'))).c_str());
				return false;
			}
			printf("CE-INFO picked: %s\n", choice.c_str());
			answers += std::to_string(number) + "\n";
			continue;
		}
		if (output.find("Continue? [yes/no]") != std::string::npos) {
			planned = true;
			break;
		}
		report("transaction", false, "pkgman exited with %d before a plan",
			status);
		log_lines("pkgman", output, 120);
		return false;
	}
	if (!planned) {
		report("transaction", false, "still solver problems after 8 rounds");
		log_lines("pkgman", output, 120);
		return false;
	}

	// the planned changes: only mesa_swpipe may go
	size_t planStart = output.find("The following changes will be made:");
	std::string plan = output.substr(planStart == std::string::npos
		? 0 : planStart);
	plan = plan.substr(0, plan.find("Continue? [yes/no]"));
	log_lines("plan", plan, 80);
	std::string unexpected;
	bool removesSwpipe = false;
	for (size_t position : find_all(plan, "uninstall package ")) {
		size_t end = plan.find('\n', position);
		std::string name = trimmed(plan.substr(position + 18,
			end == std::string::npos ? std::string::npos : end - position
				- 18));
		if (name.find(std::string(kRemoved) + "-") == 0)
			removesSwpipe = true;
		else
			unexpected += name + " ";
	}
	report("plan", unexpected.empty() && removesSwpipe,
		"%s%s", removesSwpipe ? "removes mesa_swpipe" : "keeps mesa_swpipe",
		unexpected.empty() ? ", nothing else"
			: (", also removes " + unexpected).c_str());
	if (!unexpected.empty())
		return false;

	bigtime_t start = system_time();
	write_text(answersPath, answers + "yes\n");
	int status = run_command(command, answersPath, outputPath, 1800);
	output = read_text(outputPath);
	size_t tail = output.size() > 3000 ? output.size() - 3000 : 0;
	log_lines("pkgman", output.substr(tail), 60);
	report("transaction", status == 0,
		"pkgman install %s, %d answer(s), exit %d, %.0f s", kPackages,
		(int)find_all(answers, "\n").size(), status,
		(system_time() - start) / 1e6);
	return status == 0;
}


// #pragma mark - installed packages


static std::map<std::string, std::string>
active_packages()
{
	std::map<std::string, std::string> packages;
	BPackageKit::BPackageInfoSet infos;
	BPackageKit::BPackageRoster roster;
	if (roster.GetActivePackages(
			BPackageKit::B_PACKAGE_INSTALLATION_LOCATION_SYSTEM, infos)
				!= B_OK)
		return packages;
	BPackageKit::BPackageInfoSet::Iterator it = infos.GetIterator();
	while (const BPackageKit::BPackageInfo* info = it.Next())
		packages[info->Name().String()] = info->Version().ToString().String();
	return packages;
}


static void
check_versions()
{
	std::map<std::string, std::string> packages = active_packages();
	std::string wrong;
	std::string found;
	for (const Expected& expected : kExpected) {
		std::map<std::string, std::string>::iterator it
			= packages.find(expected.name);
		std::string version = it == packages.end() ? "none" : it->second;
		found += std::string(expected.name) + "-" + version + " ";
		if (version != expected.version)
			wrong += std::string(expected.name) + " ";
	}
	report("versions", wrong.empty(), "%s%s", trimmed(found).c_str(),
		wrong.empty() ? "" : (" (wrong: " + trimmed(wrong) + ")").c_str());
	std::map<std::string, std::string>::iterator swpipe
		= packages.find(kRemoved);
	report("swpipe_removed", swpipe == packages.end(), "%s",
		swpipe == packages.end() ? "mesa_swpipe not active"
			: ("mesa_swpipe-" + swpipe->second + " still active").c_str());
	std::map<std::string, std::string>::iterator devel
		= packages.find("libglvnd_devel");
	printf("CE-INFO libglvnd_devel: %s\n", devel == packages.end()
		? "not installed" : devel->second.c_str());
}


static std::string
lavapipe_icd()
{
	// mesa_lavapipe puts it next to its driver (the recipe's
	// -Dvulkan-icd-dir=$addOnsDir/vulkan/icd.d)
	const char* patterns[] = {
		"/boot/system/add-ons/vulkan/icd.d/*lvp*.json",
		"/boot/system/data/vulkan/icd.d/*lvp*.json",
		"/boot/system/non-packaged/add-ons/vulkan/icd.d/*lvp*.json",
	};
	for (const char* pattern : patterns) {
		glob_t matches;
		if (glob(pattern, 0, NULL, &matches) == 0 && matches.gl_pathc > 0) {
			std::string path = matches.gl_pathv[0];
			globfree(&matches);
			return path;
		}
		globfree(&matches);
	}
	return "";
}


// #pragma mark - OpenGL in a child process


static bool
near(uint8 value, float wanted)
{
	int expected = (int)(wanted * 255.0f + 0.5f);
	return abs((int)value - expected) <= 3;
}


static int
gl_child(const char* setup)
{
	BApplication app("application/x-vnd.Renga-mesa-upgrade-test");
	BWindow* window = new BWindow(BRect(120, 120, 120 + kViewSize - 1,
		120 + kViewSize - 1), "mesa_upgrade_test", B_TITLED_WINDOW,
		B_NOT_RESIZABLE | B_NOT_ZOOMABLE);
	BGLView* view = new BGLView(window->Bounds(), "gl", B_FOLLOW_ALL_SIDES, 0,
		BGL_RGB | BGL_DOUBLE | BGL_DEPTH);
	window->AddChild(view);
	window->Show();
	snooze(1000000);

	view->LockGL();
	const char* renderer = (const char*)glGetString(GL_RENDERER);
	const char* version = (const char*)glGetString(GL_VERSION);
	const char* shading
		= (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
	std::string rendererText = renderer != NULL ? renderer : "(none)";
	std::string versionText = version != NULL ? version : "(none)";
	std::string shadingText = shading != NULL ? shading : "(none)";

	uint8 center[4] = { 0, 0, 0, 0 };
	uint8 corner[4] = { 0, 0, 0, 0 };
	GLenum error = GL_NO_ERROR;
	if (renderer != NULL) {
		glViewport(0, 0, kViewSize, kViewSize);
		glDisable(GL_DEPTH_TEST);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		glClearColor(kClear[0], kClear[1], kClear[2], 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glColor3f(1.0f, 0.0f, 0.0f);
		glBegin(GL_QUADS);
		glVertex2f(-0.5f, -0.5f);
		glVertex2f(0.5f, -0.5f);
		glVertex2f(0.5f, 0.5f);
		glVertex2f(-0.5f, 0.5f);
		glEnd();
		glFinish();
		glReadBuffer(GL_BACK);
		glPixelStorei(GL_PACK_ALIGNMENT, 1);
		glReadPixels(kViewSize / 2, kViewSize / 2, 1, 1, GL_RGBA,
			GL_UNSIGNED_BYTE, center);
		glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
		error = glGetError();
		view->SwapBuffers();
	}
	view->UnlockGL();

	report("renderer", rendererText.find("llvmpipe") != std::string::npos,
		"%s", rendererText.c_str());
	report("version", versionText.compare(0, 2, "4.") == 0
		|| versionText.find(" 4.") != std::string::npos, "%s",
		versionText.c_str());
	report("glsl_version", shadingText.compare(0, 2, "4.") == 0, "%s",
		shadingText.c_str());
	report("pixels", error == GL_NO_ERROR
		&& center[0] >= 250 && center[1] <= 5 && center[2] <= 5
		&& near(corner[0], kClear[0]) && near(corner[1], kClear[1])
		&& near(corner[2], kClear[2]),
		"centre %u,%u,%u (want 255,0,0), corner %u,%u,%u (want 64,128,191), "
		"GL error 0x%x", center[0], center[1], center[2], corner[0],
		corner[1], corner[2], error);

	snooze(500000);
	if (window->Lock())
		window->Quit();
	return sFailures;
}


static void
check_gl(const char* program, const char* setup, const std::string& icd)
{
	pid_t child = fork();
	if (child == 0) {
		setenv("VK_ICD_FILENAMES", icd.c_str(), 1);
		setenv("VK_DRIVER_FILES", icd.c_str(), 1);
		execl(program, program, "--gl", setup, sPrefix.c_str(), (char*)NULL);
		_exit(127);
	}
	bigtime_t start = system_time();
	bigtime_t last = start;
	bigtime_t deadline = start + 120000000LL;
	int status = 0;
	bool exited = false;
	while (child > 0 && system_time() < deadline) {
		heartbeat("waiting for the OpenGL child", start, last);
		if (waitpid(child, &status, WNOHANG) == child) {
			exited = true;
			break;
		}
		snooze(200000);
	}
	if (!exited && child > 0) {
		kill(child, SIGKILL);
		waitpid(child, &status, 0);
	}
	std::string prefix = sPrefix;
	sPrefix += std::string(setup) + ".";
	report("process", exited && WIFEXITED(status) && WEXITSTATUS(status) == 0,
		"Vulkan ICD files \"%s\": %s", icd.c_str(), !exited
			? "no exit after 120 s (crashed or hung), killed"
			: WIFSIGNALED(status) ? ("killed by signal "
				+ std::to_string(WTERMSIG(status))).c_str()
			: ("exit " + std::to_string(WEXITSTATUS(status))).c_str());
	sPrefix = prefix;
}


// #pragma mark - GLInfo and GLTeapot


static std::string
find_app(const char* name)
{
	const char* dirs[] = { "/boot/system/apps/", "/boot/system/demos/" };
	for (const char* dir : dirs) {
		std::string path = std::string(dir) + name;
		if (access(path.c_str(), X_OK) == 0)
			return path;
	}
	return "";
}


static bool
team_alive(team_id team)
{
	team_info info;
	return get_team_info(team, &info) == B_OK;
}


/*!	Launches \a name, lets it run for \a seconds, checks it is still there
	(and, with \a drawn, that its window shows more than one colour), then
	asks it to quit.
*/
static void
check_app(const char* name, int seconds, bool drawn)
{
	std::string check = name;
	for (char& c : check)
		c = tolower(c);
	std::string path = find_app(name);
	if (path.empty()) {
		report(check.c_str(), false, "%s not installed", name);
		return;
	}
	entry_ref ref;
	get_ref_for_path(path.c_str(), &ref);
	team_id team = -1;
	status_t status = be_roster->Launch(&ref, (BMessage*)NULL, &team);
	if (status != B_OK) {
		report(check.c_str(), false, "launch: %s", strerror(status));
		return;
	}
	snooze((bigtime_t)seconds * 1000000);
	bool alive = team_alive(team);

	std::string drawing;
	bool drawOk = true;
	if (alive && drawn) {
		BMessenger app(NULL, team);
		BMessage get(B_GET_PROPERTY);
		get.AddSpecifier("Frame");
		get.AddSpecifier("Window", (int32)0);
		BMessage reply;
		BRect frame;
		drawOk = false;
		if (app.SendMessage(&get, &reply, 5000000, 5000000) == B_OK
			&& reply.FindRect("result", &frame) == B_OK && frame.IsValid()) {
			// the whole screen, then the window's part of it
			BScreen screen;
			BBitmap* bitmap = NULL;
			status_t captured = screen.GetBitmap(&bitmap, false);
			BRect area = frame & screen.Frame();
			if (captured == B_OK && bitmap != NULL && area.IsValid()
				&& bitmap->ColorSpace() == B_RGB32) {
				// how much of the window is not its most common colour
				std::map<uint32, int> colours;
				for (int32 y = (int32)area.top; y <= (int32)area.bottom; y++) {
					const uint32* row = (const uint32*)((uint8*)bitmap->Bits()
						+ y * bitmap->BytesPerRow());
					for (int32 x = (int32)area.left; x <= (int32)area.right;
							x++)
						colours[row[x] & 0xffffff]++;
				}
				int most = 0;
				for (const auto& entry : colours)
					most = std::max(most, entry.second);
				int32 width = area.IntegerWidth() + 1;
				int32 height = area.IntegerHeight() + 1;
				int total = width * height;
				double other = total > 0 ? 1.0 - (double)most / total : 0;
				drawOk = other >= 0.02 && colours.size() >= 16;
				char text[160];
				snprintf(text, sizeof(text), ", window %dx%d: %zu colours, "
					"%.1f%% not the background", (int)width, (int)height,
					colours.size(), other * 100);
				drawing = text;
			} else {
				char text[200];
				snprintf(text, sizeof(text), ", no screen capture (%s, "
					"colour space 0x%x, window %.0f,%.0f-%.0f,%.0f)",
					strerror(captured), bitmap != NULL
						? (unsigned)bitmap->ColorSpace() : 0u, frame.left,
					frame.top, frame.right, frame.bottom);
				drawing = text;
			}
			delete bitmap;
		} else
			drawing = ", no window frame by scripting";
	}
	report(check.c_str(), alive && drawOk, "%s %s after %d s%s", name,
		alive ? "running" : "gone (crashed?)", seconds, drawing.c_str());

	if (team_alive(team)) {
		BMessenger(NULL, team).SendMessage(B_QUIT_REQUESTED);
		for (int i = 0; i < 25 && team_alive(team); i++)
			snooze(200000);
		if (team_alive(team))
			kill_team(team);
	}
}


// #pragma mark - the boots


static bool
team_running(const char* start, const char* part = NULL)
{
	int32 cookie = 0;
	team_info info;
	while (get_next_team_info(&cookie, &info) == B_OK) {
		if (strncmp(info.args, start, strlen(start)) == 0
			|| (part != NULL && strstr(info.args, part) != NULL))
			return true;
	}
	return false;
}


/*!	The desktop as bootstate_test sees it: Deskbar, or the renga-* images'
	first-login phase (FirstBootPrompt), which has no Deskbar.
*/
static bool
deskbar_running()
{
	return team_running("/boot/system/Deskbar", "FirstBootPrompt");
}


/*!	The second boot's desktop check: app_server and the session are up,
	and Deskbar runs (started here in the first-login phase, as
	deskbar_test does, and left running).
*/
static void
check_desktop()
{
	bool appServer = team_running("/boot/system/servers/app_server");
	bool session = deskbar_running();
	bool firstLogin = !team_running("/boot/system/Deskbar")
		&& team_running("", "FirstBootPrompt");
	if (session && !team_running("/boot/system/Deskbar"))
		be_roster->Launch("application/x-vnd.Be-TSKB");
	for (int i = 0; i < 60 && !team_running("/boot/system/Deskbar"); i++)
		snooze(500000);
	bool deskbar = team_running("/boot/system/Deskbar");
	report("desktop", appServer && session && deskbar,
		"app_server %s, %s, Deskbar %s", appServer ? "runs" : "missing",
		firstLogin ? "first-login phase" : session ? "session up"
			: "no session", deskbar ? "runs" : "does not start");
}


static void
gl_checks(const char* program)
{
	std::string icd = lavapipe_icd();
	report("lavapipe_icd", !icd.empty(), "%s",
		icd.empty() ? "no lavapipe ICD file" : icd.c_str());
	check_gl(program, "no_icd", "/boot/system/non-packaged/data/ce-harness/"
		"no-such-icd.json");
	if (!icd.empty())
		check_gl(program, "lavapipe", icd);
	check_app("GLInfo", 5, false);
	check_app("GLTeapot", 10, true);
}


static bool
wait_for_network()
{
	bigtime_t start = system_time();
	bigtime_t last = start;
	while (system_time() - start < 180000000LL) {
		heartbeat("waiting for DNS", start, last);
		struct addrinfo hints = {};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		struct addrinfo* result = NULL;
		if (getaddrinfo(kRepositoryHost, "443", &hints, &result) == 0) {
			freeaddrinfo(result);
			report("network", true, "%s resolved after %.0f s",
				kRepositoryHost, (system_time() - start) / 1e6);
			return true;
		}
		snooze(2000000);
	}
	report("network", false, "%s not resolved after 180 s (does the run "
		"have the user-mode network?)", kRepositoryHost);
	run_command("ifconfig", NULL, "/tmp/ce-mesa-upgrade.ifconfig", 20);
	log_lines("ifconfig", read_text("/tmp/ce-mesa-upgrade.ifconfig"), 40);
	return false;
}


int
main(int argc, char** argv)
{
	if (argc >= 3 && strcmp(argv[1], "--gl") == 0) {
		sPrefix = std::string(argc >= 4 ? argv[3] : "") + argv[2] + ".";
		return gl_child(argv[2]);
	}

	struct stat st;
	if (stat(kFlagFile, &st) != 0) {
		printf("CE-INFO mesa_upgrade_test: not a --mesa-upgrade run\n");
		return 0;
	}

	// for BScreen (the GLTeapot capture); the children exec at once
	BApplication app("application/x-vnd.Renga-mesa-upgrade-check");
	std::string phase = trimmed(read_text(kPhaseFile));
	printf("CE-SUITE %s START\n", SUITE);
	fflush(stdout);
	// GUI checks need the desktop (the second phase of a renga-* image)
	bigtime_t start = system_time();
	bigtime_t last = start;
	while (system_time() - start < 120000000LL && !deskbar_running()) {
		heartbeat("waiting for the desktop session", start, last);
		snooze(1000000);
	}
	printf("CE-INFO mesa_upgrade_test: desktop session %s after %.0f s\n",
		deskbar_running() ? "up" : "not up", (system_time() - start) / 1e6);

	if (phase.empty()) {
		sPrefix = "upgrade.";
		std::map<std::string, std::string> before = active_packages();
		printf("CE-INFO before: mesa %s, mesa_swpipe %s, libglvnd %s\n",
			before.count("mesa") ? before["mesa"].c_str() : "none",
			before.count(kRemoved) ? before[kRemoved].c_str() : "none",
			before.count("libglvnd") ? before["libglvnd"].c_str() : "none");
		if (wait_for_network() && run_transaction()) {
			check_versions();
			gl_checks(argv[0]);
		}
		// the second boot checks the upgraded system as it starts
		write_text(kPhaseFile, "installed\n");
	} else if (phase == "installed") {
		sPrefix = "after.";
		check_desktop();
		check_versions();
		gl_checks(argv[0]);
		write_text(kPhaseFile, "done\n");
	} else
		printf("CE-INFO mesa_upgrade_test: phase \"%s\", nothing to do\n",
			phase.c_str());

	printf("CE-SUITE %s DONE failures=%d\n", SUITE, sFailures);
	return sFailures == 0 ? 0 : 1;
}
