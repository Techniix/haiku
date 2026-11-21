/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	linux: a shell, or one command, inside Renguin's Linux root
	(/boot/system/non-packaged/compat/linux), with a clean Linux environment.

	Started from a Haiku shell, a Linux program used to inherit Haiku's
	environment: PATH found Haiku's own tools first (Haiku's wget instead of
	Alpine's), and ip, apk or nslookup were "not found". This command starts
	the Linux root's login shell (the user's shell from the root's
	/etc/passwd, otherwise /bin/sh) or runs one command, with:
	  PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin,
	  HOME, TERM, USER and LOGNAME from Haiku (defaults when unset),
	  LANG, LANGUAGE and the formatting LC_* from Haiku's Locale settings
	  in the form Linux C libraries know (see linux_locale_environment()),
	  XDG_RUNTIME_DIR=/tmp/xdg-runtime and WAYLAND_DISPLAY=wayland-0 (Renga's
	  Wayland compositor), DBUS_SESSION_BUS_ADDRESS (Renga's session bus),
	  SHELL.
	The interactive shell shows that it is Linux: the Terminal window is
	titled "Linux (Renguin)", a line says so at start-up, and the prompt
	starts with a coloured "[linux]" (/etc/profile.d/renga.sh in the root,
	written if it is missing).

	Without a Linux root it says how to get one; "linux --setup" downloads
	Alpine Linux's minirootfs (pinned version and SHA-256), unpacks it,
	makes its absolute links relative (Haiku programs see the root through
	Haiku's VFS), leaves /tmp out (Linux programs share Haiku's /tmp), and
	links /etc/resolv.conf to Haiku's network settings.
*/


#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <FormattingConventions.h>
#include <Locale.h>
#include <LocaleRoster.h>
#include <Message.h>
#include <String.h>


static const char* kRoot = "/boot/system/non-packaged/compat/linux";
static const char* kRenguinModule
	= "/boot/system/add-ons/kernel/personalities/renguin";
static const char* kLinuxPath
	= "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";

// The Linux root "linux --setup" installs (the harness's own root is the
// same release: 3rdparty/ce/tests/renguin/root/alpine.pins).
static const char* kAlpineRelease = "3.24.4";
static const char* kAlpineURL = "https://dl-cdn.alpinelinux.org/alpine/v3.24/"
	"releases/x86_64/alpine-minirootfs-3.24.4-x86_64.tar.gz";
static const char* kAlpineSHA256
	= "cdaa38cb8de3537967ee5b6c9b6cd00cc962da2b018e6eea48696bb274923012";

static const char* kProfileScript =
	"# Written by Renga's \"linux\" command: shows that this shell runs\n"
	"# Linux programs (Renguin), with a coloured [linux] before the prompt.\n"
	"case $- in\n"
	"	*i*) PS1='\\[\\033[1;32m\\][linux]\\[\\033[0m\\] \\u@\\h:\\w\\$ ' ;;\n"
	"esac\n";


static std::string
root_path(const char* linuxPath)
{
	return std::string(kRoot) + linuxPath;
}


static bool
exists(const std::string& path)
{
	struct stat st;
	return lstat(path.c_str(), &st) == 0;
}


static bool
root_installed()
{
	return exists(root_path("/bin/sh"));
}


static int
run(const std::string& command)
{
	int status = system(command.c_str());
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}


//! Absolute links inside \a directory become relative ones (recursively).
static void
make_links_relative(const std::string& root, const std::string& directory)
{
	DIR* dir = opendir((root + directory).c_str());
	if (dir == NULL)
		return;
	while (dirent* entry = readdir(dir)) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;
		std::string linuxPath = directory + "/" + entry->d_name;
		std::string hostPath = root + linuxPath;
		struct stat st;
		if (lstat(hostPath.c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			make_links_relative(root, linuxPath);
			continue;
		}
		if (!S_ISLNK(st.st_mode))
			continue;
		char target[PATH_MAX];
		ssize_t length = readlink(hostPath.c_str(), target, sizeof(target) - 1);
		if (length <= 0 || target[0] != '/')
			continue;
		target[length] = '\0';
		// from the link's directory up to the root, then down to the target
		std::string relative;
		for (size_t i = 1; i < directory.size() + 1; i++) {
			if (i == directory.size() || directory[i] == '/')
				relative += "../";
		}
		if (directory.empty())
			relative = "";
		relative += target + 1;
		if (relative.empty())
			relative = ".";
		unlink(hostPath.c_str());
		symlink(relative.c_str(), hostPath.c_str());
	}
	closedir(dir);
}


static int
setup()
{
	if (root_installed()) {
		printf("A Linux root is already installed in %s.\n", kRoot);
		return 0;
	}
	printf("Getting a Linux root: Alpine Linux %s (minirootfs, about 4 MB)\n"
		"from %s\n", kAlpineRelease, kAlpineURL);
	std::string archive = "/tmp/renga-linux-root.tar.gz";
	if (run("curl -fL --retry 3 -o " + archive + " '" + kAlpineURL + "'") != 0
		&& run("wget -O " + archive + " '" + kAlpineURL + "'") != 0) {
		fprintf(stderr, "linux: the download failed; check the network "
			"connection and try \"linux --setup\" again.\n");
		return 1;
	}
	FILE* sum = popen(("sha256sum " + archive).c_str(), "r");
	char line[256] = "";
	if (sum != NULL) {
		if (fgets(line, sizeof(line), sum) == NULL)
			line[0] = '\0';
		pclose(sum);
	}
	if (strncmp(line, kAlpineSHA256, strlen(kAlpineSHA256)) != 0) {
		fprintf(stderr, "linux: the download does not match its SHA-256 "
			"(want %s); not installed.\n", kAlpineSHA256);
		unlink(archive.c_str());
		return 1;
	}
	if (run(std::string("mkdir -p ") + kRoot + " && tar -xzf " + archive
			+ " -C " + kRoot) != 0) {
		fprintf(stderr, "linux: unpacking into %s failed.\n", kRoot);
		unlink(archive.c_str());
		return 1;
	}
	unlink(archive.c_str());
	make_links_relative(kRoot, "");
	run(std::string("rm -rf ") + kRoot + "/tmp " + kRoot + "/var/tmp");
	unlink(root_path("/etc/resolv.conf").c_str());
	symlink("/boot/system/settings/network/resolv.conf",
		root_path("/etc/resolv.conf").c_str());
	printf("Done: Alpine Linux %s in %s.\n"
		"Install more with \"apk add <package>\" in the Linux shell.\n",
		kAlpineRelease, kRoot);
	return 0;
}


static void
print_missing_root()
{
	fprintf(stderr,
		"No Linux root is installed (%s).\n"
		"Get one with:  linux --setup\n"
		"(Alpine Linux %s's minirootfs, about 4 MB, from alpinelinux.org.)\n",
		kRoot, kAlpineRelease);
}


static void
write_profile_script()
{
	std::string path = root_path("/etc/profile.d/renga.sh");
	if (exists(path) || !exists(root_path("/etc/profile.d")))
		return;
	FILE* file = fopen(path.c_str(), "w");
	if (file == NULL)
		return;
	fputs(kProfileScript, file);
	fclose(file);
}


//! The user's shell in the Linux root's /etc/passwd, else /bin/sh.
static std::string
login_shell()
{
	FILE* passwd = fopen(root_path("/etc/passwd").c_str(), "r");
	std::string shell = "/bin/sh";
	if (passwd == NULL)
		return shell;
	char line[512];
	uid_t uid = getuid();
	while (fgets(line, sizeof(line), passwd) != NULL) {
		// name:password:uid:gid:gecos:home:shell
		char* fields[7] = {};
		char* cursor = line;
		int count = 0;
		while (count < 7) {
			fields[count++] = cursor;
			char* colon = strchr(cursor, ':');
			if (colon == NULL)
				break;
			*colon = '\0';
			cursor = colon + 1;
		}
		if (count < 7 || (uid_t)atoi(fields[2]) != uid)
			continue;
		char* end = fields[6] + strcspn(fields[6], "\r\n");
		*end = '\0';
		if (fields[6][0] == '/' && exists(root_path(fields[6])))
			shell = fields[6];
		break;
	}
	fclose(passwd);
	return shell;
}


static std::string
alpine_release()
{
	FILE* file = fopen(root_path("/etc/alpine-release").c_str(), "r");
	char line[64] = "";
	if (file != NULL) {
		if (fgets(line, sizeof(line), file) == NULL)
			line[0] = '\0';
		fclose(file);
	}
	line[strcspn(line, "\r\n")] = '\0';
	return line[0] != '\0' ? std::string("Alpine ") + line : "Linux";
}


// #pragma mark - locale


/*!	True for the names that mean "no locale chosen": unset, empty, C or
	POSIX with or without a character set (C.UTF-8, C.utf8, POSIX.UTF-8).
*/
static bool
is_default_locale(const char* name)
{
	if (name == NULL || name[0] == '\0')
		return true;
	size_t length = strcspn(name, ".@");
	return (length == 1 && (name[0] == 'C' || name[0] == 'c'))
		|| (length == 5 && strncasecmp(name, "POSIX", 5) == 0);
}


/*!	The country a language is most often written with, for the language-only
	names Haiku uses for messages ("de.UTF-8"): Linux C libraries and the
	programs on them (ICU, Chromium, GTK, Qt) expect language_COUNTRY.
*/
static const char*
default_country(const std::string& language)
{
	static const char* const kCountries[][2] = {
		{ "ar", "EG" }, { "be", "BY" }, { "bg", "BG" }, { "ca", "ES" },
		{ "cs", "CZ" }, { "da", "DK" }, { "de", "DE" }, { "el", "GR" },
		{ "en", "US" }, { "eo", "XX" }, { "es", "ES" }, { "et", "EE" },
		{ "eu", "ES" }, { "fa", "IR" }, { "fi", "FI" }, { "fr", "FR" },
		{ "fur", "IT" }, { "gl", "ES" }, { "he", "IL" }, { "hi", "IN" },
		{ "hr", "HR" }, { "hu", "HU" }, { "id", "ID" }, { "it", "IT" },
		{ "ja", "JP" }, { "ko", "KR" }, { "lt", "LT" }, { "lv", "LV" },
		{ "nb", "NO" }, { "nl", "NL" }, { "pa", "IN" }, { "pl", "PL" },
		{ "pt", "PT" }, { "ro", "RO" }, { "ru", "RU" }, { "sk", "SK" },
		{ "sl", "SI" }, { "sq", "AL" }, { "sr", "RS" }, { "sv", "SE" },
		{ "th", "TH" }, { "tr", "TR" }, { "uk", "UA" }, { "vi", "VN" },
		{ "zh", "CN" },
	};
	for (size_t i = 0; i < sizeof(kCountries) / sizeof(kCountries[0]); i++) {
		if (language == kCountries[i][0])
			return kCountries[i][1];
	}
	return NULL;
}


/*!	A Haiku locale name as Linux C libraries name it: "de.UTF-8" ->
	"de_DE.UTF-8", "en_GB.UTF-8@strings=messages" -> "en_GB.UTF-8" (the
	modifier is Haiku's own), "pt-BR" -> "pt_BR.UTF-8", "zh_Hant_TW" ->
	"zh_TW.UTF-8", "zh_Hans" -> "zh_CN.UTF-8"; C, POSIX and C.UTF-8 ->
	"en_US.UTF-8", Haiku's default language. Never returns C.UTF-8: with it
	Linux programs found no language to translate to ("Translation:
	Unsupported language 'C.UTF-8'" on hardware).
*/
static std::string
linux_locale_name(const char* haikuName)
{
	if (is_default_locale(haikuName))
		return "en_US.UTF-8";

	std::string name(haikuName, strcspn(haikuName, ".@"));
	for (size_t i = 0; i < name.size(); i++) {
		if (name[i] == '-')
			name[i] = '_';
	}

	// language[_Script][_COUNTRY]
	std::vector<std::string> parts;
	size_t start = 0;
	while (start <= name.size()) {
		size_t end = name.find('_', start);
		if (end == std::string::npos)
			end = name.size();
		parts.push_back(name.substr(start, end - start));
		start = end + 1;
	}
	std::string language = parts[0];
	for (size_t i = 0; i < language.size(); i++)
		language[i] = tolower(language[i]);
	if (language.empty() || language.size() > 3
		|| strspn(language.c_str(), "abcdefghijklmnopqrstuvwxyz")
			!= language.size())
		return "en_US.UTF-8";

	std::string country;
	std::string script;
	for (size_t i = 1; i < parts.size(); i++) {
		const std::string& part = parts[i];
		if (part.size() == 4)
			script = part;
		else if (part.size() == 2 || part.size() == 3)
			country = part;
	}
	for (size_t i = 0; i < country.size(); i++)
		country[i] = toupper(country[i]);
	if (country.empty() && language == "zh")
		country = strcasecmp(script.c_str(), "Hant") == 0 ? "TW" : "CN";
	if (country.empty() && language == "sr" && strcasecmp(script.c_str(),
			"Latn") == 0)
		return "sr_RS.UTF-8@latin";
	if (country.empty()) {
		const char* fallback = default_country(language);
		if (fallback != NULL && strcmp(fallback, "XX") != 0)
			country = fallback;
	}
	if (country.empty())
		return language + ".UTF-8";
	return language + "_" + country + ".UTF-8";
}


//!	The first of \a names (environment variables) that names a locale.
static const char*
chosen_locale(const char* const* names)
{
	for (int i = 0; names[i] != NULL; i++) {
		const char* value = getenv(names[i]);
		if (!is_default_locale(value))
			return value;
	}
	return NULL;
}


/*!	LANG, LANGUAGE and the formatting categories for the Linux side.
	Messages: LC_ALL, LC_MESSAGES or LANG from Haiku's environment when one
	names a real locale (Terminal's profile sets LC_MESSAGES from the Locale
	preferences), otherwise the Locale preferences' first language.
	LANGUAGE lists all preferred languages (gettext's fallback list).
	Formatting (numbers, dates, money, units, paper): LC_ALL, the category
	itself, LC_NUMERIC, or the Locale preferences' formatting conventions;
	set only where it differs from LANG.
*/
static void
linux_locale_environment(std::vector<std::string>& environment)
{
	BMessage preferred;
	BLocaleRoster::Default()->GetPreferredLanguages(&preferred);

	static const char* const kMessages[]
		= { "LC_ALL", "LC_MESSAGES", "LANG", NULL };
	const char* messages = chosen_locale(kMessages);
	std::string lang;
	if (messages != NULL)
		lang = linux_locale_name(messages);
	else {
		const char* first = NULL;
		if (preferred.FindString("language", &first) != B_OK)
			first = "en";
		lang = linux_locale_name(first);
	}
	environment.push_back("LANG=" + lang);

	std::string languages;
	const char* language;
	for (int32 i = 0; preferred.FindString("language", i, &language) == B_OK;
			i++) {
		std::string entry = linux_locale_name(language);
		entry = entry.substr(0, entry.find('.'));
		if (!languages.empty())
			languages += ":";
		languages += entry;
	}
	if (languages.empty())
		languages = lang.substr(0, lang.find('.'));
	environment.push_back("LANGUAGE=" + languages);

	std::string formatting;
	BFormattingConventions conventions;
	if (BLocale::Default()->GetFormattingConventions(&conventions) == B_OK
		&& conventions.LanguageCode() != NULL) {
		std::string id = conventions.LanguageCode();
		if (conventions.CountryCode() != NULL)
			id += std::string("_") + conventions.CountryCode();
		formatting = linux_locale_name(id.c_str());
	}

	static const char* const kCategories[] = { "LC_NUMERIC", "LC_TIME",
		"LC_MONETARY", "LC_MEASUREMENT", "LC_PAPER", NULL };
	for (int i = 0; kCategories[i] != NULL; i++) {
		const char* names[] = { "LC_ALL", kCategories[i], "LC_NUMERIC",
			NULL };
		const char* value = chosen_locale(names);
		std::string category = value != NULL ? linux_locale_name(value)
			: formatting;
		if (!category.empty() && category != lang)
			environment.push_back(std::string(kCategories[i]) + "=" + category);
	}
}


// #pragma mark -


static std::vector<std::string>
linux_environment(const std::string& shell)
{
	std::vector<std::string> environment;
	const char* home = getenv("HOME");
	const char* term = getenv("TERM");
	const char* user = getenv("USER");
	if (user == NULL) {
		struct passwd* entry = getpwuid(getuid());
		user = entry != NULL ? entry->pw_name : "user";
	}
	environment.push_back(std::string("PATH=") + kLinuxPath);
	environment.push_back(std::string("HOME=")
		+ (home != NULL ? home : "/boot/home"));
	environment.push_back(std::string("TERM=")
		+ (term != NULL ? term : "xterm-256color"));
	linux_locale_environment(environment);
	environment.push_back(std::string("USER=") + user);
	environment.push_back(std::string("LOGNAME=") + user);
	environment.push_back("XDG_RUNTIME_DIR=/tmp/xdg-runtime");
	environment.push_back("WAYLAND_DISPLAY=wayland-0");
	// Renga's session bus (session_bus): notifications, UPower, portals,
	// Secret Service
	environment.push_back(
		"DBUS_SESSION_BUS_ADDRESS=unix:path=/tmp/xdg-runtime/bus");
	environment.push_back("SHELL=" + shell);
	// Qt's colours from Renga's appearance (linux-apps theme) need the
	// qt6ct or qt5ct platform theme; used when the root has one of them
	if (exists(root_path("/usr/lib/qt6/plugins/platformthemes/libqt6ct.so")))
		environment.push_back("QT_QPA_PLATFORMTHEME=qt6ct");
	else if (exists(root_path(
			"/usr/lib/qt5/plugins/platformthemes/libqt5ct.so"))) {
		environment.push_back("QT_QPA_PLATFORMTHEME=qt5ct");
	}
	return environment;
}


/*!	Brings the GTK and Qt colours up to date with Renga's appearance
	(linux-apps theme, F-27 "Matching look"), in the background: it needs
	app_server and must not delay the program.
*/
static void
update_theme()
{
	// only when Renga's appearance or the Renguin settings changed since
	// the files were written
	const char* home = getenv("HOME") != NULL ? getenv("HOME") : "/boot/home";
	std::string written = std::string(home) + "/.config/gtk-3.0/settings.ini";
	std::string appearance = std::string(home)
		+ "/config/settings/system/app_server/appearance";
	std::string settings = std::string(home)
		+ "/config/settings/Renguin/settings";
	struct stat writtenStat, other;
	if (stat(written.c_str(), &writtenStat) == 0
		&& (stat(appearance.c_str(), &other) != 0
			|| other.st_mtime <= writtenStat.st_mtime)
		&& (stat(settings.c_str(), &other) != 0
			|| other.st_mtime <= writtenStat.st_mtime)) {
		return;
	}

	// twice forked: the Linux program this process becomes must not get a
	// child it does not know about (wait() would report it)
	pid_t child = fork();
	if (child < 0)
		return;
	if (child > 0) {
		waitpid(child, NULL, 0);
		return;
	}
	if (fork() != 0)
		_exit(0);
	int null = open("/dev/null", O_RDWR);
	if (null >= 0) {
		dup2(null, 1);
		dup2(null, 2);
	}
	execl("/bin/linux-apps", "linux-apps", "theme", (char*)NULL);
	_exit(127);
}


static void
usage()
{
	printf("Usage: linux [<command> [<argument>...]]\n"
		"       linux --setup\n"
		"Starts the Linux root's shell, or runs one Linux command, under\n"
		"Renguin with a clean Linux environment (PATH and the rest).\n"
		"--setup gets a Linux root (Alpine Linux %s) if there is none.\n",
		kAlpineRelease);
}


int
main(int argc, char** argv)
{
	if (argc > 1 && (strcmp(argv[1], "--help") == 0
			|| strcmp(argv[1], "-h") == 0)) {
		usage();
		return 0;
	}
	if (!exists(kRenguinModule)) {
		fprintf(stderr, "linux: this system cannot run Linux programs "
			"(Renguin is not installed).\n");
		return 1;
	}
	if (argc > 1 && strcmp(argv[1], "--setup") == 0) {
		int status = setup();
		if (status != 0 || argc == 2 || strcmp(argv[2], "--shell") != 0)
			return status;
		// "--setup --shell" (the Linux shell's Deskbar entry): go on
		argc = 1;
	}
	if (!root_installed()) {
		print_missing_root();
		return 1;
	}

	write_profile_script();
	std::string shell = login_shell();
	update_theme();
	std::vector<std::string> environment = linux_environment(shell);
	std::vector<char*> envp;
	for (size_t i = 0; i < environment.size(); i++)
		envp.push_back((char*)environment[i].c_str());
	envp.push_back(NULL);

	if (argc > 1) {
		// one command, looked up with the Linux PATH by the root's sh
		std::string sh = root_path("/bin/sh");
		std::vector<char*> args;
		args.push_back((char*)"sh");
		args.push_back((char*)"-c");
		args.push_back((char*)"exec \"$@\"");
		args.push_back((char*)"sh");
		for (int i = 1; i < argc; i++)
			args.push_back(argv[i]);
		args.push_back(NULL);
		execve(sh.c_str(), args.data(), envp.data());
		fprintf(stderr, "linux: cannot run the root's /bin/sh: %s\n",
			strerror(errno));
		return 127;
	}

	if (isatty(STDOUT_FILENO)) {
		// the Terminal window's title
		printf("\033]0;Linux (Renguin)\007");
	}
	printf("Renguin Linux shell (%s). Type 'exit' to return to Renga.\n",
		alpine_release().c_str());
	fflush(stdout);

	// a login shell, so the root's /etc/profile (and renga.sh) runs
	std::string name = shell.substr(shell.rfind('/') + 1);
	std::string loginName = "-" + name;
	char* args[] = { (char*)loginName.c_str(), NULL };
	std::string hostShell = root_path(shell.c_str());
	execve(hostShell.c_str(), args, envp.data());
	fprintf(stderr, "linux: cannot start %s: %s\n", shell.c_str(),
		strerror(errno));
	return 127;
}
