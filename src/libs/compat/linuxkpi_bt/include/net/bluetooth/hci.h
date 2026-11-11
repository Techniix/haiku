/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * LinuxKPI Bluetooth: <net/bluetooth/hci.h>, the HCI wire format. Values
 * are from the Bluetooth Core Specification (Vol 4 Part E "Host Controller
 * Interface Functional Specification", Vol 4 Part A "UART Transport Layer"
 * for the packet indicators); identifier names follow Linux so drivers
 * build unchanged. Only what transport drivers use; extend as needed.
 */
#ifndef _LKPI_NET_BLUETOOTH_HCI_H_
#define _LKPI_NET_BLUETOOTH_HCI_H_

#include <linux/types.h>
#include <linux/skbuff.h>
#include <net/bluetooth/bluetooth.h>

/* frame sizes */
#define	HCI_MAX_ACL_SIZE	1024
#define	HCI_MAX_SCO_SIZE	255
#define	HCI_MAX_ISO_SIZE	251
#define	HCI_MAX_EVENT_SIZE	260
#define	HCI_MAX_FRAME_SIZE	(HCI_MAX_ACL_SIZE + 4)

/* devices */
#define	HCI_DEV_NONE	0xffff

/* bus types (hdev->bus) */
#define	HCI_VIRTUAL	0
#define	HCI_USB		1
#define	HCI_PCCARD	2
#define	HCI_UART	3
#define	HCI_RS232	4
#define	HCI_PCI		5
#define	HCI_SDIO	6
#define	HCI_SPI		7
#define	HCI_I2C		8
#define	HCI_SMD		9
#define	HCI_VIRTIO	10
#define	HCI_IPC		11

/* controller types */
#define	HCI_PRIMARY	0x00
#define	HCI_AMP		0x01

/* packet indicators (UART transport; Linux uses them everywhere) */
#define	HCI_COMMAND_PKT		0x01
#define	HCI_ACLDATA_PKT		0x02
#define	HCI_SCODATA_PKT		0x03
#define	HCI_EVENT_PKT		0x04
#define	HCI_ISODATA_PKT		0x05
#define	HCI_DIAG_PKT		0xf0
#define	HCI_DRV_PKT		0xf1
#define	HCI_VENDOR_PKT		0xff

/* timeouts (jiffies) */
#define	HCI_DISCONN_TIMEOUT	msecs_to_jiffies(2000)
#define	HCI_CMD_TIMEOUT		msecs_to_jiffies(2000)
#define	HCI_NCMD_TIMEOUT	msecs_to_jiffies(4000)
#define	HCI_ACL_TX_TIMEOUT	msecs_to_jiffies(45000)
#define	HCI_INIT_TIMEOUT	msecs_to_jiffies(10000)
#define	HCI_POWER_OFF_TIMEOUT	msecs_to_jiffies(5000)

/* hdev->flags bits */
enum {
	HCI_UP,
	HCI_INIT,
	HCI_RUNNING,
	HCI_PSCAN,
	HCI_ISCAN,
	HCI_AUTH,
	HCI_ENCRYPT,
	HCI_INQUIRY,
	HCI_RAW,
	HCI_RESET,
};

/* hdev->dev_flags bits (hci_dev_test_flag() & co.) */
enum {
	HCI_SETUP,
	HCI_CONFIG,
	HCI_DEBUGFS_CREATED,
	HCI_POWERING_DOWN,
	HCI_AUTO_OFF,
	HCI_RFKILLED,
	HCI_MGMT,
	HCI_BONDABLE,
	HCI_SERVICE_CACHE,
	HCI_KEEP_DEBUG_KEYS,
	HCI_USE_DEBUG_KEYS,
	HCI_UNREGISTER,
	HCI_UNCONFIGURED,
	HCI_USER_CHANNEL,
	HCI_EXT_CONFIGURED,
	HCI_LE_ADV,
	HCI_LE_SCAN,
	HCI_SSP_ENABLED,
	HCI_SC_ENABLED,
	HCI_SC_ONLY,
	HCI_PRIVACY,
	HCI_LIMITED_PRIVACY,
	HCI_RPA_EXPIRED,
	HCI_RPA_RESOLVING,
	HCI_HS_ENABLED,
	HCI_LE_ENABLED,
	HCI_ADVERTISING,
	HCI_ADVERTISING_CONNECTABLE,
	HCI_CONNECTABLE,
	HCI_DISCOVERABLE,
	HCI_LIMITED_DISCOVERABLE,
	HCI_LINK_SECURITY,
	HCI_PERIODIC_INQ,
	HCI_FAST_CONNECTABLE,
	HCI_BREDR_ENABLED,
	HCI_LE_SCAN_INTERRUPTED,
	HCI_WIDEBAND_SPEECH_ENABLED,
	HCI_EVENT_FILTER_CONFIGURED,
	HCI_PA_SYNC,
	HCI_SCO_FLOWCTL,
	HCI_DUT_MODE,
	HCI_VENDOR_DIAG,
	HCI_FORCE_BREDR_SMP,
	HCI_FORCE_STATIC_ADDR,
	HCI_LL_RPA_RESOLUTION,
	HCI_CMD_PENDING,
	HCI_FORCE_NO_MITM,
	HCI_QUALITY_REPORT,
	HCI_OFFLOAD_CODECS_ENABLED,
	HCI_LE_SIMULTANEOUS_ROLES,
	HCI_CMD_DRAIN_WORKQUEUE,
	HCI_MESH_EXPERIMENTAL,
	HCI_MESH,
	HCI_MESH_SENDING,
	__HCI_NUM_FLAGS,
};

