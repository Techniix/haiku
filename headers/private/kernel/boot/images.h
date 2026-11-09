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
#else
	// Renga: the official HAIKU logo is reserved for official builds
	// (Haiku, Inc. distribution guidelines), so other builds show the Renga
	// wordmark. This is the first frame of the kernel's splash animation
	// (src/system/kernel/boot_splash_render.cpp), rendered on the host by
	// 3rdparty/ce/artwork/make_splash_renga.py.
#	include <boot/images-renga.h>
#endif

#endif	/* KERNEL_BOOT_ARCH_H */
