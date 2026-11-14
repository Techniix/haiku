/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */


#include "WaylandSwitch.h"

#include <stdio.h>
#include <string.h>

#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <Messenger.h>
#include <OS.h>
#include <Path.h>
#include <Roster.h>
#include <driver_settings.h>

#include <LaunchRoster.h>


static const char* kCompositorSignature
	= "application/x-vnd.Renga-WaylandCompositor";


static status_t
settings_path(BPath& path, bool create)
{
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status != B_OK)
		return status;
	path.Append("Renga");
	if (create)
		create_directory(path.Path(), 0755);
	return path.Append("wayland");
}


bool
wayland_compositor_enabled()
{
	BPath path;
	if (settings_path(path, false) != B_OK)
		return true;
	void* handle = load_driver_settings(path.Path());
	if (handle == NULL)
		return true;
	bool disabled = get_driver_boolean_parameter(handle, "disabled", false,
		true);
	unload_driver_settings(handle);
	return !disabled;
}


bool
wayland_compositor_running()
{
	return be_roster->IsRunning(kCompositorSignature);
}


status_t
set_wayland_compositor_enabled(bool enabled)
{
	BPath path;
	status_t status = settings_path(path, true);
	if (status != B_OK)
		return status;

	// The launch job reads this at login (data/launch/user_wayland).
	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	status = file.InitCheck();
	if (status != B_OK)
		return status;
	const char* text = enabled
		? "# Renga: the Wayland compositor (Screen preferences)\n"
		: "# Renga: the Wayland compositor (Screen preferences)\n"
			"disabled true\n";
	file.Write(text, strlen(text));

	// Now: through the launch daemon, so that it neither restarts a
	// stopped service nor runs a second one.
	BLaunchRoster roster;
	if (enabled) {
		if (wayland_compositor_running())
			return B_OK;
		status = roster.Start(kCompositorSignature);
		if (status != B_OK)
			status = be_roster->Launch(kCompositorSignature);
	} else {
		status = roster.Stop(kCompositorSignature);
		if (status != B_OK && wayland_compositor_running()) {
			status = BMessenger(kCompositorSignature)
				.SendMessage(B_QUIT_REQUESTED);
		}
	}
	return status;
}
