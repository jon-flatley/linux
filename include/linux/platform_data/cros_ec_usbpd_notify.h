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
 */

#ifndef __LINUX_PLATFORM_DATA_CROS_EC_USBPD_NOTIFY_H
#define __LINUX_PLATFORM_DATA_CROS_EC_USBPD_NOTIFY_H

#include <linux/notifier.h>

/**
 * cros_ec_usbpd_register_notify - register a notifier callback for USB PD
 * events. On ACPI platforms this corrisponds to to host events on the ECPD
 * "GOOG0003" ACPI device. On non-ACPI platforms this will filter mkbp events
 * for USB PD events.
 *
 * @nb: Notifier block pointer to register
 */
int cros_ec_usbpd_register_notify(struct notifier_block *nb);

/**
 * cros_ec_usbpd_unregister_notify - unregister a notifier callback that was
 * previously registered with cros_ec_usbpd_register_notify().
 *
 * @nb: Notifier block pointer to unregister
 */
void cros_ec_usbpd_unregister_notify(struct notifier_block *nb);

#endif  /* __LINUX_PLATFORM_DATA_CROS_EC_USBPD_NOTIFY_H */
