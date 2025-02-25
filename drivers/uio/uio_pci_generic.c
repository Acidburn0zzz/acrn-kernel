// SPDX-License-Identifier: GPL-2.0
/* uio_pci_generic - generic UIO driver for PCI 2.3 devices
 *
 * Copyright (C) 2009 Red Hat, Inc.
 * Author: Michael S. Tsirkin <mst@redhat.com>
 *
 * Since the driver does not declare any device ids, you must allocate
 * id and bind the device to the driver yourself.  For example:
 *
 * # echo "8086 10f5" > /sys/bus/pci/drivers/uio_pci_generic/new_id
 * # echo -n 0000:00:19.0 > /sys/bus/pci/drivers/e1000e/unbind
 * # echo -n 0000:00:19.0 > /sys/bus/pci/drivers/uio_pci_generic/bind
 * # ls -l /sys/bus/pci/devices/0000:00:19.0/driver
 * .../0000:00:19.0/driver -> ../../../bus/pci/drivers/uio_pci_generic
 *
 * Driver won't bind to devices which do not support the Interrupt Disable Bit
 * in the command register. All devices compliant to PCI 2.3 (circa 2002) and
 * all compliant PCI Express devices should support this bit.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uio_driver.h>
#ifdef CONFIG_PCI_MSI
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/eventfd.h>
#endif

#define DRIVER_VERSION	"0.01.0"
#define DRIVER_AUTHOR	"Michael S. Tsirkin <mst@redhat.com>"
#define DRIVER_DESC	"Generic UIO driver for PCI 2.3 devices"

#ifdef CONFIG_PCI_MSI
struct uio_msix_info {
	struct msix_entry *entries;
	struct eventfd_ctx **evts;
	int nvecs;
};
#endif

struct uio_pci_generic_dev {
	struct uio_info info;
	struct pci_dev *pdev;
#ifdef CONFIG_PCI_MSI
	struct uio_msix_info msix_info;
#endif
};

#ifdef CONFIG_PCI_MSI
static irqreturn_t uio_msix_handler(int irq, void *arg)
{
	struct eventfd_ctx *evt = arg;

	eventfd_signal(evt, 1);
	return IRQ_HANDLED;
}

static int map_msix_eventfd(struct uio_pci_generic_dev *gdev,
		int fd, int vector)
{
	int irq, err;
	struct eventfd_ctx *evt;

	/* Passing -1 is used to disable interrupt */
	if (fd < 0) {
		pci_disable_msi(gdev->pdev);
		return 0;
	}

	if (vector >= gdev->msix_info.nvecs)
		return -EINVAL;

	irq = gdev->msix_info.entries[vector].vector;
	evt = gdev->msix_info.evts[vector];
	if (evt) {
		free_irq(irq, evt);
		eventfd_ctx_put(evt);
		gdev->msix_info.evts[vector] = NULL;
	}

	evt = eventfd_ctx_fdget(fd);
	if (!evt)
		return -EINVAL;

	err = request_irq(irq, uio_msix_handler, 0, "UIO IRQ", evt);
	if (err) {
		eventfd_ctx_put(evt);
		return err;
	}

	gdev->msix_info.evts[vector] = evt;
	return 0;
}

static int uio_msi_ioctl(struct uio_info *info, unsigned int cmd,
		unsigned long arg)
{
	struct uio_pci_generic_dev *gdev;
	struct uio_msix_data data;
	int err = -EOPNOTSUPP;

	gdev = container_of(info, struct uio_pci_generic_dev, info);

	switch (cmd) {
	case UIO_MSIX_DATA: {
		if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
			return -EFAULT;

		err = map_msix_eventfd(gdev, data.fd, data.vector);
		break;
	}
	default:
		pr_warn("Not support ioctl cmd: 0x%x\n", cmd);
		break;
	}

	return err;
}

