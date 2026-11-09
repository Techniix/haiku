/*
** Copyright 2003, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
** Distributed under the terms of the MIT License.
*/
#ifndef MENU_H
#define MENU_H


#include <boot/vfs.h>


class PathBlocklist;


extern int32 boot_menu_timeout(BootVolume& volume);
extern status_t user_menu(BootVolume& _bootVolume,
	PathBlocklist& _pathBlocklist, int32 timeout = 0);


#endif	/* MENU_H */