/*
 * Driver quirks (hci_set_quirk()). Recorded and queryable; the Linux HCI
 * core's own init sequence is not run here (Haiku's bluetooth_server does
 * the controller bring-up), so most only matter for the setup paths.
 */
enum {
	HCI_QUIRK_RESET_ON_CLOSE,
	HCI_QUIRK_RAW_DEVICE,
	HCI_QUIRK_FIXUP_BUFFER_SIZE,
	HCI_QUIRK_FIXUP_INQUIRY_MODE,
	HCI_QUIRK_BROKEN_LOCAL_COMMANDS,
	HCI_QUIRK_BROKEN_STORED_LINK_KEY,
	HCI_QUIRK_BOGUS_HCI_VERSION,
	HCI_QUIRK_INVALID_BDADDR,
	HCI_QUIRK_USE_BDADDR_PROPERTY,
	HCI_QUIRK_BROKEN_ERR_DATA_REPORTING,
	HCI_QUIRK_STRICT_DUPLICATE_FILTER,
	HCI_QUIRK_SIMULTANEOUS_DISCOVERY,
	HCI_QUIRK_NON_PERSISTENT_SETUP,
	HCI_QUIRK_NON_PERSISTENT_DIAG,
	HCI_QUIRK_EXTERNAL_CONFIG,
	HCI_QUIRK_USE_BDADDR_PROPERTY_BROKEN,
	HCI_QUIRK_BROKEN_SCAN_ENABLE,
	HCI_QUIRK_VALID_LE_STATES,
	HCI_QUIRK_WIDEBAND_SPEECH_SUPPORTED,
	HCI_QUIRK_BROKEN_FILTER_CLEAR_ALL,
	HCI_QUIRK_NO_SUSPEND_NOTIFIER,
	HCI_QUIRK_LE_STATES,
	HCI_QUIRK_BROKEN_LE_CODED,
	HCI_QUIRK_BROKEN_SET_RPA_TIMEOUT,
	HCI_QUIRK_BROKEN_EXT_SCAN,
	HCI_QUIRK_BROKEN_READ_TRANSMIT_POWER,
	HCI_QUIRK_BROKEN_ENHANCED_SETUP_SYNC_CONN,
	HCI_QUIRK_BROKEN_MWS_TRANSPORT_CONFIG,
	HCI_QUIRK_BROKEN_EXT_CREATE_CONN,
	HCI_QUIRK_BROKEN_WRITE_AUTH_PAYLOAD_TIMEOUT,
	HCI_QUIRK_BROKEN_READ_ENC_KEY_SIZE,
	HCI_QUIRK_BROKEN_LE_TX_POWER,
	HCI_QUIRK_BROKEN_SET_LE_SCAN_PARAMS_AFTER_CONNECTION,
	HCI_QUIRK_BROKEN_PARK_LINK_STATUS,
	HCI_QUIRK_BROKEN_LOCAL_EXT_FEATURES_PAGE_2,
	HCI_QUIRK_BROKEN_READ_VOICE_SETTING,
	HCI_QUIRK_BROKEN_READ_PAGE_SCAN_TYPE,
	HCI_QUIRK_FIXUP_LE_EXT_ADV_REPORT_PHY,
	HCI_QUIRK_BROKEN_LE_EXT_ADV_REPORT_PHY,
	__HCI_NUM_QUIRKS,
};

