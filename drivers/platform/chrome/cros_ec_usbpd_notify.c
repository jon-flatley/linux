// SPDX-License-Identifier: GPL-2.0
/*
 * cros_ec_usbpd - ChromeOS EC Power Delivery Driver
 *
 * Copyright (C) 2019 Google, Inc
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * This driver serves as the receiver of cros_ec PD host events.
 */

#include <linux/acpi.h>
#include <linux/mfd/cros_ec.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_data/cros_ec_commands.h>
#include <linux/platform_data/cros_ec_usbpd_notify.h>
#include <linux/platform_data/cros_ec_proto.h>
#include <linux/platform_device.h>

#define DRV_NAME "cros-ec-usbpd-notify"
#define ACPI_DRV_NAME "GOOG0003"

static BLOCKING_NOTIFIER_HEAD(cros_ec_usbpd_notifier_list);

int cros_ec_usbpd_register_notify(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(
			&cros_ec_usbpd_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(cros_ec_usbpd_register_notify);

void cros_ec_usbpd_unregister_notify(struct notifier_block *nb)
{
	blocking_notifier_chain_unregister(&cros_ec_usbpd_notifier_list, nb);
}
EXPORT_SYMBOL_GPL(cros_ec_usbpd_unregister_notify);

static void cros_ec_usbpd_notify(u32 event)
{
	blocking_notifier_call_chain(&cros_ec_usbpd_notifier_list, event, NULL);
}

#ifdef CONFIG_ACPI

static int cros_ec_usbpd_add_acpi(struct acpi_device *adev)
{
	return 0;
}

static int cros_ec_usbpd_remove_acpi(struct acpi_device *adev)
{
	return 0;
}
static void cros_ec_usbpd_notify_acpi(struct acpi_device *adev, u32 event)
{
	cros_ec_usbpd_notify(event);
}

static const struct acpi_device_id cros_ec_usbpd_acpi_device_ids[] = {
	{ ACPI_DRV_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, cros_ec_usbpd_acpi_device_ids);

static struct acpi_driver cros_ec_usbpd_driver = {
	.name = DRV_NAME,
	.class = DRV_NAME,
	.ids = cros_ec_usbpd_acpi_device_ids,
	.ops = {
		.add = cros_ec_usbpd_add_acpi,
		.remove = cros_ec_usbpd_remove_acpi,
		.notify = cros_ec_usbpd_notify_acpi,
	},
};
module_acpi_driver(cros_ec_usbpd_driver);

#else /* CONFIG_ACPI */

static int cros_ec_usbpd_notify_plat(struct notifier_block *nb,
		unsigned long queued_during_suspend, void *data)
{
	struct cros_ec_device *ec_dev = (struct cros_ec_device *)data;
	u32 host_event = cros_ec_get_host_event(ec_dev);

	if (host_event & EC_HOST_EVENT_MASK(EC_HOST_EVENT_PD_MCU)) {
		cros_ec_usbpd_notify(host_event);
		return NOTIFY_OK;
	} else {
		return NOTIFY_DONE;
	}

}

static int cros_ec_usbpd_probe_plat(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cros_ec_device *ec_dev = dev_get_drvdata(dev->parent);
	struct notifier_block *nb;
	int ret;

	nb = devm_kzalloc(dev, sizeof(*nb), GFP_KERNEL);
	if (!nb)
		return -ENOMEM;

	nb->notifier_call = cros_ec_usbpd_notify_plat;
	dev_set_drvdata(dev, nb);
	ret = blocking_notifier_chain_register(&ec_dev->event_notifier, nb);

	if (ret < 0)
		dev_warn(dev, "Failed to register notifier\n");

	return 0;
}

static int cros_ec_usbpd_remove_plat(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cros_ec_device *ec_dev = dev_get_drvdata(dev->parent);
	struct notifier_block *nb =
		(struct notifier_block *)dev_get_drvdata(dev);

	blocking_notifier_chain_unregister(&ec_dev->event_notifier, nb);

	return 0;
}

static const struct of_device_id cros_ec_usbpd_of_match[] = {
	{ .compatible = "google,cros-ec-pd-update" },
	{ }
};
MODULE_DEVICE_TABLE(of, cros_ec_usbpd_of_match);

static struct platform_driver cros_ec_usbpd_driver = {
	.driver = {
		.name = DRV_NAME,
		.of_match_table = of_match_ptr(cros_ec_usbpd_of_match),
	},
	.probe = cros_ec_usbpd_probe_plat,
	.remove = cros_ec_usbpd_remove_plat,
};
module_platform_driver(cros_ec_usbpd_driver);

#endif /* CONFIG_ACPI */

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ChromeOS power delivery device");
MODULE_AUTHOR("Jon Flatley <jflat@chromium.org>");
MODULE_ALIAS("platform:" DRV_NAME);
