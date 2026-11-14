/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef WAYLAND_SWITCH_H
#define WAYLAND_SWITCH_H


#include <SupportDefs.h>


// Renga (roadmap F-43 phase C): the switch for the Wayland compositor, which
// shows Wayland programs (native ports and Linux programs under Renguin) as
// windows. Its launch job (data/launch/user_wayland) is skipped at login
// while ~/config/settings/Renga/wayland says "disabled"; switching also
// starts or stops the running compositor through the launch daemon.

bool		wayland_compositor_enabled();
status_t	set_wayland_compositor_enabled(bool enabled);
bool		wayland_compositor_running();


#endif	// WAYLAND_SWITCH_H
