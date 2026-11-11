/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Haiku overlay: extends LinuxKPI's linux/module.h (FreeBSD import).
 *
 * MODULE_VERSION is FreeBSD's two-argument kernel-module macro there
 * (sys/module.h, empty in Haiku's freebsd_network), while Linux drivers
 * use the one-argument Linux form (btmtk.c: MODULE_VERSION(VERSION)).
 * Neither records anything on Haiku, so accept both arities.
 */
#ifndef _LKPI_HAIKU_LINUX_MODULE_H_
#define _LKPI_HAIKU_LINUX_MODULE_H_

#include_next <linux/module.h>

#undef MODULE_VERSION
#define	MODULE_VERSION(...)

#endif