static int pci_generic_init_msix(struct uio_pci_generic_dev *gdev)
{
	unsigned char *buffer;
	int i, nvecs, ret;

	nvecs = pci_msix_vec_count(gdev->pdev);
	if (!nvecs)
		return -EINVAL;

	buffer = kzalloc(nvecs * (sizeof(struct msix_entry) +
			sizeof(struct eventfd_ctx *)), GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	gdev->msix_info.entries = (struct msix_entry *)buffer;
	gdev->msix_info.evts = (struct eventfd_ctx **)
		((unsigned char *)buffer + nvecs * sizeof(struct msix_entry));
	gdev->msix_info.nvecs = nvecs;

	for (i = 0; i < nvecs; ++i)
		gdev->msix_info.entries[i].entry = i;

	ret = pci_enable_msix_exact(gdev->pdev, gdev->msix_info.entries, nvecs);
	if (ret < 0) {
		kfree(gdev->msix_info.entries);
		gdev->msix_info.entries = NULL;
	}

	return ret;
}
#endif

static inline struct uio_pci_generic_dev *
to_uio_pci_generic_dev(struct uio_info *info)
{
	return container_of(info, struct uio_pci_generic_dev, info);
}

static int release(struct uio_info *info, struct inode *inode)
{
	struct uio_pci_generic_dev *gdev = to_uio_pci_generic_dev(info);

	/*
	 * This driver is insecure when used with devices doing DMA, but some
	 * people (mis)use it with such devices.
	 * Let's at least make sure DMA isn't left enabled after the userspace
	 * driver closes the fd.
	 * Note that there's a non-zero chance doing this will wedge the device
	 * at least until reset.
	 */
	pci_clear_master(gdev->pdev);
	return 0;
}

/* Interrupt handler. Read/modify/write the command register to disable
 * the interrupt. */
static irqreturn_t irqhandler(int irq, struct uio_info *info)
{
	struct uio_pci_generic_dev *gdev = to_uio_pci_generic_dev(info);

	if (!pci_check_and_mask_intx(gdev->pdev))
		return IRQ_NONE;

	/* UIO core will signal the user process. */
	return IRQ_HANDLED;
}

static int probe(struct pci_dev *pdev,
			   const struct pci_device_id *id)
{
	struct uio_pci_generic_dev *gdev;
	int err;

	err = pci_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "%s: pci_enable_device failed: %d\n",
			__func__, err);
		return err;
	}

	if (pdev->irq && !pci_intx_mask_supported(pdev)) {
		err = -ENODEV;
		goto err_verify;
	}

	gdev = kzalloc(sizeof(struct uio_pci_generic_dev), GFP_KERNEL);
	if (!gdev) {
		err = -ENOMEM;
		goto err_alloc;
	}

	gdev->info.name = "uio_pci_generic";
	gdev->info.version = DRIVER_VERSION;
	gdev->info.release = release;
#ifdef CONFIG_PCI_MSI
	gdev->info.ioctl = uio_msi_ioctl;
#endif
	gdev->pdev = pdev;

	if (pdev->irq) {
		gdev->info.irq = pdev->irq;
		gdev->info.irq_flags = IRQF_SHARED;
		gdev->info.handler = irqhandler;
	} else {
#ifdef CONFIG_PCI_MSI
		err = pci_generic_init_msix(gdev);
		if (!err)
			dev_notice(&pdev->dev, "MSIX is enabled for UIO device.\n");
#else
		dev_warn(&pdev->dev, "No IRQ assigned to device: "
			 "no support for interrupts?\n");
#endif
	}

	err = uio_register_device(&pdev->dev, &gdev->info);
	if (err)
		goto err_register;
	pci_set_drvdata(pdev, gdev);

	return 0;

err_register:
#ifdef CONFIG_PCI_MSI
	if (gdev->msix_info.entries != NULL)
		kfree(gdev->msix_info.entries);
#endif
	kfree(gdev);
err_alloc:
err_verify:
	pci_disable_device(pdev);
	return err;
}

static void remove(struct pci_dev *pdev)
{
#ifdef CONFIG_PCI_MSI
	int i, irq;
	struct eventfd_ctx *evt;
#endif
	struct uio_pci_generic_dev *gdev = pci_get_drvdata(pdev);

	uio_unregister_device(&gdev->info);

#ifdef CONFIG_PCI_MSI
	if (gdev->msix_info.entries != NULL) {
		for (i = 0; i < gdev->msix_info.nvecs; i++) {
			irq = gdev->msix_info.entries[i].vector;
			evt = gdev->msix_info.evts[i];
			if (evt) {
				free_irq(irq, evt);
				eventfd_ctx_put(evt);
				gdev->msix_info.evts[i] = NULL;
			}
		}
		pci_disable_msix(pdev);
		kfree(gdev->msix_info.entries);
		gdev->msix_info.entries = NULL;
	}
#endif
	pci_disable_device(pdev);
	kfree(gdev);
}

static struct pci_driver uio_pci_driver = {
	.name = "uio_pci_generic",
	.id_table = NULL, /* only dynamic id's */
	.probe = probe,
	.remove = remove,
};

module_pci_driver(uio_pci_driver);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
