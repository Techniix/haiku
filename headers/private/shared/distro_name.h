/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _DISTRO_NAME_H
#define _DISTRO_NAME_H


/*!	The name of the operating system as the user sees it: in the boot
	loader, AboutSystem, Deskbar, FirstBootPrompt and the Installer.

	Renga is based on Haiku. Haiku, Inc.'s distribution guidelines reserve
	the name "Haiku" (and the HAIKU logo) for the official distribution,
	which is built with HAIKU_DISTRO_COMPATIBILITY=official; every other
	build calls itself Renga.

	This is only the display name. Everything software uses to detect the
	platform keeps saying Haiku: uname -s and B_SYSTEM_NAME, __HAIKU__,
	B_HAIKU_* constants, package names, MIME signatures and paths.

	The macro expands to a string literal, so it can be pasted into other
	literals, B_TRANSLATE() ones included: the catalog extraction runs the
	preprocessor first, so the catalog key carries the name. In official
	builds the keys are therefore the same as upstream's, and their
	translations apply; in Renga builds a string that names the system is a
	new key, shown in English until it is translated.
*/
#ifdef HAIKU_DISTRO_COMPATIBILITY_OFFICIAL
#	define DISTRO_NAME				"Haiku"
#	define DISTRO_NAME_UPPERCASE	"HAIKU"
#else
#	define DISTRO_NAME				"Renga"
#	define DISTRO_NAME_UPPERCASE	"RENGA"
#endif


#endif	// _DISTRO_NAME_H
