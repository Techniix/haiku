/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * LinuxKPI Bluetooth: <net/bluetooth/bluetooth.h> for Linux HCI transport
 * drivers (drivers/bluetooth). Written for LinuxKPI_HaikuOS from the Linux
 * driver-facing API (names, semantics); values come from the Bluetooth
 * Core Specification. Sockets, L2CAP, SCO and mgmt are Haiku's own stack
 * and are not provided here.
 */
#ifndef _LKPI_NET_BLUETOOTH_BLUETOOTH_H_
#define _LKPI_NET_BLUETOOTH_BLUETOOTH_H_

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/string.h>
#include <linux/unaligned.h>

/* skb headroom drivers can push a packet-type byte into */
#define	BT_SKB_RESERVE	8

typedef struct {
	__u8 b[6];
} __packed bdaddr_t;

#define	BDADDR_ANY	(&(bdaddr_t){{0, 0, 0, 0, 0, 0}})
#define	BDADDR_NONE	(&(bdaddr_t){{0xff, 0xff, 0xff, 0xff, 0xff, 0xff}})

static inline int
bacmp(const bdaddr_t *ba1, const bdaddr_t *ba2)
{
	return (memcmp(ba1, ba2, sizeof(bdaddr_t)));
}

static inline void
bacpy(bdaddr_t *dst, const bdaddr_t *src)
{
	memcpy(dst, src, sizeof(bdaddr_t));
}

static inline void
baswap(bdaddr_t *dst, const bdaddr_t *src)
{
	int i;

	for (i = 0; i < 6; i++)
		dst->b[i] = src->b[5 - i];
}

/* control block of every HCI sk_buff (skb->cb) */
struct hci_ctrl {
	struct sock	*sk;
	u16		opcode;
	u8		req_flags;
	u8		req_event;
};

struct bt_skb_cb {
	u8		pkt_type;
	u8		force_active;
	u16		expect;
	u16		pkt_seqnum;
	u8		incoming:1;
	u8		pkt_status:2;
	struct hci_ctrl	hci;
};
#define	bt_cb(skb)	((struct bt_skb_cb *)((skb)->cb))

#define	hci_skb_pkt_type(skb)	bt_cb((skb))->pkt_type
#define	hci_skb_pkt_status(skb)	bt_cb((skb))->pkt_status
#define	hci_skb_expect(skb)	bt_cb((skb))->expect
#define	hci_skb_opcode(skb)	bt_cb((skb))->hci.opcode
#define	hci_skb_event(skb)	bt_cb((skb))->hci.req_event
#define	hci_skb_sk(skb)		bt_cb((skb))->hci.sk

static inline struct sk_buff *
bt_skb_alloc(unsigned int len, gfp_t how)
{
	struct sk_buff *skb;

	skb = alloc_skb(len + BT_SKB_RESERVE, how);
	if (skb != NULL) {
		skb_reserve(skb, BT_SKB_RESERVE);
		memset(bt_cb(skb), 0, sizeof(struct bt_skb_cb));
	}
	return (skb);
}

/* Bluetooth error code (Core Spec Vol 1 Part F) -> negative errno */
int	bt_to_errno(u16 code);

/* logging: Linux appends the newline */
#define	BT_INFO(fmt, ...)	printf("Bluetooth: " fmt "\n", ##__VA_ARGS__)
#define	BT_WARN(fmt, ...)	printf("Bluetooth: " fmt "\n", ##__VA_ARGS__)
#define	BT_ERR(fmt, ...)	printf("Bluetooth: " fmt "\n", ##__VA_ARGS__)
#define	BT_DBG(fmt, ...)	do { } while (0)
#define	BT_ERR_RATELIMITED(fmt, ...)	BT_ERR(fmt, ##__VA_ARGS__)

#define	bt_dev_name(hdev)	((hdev) ? (hdev)->name : "null")

#define	bt_dev_info(hdev, fmt, ...)					\
	BT_INFO("%s: " fmt, bt_dev_name(hdev), ##__VA_ARGS__)
#define	bt_dev_warn(hdev, fmt, ...)					\
	BT_WARN("%s: " fmt, bt_dev_name(hdev), ##__VA_ARGS__)
#define	bt_dev_err(hdev, fmt, ...)					\
	BT_ERR("%s: " fmt, bt_dev_name(hdev), ##__VA_ARGS__)
#define	bt_dev_dbg(hdev, fmt, ...)					\
	BT_DBG("%s: " fmt, bt_dev_name(hdev), ##__VA_ARGS__)
#define	bt_dev_warn_ratelimited	bt_dev_warn
#define	bt_dev_err_ratelimited	bt_dev_err

#endif /* _LKPI_NET_BLUETOOTH_BLUETOOTH_H_ */