/* opcodes (OGF << 10 | OCF) */
#define	hci_opcode_pack(ogf, ocf)	((__u16)(((ocf) & 0x03ff) | ((ogf) << 10)))
#define	hci_opcode_ogf(op)		((op) >> 10)
#define	hci_opcode_ocf(op)		((op) & 0x03ff)

#define	HCI_OP_NOP			0x0000
#define	HCI_OP_RESET			0x0c03
#define	HCI_OP_READ_LOCAL_VERSION	0x1001
#define	HCI_OP_READ_LOCAL_COMMANDS	0x1002
#define	HCI_OP_READ_LOCAL_FEATURES	0x1003
#define	HCI_OP_READ_BUFFER_SIZE		0x1005
#define	HCI_OP_READ_BD_ADDR		0x1009
#define	HCI_OP_WRITE_SCAN_ENABLE	0x0c1a

struct hci_rp_read_local_version {
	__u8	status;
	__u8	hci_ver;
	__le16	hci_rev;
	__u8	lmp_ver;
	__le16	manufacturer;
	__le16	lmp_subver;
} __packed;

struct hci_rp_read_bd_addr {
	__u8	status;
	bdaddr_t bdaddr;
} __packed;

/* events */
#define	HCI_EV_INQUIRY_COMPLETE		0x01
#define	HCI_EV_CONN_COMPLETE		0x03
#define	HCI_EV_DISCONN_COMPLETE		0x05
#define	HCI_EV_CMD_COMPLETE		0x0e
#define	HCI_EV_CMD_STATUS		0x0f
#define	HCI_EV_HARDWARE_ERROR		0x10
#define	HCI_EV_NUM_COMP_PKTS		0x13
#define	HCI_EV_SYNC_CONN_COMPLETE	0x2c
#define	HCI_EV_LE_META			0x3e
#define	HCI_EV_VENDOR			0xff

struct hci_ev_cmd_complete {
	__u8	ncmd;
	__le16	opcode;
} __packed;

struct hci_ev_cmd_status {
	__u8	status;
	__u8	ncmd;
	__le16	opcode;
} __packed;

struct hci_ev_hardware_error {
	__u8	code;
} __packed;

/* packet headers */
#define	HCI_COMMAND_HDR_SIZE	3
#define	HCI_EVENT_HDR_SIZE	2
#define	HCI_ACL_HDR_SIZE	4
#define	HCI_SCO_HDR_SIZE	3
#define	HCI_ISO_HDR_SIZE	4

struct hci_command_hdr {
	__le16	opcode;
	__u8	plen;
} __packed;

struct hci_event_hdr {
	__u8	evt;
	__u8	plen;
} __packed;

struct hci_acl_hdr {
	__le16	handle;		/* 12 bits handle, 4 bits flags */
	__le16	dlen;
} __packed;

struct hci_sco_hdr {
	__le16	handle;
	__u8	dlen;
} __packed;

struct hci_iso_hdr {
	__le16	handle;
	__le16	dlen;
	__u8	data[];
} __packed;

static inline struct hci_event_hdr *
hci_event_hdr(const struct sk_buff *skb)
{
	return ((struct hci_event_hdr *)skb->data);
}

static inline struct hci_acl_hdr *
hci_acl_hdr(const struct sk_buff *skb)
{
	return ((struct hci_acl_hdr *)skb->data);
}

static inline struct hci_sco_hdr *
hci_sco_hdr(const struct sk_buff *skb)
{
	return ((struct hci_sco_hdr *)skb->data);
}

static inline struct hci_iso_hdr *
hci_iso_hdr(const struct sk_buff *skb)
{
	return ((struct hci_iso_hdr *)skb->data);
}

#define	hci_handle(h)		((h) & 0x0fff)
#define	hci_flags(h)		((h) >> 12)
#define	hci_handle_pack(h, f)	((__u16)(((h) & 0x0fff) | ((f) << 12)))
#define	hci_iso_data_len(h)	((h) & 0x3fff)

/* HCI error codes used by drivers (Core Spec Vol 1 Part F) */
#define	HCI_ERROR_UNKNOWN_CONN_ID	0x02
#define	HCI_ERROR_HARDWARE_FAILURE	0x03
#define	HCI_ERROR_REMOTE_USER_TERM	0x13
#define	HCI_ERROR_LOCAL_HOST_TERM	0x16
#define	HCI_ERROR_UNSPECIFIED		0x1f

#endif /* _LKPI_NET_BLUETOOTH_HCI_H_ */
