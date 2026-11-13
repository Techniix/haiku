/*
 * Copyright 2026, Renga contributors.
 * Distributed under the terms of the MIT License.
 *
 * Linux ioctl number encoding (<linux/ioctl.h>, asm-generic layout: 8 bits
 * number, 8 bits type, 14 bits size, 2 bits direction), used by the Linux
 * device interfaces Renga offers (evdev, uinput, hidraw). The values match
 * Linux, so Linux binaries under Renguin send the same numbers.
 *
 * BSD's <sys/ioccom.h> defines _IO/_IOR/_IOW/_IOWR with another layout.
 * When it was included first, its macros keep their names; the Linux
 * headers here only use the _LINUX_IO* names, so their numbers stay Linux
 * numbers either way.
 */
#ifndef _LINUX_IOCTL_H
#define _LINUX_IOCTL_H


#define _LINUX_IOC_NRBITS		8
#define _LINUX_IOC_TYPEBITS		8
#define _LINUX_IOC_SIZEBITS		14
#define _LINUX_IOC_NRSHIFT		0
#define _LINUX_IOC_TYPESHIFT	(_LINUX_IOC_NRSHIFT + _LINUX_IOC_NRBITS)
#define _LINUX_IOC_SIZESHIFT	(_LINUX_IOC_TYPESHIFT + _LINUX_IOC_TYPEBITS)
#define _LINUX_IOC_DIRSHIFT		(_LINUX_IOC_SIZESHIFT + _LINUX_IOC_SIZEBITS)

#define _LINUX_IOC_NONE		0U
#define _LINUX_IOC_WRITE	1U
#define _LINUX_IOC_READ		2U

#define _LINUX_IOC(dir, type, nr, size) \
	((unsigned int)(((dir) << _LINUX_IOC_DIRSHIFT) \
		| ((type) << _LINUX_IOC_TYPESHIFT) \
		| ((nr) << _LINUX_IOC_NRSHIFT) \
		| ((size) << _LINUX_IOC_SIZESHIFT)))

#define _LINUX_IO(type, nr)			_LINUX_IOC(_LINUX_IOC_NONE, (type), (nr), 0)
#define _LINUX_IOR(type, nr, t)		_LINUX_IOC(_LINUX_IOC_READ, (type), (nr), \
										sizeof(t))
#define _LINUX_IOW(type, nr, t)		_LINUX_IOC(_LINUX_IOC_WRITE, (type), (nr), \
										sizeof(t))
#define _LINUX_IOWR(type, nr, t)	_LINUX_IOC(_LINUX_IOC_READ \
										| _LINUX_IOC_WRITE, (type), (nr), \
										sizeof(t))

#define _LINUX_IOC_DIR(nr)	(((nr) >> _LINUX_IOC_DIRSHIFT) & 3U)
#define _LINUX_IOC_TYPE(nr)	(((nr) >> _LINUX_IOC_TYPESHIFT) & 0xffU)
#define _LINUX_IOC_NR(nr)	(((nr) >> _LINUX_IOC_NRSHIFT) & 0xffU)
#define _LINUX_IOC_SIZE(nr)	(((nr) >> _LINUX_IOC_SIZESHIFT) & 0x3fffU)

/* The Linux names, unless something else defined them already. */
#ifndef _IOC_READ
#	define _IOC_NONE	_LINUX_IOC_NONE
#	define _IOC_WRITE	_LINUX_IOC_WRITE
#	define _IOC_READ	_LINUX_IOC_READ
#endif
#ifndef _IOC_SIZE
#	define _IOC_NRBITS		_LINUX_IOC_NRBITS
#	define _IOC_TYPEBITS	_LINUX_IOC_TYPEBITS
#	define _IOC_SIZEBITS	_LINUX_IOC_SIZEBITS
#	define _IOC_NRSHIFT		_LINUX_IOC_NRSHIFT
#	define _IOC_TYPESHIFT	_LINUX_IOC_TYPESHIFT
#	define _IOC_SIZESHIFT	_LINUX_IOC_SIZESHIFT
#	define _IOC_DIRSHIFT	_LINUX_IOC_DIRSHIFT
#	define _IOC_DIR(nr)		_LINUX_IOC_DIR(nr)
#	define _IOC_TYPE(nr)	_LINUX_IOC_TYPE(nr)
#	define _IOC_NR(nr)		_LINUX_IOC_NR(nr)
#	define _IOC_SIZE(nr)	_LINUX_IOC_SIZE(nr)
#endif

#ifndef _IOC
#	define _IOC(dir, type, nr, size)	_LINUX_IOC((dir), (type), (nr), (size))
#endif
#ifndef _IO
#	define _IO(type, nr)			_LINUX_IO((type), (nr))
#	define _IOR(type, nr, t)		_LINUX_IOR((type), (nr), t)
#	define _IOW(type, nr, t)		_LINUX_IOW((type), (nr), t)
#	define _IOWR(type, nr, t)		_LINUX_IOWR((type), (nr), t)
#endif


#endif	/* _LINUX_IOCTL_H */
