// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * HID driver for Nintendo Switch 2 controllers
 *
 * Copyright (c) 2025 Valve Software
 *
 * Originally derived from hid-switch2.c by Vicki Pfau <vi@endrift.com>
 * Subsequent modifications and Bluetooth support: Peter Novak
 *
 * This driver is based on the following work:
 *   https://gist.github.com/shinyquagsire23/66f006b46c56216acbaac6c1e2279b64
 *   https://github.com/ndeadly/switch2_controller_research
 *   hid-nintendo driver
 */

#include "switch2.h"
#include "switch2-gatt.h"
#include <linux/device.h>
#include <linux/hid.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/usb.h>

/* drivers/hid/usbhid/usbhid.h is private to usbhid, so keep the small
 * conversion helper locally for the physical USB path. */
static inline struct usb_device *hid_to_usb_dev(struct hid_device *hdev)
{
	return interface_to_usbdev(to_usb_interface(hdev->dev.parent));
}

static int switch2_attach_hid(struct switch2_controller *ns2,
	struct hid_device *hdev, enum switch2_controller_type type)
{
	int ret;

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "parse failed %d\n", ret);
		return ret;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "hw_start failed %d\n", ret);
		return ret;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "hw_open failed %d\n", ret);
		goto err_stop;
	}

	mutex_lock(&ns2->lock);
	if (ns2->hdev && ns2->hdev != hdev) {
		ret = -EBUSY;
		goto err_unlock;
	}

	ns2->hdev = hdev;
	ns2->type = type;
	hid_set_drvdata(hdev, ns2);

	ret = switch2_controller_activate_locked(ns2);
	if (ret) {
		ns2->hdev = NULL;
		hid_set_drvdata(hdev, NULL);
	}
	mutex_unlock(&ns2->lock);

	if (ret)
		goto err_close;

	return 0;

err_unlock:
	mutex_unlock(&ns2->lock);
err_close:
	hid_hw_close(hdev);
err_stop:
	hid_hw_stop(hdev);
	return ret;
}

static int switch2_hid_event(struct hid_device *hdev,
	struct hid_report *report, u8 *raw_data, int size)
{
	(void)report;
	struct switch2_controller *ns2 = hid_get_drvdata(hdev);

	if (!ns2 || !raw_data || size <= 0)
		return -EINVAL;

	switch (raw_data[0]) {
	case NS2_REPORT_JCL:
	case NS2_REPORT_JCR:
		if (hdev->bus == BUS_BLUETOOTH)
			return switch2_input_receive_ble_side(ns2, raw_data, size);
		fallthrough;
	case NS2_REPORT_COMMON:
	case NS2_REPORT_PRO:
	case NS2_REPORT_GC:
		return switch2_input_receive_state(ns2, raw_data, size);
	default:
		return 0;
	}
}

static int switch2_hid_get_controller_key(struct hid_device *hdev,
	char *key, size_t key_size)
{
	struct usb_device *udev;
	int ret;

	if (!hdev || !key || !key_size)
		return -EINVAL;

	if (hid_is_usb(hdev)) {
		udev = hid_to_usb_dev(hdev);

		ret = usb_make_path(udev, key, key_size);
		if (ret < 0)
			return ret;

		return 0;
	}

	/* Bluetooth/GATT-backed HIDs do not have a struct usb_device.  Their
	 * physical path uniquely identifies the physical Joy-Con. */
	if (hdev->phys[0]) {
		ret = strscpy(key, hdev->phys, key_size);
		if (ret < 0)
			return ret;

		return 0;
	}

	/* Fallback for virtual HID devices without a physical-path string. */
	ret = strscpy(key, dev_name(&hdev->dev), key_size);
	if (ret < 0)
		return ret;

	return 0;
}

static int switch2_hid_probe(struct hid_device *hdev,
	const struct hid_device_id *id)
{
	struct switch2_controller *ns2;
	char key[64];
	int ret;

	ret = switch2_hid_get_controller_key(hdev, key, sizeof(key));
	if (ret)
		return ret;

	ns2 = switch2_controller_get(key);
	if (IS_ERR(ns2))
		return PTR_ERR(ns2);

	ret = switch2_attach_hid(ns2, hdev, id->driver_data);
	if (ret) {
		switch2_controller_put(ns2);
		return ret;
	}

	return 0;
}

static void switch2_hid_remove(struct hid_device *hdev)
{
	struct switch2_controller *ns2 = hid_get_drvdata(hdev);

	switch2_controller_deactivate(ns2);
	hid_set_drvdata(hdev, NULL);
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
	switch2_controller_put(ns2);
}

static const struct hid_device_id switch2_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_NS2_JOYCONL),
	  .driver_data = NS2_CTLR_TYPE_JCL },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_NS2_JOYCONR),
	  .driver_data = NS2_CTLR_TYPE_JCR },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_NS2_PROCON),
	  .driver_data = NS2_CTLR_TYPE_PRO },
	{ HID_USB_DEVICE(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_NS2_GCCON),
	  .driver_data = NS2_CTLR_TYPE_GC },

	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_NS2_JOYCONL),
	  .driver_data = NS2_CTLR_TYPE_JCL },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_NS2_JOYCONR),
	  .driver_data = NS2_CTLR_TYPE_JCR },
	{}
};
MODULE_DEVICE_TABLE(hid, switch2_devices);

static struct hid_driver switch2_hid_driver = {
	.name		= "switch2",
	.id_table	= switch2_devices,
	.probe		= switch2_hid_probe,
	.remove		= switch2_hid_remove,
	.raw_event	= switch2_hid_event,
};

static int __init switch2_hid_module_init(void)
{
	int ret;

	/* Register the HID driver first: an ATTACH write to /dev/switch2-gatt
	 * may immediately publish a BUS_BLUETOOTH hid_device. */
	ret = hid_register_driver(&switch2_hid_driver);
	if (ret)
		return ret;

	ret = switch2_gatt_init();
	if (ret)
		hid_unregister_driver(&switch2_hid_driver);
	return ret;
}

static void __exit switch2_hid_module_exit(void)
{
	/* Stop new BLE transport sessions before removing the HID driver. Open
	 * transport fds hold THIS_MODULE and therefore block module unload. */
	switch2_gatt_exit();
	hid_unregister_driver(&switch2_hid_driver);
	switch2_controller_cleanup();
}

module_init(switch2_hid_module_init);
module_exit(switch2_hid_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vicki Pfau <vi@endrift.com>");
MODULE_AUTHOR("Peter Novak");
MODULE_DESCRIPTION("HID driver for Nintendo Switch 2 controllers (USB and Bluetooth)");
