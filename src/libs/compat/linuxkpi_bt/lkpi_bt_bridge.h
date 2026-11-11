/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Private interface between the two halves of LinuxKPI Bluetooth:
 *   lkpi_hci.c         LinuxKPI side: struct hci_dev, Linux HCI core API
 *   lkpi_bt_haiku.cpp  Haiku side: device node + Haiku's HCI module
 *                      ("bluetooth/hci/v1", <bluetooth/HCI/btHCI_transport.h>)
 * Only plain C types cross. Packet types use the H:4 indicators, which
 * Linux uses as hci_skb_pkt_type(): 1 command, 2 ACL, 3 SCO, 4 event.
 */
#ifndef _LKPI_BT_BRIDGE_H_
#define _LKPI_BT_BRIDGE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define	LKPI_BT_CMD	1
#define	LKPI_BT_ACL	2
#define	LKPI_BT_SCO	3
#define	LKPI_BT_EVT	4

/* results; each side maps them to its own error space */
#define	LKPI_BT_OK		0
#define	LKPI_BT_NODEV		1	/* device gone */
#define	LKPI_BT_NOMEM		2
#define	LKPI_BT_INVAL		3
#define	LKPI_BT_IO		4	/* driver/transport error */
#define	LKPI_BT_UNSUPP		5
#define	LKPI_BT_TIMEOUT		6

struct lkpi_bt_node;

struct lkpi_bt_node_info {
	const char	*driver;	/* add-on name (devfs directory) */
	int		index;		/* hci index: hci<index>, node <index> */
	uint16_t	vendor;		/* USB/PCI ids, 0 if unknown */
	uint16_t	product;
	const char	*desc;		/* human-readable, may be NULL */
};

/* ---- implemented by lkpi_bt_haiku.cpp, called by lkpi_hci.c ---------- */

/* publish /dev/bluetooth/h2/<driver>/<index> for the hci_dev 'ctx' */
int	lkpi_bt_node_create(void *ctx, const struct lkpi_bt_node_info *info,
	    struct lkpi_bt_node **node);
/* unpublish; after return no call reaches 'ctx' (waits for running ones);
 * the node itself lives on until its last open handle is freed */
void	lkpi_bt_node_destroy(struct lkpi_bt_node *node);
/* a frame from the controller, for Haiku's stack: events and ACL data.
 * Dropped (LKPI_BT_NODEV) unless bluetooth_server has the node open. */
int	lkpi_bt_node_rx(struct lkpi_bt_node *node, int type, const void *data,
	    size_t len);

/* ---- implemented by lkpi_hci.c, called by lkpi_bt_haiku.cpp --------- */

/* BT_UP: driver open(), then setup() the first time; may sleep long */
int	lkpi_hci_up(void *ctx);
/* last close: driver shutdown()/close() */
void	lkpi_hci_down(void *ctx);
/* a frame from Haiku's stack for the controller (copied) */
int	lkpi_hci_send(void *ctx, int type, const void *data, size_t len);
/* statistics for GET_STATS (Haiku's bt_hci_statistics counts uint8) */
struct lkpi_bt_stats {
	uint32_t	cmd_tx, evt_rx, acl_tx, acl_rx, sco_tx, sco_rx;
	uint32_t	err_tx, err_rx, byte_tx, byte_rx;
};
void	lkpi_hci_stats(void *ctx, struct lkpi_bt_stats *st);

#ifdef __cplusplus
}
#endif

#endif /* _LKPI_BT_BRIDGE_H_ */
