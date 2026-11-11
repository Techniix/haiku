/*
 * Copyright 2026, LinuxKPI_HaikuOS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Stage (a)/(b) test driver, written purely against the Linux kernel API.
 * It must build unmodified on Linux too; on Haiku it runs on LinuxKPI.
 *
 * (a) attach to a MediaTek MT7921 (or any listed ID), map BAR0, allocate one
 *     MSI vector, install a handler, log everything, tear it all down.
 * (b) request_firmware() the MT7921 blobs and log size + checksum.
 *
 * Self-test in probe(): Haiku's freebsd_network keeps a driver loaded only
 * if it attached a network interface, and does not detach devices when it
 * gives up. So this driver runs the whole setup/teardown sequence inside
 * probe() and then declines the device with -ENODEV on purpose; Haiku then
 * deletes the device and unloads the driver cleanly. Everything is logged to
 * syslog with the prefix "stage a:" / "stage b:".
 *
 * Only MSI is requested: with legacy INTx the freebsd_network glue cannot
 * know whether a shared interrupt was ours (see docs/ARCHITECTURE.md).
 *
 * QEMU "edu" device (1234:11e8): the same driver also runs a functional
 * test of the LinuxKPI core against a device that really does MMIO, MSI and
 * DMA, so the core is covered without MediaTek hardware (Renga's QEMU
 * harness attaches one; 3rdparty/ce/tests/linuxkpi). See pcitest_edu()
 * below for what it checks; results are lines "edu: <check> PASS|FAIL ...".
 */
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/firmware.h>
#include <linux/crc32.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <asm/io.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/workqueue.h>

#define DRV_NAME "lkpi_pcitest"

static bool load_firmware = true;
module_param(load_firmware, bool, 0444);
MODULE_PARM_DESC(load_firmware, "stage (b): request the MT7921/MT7922 firmware");

/* Same file names as Linux mt7921 (mt7921.h): MT7922 parts (0616, 7922)
 * use the MT7922 blobs, everything else the MT7961 ones. */
static const char *const fw_names_7961[] = {
	"mediatek/WIFI_MT7961_patch_mcu_1_2_hdr.bin",
	"mediatek/WIFI_RAM_CODE_MT7961_1.bin",
};
static const char *const fw_names_7922[] = {
	"mediatek/WIFI_MT7922_patch_mcu_1_1_hdr.bin",
	"mediatek/WIFI_RAM_CODE_MT7922_1.bin",
};

struct pcitest {
	struct pci_dev	*pdev;
	void __iomem	*bar0;
	int		irq;
	atomic_t	irqs;
	bool		edu;		/* QEMU edu device: claim its interrupts */
	spinlock_t	irq_lock;
	u32		irq_status;	/* edu: status bits seen by the handler */
	struct completion irq_done;
};

/*
 * QEMU's "edu" educational device (hw/misc/edu.c, docs/specs/edu.rst in
 * QEMU). Registers are 32 bit below 0x80, 32 or 64 bit from 0x80.
 */
#define	EDU_VENDOR		0x1234
#define	EDU_DEVICE		0x11e8
#define	EDU_REG_ID		0x00	/* 0xRRrr00ed */
#define	EDU_REG_LIVENESS	0x04	/* reads back the inverse */
#define	EDU_REG_FACTORIAL	0x08
#define	EDU_REG_STATUS		0x20	/* 0x01 computing, 0x80 irq when done */
#define	EDU_REG_IRQ_STATUS	0x24
#define	EDU_REG_IRQ_RAISE	0x60
#define	EDU_REG_IRQ_ACK		0x64
#define	EDU_REG_DMA_SRC		0x80
#define	EDU_REG_DMA_DST		0x88
#define	EDU_REG_DMA_COUNT	0x90
#define	EDU_REG_DMA_CMD		0x98	/* 1 start, 2 EDU->RAM, 4 irq 0x100 */
#define	EDU_STATUS_IRQFACT	0x80
#define	EDU_IRQ_FACT		0x001
#define	EDU_IRQ_DMA		0x100
#define	EDU_DMA_START		0x01
#define	EDU_DMA_TO_RAM		0x02
#define	EDU_DMA_IRQ		0x04
#define	EDU_BUF			0x40000	/* device buffer, 4 KiB */
#define	EDU_BUF_SIZE		4096
/*
 * The device masks DMA addresses with its "dma_mask" property (28 bits by
 * default); an address above it is clamped and QEMU logs "EDU: clamping
 * DMA". The driver uses 24 bits, which fits the default too; Renga's
 * harness starts the device with dma_mask=0xffffff, so that nearly all of
 * a guest's memory lies above the mask and streaming mappings have to be
 * bounced by the DMA API (a 28-bit mask is rarely exceeded in a 1 GiB
 * guest: Haiku hands out low memory first).
 */
