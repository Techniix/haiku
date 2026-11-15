/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */

/*!	Linux shell: the Deskbar entry for Renguin's Linux shell. Opens
	Terminal running the "linux" command (window titled "Linux (Renguin)");
	when no Linux root is installed yet, Terminal runs "linux --setup
	--shell", which gets one first and then starts the shell. Without
	Renguin (no Linux personality in the system) it says so.
*/


#include <Alert.h>
#include <Application.h>
#include <Roster.h>

#include <sys/stat.h>


static const char* kSignature = "application/x-vnd.Renga-LinuxShell";
static const char* kTerminalSignature = "application/x-vnd.Haiku-Terminal";
static const char* kRenguinModule
	= "/boot/system/add-ons/kernel/personalities/renguin";
static const char* kRootShell = "/boot/system/non-packaged/compat/linux/bin/sh";


class LinuxShellApp : public BApplication {
public:
	LinuxShellApp()
		:
		BApplication(kSignature)
	{
	}

	virtual void ReadyToRun()
	{
		struct stat st;
		if (stat(kRenguinModule, &st) != 0) {
			BAlert* alert = new BAlert("Linux shell",
				"This system cannot run Linux programs: Renguin is not "
				"installed.", "OK", NULL, NULL, B_WIDTH_AS_USUAL,
				B_STOP_ALERT);
			alert->Go();
			Quit();
			return;
		}
		bool haveRoot = lstat(kRootShell, &st) == 0;
		const char* withRoot[] = { "-t", "Linux (Renguin)", "/bin/linux",
			NULL };
		const char* withoutRoot[] = { "-t", "Linux (Renguin)", "/bin/linux",
			"--setup", "--shell", NULL };
		const char** args = haveRoot ? withRoot : withoutRoot;
		int count = 0;
		while (args[count] != NULL)
			count++;
		be_roster->Launch(kTerminalSignature, count, (char**)args);
		Quit();
	}
};


int
main()
{
	LinuxShellApp app;
	app.Run();
	return 0;
}
