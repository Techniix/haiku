/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Private interface between the two halves of LinuxKPI USB:
 *   lkpi_usb.c        LinuxKPI side: Linux structures, URBs, anchors
 *   lkpi_usb_haiku.c  Haiku side: bus_managers/usb/v3 (<USB3.h>)
 * Neither side's headers can see the other's (Linux and Haiku both define
 * struct usb_interface_descriptor & co.), so only plain C types and opaque
 * pointers cross. Descriptors cross as raw bytes in USB wire format.
 *
 * Errors cross as LKPI_HK_* codes, not errnos: LinuxKPI uses FreeBSD errno
 * numbers and Haiku its own, and each side maps the codes to its own.
 */
#ifndef _LKPI_USB_BRIDGE_H_
#define _LKPI_USB_BRIDGE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lkpi_hk_dev;		/* one per Haiku usb_device */

/* transfer types (same numbering as Linux pipes) */
#define	LKPI_HK_ISOC	0
#define	LKPI_HK_INTR	1
#define	LKPI_HK_CTRL	2
#define	LKPI_HK_BULK	3

/* status codes (>= 0); lkpi_usb.c turns them into Linux errnos */
#define	LKPI_HK_OK		0
#define	LKPI_HK_CANCELED	1	/* lkpi_hk_cancel*() */
#define	LKPI_HK_STALL		2	/* -EPIPE */
#define	LKPI_HK_CRC		3	/* -EILSEQ */
#define	LKPI_HK_OVERRUN		4	/* -EOVERFLOW (babble) */
#define	LKPI_HK_SHORT		5	/* short packet: -EREMOTEIO */
#define	LKPI_HK_TIMEOUT		6	/* -ETIMEDOUT */
#define	LKPI_HK_GONE		7	/* device unplugged: -ENODEV */
#define	LKPI_HK_NOMEM		8	/* -ENOMEM */
#define	LKPI_HK_INVAL		9	/* -EINVAL */
#define	LKPI_HK_UNSUPP		10	/* -EOPNOTSUPP */
#define	LKPI_HK_ERROR		11	/* other: -EPROTO */

typedef void (*lkpi_hk_done_fn)(void *cookie, int status, size_t actual);

/* ---- implemented by lkpi_usb_haiku.c, called by lkpi_usb.c ---------- */

/*
 * Which devices Haiku should report (and, on hotplug, load this add-on
 * for): a superset of the Linux id tables. 0 is a wildcard, as in Haiku's
 * usb_support_descriptor; the class triple is matched against the device
 * and every interface. The exact Linux match happens in lkpi_usb.c.
 */
struct lkpi_hk_match {
	uint16_t	vendor;
	uint16_t	product;
	uint8_t		dev_class;
	uint8_t		dev_subclass;
	uint8_t		dev_protocol;
};

/* driver_name must be the add-on's file name: Haiku's USB stack asks devfs
 * to (re)load the driver of that name when a matching device appears.
 * The match list is set once: Haiku keeps it after the add-on unloads
 * (that is what reloads it), and replacing it while running is unsafe. */
int	lkpi_hk_usb_start(const char *driver_name,
	    const struct lkpi_hk_match *m, size_t count);
void	lkpi_hk_usb_stop(void);

const uint8_t *lkpi_hk_device_descriptor(struct lkpi_hk_dev *hk);
uint32_t lkpi_hk_device_id(struct lkpi_hk_dev *hk);	/* Haiku usb_id */
int	lkpi_hk_config_value(struct lkpi_hk_dev *hk);	/* bConfigurationValue */
int	lkpi_hk_num_interfaces(struct lkpi_hk_dev *hk);
int	lkpi_hk_num_alts(struct lkpi_hk_dev *hk, int ifidx);
int	lkpi_hk_active_alt(struct lkpi_hk_dev *hk, int ifidx);
const uint8_t *lkpi_hk_alt_descriptor(struct lkpi_hk_dev *hk, int ifidx,
	    int alt);					/* 9 bytes */
int	lkpi_hk_alt_num_endpoints(struct lkpi_hk_dev *hk, int ifidx, int alt);
const uint8_t *lkpi_hk_ep_descriptor(struct lkpi_hk_dev *hk, int ifidx,
	    int alt, int ep);				/* 7 bytes */
/* class/vendor descriptors following an alt setting, concatenated */
size_t	lkpi_hk_alt_extra(struct lkpi_hk_dev *hk, int ifidx, int alt,
	    uint8_t *buf, size_t size);

/* select the configuration (if none is set yet) before binding */
int	lkpi_hk_activate(struct lkpi_hk_dev *hk);
int	lkpi_hk_set_alt(struct lkpi_hk_dev *hk, int ifidx, int alt);
/* pipe of endpoint address "epaddr" in the active alt settings, or NULL */
void	*lkpi_hk_pipe(struct lkpi_hk_dev *hk, uint8_t epaddr);

int	lkpi_hk_queue(struct lkpi_hk_dev *hk, void *pipe, int type,
	    void *buf, size_t len, lkpi_hk_done_fn done, void *cookie);
int	lkpi_hk_queue_control(struct lkpi_hk_dev *hk, const uint8_t setup[8],
	    void *buf, lkpi_hk_done_fn done, void *cookie);
/* cancels every queued transfer on the pipe (Haiku has no per-transfer
 * cancel); each one completes with -ECONNRESET */
int	lkpi_hk_cancel(struct lkpi_hk_dev *hk, void *pipe);
int	lkpi_hk_cancel_control(struct lkpi_hk_dev *hk);
int	lkpi_hk_clear_halt(struct lkpi_hk_dev *hk, void *pipe);

/* ---- implemented by lkpi_usb.c, called by lkpi_usb_haiku.c ---------- */

/* new device: returns LKPI_HK_OK if a Linux driver bound to one of its
 * interfaces and fills *ctx; else another LKPI_HK_* code. Runs in Haiku's
 * USB explore thread; may sleep. */
int	lkpi_usb_device_added(struct lkpi_hk_dev *hk, void **ctx);
void	lkpi_usb_device_removed(void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* _LKPI_USB_BRIDGE_H_ */