#define	EDU_DMA_BITS		24

static irqreturn_t
pcitest_irq(int irq, void *arg)
{
	struct pcitest *t = arg;

	atomic_inc(&t->irqs);
	if (t->edu) {
		u32 status = readl(t->bar0 + EDU_REG_IRQ_STATUS);
		unsigned long flags;

		if (status == 0)
			return IRQ_NONE;
		writel(status, t->bar0 + EDU_REG_IRQ_ACK);
		spin_lock_irqsave(&t->irq_lock, flags);
		t->irq_status |= status;
		spin_unlock_irqrestore(&t->irq_lock, flags);
		complete(&t->irq_done);
		return IRQ_HANDLED;
	}
	/* We never enabled any interrupt source on the device, so any hit is
	 * spurious or shared: tell the core it was not ours. */
	return IRQ_NONE;
}

static void
pcitest_firmware(struct pcitest *t)
{
	const struct firmware *fw;
	const char *const *fw_names;
	unsigned int i;
	int err;

	fw_names = (t->pdev->device == 0x0616 || t->pdev->device == 0x7922) ?
	    fw_names_7922 : fw_names_7961;
	for (i = 0; i < 2; i++) {
		err = request_firmware(&fw, fw_names[i], &t->pdev->dev);
		if (err != 0) {
			dev_err(&t->pdev->dev, "stage b: %s: error %d\n",
			    fw_names[i], err);
			continue;
		}
		dev_info(&t->pdev->dev, "stage b: %s: %zu bytes, crc32 %08x\n",
		    fw_names[i], fw->size, crc32_le(~0, fw->data, fw->size) ^ ~0);
		release_firmware(fw);
	}
}


/*
 * QEMU edu functional test of the LinuxKPI core. Each check prints
 * "edu: <name> PASS|FAIL <detail>" (Renga's harness turns them into checks).
 */
