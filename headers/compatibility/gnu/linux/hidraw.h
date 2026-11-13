/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux hidraw interface (<linux/hidraw.h>) for raw access to game
 * controllers, /dev/hidraw*. Layout and numbers are Linux's. Renga publishes
 * hidraw nodes only for game controllers, never for keyboards, mice or
 * tablets, and only while "Raw controller access" is on in the Gamepads
 * preferences (3rdparty/ce/PORTING.md, "Gamepads").
 */
#ifndef _LINUX_HIDRAW_H
#define _LINUX_HIDRAW_H


#include <stdint.h>

#include <linux/ioctl.h>


#define HID_MAX_DESCRIPTOR_SIZE	4096

struct hidraw_report_descriptor {
	uint32_t	size;
	uint8_t		value[HID_MAX_DESCRIPTOR_SIZE];
};

struct hidraw_devinfo {
	uint32_t	bustype;
	int16_t		vendor;
	int16_t		product;
};

#define HIDIOCGRDESCSIZE	_LINUX_IOR('H', 0x01, int)
#define HIDIOCGRDESC		_LINUX_IOR('H', 0x02, struct hidraw_report_descriptor)
#define HIDIOCGRAWINFO		_LINUX_IOR('H', 0x03, struct hidraw_devinfo)
#define HIDIOCGRAWNAME(len)	_LINUX_IOC(_LINUX_IOC_READ, 'H', 0x04, len)
#define HIDIOCGRAWPHYS(len)	_LINUX_IOC(_LINUX_IOC_READ, 'H', 0x05, len)
#define HIDIOCSFEATURE(len)	_LINUX_IOC(_LINUX_IOC_WRITE | _LINUX_IOC_READ, \
								'H', 0x06, len)
#define HIDIOCGFEATURE(len)	_LINUX_IOC(_LINUX_IOC_WRITE | _LINUX_IOC_READ, \
								'H', 0x07, len)
#define HIDIOCGRAWUNIQ(len)	_LINUX_IOC(_LINUX_IOC_READ, 'H', 0x08, len)
#define HIDIOCSINPUT(len)	_LINUX_IOC(_LINUX_IOC_WRITE | _LINUX_IOC_READ, \
								'H', 0x09, len)
#define HIDIOCGINPUT(len)	_LINUX_IOC(_LINUX_IOC_WRITE | _LINUX_IOC_READ, \
								'H', 0x0A, len)
#define HIDIOCSOUTPUT(len)	_LINUX_IOC(_LINUX_IOC_WRITE | _LINUX_IOC_READ, \
								'H', 0x0B, len)
#define HIDIOCGOUTPUT(len)	_LINUX_IOC(_LINUX_IOC_WRITE | _LINUX_IOC_READ, \
								'H', 0x0C, len)
#define HIDIOCREVOKE		_LINUX_IOW('H', 0x0D, int)
#define HIDIOCTL_LAST		_LINUX_IOC_NR(HIDIOCREVOKE)

#define HIDRAW_FIRST_MINOR	0
#define HIDRAW_MAX_DEVICES	64
#define HIDRAW_BUFFER_SIZE	64


#endif	/* _LINUX_HIDRAW_H */
