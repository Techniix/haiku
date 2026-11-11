/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * LinuxKPI Bluetooth: <net/bluetooth/hci_core.h>, the HCI core as a
 * transport driver sees it: struct hci_dev, registration, frame
 * reception, synchronous commands. Implementation: linuxkpi_bt/lkpi_hci.c.
 *
 * Model on Haiku: Haiku's own Bluetooth stack (bluetooth_server, the
 * "bluetooth/hci/v1" module, L2CAP) is the host stack. The Linux driver is
 * a transport plus vendor setup (firmware download, quirks). Every
 * registered hci_dev appears as /dev/bluetooth/h2/<driver>/<n>; when
 * bluetooth_server brings it up (BT_UP) this core runs the driver's
 * open() and, once per device, setup(), then passes frames both ways.
 * The Linux core's own controller init sequence (reset, reading features,
 * mgmt) is NOT run: bluetooth_server does that, as with h2generic.
 */
#ifndef _LKPI_NET_BLUETOOTH_HCI_CORE_H_
#define _LKPI_NET_BLUETOOTH_HCI_CORE_H_

#include <linux/types.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/skbuff.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci.h>

struct hci_dev_stats {
	__u32	err_rx;
	__u32	err_tx;
	__u32	cmd_tx;
	__u32	evt_rx;
	__u32	acl_tx;
	__u32	acl_rx;
	__u32	sco_tx;
	__u32	sco_rx;
	__u32	byte_rx;
	__u32	byte_tx;
};

/* device core dumps: not available on Haiku (registration fails with
 * -EOPNOTSUPP), the state lets driver code compile and run unchanged */
enum devcoredump_state {
	HCI_DEVCOREDUMP_IDLE,
	HCI_DEVCOREDUMP_ACTIVE,
	HCI_DEVCOREDUMP_DONE,
	HCI_DEVCOREDUMP_ABORT,
	HCI_DEVCOREDUMP_TIMEOUT,
};

struct hci_devcoredump {
	bool			supported;	/* always false */
	enum devcoredump_state	state;
	unsigned long		timeout;
	struct delayed_work	dump_timeout;	/* initialised, never fires */
};

struct hci_dev;

typedef void (*coredump_t)(struct hci_dev *hdev);
typedef void (*dmp_hdr_t)(struct hci_dev *hdev, struct sk_buff *skb);
typedef void (*notify_change_t)(struct hci_dev *hdev, int state);

/* hdev->notify() events */
#define	HCI_NOTIFY_CONN_ADD		1
#define	HCI_NOTIFY_CONN_DEL		2
#define	HCI_NOTIFY_VOICE_SETTING	3
#define	HCI_NOTIFY_ENABLE_SCO_CVSD	4
#define	HCI_NOTIFY_ENABLE_SCO_TRANSP	5
#define	HCI_NOTIFY_DISABLE_SCO		6

/* hdev->wakeup() & suspend */
#define	SUSPEND_NOTIFIER	0

struct lkpi_hci;		/* private (lkpi_hci.c) */

struct hci_dev {
	struct list_head	list;
	struct mutex		lock;

	char			name[8];
	unsigned long		flags;		/* HCI_UP, ... */
	__u16			id;
	__u8			bus;		/* HCI_USB, ... */
	bdaddr_t		bdaddr;
	bdaddr_t		public_addr;
	bdaddr_t		random_addr;
	bdaddr_t		static_addr;
	__u8			dev_type;
	__u8			hci_ver;
	__u16			hci_rev;
	__u8			lmp_ver;
	__u16			manufacturer;
	__u16			lmp_subver;
	__u16			voice_setting;
	__u8			num_iac;
	__u16			acl_mtu;
	__u16			sco_mtu;
	__u16			le_mtu;
	__u16			iso_mtu;
	__u16			acl_pkts;
	__u16			sco_pkts;
	__u16			le_pkts;
	__u16			iso_pkts;
	__u16			msft_opcode;
	bool			aosp_capable;
	bool			aosp_quality_report;
	bool			suspended;
	unsigned int		wake_reason;