#define	EDU_RESULT(name, ok, fmt, ...)					\
	dev_info(&t->pdev->dev, "edu: %s %s " fmt "\n", (name),		\
	    (ok) ? "PASS" : "FAIL", ##__VA_ARGS__)

/* Waits for the handler to see any of "bits"; returns the bits seen. */
static u32
edu_wait_irq(struct pcitest *t, u32 bits, unsigned int ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(ms);
	unsigned long flags;
	u32 seen;

	for (;;) {
		spin_lock_irqsave(&t->irq_lock, flags);
		seen = t->irq_status & bits;
		t->irq_status &= ~seen;
		spin_unlock_irqrestore(&t->irq_lock, flags);
		if (seen != 0 || time_after(jiffies, deadline))
			return (seen);
		wait_for_completion_timeout(&t->irq_done, msecs_to_jiffies(50));
	}
}

/* One DMA transfer (count bytes) through the device; waits for its irq. */
static bool
edu_dma(struct pcitest *t, dma_addr_t ram, bool to_ram, size_t count)
{
	u64 cmd = EDU_DMA_START | EDU_DMA_IRQ | (to_ram ? EDU_DMA_TO_RAM : 0);

	writeq(to_ram ? EDU_BUF : ram, t->bar0 + EDU_REG_DMA_SRC);
	writeq(to_ram ? ram : EDU_BUF, t->bar0 + EDU_REG_DMA_DST);
	writeq(count, t->bar0 + EDU_REG_DMA_COUNT);
	reinit_completion(&t->irq_done);
	writeq(cmd, t->bar0 + EDU_REG_DMA_CMD);
	/* QEMU runs the transfer from a 100 ms timer */
	if ((edu_wait_irq(t, EDU_IRQ_DMA, 2000) & EDU_IRQ_DMA) == 0)
		return (false);
	return ((readq(t->bar0 + EDU_REG_DMA_CMD) & EDU_DMA_START) == 0);
}

static void
edu_fill(u8 *p, size_t len, u8 seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		p[i] = (u8)(seed + i * 7 + (i >> 8));
}

static bool
edu_check(const u8 *p, size_t len, u8 seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (p[i] != (u8)(seed + i * 7 + (i >> 8)))
			return (false);
	return (true);
}

/* timer_list, delayed work and hrtimer: run in their own contexts and
 * complete a completion each; the test measures when they fired. */
struct edu_timers {
	struct timer_list	timer;
	struct delayed_work	dwork;
	struct hrtimer		hrtimer;
	struct completion	timer_done, dwork_done, hrtimer_done;
	unsigned long		timer_at, dwork_at;
	ktime_t			hrtimer_at;
};

static void
edu_timer_fn(struct timer_list *tl)
{
	struct edu_timers *et = timer_container_of(et, tl, timer);

	et->timer_at = jiffies;
	complete(&et->timer_done);
}

static void
edu_dwork_fn(struct work_struct *work)
{
	struct edu_timers *et = container_of(to_delayed_work(work),
	    struct edu_timers, dwork);

	et->dwork_at = jiffies;
	complete(&et->dwork_done);
}

static enum hrtimer_restart
edu_hrtimer_fn(struct hrtimer *hr)
{
	struct edu_timers *et = container_of(hr, struct edu_timers, hrtimer);

	et->hrtimer_at = ktime_get();
	complete(&et->hrtimer_done);
	return (HRTIMER_NORESTART);
}

static void
pcitest_edu_timers(struct pcitest *t)
{
	struct edu_timers *et;
	unsigned long j0;
	ktime_t k0;
	long r;

	et = kzalloc(sizeof(*et), GFP_KERNEL);
	if (et == NULL) {
		EDU_RESULT("timers", false, "no memory");
		return;
	}
	init_completion(&et->timer_done);
	init_completion(&et->dwork_done);
	init_completion(&et->hrtimer_done);

	timer_setup(&et->timer, edu_timer_fn, 0);
	j0 = jiffies;
	mod_timer(&et->timer, j0 + msecs_to_jiffies(30));
	r = wait_for_completion_timeout(&et->timer_done, msecs_to_jiffies(2000));
	EDU_RESULT("timer_list", r > 0 && et->timer_at - j0 >= 25,
	    "fired after %lu ms (want 30)", r > 0 ? et->timer_at - j0 : 0UL);
	/* re-arm and delete before it fires: must never run */
	reinit_completion(&et->timer_done);
	mod_timer(&et->timer, jiffies + msecs_to_jiffies(40));
	r = timer_delete_sync(&et->timer);
	msleep(80);
	EDU_RESULT("timer_cancel", r == 1 && !completion_done(&et->timer_done),
	    "timer_delete_sync %ld, ran %d", r, completion_done(&et->timer_done));

	INIT_DELAYED_WORK(&et->dwork, edu_dwork_fn);
	j0 = jiffies;
	schedule_delayed_work(&et->dwork, msecs_to_jiffies(30));
	r = wait_for_completion_timeout(&et->dwork_done, msecs_to_jiffies(2000));
	EDU_RESULT("delayed_work", r > 0 && et->dwork_at - j0 >= 25,
	    "ran after %lu ms (want 30)", r > 0 ? et->dwork_at - j0 : 0UL);
	reinit_completion(&et->dwork_done);
	schedule_delayed_work(&et->dwork, msecs_to_jiffies(40));
	r = cancel_delayed_work_sync(&et->dwork);
	msleep(80);
	EDU_RESULT("delayed_work_cancel",
	    r && !completion_done(&et->dwork_done), "cancel %ld, ran %d", r,
	    completion_done(&et->dwork_done));

	hrtimer_setup(&et->hrtimer, edu_hrtimer_fn, CLOCK_MONOTONIC,
	    HRTIMER_MODE_REL);
	k0 = ktime_get();
	hrtimer_start(&et->hrtimer, ms_to_ktime(5), HRTIMER_MODE_REL);
	r = wait_for_completion_timeout(&et->hrtimer_done,
	    msecs_to_jiffies(2000));
	EDU_RESULT("hrtimer", r > 0 &&
	    ktime_to_us(ktime_sub(et->hrtimer_at, k0)) >= 4500,
	    "fired after %lld us (want 5000)",
	    r > 0 ? (long long)ktime_to_us(ktime_sub(et->hrtimer_at, k0)) : 0LL);
	hrtimer_cancel(&et->hrtimer);
	kfree(et);
}

static void
pcitest_edu(struct pcitest *t)
{
	struct device *dev = &t->pdev->dev;
	u32 id, v, seen;
	void *coh;
	dma_addr_t coh_dma, dma;
	u8 *bufs[64], *high = NULL, *back;
	unsigned int i, nbufs = 0;
	bool ok;

	id = readl(t->bar0 + EDU_REG_ID);
	EDU_RESULT("mmio_id", (id & 0xffff) == 0x00ed, "id %#x", id);
	writel(0x12345678, t->bar0 + EDU_REG_LIVENESS);
	v = readl(t->bar0 + EDU_REG_LIVENESS);
	EDU_RESULT("mmio_liveness", v == ~0x12345678u, "wrote 0x12345678 read %#x",
	    v);

	/* MSI: the device raises the value written to the raise register */
	reinit_completion(&t->irq_done);
	writel(0x40, t->bar0 + EDU_REG_IRQ_RAISE);
	seen = edu_wait_irq(t, 0x40, 1000);
	EDU_RESULT("msi_raise", seen == 0x40 && t->pdev->msi_enabled,
	    "status %#x, msi %d, %d interrupts", seen, t->pdev->msi_enabled,
	    atomic_read(&t->irqs));

	/* computation with completion interrupt */
	writel(EDU_STATUS_IRQFACT, t->bar0 + EDU_REG_STATUS);
	reinit_completion(&t->irq_done);
	writel(10, t->bar0 + EDU_REG_FACTORIAL);
	seen = edu_wait_irq(t, EDU_IRQ_FACT, 2000);
	v = readl(t->bar0 + EDU_REG_FACTORIAL);
	EDU_RESULT("msi_factorial", seen == EDU_IRQ_FACT && v == 3628800,
	    "10! = %u, irq %#x", v, seen);
	writel(0, t->bar0 + EDU_REG_STATUS);

	/* DMA */
	if (dma_set_mask_and_coherent(dev, DMA_BIT_MASK(EDU_DMA_BITS)) != 0) {
		EDU_RESULT("dma_mask", false, "dma_set_mask_and_coherent failed");
		return;
	}
	coh = dma_alloc_coherent(dev, EDU_BUF_SIZE, &coh_dma, GFP_KERNEL);
	EDU_RESULT("dma_coherent_alloc", coh != NULL &&
	    coh_dma + EDU_BUF_SIZE - 1 <= DMA_BIT_MASK(EDU_DMA_BITS),
	    "bus %#llx", (unsigned long long)coh_dma);
	if (coh == NULL)
		return;
	edu_fill(coh, EDU_BUF_SIZE, 0x11);
	ok = edu_dma(t, coh_dma, false, EDU_BUF_SIZE);
	memset(coh, 0, EDU_BUF_SIZE);
	ok = ok && edu_dma(t, coh_dma, true, EDU_BUF_SIZE);
	EDU_RESULT("dma_coherent_roundtrip", ok && edu_check(coh, EDU_BUF_SIZE,
	    0x11), "4096 bytes to the device and back");

	/*
	 * Streaming DMA above the device's mask: kmalloc() until a buffer
	 * lies above 16 MiB (most of a guest's memory does), map it, let the
	 * device read it, then let the device write into another mapping of
	 * it. Without bouncing QEMU would clamp the address (and read or
	 * write the wrong memory).
	 */
	for (i = 0; i < ARRAY_SIZE(bufs) && high == NULL; i++) {
		bufs[i] = kmalloc(EDU_BUF_SIZE, GFP_KERNEL);
		if (bufs[i] == NULL)
			break;
		nbufs++;
		if (virt_to_phys(bufs[i]) + EDU_BUF_SIZE - 1 >
		    DMA_BIT_MASK(EDU_DMA_BITS))
			high = bufs[i];
	}
	if (high == NULL) {
		dev_info(dev, "edu: INFO no buffer above the DMA mask in %u "
		    "allocations; bounce path not reached\n", nbufs);
		high = nbufs > 0 ? bufs[0] : NULL;
	}
	back = kmalloc(EDU_BUF_SIZE, GFP_KERNEL);
	if (high != NULL && back != NULL) {
		u64 phys = virt_to_phys(high);

		edu_fill(high, EDU_BUF_SIZE, 0x5a);
		dma = dma_map_single(dev, high, EDU_BUF_SIZE, DMA_TO_DEVICE);
		ok = !dma_mapping_error(dev, dma) &&
		    dma + EDU_BUF_SIZE - 1 <= DMA_BIT_MASK(EDU_DMA_BITS);
		EDU_RESULT("dma_map_bounce", ok, "phys %#llx -> bus %#llx%s",
		    (unsigned long long)phys, (unsigned long long)dma,
		    phys != dma ? " (bounced)" : "");
		ok = ok && edu_dma(t, dma, false, EDU_BUF_SIZE);
		if (!dma_mapping_error(dev, dma))
			dma_unmap_single(dev, dma, EDU_BUF_SIZE, DMA_TO_DEVICE);

		/* device -> RAM into the same high buffer, then compare */
		memset(high, 0, EDU_BUF_SIZE);
		dma = dma_map_single(dev, high, EDU_BUF_SIZE, DMA_FROM_DEVICE);
		ok = ok && !dma_mapping_error(dev, dma) &&
		    edu_dma(t, dma, true, EDU_BUF_SIZE);
		if (!dma_mapping_error(dev, dma))
			dma_unmap_single(dev, dma, EDU_BUF_SIZE,
			    DMA_FROM_DEVICE);
		EDU_RESULT("dma_streaming_roundtrip",
		    ok && edu_check(high, EDU_BUF_SIZE, 0x5a),
		    "4096 bytes at phys %#llx to the device and back",
		    (unsigned long long)phys);

		/* dma_sync_single_for_cpu() on a bidirectional mapping */
		memset(back, 0, EDU_BUF_SIZE);
		dma = dma_map_single(dev, back, EDU_BUF_SIZE, DMA_BIDIRECTIONAL);
		ok = !dma_mapping_error(dev, dma) &&
		    edu_dma(t, dma, true, EDU_BUF_SIZE);
		if (!dma_mapping_error(dev, dma)) {
			dma_sync_single_for_cpu(dev, dma, EDU_BUF_SIZE,
			    DMA_BIDIRECTIONAL);
			ok = ok && edu_check(back, EDU_BUF_SIZE, 0x5a);
			dma_unmap_single(dev, dma, EDU_BUF_SIZE,
			    DMA_BIDIRECTIONAL);
		}
		EDU_RESULT("dma_sync_for_cpu", ok, "phys %#llx",
		    (unsigned long long)virt_to_phys(back));
	} else
		EDU_RESULT("dma_streaming_roundtrip", false, "no memory");
	kfree(back);
	for (i = 0; i < nbufs; i++)
		kfree(bufs[i]);
	dma_free_coherent(dev, EDU_BUF_SIZE, coh, coh_dma);

	pcitest_edu_timers(t);
	EDU_RESULT("done", true, "%d interrupts", atomic_read(&t->irqs));
}

static void pcitest_teardown(struct pcitest *t);

static int
pcitest_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct pcitest *t;
	int err;

	dev_info(&pdev->dev, "stage a: probe %04x:%04x (subsys %04x:%04x) rev %02x\n",
	    pdev->vendor, pdev->device, pdev->subsystem_vendor,
	    pdev->subsystem_device, pdev->revision);

	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (t == NULL)
		return -ENOMEM;
	t->pdev = pdev;
	atomic_set(&t->irqs, 0);
	t->edu = pdev->vendor == EDU_VENDOR && pdev->device == EDU_DEVICE;
	spin_lock_init(&t->irq_lock);
	init_completion(&t->irq_done);

	err = pci_enable_device(pdev);
	if (err != 0)
		goto fail_free;
	pci_set_master(pdev);

	err = pci_request_regions(pdev, DRV_NAME);
	if (err != 0)
		goto fail_disable;

	t->bar0 = pci_iomap(pdev, 0, 0);
	if (t->bar0 == NULL) {
		err = -ENOMEM;
		goto fail_regions;
	}
	dev_info(&pdev->dev, "stage a: BAR0 start %#llx len %#llx mapped at %p\n",
	    (unsigned long long)pci_resource_start(pdev, 0),
	    (unsigned long long)pci_resource_len(pdev, 0), t->bar0);

	err = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
	if (err < 0)
		goto fail_unmap;
	t->irq = pci_irq_vector(pdev, 0);
	err = request_irq(t->irq, pcitest_irq, IRQF_SHARED, DRV_NAME, t);
	if (err != 0)
		goto fail_vectors;
	dev_info(&pdev->dev, "stage a: irq %d installed (msi=%d)\n", t->irq,
	    pdev->msi_enabled);

	pci_set_drvdata(pdev, t);

	if (t->edu)
		pcitest_edu(t);
	else if (load_firmware)
		pcitest_firmware(t);

	dev_info(&pdev->dev, "stage a: attach OK, tearing down again\n");
	pcitest_teardown(t);
	dev_info(&pdev->dev, "stage a: self-test complete; declining the "
	    "device on purpose (-ENODEV)\n");
	return -ENODEV;

fail_vectors:
	pci_free_irq_vectors(pdev);
fail_unmap:
	pci_iounmap(pdev, t->bar0);
fail_regions:
	pci_release_regions(pdev);
fail_disable:
	pci_disable_device(pdev);
fail_free:
	kfree(t);
	dev_err(&pdev->dev, "stage a: probe failed: %d\n", err);
	return err;
}

