/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux uinput interface (<linux/uinput.h>): virtual input devices made by a
 * program through /dev/input/uinput (also reachable as /dev/uinput). Layout
 * and numbers are Linux's. Renga differences (3rdparty/ce/PORTING.md,
 * "Gamepads"): force feedback uploads are handled by the system, so
 * UI_BEGIN_FF_UPLOAD and friends are never needed; the virtual device reads
 * EV_FF play and stop events like on Linux and can ask for the resulting
 * rumble strength with UI_RENGA_GET_RUMBLE.
 */
#ifndef _LINUX_UINPUT_H
#define _LINUX_UINPUT_H


#include <linux/input.h>


#define UINPUT_VERSION			5
#define UINPUT_MAX_NAME_SIZE	80

struct uinput_ff_upload {
	uint32_t			request_id;
	int32_t				retval;
	struct ff_effect	effect;
	struct ff_effect	old;
};

struct uinput_ff_erase {
	uint32_t	request_id;
	int32_t		retval;
	uint32_t	effect_id;
};

#define UINPUT_IOCTL_BASE	'U'
#define UI_DEV_CREATE		_LINUX_IO(UINPUT_IOCTL_BASE, 1)
#define UI_DEV_DESTROY		_LINUX_IO(UINPUT_IOCTL_BASE, 2)

struct uinput_setup {
	struct input_id	id;
	char			name[UINPUT_MAX_NAME_SIZE];
	uint32_t		ff_effects_max;
};
#define UI_DEV_SETUP		_LINUX_IOW(UINPUT_IOCTL_BASE, 3, struct uinput_setup)

struct uinput_abs_setup {
	uint16_t				code;
	struct input_absinfo	absinfo;
};
#define UI_ABS_SETUP		_LINUX_IOW(UINPUT_IOCTL_BASE, 4, \
								struct uinput_abs_setup)

#define UI_SET_EVBIT		_LINUX_IOW(UINPUT_IOCTL_BASE, 100, int)
#define UI_SET_KEYBIT		_LINUX_IOW(UINPUT_IOCTL_BASE, 101, int)
#define UI_SET_RELBIT		_LINUX_IOW(UINPUT_IOCTL_BASE, 102, int)
#define UI_SET_ABSBIT		_LINUX_IOW(UINPUT_IOCTL_BASE, 103, int)
#define UI_SET_MSCBIT		_LINUX_IOW(UINPUT_IOCTL_BASE, 104, int)
#define UI_SET_LEDBIT		_LINUX_IOW(UINPUT_IOCTL_BASE, 105, int)
#define UI_SET_SNDBIT		_LINUX_IOW(UINPUT_IOCTL_BASE, 106, int)
#define UI_SET_FFBIT		_LINUX_IOW(UINPUT_IOCTL_BASE, 107, int)
#define UI_SET_PHYS			_LINUX_IOW(UINPUT_IOCTL_BASE, 108, char*)
#define UI_SET_SWBIT		_LINUX_IOW(UINPUT_IOCTL_BASE, 109, int)
#define UI_SET_PROPBIT		_LINUX_IOW(UINPUT_IOCTL_BASE, 110, int)

#define UI_BEGIN_FF_UPLOAD	_LINUX_IOWR(UINPUT_IOCTL_BASE, 200, \
								struct uinput_ff_upload)
#define UI_END_FF_UPLOAD	_LINUX_IOW(UINPUT_IOCTL_BASE, 201, \
								struct uinput_ff_upload)
#define UI_BEGIN_FF_ERASE	_LINUX_IOWR(UINPUT_IOCTL_BASE, 202, \
								struct uinput_ff_erase)
#define UI_END_FF_ERASE		_LINUX_IOW(UINPUT_IOCTL_BASE, 203, \
								struct uinput_ff_erase)

#define UI_GET_SYSNAME(len)	_LINUX_IOC(_LINUX_IOC_READ, UINPUT_IOCTL_BASE, 44, \
								len)
#define UI_GET_VERSION		_LINUX_IOR(UINPUT_IOCTL_BASE, 45, unsigned int)

/* Renga additions. */
struct renga_uinput_rumble {
	uint16_t	strong_magnitude;
	uint16_t	weak_magnitude;
	uint32_t	play_count;		/* effects started since creation */
	int32_t		player;			/* the player number given to the device */
	int32_t		_reserved;
};
#define UI_RENGA_GET_RUMBLE	_LINUX_IOR(UINPUT_IOCTL_BASE, 240, \
								struct renga_uinput_rumble)
/* Report a battery level (0-100) and charging state for the device. */
#define UI_RENGA_SET_BATTERY	_LINUX_IOW(UINPUT_IOCTL_BASE, 241, \
									struct renga_input_battery)

#define EV_UINPUT			0x0101
#define UI_FF_UPLOAD		1
#define UI_FF_ERASE			2

struct uinput_user_dev {
	char			name[UINPUT_MAX_NAME_SIZE];
	struct input_id	id;
	uint32_t		ff_effects_max;
	int32_t			absmax[ABS_CNT];
	int32_t			absmin[ABS_CNT];
	int32_t			absfuzz[ABS_CNT];
	int32_t			absflat[ABS_CNT];
};


#endif	/* _LINUX_UINPUT_H */