	struct hci_dev_stats	stat;
	struct hci_devcoredump	dump;
	struct device		dev;
	void			*debugfs;	/* no debugfs: always NULL */

	DECLARE_BITMAP(dev_flags, __HCI_NUM_FLAGS);
	DECLARE_BITMAP(quirk_flags, __HCI_NUM_QUIRKS);

	/* driver hooks */
	int (*open)(struct hci_dev *hdev);
	int (*close)(struct hci_dev *hdev);
	int (*flush)(struct hci_dev *hdev);
	int (*setup)(struct hci_dev *hdev);
	int (*shutdown)(struct hci_dev *hdev);
	int (*send)(struct hci_dev *hdev, struct sk_buff *skb);
	void (*notify)(struct hci_dev *hdev, unsigned int evt);
	void (*hw_error)(struct hci_dev *hdev, u8 code);
	int (*post_init)(struct hci_dev *hdev);
	int (*set_diag)(struct hci_dev *hdev, bool enable);
	int (*set_bdaddr)(struct hci_dev *hdev, const bdaddr_t *bdaddr);
	void (*cmd_timeout)(struct hci_dev *hdev);
	void (*reset)(struct hci_dev *hdev);
	bool (*wakeup)(struct hci_dev *hdev);
	int (*set_quality_report)(struct hci_dev *hdev, bool enable);
	int (*get_data_path_id)(struct hci_dev *hdev, __u8 *data_path);
	bool (*is_quality_report_evt)(struct sk_buff *skb);
	bool (*pull_quality_report_data)(struct sk_buff *skb);
	u8 (*classify_pkt_type)(struct hci_dev *hdev, struct sk_buff *skb);

	struct lkpi_hci		*lkpi;
	unsigned long		lkpi_priv[] __aligned(sizeof(void *) * 2);
};

#define	HCI_PHY_HANDLE(handle)	(handle & 0xff)

/* allocation and registration */
struct hci_dev	*hci_alloc_dev_priv(int sizeof_priv);
static inline struct hci_dev *
hci_alloc_dev(void)
{
	return (hci_alloc_dev_priv(0));
}
void	hci_free_dev(struct hci_dev *hdev);
int	hci_register_dev(struct hci_dev *hdev);
void	hci_unregister_dev(struct hci_dev *hdev);
void	hci_release_dev(struct hci_dev *hdev);
int	hci_reset_dev(struct hci_dev *hdev);
int	hci_suspend_dev(struct hci_dev *hdev);
int	hci_resume_dev(struct hci_dev *hdev);

static inline void *
hci_get_priv(struct hci_dev *hdev)
{
	return (hdev->lkpi_priv);
}

static inline void *
hci_get_drvdata(struct hci_dev *hdev)
{
	return (dev_get_drvdata(&hdev->dev));
}

static inline void
hci_set_drvdata(struct hci_dev *hdev, void *data)
{
	dev_set_drvdata(&hdev->dev, data);
}

#define	SET_HCIDEV_DEV(hdev, pdev)	((hdev)->dev.parent = (pdev))
#define	SET_HCI_DRV(hdev, drv)		do { } while (0)

#define	hci_dev_lock(d)		mutex_lock(&(d)->lock)
#define	hci_dev_unlock(d)	mutex_unlock(&(d)->lock)

#define	hci_dev_set_flag(hdev, nr)	set_bit((nr), (hdev)->dev_flags)
#define	hci_dev_clear_flag(hdev, nr)	clear_bit((nr), (hdev)->dev_flags)
#define	hci_dev_change_flag(hdev, nr)	change_bit((nr), (hdev)->dev_flags)
#define	hci_dev_test_flag(hdev, nr)	test_bit((nr), (hdev)->dev_flags)
#define	hci_dev_test_and_set_flag(hdev, nr) \
	test_and_set_bit((nr), (hdev)->dev_flags)
