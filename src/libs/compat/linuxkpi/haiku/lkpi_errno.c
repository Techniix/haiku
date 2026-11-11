/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * errno translation between Haiku (negative values) and the positive
 * FreeBSD values LinuxKPI code uses (see include/lkpi_errno.h).
 *
 * Built WITHOUT the LinuxKPI preinclude: here E* names have Haiku's values,
 * and lkpi_errno_list.h supplies the FreeBSD value for each name.
 */
#include <errno.h>
#include <SupportDefs.h>
#include <Errors.h>

int lkpi_errno_from_haiku(int haikuError);
int lkpi_errno_to_haiku(int fbsdErrno);

#define FBSD_EIO	5

int
lkpi_errno_from_haiku(int error)
{
	if (error >= 0)		/* B_OK, or already a positive errno */
		return (error);

	switch (error) {
#define X(name, value)	case name: return (value);
#define Y(name, value)
#include "lkpi_errno_list.h"
#undef X
#undef Y
	case B_ERROR:		return (FBSD_EIO);
	default:		return (FBSD_EIO);
	}
}

int
lkpi_errno_to_haiku(int error)
{
	if (error < 0)
		error = -error;
	if (error == 0)
		return (B_OK);

	switch (error) {
#define X(name, value)	case value: return (name);
#define Y(name, value)
#include "lkpi_errno_list.h"
#undef X
#undef Y
	default:		return (B_ERROR);
	}
}