static void
pcitest_teardown(struct pcitest *t)
{
	struct pci_dev *pdev = t->pdev;

	free_irq(t->irq, t);
	pci_free_irq_vectors(pdev);
	pci_iounmap(pdev, t->bar0);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
	dev_info(&pdev->dev, "stage a: detach OK (%d interrupts seen)\n",
	    atomic_read(&t->irqs));
	kfree(t);
}

static void
pcitest_remove(struct pci_dev *pdev)
{
	struct pcitest *t = pci_get_drvdata(pdev);

	/* Not reached in self-test mode (probe declines); kept so the driver
	 * is also a normal, complete Linux PCI driver. */
	if (t != NULL)
		pcitest_teardown(t);
}

static const struct pci_device_id pcitest_ids[] = {
	{ PCI_DEVICE(0x14c3, 0x7961) },	/* MT7921 */
	{ PCI_DEVICE(0x14c3, 0x0608) },	/* MT7921K / RZ608 */
	{ PCI_DEVICE(0x14c3, 0x0616) },	/* MT7922 variant / RZ616 */
	{ PCI_DEVICE(0x14c3, 0x7922) },	/* MT7922 */
	{ PCI_DEVICE(EDU_VENDOR, EDU_DEVICE) },	/* QEMU edu (core test) */
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, pcitest_ids);

static struct pci_driver pcitest_driver = {
	.name		= DRV_NAME,
	.id_table	= pcitest_ids,
	.probe		= pcitest_probe,
	.remove		= pcitest_remove,
};
module_pci_driver(pcitest_driver);

MODULE_DESCRIPTION("LinuxKPI_HaikuOS stage a/b PCI test driver");
MODULE_LICENSE("Dual MIT/GPL");
