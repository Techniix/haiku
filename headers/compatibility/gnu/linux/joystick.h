/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux joystick interface (<linux/joystick.h>): the "classic" /dev/input/js*
 * API. Layout and numbers are Linux's. Renga has no js nodes: every game
 * controller is an evdev device (/dev/input/event*, <linux/input.h>). The
 * header exists so that programs written for Linux that support both APIs
 * (SDL's Linux joystick back end, for example) compile unchanged; on Renga
 * they only ever find evdev nodes, and the JSIOC* requests fail with an error
 * on any other descriptor.
 */
#ifndef _LINUX_JOYSTICK_H
#define _LINUX_JOYSTICK_H


#include <stdint.h>

#include <linux/input.h>


#define JS_VERSION		0x020100

#define JS_EVENT_BUTTON	0x01
#define JS_EVENT_AXIS	0x02
#define JS_EVENT_INIT	0x80

struct js_event {
	uint32_t	time;
	int16_t		value;
	uint8_t		type;
	uint8_t		number;
};

#define JSIOCGVERSION		_LINUX_IOR('j', 0x01, uint32_t)
#define JSIOCGAXES			_LINUX_IOR('j', 0x11, uint8_t)
#define JSIOCGBUTTONS		_LINUX_IOR('j', 0x12, uint8_t)
#define JSIOCGNAME(len)		_LINUX_IOC(_LINUX_IOC_READ, 'j', 0x13, len)

#define JSIOCSCORR			_LINUX_IOW('j', 0x21, struct js_corr)
#define JSIOCGCORR			_LINUX_IOR('j', 0x22, struct js_corr)

#define JSIOCSAXMAP			_LINUX_IOW('j', 0x31, uint8_t[ABS_CNT])
#define JSIOCGAXMAP			_LINUX_IOR('j', 0x32, uint8_t[ABS_CNT])
#define JSIOCSBTNMAP		_LINUX_IOW('j', 0x33, uint16_t[KEY_MAX - BTN_MISC + 1])
#define JSIOCGBTNMAP		_LINUX_IOR('j', 0x34, uint16_t[KEY_MAX - BTN_MISC + 1])

#define JS_CORR_NONE		0x00
#define JS_CORR_BROKEN		0x01

struct js_corr {
	int32_t		coef[8];
	int16_t		prec;
	uint16_t	type;
};


#endif	/* _LINUX_JOYSTICK_H */
