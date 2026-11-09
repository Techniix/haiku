/*
 * Copyright 2006-2010, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Philippe Houdoin, phoudoin at haiku-os dot org
 */
#ifndef KERNEL_BOOT_IMAGES_H
#define KERNEL_BOOT_IMAGES_H

#ifdef HAIKU_DISTRO_COMPATIBILITY_OFFICIAL
#	ifdef HAIKU_OFFICIAL_RELEASE
#		include <boot/images-tm.h>
#	else
#		include <boot/images-tm-development.h>
#	endif
#elif defined(HAIKU_CE_NEUTRAL_SPLASH)
	// Haiku Community Edition, name-neutral wordmark ("COMMUNITY EDITION"
	// only), for builds that must not carry the Haiku name. Enable with
	// "TARGET_DEFINES += HAIKU_CE_NEUTRAL_SPLASH ;" in UserBuildConfig.
#	include <boot/images-ce-neutral.h>
#else
	// Haiku Community Edition wordmark: "HAIKU" with "COMMUNITY EDITION",
	// set in Lato rather than the official HAIKU logo, which is reserved
	// for official builds. Before distributing images publicly, see the
	// note on Haiku, Inc.'s distribution guidelines in
	// 3rdparty/ce/artwork/make_splash_logo.py.
	// Source: data/artwork/boot_splash/splash_logo-ce.png.
#	include <boot/images-ce.h>
#endif

#endif	/* KERNEL_BOOT_ARCH_H */