#define	hci_dev_test_and_clear_flag(hdev, nr) \
	test_and_clear_bit((nr), (hdev)->dev_flags)

#define	hci_set_quirk(hdev, nr)		set_bit((nr), (hdev)->quirk_flags)
#define	hci_clear_quirk(hdev, nr)	clear_bit((nr), (hdev)->quirk_flags)
#define	hci_test_quirk(hdev, nr)	test_bit((nr), (hdev)->quirk_flags)

/* frames */
int	hci_recv_frame(struct hci_dev *hdev, struct sk_buff *skb);
int	hci_recv_diag(struct hci_dev *hdev, struct sk_buff *skb);
int	hci_send_frame(struct hci_dev *hdev, struct sk_buff *skb);

/* synchronous commands: return the event parameters, or ERR_PTR() */
struct sk_buff *__hci_cmd_sync(struct hci_dev *hdev, u16 opcode, u32 plen,
	    const void *param, u32 timeout);
struct sk_buff *__hci_cmd_sync_ev(struct hci_dev *hdev, u16 opcode, u32 plen,
	    const void *param, u8 event, u32 timeout);
int	__hci_cmd_sync_status(struct hci_dev *hdev, u16 opcode, u32 plen,
	    const void *param, u32 timeout);
struct sk_buff *hci_cmd_sync(struct hci_dev *hdev, u16 opcode, u32 plen,
	    const void *param, u32 timeout);
int	__hci_cmd_send(struct hci_dev *hdev, u16 opcode, u32 plen,
	    const void *param);

typedef int (*hci_cmd_sync_work_func_t)(struct hci_dev *hdev, void *data);
typedef void (*hci_cmd_sync_work_destroy_t)(struct hci_dev *hdev, void *data,
	    int err);
int	hci_cmd_sync_queue(struct hci_dev *hdev, hci_cmd_sync_work_func_t func,
	    void *data, hci_cmd_sync_work_destroy_t destroy);
int	hci_cmd_sync_submit(struct hci_dev *hdev, hci_cmd_sync_work_func_t func,
	    void *data, hci_cmd_sync_work_destroy_t destroy);
void	hci_cmd_sync_cancel(struct hci_dev *hdev, int err);
void	hci_cmd_sync_cancel_sync(struct hci_dev *hdev, int err);

/* vendor extensions: recorded, used by Haiku's stack only if it learns */
static inline void
hci_set_msft_opcode(struct hci_dev *hdev, __u16 opcode)
{
	hdev->msft_opcode = opcode;
}

static inline void
hci_set_aosp_capable(struct hci_dev *hdev)
{
	hdev->aosp_capable = true;
}

/* connections are Haiku's (L2CAP); drivers only count them */
#define	ACL_LINK	0x01
#define	SCO_LINK	0x00
#define	ESCO_LINK	0x02
#define	LE_LINK		0x80
#define	ISO_LINK	0x83
unsigned int	hci_conn_num(struct hci_dev *hdev, __u8 type);
unsigned int	hci_conn_count(struct hci_dev *hdev);
static inline bool
hci_discovery_active(struct hci_dev *hdev)
{
	return (false);
}

/* device core dumps (not available: see struct hci_devcoredump) */
int	hci_devcd_register(struct hci_dev *hdev, coredump_t coredump,
	    dmp_hdr_t dmp_hdr, notify_change_t notify_change);
int	hci_devcd_init(struct hci_dev *hdev, u32 dump_size);
int	hci_devcd_append(struct hci_dev *hdev, struct sk_buff *skb);
int	hci_devcd_append_pattern(struct hci_dev *hdev, u8 pattern, u32 len);
int	hci_devcd_complete(struct hci_dev *hdev);
int	hci_devcd_abort(struct hci_dev *hdev);

#endif /* _LKPI_NET_BLUETOOTH_HCI_CORE_H_ */
