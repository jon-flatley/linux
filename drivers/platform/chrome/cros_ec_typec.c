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

#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/mfd/cros_ec.h>
#include <linux/platform_data/cros_ec_commands.h>
#include <linux/platform_data/cros_ec_usbpd_notify.h>
#include <linux/platform_data/cros_ec_proto.h>
#include <linux/usb/typec.h>
#include <linux/slab.h>

#define DRV_NAME "cros-ec-typec"

struct port_data {
	int port_num;
	struct typec_port *port;
	struct typec_partner *partner;
	struct usb_pd_identity p_identity;
	struct typec_data *typec;
	struct typec_capability caps;
};

struct typec_data {
	struct device *dev;
	struct cros_ec_dev *ec_dev;
	struct port_data *ports[EC_USB_PD_MAX_PORTS];
	unsigned int num_ports;
	struct notifier_block notifier;

	int (*port_update)(struct typec_data *typec, int port_num);
};

#define caps_to_port_data(_caps_) container_of(_caps_, struct port_data, caps)

static int cros_typec_ec_command(struct typec_data *typec,
				 unsigned int version,
				 unsigned int command,
				 void *outdata,
				 unsigned int outsize,
				 void *indata,
				 unsigned int insize)
{
	struct cros_ec_dev *ec_dev = typec->ec_dev;
	struct cros_ec_command *msg;
	int ret;
	char host_cmd[32];

	msg = kzalloc(sizeof(*msg) + max(outsize, insize), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	msg->version = version;
	msg->command = ec_dev->cmd_offset + command;
	msg->outsize = outsize;
	msg->insize = insize;

	if (outsize)
		memcpy(msg->data, outdata, outsize);

	sprintf(host_cmd, "typec HC 0x%x req: ", command);
	print_hex_dump(KERN_DEBUG, host_cmd, DUMP_PREFIX_NONE, 16, 1, outdata,
			outsize, 0);

	ret = cros_ec_cmd_xfer_status(typec->ec_dev->ec_dev, msg);
	if (ret >= 0 && insize)
		memcpy(indata, msg->data, insize);
	sprintf(host_cmd, "typec HC 0x%x res: ", command);
	print_hex_dump(KERN_DEBUG, host_cmd, DUMP_PREFIX_NONE, 16, 1, indata,
			insize, 0);

	kfree(msg);
	return ret;
}

static int cros_typec_get_cmd_version(struct typec_data *typec, int cmd,
		uint8_t *version_mask)
{
	struct ec_params_get_cmd_versions req_v0;
	struct ec_params_get_cmd_versions_v1 req_v1;
	struct ec_response_get_cmd_versions res;
	int ret;

	req_v1.cmd = cmd;
	ret = cros_typec_ec_command(typec, 1, EC_CMD_GET_CMD_VERSIONS, &req_v1,
			sizeof(req_v1), &res, sizeof(res));
	if (ret < 0) {
		req_v0.cmd = cmd;
		ret = cros_typec_ec_command(typec, 0, EC_CMD_GET_CMD_VERSIONS,
				&req_v0, sizeof(req_v0), &res, sizeof(res));
		if (ret < 0)
			return ret;
	}
	*version_mask = res.version_mask;
	dev_dbg(typec->dev, "EC CMD 0x%hhx has version mask 0x%hhx\n", cmd,
			*version_mask);
	return 0;
}

static int cros_typec_query_pd_port_count(struct typec_data *typec)
{
	struct ec_response_usb_pd_ports res;
	int ret;

	ret = cros_typec_ec_command(typec, 0, EC_CMD_USB_PD_PORTS, NULL, 0,
			&res, sizeof(res));
	if (ret < 0)
		return ret;
	typec->num_ports = res.num_ports;
	return 0;
}

static int cros_typec_port_update(struct typec_data *typec,
				  int port_num,
				  struct ec_response_usb_pd_control *res,
				  size_t res_size,
				  int cmd_ver)
{
	struct port_data *port;
	struct ec_params_usb_pd_control req;
	int ret;

	if (port_num < 0 || port_num >= typec->num_ports) {
		dev_err(typec->dev, "cannot get status for invalid port %d\n",
				port_num);
		return -EPERM;
	}
	port = typec->ports[port_num];

	req.port = port_num;
	req.role = USB_PD_CTRL_ROLE_NO_CHANGE;
	req.mux = USB_PD_CTRL_MUX_NO_CHANGE;
	req.swap = USB_PD_CTRL_SWAP_NONE;

	ret = cros_typec_ec_command(typec, cmd_ver, EC_CMD_USB_PD_CONTROL, &req,
			sizeof(req), res, res_size);
	if (ret < 0)
		return ret;
	dev_dbg(typec->dev, "Enabled %d: 0x%hhx\n", port_num, res->enabled);
	dev_dbg(typec->dev, "Role %d: 0x%hhx\n", port_num, res->role);
	dev_dbg(typec->dev, "Polarity %d: 0x%hhx\n", port_num, res->polarity);

	return 0;
}

static int cros_typec_query_pd_info(struct typec_data *typec, int port_num)
{
	struct ec_params_usb_pd_info_request req;
	struct ec_params_usb_pd_discovery_entry res;
	int ret;

	req.port = port_num;
	ret = cros_typec_ec_command(typec, 0, EC_CMD_USB_PD_DISCOVERY, &req,
			sizeof(req), &res, sizeof(res));
	if (ret < 0)
		return ret;
	/* FIXME Needs the rest of the info in PD Spec 6.4.4.3.1.1 */
	typec->ports[port_num]->p_identity.id_header = res.vid;

	/* FIXME Needs bcdDevice to match PD Spec 6.4.4.3.1.3 */
	typec->ports[port_num]->p_identity.product = res.pid << 16;
	return 0;
}

static int cros_typec_port_update_v0(struct typec_data *typec, int port_num)
{
	struct port_data *port;
	struct ec_response_usb_pd_control res;
	enum typec_orientation polarity;
	int ret;

	port = typec->ports[port_num];
	ret = cros_typec_port_update(typec, port_num, &res, sizeof(res), 0);
	if (ret < 0)
		return ret;
	dev_dbg(typec->dev, "State %d: %hhx\n", port_num, res.state);

	if (!res.enabled)
		polarity = TYPEC_ORIENTATION_NONE;
	else if (!res.polarity)
		polarity = TYPEC_ORIENTATION_NORMAL;
	else
		polarity = TYPEC_ORIENTATION_REVERSE;

	typec_set_pwr_role(port->port, res.role ? TYPEC_SOURCE : TYPEC_SINK);
	typec_set_orientation(port->port, polarity);

	return 0;
}

static int cros_typec_add_partner(struct typec_data *typec, int port_num,
		bool pd_enabled)
{
	struct port_data *port;
	struct typec_partner_desc p_desc;
	int ret;

	port = typec->ports[port_num];
	p_desc.usb_pd = pd_enabled;
	p_desc.identity = &port->p_identity;

	port->partner = typec_register_partner(port->port, &p_desc);
	if (IS_ERR_OR_NULL(port->partner)) {
		dev_err(typec->dev, "Port %d partner register failed\n",
				port_num);
		port->partner = NULL;
		return PTR_ERR(port->partner);
	}

	ret = cros_typec_query_pd_info(typec, port_num);
	if (ret < 0) {
		dev_err(typec->dev, "Port %d PD query failed\n", port_num);
		typec_unregister_partner(port->partner);
		port->partner = NULL;
		return ret;
	}

	ret = typec_partner_set_identity(port->partner);
	return ret;
}

static int cros_typec_set_port_params_v1_v2(struct typec_data *typec,
		int port_num, struct ec_response_usb_pd_control_v1 *res)
{
	struct port_data *port;
	enum typec_orientation polarity;
	int ret;

	port = typec->ports[port_num];
	if (!(res->enabled & PD_CTRL_RESP_ENABLED_CONNECTED))
		polarity = TYPEC_ORIENTATION_NONE;
	else if (!res->polarity)
		polarity = TYPEC_ORIENTATION_NORMAL;
	else
		polarity = TYPEC_ORIENTATION_REVERSE;
	typec_set_orientation(port->port, polarity);
	typec_set_data_role(port->port, res->role & PD_CTRL_RESP_ROLE_DATA ?
			TYPEC_HOST : TYPEC_DEVICE);
	typec_set_pwr_role(port->port, res->role & PD_CTRL_RESP_ROLE_POWER ?
			TYPEC_SOURCE : TYPEC_SINK);
	typec_set_vconn_role(port->port, res->role & PD_CTRL_RESP_ROLE_VCONN ?
			TYPEC_SOURCE : TYPEC_SINK);

	if (res->enabled & PD_CTRL_RESP_ENABLED_CONNECTED && !port->partner) {
		bool pd_enabled =
			res->enabled & PD_CTRL_RESP_ENABLED_PD_CAPABLE;
		ret = cros_typec_add_partner(typec, port_num, pd_enabled);
		if (!ret)
			return ret;
	}
	if (!(res->enabled & PD_CTRL_RESP_ENABLED_CONNECTED) && port->partner) {
		typec_unregister_partner(port->partner);
		port->partner = NULL;
	}
	return 0;
}

static int cros_typec_port_update_v1(struct typec_data *typec, int port_num)
{
	/*struct port_data *port;*/
	struct ec_response_usb_pd_control_v1 res;
	struct ec_response_usb_pd_control *res_v0 =
		(struct ec_response_usb_pd_control *) &res;
	int ret;

	ret = cros_typec_port_update(typec, port_num, res_v0, sizeof(res), 1);
	if (ret < 0)
		return ret;
	dev_dbg(typec->dev, "State %d: %s\n", port_num, res.state);

	ret = cros_typec_set_port_params_v1_v2(typec, port_num, &res);
	return ret;
}

static int cros_typec_try_role(const struct typec_capability *cap, int role)
{
	return 0;
}

static int cros_typec_dr_set(const struct typec_capability *cap,
		enum typec_data_role role)
{
	return 0;
}

static int cros_typec_pr_set(const struct typec_capability *cap,
		enum typec_role role)
{
	return 0;
}

static int cros_typec_vconn_set(const struct typec_capability *cap,
		enum typec_role role)
{
	return 0;
}

static int cros_typec_ec_event(struct notifier_block *nb,
			       unsigned long queued_during_suspend,
			       void *_notify)
{
	struct typec_data *typec;
	int i;

	typec = container_of(nb, struct typec_data, notifier);

	for (i = 0; i < typec->num_ports; ++i)
		typec->port_update(typec, i);

	return NOTIFY_DONE;

}

static void cros_typec_unregister_notifier(void *data)
{
	struct typec_data *typec = data;

	cros_ec_usbpd_unregister_notify(&typec->notifier);
}

static int cros_typec_probe(struct platform_device *pd)
{
	struct cros_ec_dev *ec_dev = dev_get_drvdata(pd->dev.parent);
	struct device *dev = &pd->dev;
	struct typec_data *typec;
	uint8_t ver_mask;
	int i;
	int ret;

	dev_dbg(dev, "Probing Cros EC Type-C device.\n");

	typec = devm_kzalloc(dev, sizeof(*typec), GFP_KERNEL);
	if (!typec)
		return -ENOMEM;

	typec->dev = dev;
	typec->ec_dev = ec_dev;

	platform_set_drvdata(pd, typec);

	ret = cros_typec_query_pd_port_count(typec);
	if (ret < 0) {
		dev_err(dev, "Failed to get PD port count from EC\n");
		return ret;
	}
	if (typec->num_ports > EC_USB_PD_MAX_PORTS) {
		dev_err(dev, "EC reported too many ports. got: %d, max: %d\n",
				typec->num_ports, EC_USB_PD_MAX_PORTS);
		return -EOVERFLOW;
	}

	ret = cros_typec_get_cmd_version(typec, EC_CMD_USB_PD_CONTROL,
			&ver_mask);
	if (ret < 0) {
		dev_err(dev, "Failed to get supported PD command versions\n");
		return ret;
	}
	/* No reason to support EC_CMD_USB_PD_CONTROL v2 as it doesn't add any
	 * useful information
	 */
	if (ver_mask & EC_VER_MASK(1)) {
		dev_dbg(dev, "Using PD command ver 1\n");
		typec->port_update = cros_typec_port_update_v1;
	} else {
		dev_dbg(dev, "Using PD command ver 0\n");
		typec->port_update = cros_typec_port_update_v0;
	}

	for (i = 0; i < typec->num_ports; ++i) {
		struct port_data *port;

		port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
		if (!port) {
			ret = -ENOMEM;
			goto unregister_ports;
		}
		port->typec = typec;
		port->port_num = i;
		typec->ports[i] = port;

		/* TODO Make sure these values are accurate */
		port->caps.type = TYPEC_PORT_DRP;
		port->caps.data = TYPEC_PORT_DFP;
		port->caps.prefer_role = TYPEC_SINK;

		port->caps.try_role = cros_typec_try_role;
		port->caps.dr_set = cros_typec_dr_set;
		port->caps.pr_set = cros_typec_pr_set;
		port->caps.vconn_set = cros_typec_vconn_set;
		port->caps.port_type_set = NULL; /* Not permitted by PD spec */

		port->port = typec_register_port(dev, &port->caps);
		if (IS_ERR_OR_NULL(port->port)) {
			dev_err(dev, "Failed to register typec port %d\n", i);
			ret = PTR_ERR(port->port);
			goto unregister_ports;
		}

		ret = typec->port_update(typec, i);
		if (ret < 0) {
			dev_err(dev, "Failed to update typec port %d\n", i);
			goto unregister_ports;
		}
	}

	typec->notifier.notifier_call = cros_typec_ec_event;
	ret = cros_ec_usbpd_register_notify(&typec->notifier);
	if (ret < 0) {
		dev_warn(dev, "Failed to register notifier\n");
	} else {
		ret = devm_add_action_or_reset(dev,
				cros_typec_unregister_notifier, typec);
		if (ret < 0)
			goto unregister_ports;
		dev_dbg(dev, "Registered EC notifier\n");
	}

	return 0;

unregister_ports:
	for (i = 0; i < typec->num_ports; ++i) {
		if (typec->ports[i] && typec->ports[i]->port)
			typec_unregister_port(typec->ports[i]->port);
	}

	return ret;
}

static struct platform_driver cros_ec_typec_driver = {
	.driver = {
		.name = DRV_NAME,
	},
	.probe = cros_typec_probe
};

module_platform_driver(cros_ec_typec_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ChromeOS EC USB-C connectors");
MODULE_AUTHOR("Jon Flatley <jflat@chromium.org>");
MODULE_ALIAS("platform:" DRV_NAME);
