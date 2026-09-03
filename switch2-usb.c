// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * USB transport driver for Nintendo Switch 2 controllers
 *
 * Copyright (c) 2025 Valve Software
 *
 * Original implementation: Vicki Pfau <vi@endrift.com>
 * Subsequent modifications: Peter Novak
 *
 * This driver is based on the following work:
 *   https://gist.github.com/shinyquagsire23/66f006b46c56216acbaac6c1e2279b64
 *   https://github.com/ndeadly/switch2_controller_research
 */

#include "switch2.h"
#include <linux/module.h>
#include <linux/usb/input.h>

#define NS2_BULK_SIZE 64
#define NS2_IN_URBS 2
#define NS2_OUT_URBS 4


struct switch2_usb;

struct switch2_urb {
	struct urb *urb;
	uint8_t *data;
	bool active;
	struct switch2_usb *owner;
	struct work_struct work;
};

struct switch2_usb {
	struct switch2_transport transport;
	struct usb_device *udev;

	struct switch2_urb bulk_in[NS2_IN_URBS];
	struct usb_anchor bulk_in_anchor;
	spinlock_t bulk_in_lock;

	struct switch2_urb bulk_out[NS2_OUT_URBS];
	struct usb_anchor bulk_out_anchor;
	spinlock_t bulk_out_lock;

};

static void switch2_usb_free_urbs(struct switch2_usb *ns2_usb,
	struct switch2_urb *urbs, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (urbs[i].data)
			usb_free_coherent(ns2_usb->udev, NS2_BULK_SIZE,
				urbs[i].data, urbs[i].urb->transfer_dma);
		usb_free_urb(urbs[i].urb);
	}
}

static void switch2_usb_cancel_input_work(struct switch2_usb *ns2_usb)
{
	unsigned int i;

	for (i = 0; i < NS2_IN_URBS; i++) {
		if (ns2_usb->bulk_in[i].owner)
			cancel_work_sync(&ns2_usb->bulk_in[i].work);
	}
}

static void switch2_bulk_in(struct urb *urb)
{
	struct switch2_usb *ns2_usb = urb->context;
	struct switch2_urb *completed = NULL;
	bool schedule = false;
	unsigned long flags;
	int i;

	switch (urb->status) {
	case 0:
		schedule = true;
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		return;
	default:
		dev_dbg(&ns2_usb->udev->dev, "unknown urb status: %d\n",
			urb->status);
		break;
	}

	spin_lock_irqsave(&ns2_usb->bulk_in_lock, flags);
	for (i = 0; i < NS2_IN_URBS; i++) {
		struct switch2_urb *slot = &ns2_usb->bulk_in[i];
		int err;

		if (slot->urb == urb) {
			completed = slot;
			continue;
		}

		if (slot->active)
			continue;

		usb_anchor_urb(slot->urb, &ns2_usb->bulk_in_anchor);
		err = usb_submit_urb(slot->urb, GFP_ATOMIC);
		if (err) {
			usb_unanchor_urb(slot->urb);
			dev_dbg(&ns2_usb->udev->dev,
				"failed to queue input urb: %d\n", err);
		} else {
			slot->active = true;
		}
	}
	if (!schedule && completed)
		completed->active = false;
	spin_unlock_irqrestore(&ns2_usb->bulk_in_lock, flags);

	if (schedule && completed)
		schedule_work(&completed->work);
}

static void switch2_bulk_out(struct urb *urb)
{
	struct switch2_usb *ns2_usb = urb->context;
	int i;

	switch (urb->status) {
	case 0:
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		return;
	default:
		dev_dbg(&ns2_usb->udev->dev, "unknown urb status: %d\n", urb->status);
		break;
	}

	guard(spinlock_irqsave)(&ns2_usb->bulk_out_lock);
	for (i = 0; i < NS2_OUT_URBS; i++) {
		if (ns2_usb->bulk_out[i].urb != urb)
			continue;

		ns2_usb->bulk_out[i].active = false;
		break;
	}
}

static int switch2_usb_send_command(struct switch2_transport *transport,
	enum switch2_cmd command, uint8_t subcommand,
	const void *message, size_t size)
{
	struct switch2_usb *ns2_usb =
		container_of(transport, struct switch2_usb, transport);
	struct switch2_urb *urb = NULL;
	unsigned long flags;
	int packet_size;
	int i;
	int ret;

	spin_lock_irqsave(&ns2_usb->bulk_out_lock, flags);
	for (i = 0; i < NS2_OUT_URBS; i++) {
		if (ns2_usb->bulk_out[i].active)
			continue;

		urb = &ns2_usb->bulk_out[i];
		urb->active = true;
		break;
	}
	spin_unlock_irqrestore(&ns2_usb->bulk_out_lock, flags);

	if (!urb) {
		dev_warn(&ns2_usb->udev->dev, "output queue full, dropping message\n");
		return -ENOBUFS;
	}

	packet_size = switch2_transport_build_command(transport, command, subcommand,
		message, size, urb->data, NS2_BULK_SIZE);
	if (packet_size < 0) {
		ret = packet_size;
		goto err_release;
	}

	urb->urb->transfer_buffer_length = packet_size;

	print_hex_dump_debug("sending cmd: ", DUMP_PREFIX_OFFSET,
		16, 1, urb->data, packet_size, false);

	usb_anchor_urb(urb->urb, &ns2_usb->bulk_out_anchor);
	ret = usb_submit_urb(urb->urb, GFP_ATOMIC);
	if (ret) {
		dev_warn(&ns2_usb->udev->dev, "failed to submit urb: %i\n", ret);
		usb_unanchor_urb(urb->urb);
		goto err_release;
	}

	return 0;

err_release:
	spin_lock_irqsave(&ns2_usb->bulk_out_lock, flags);
	urb->active = false;
	spin_unlock_irqrestore(&ns2_usb->bulk_out_lock, flags);
	return ret;
}

static void switch2_usb_input_work(struct work_struct *work)
{
	struct switch2_urb *slot = container_of(work, struct switch2_urb, work);
	struct switch2_usb *ns2_usb = slot->owner;
	unsigned long flags;
	int err;

	err = switch2_transport_receive(&ns2_usb->transport,
		slot->urb->transfer_buffer, slot->urb->actual_length);
	if (err)
		dev_dbg(&ns2_usb->udev->dev, "receive command failed: %d\n", err);

	spin_lock_irqsave(&ns2_usb->bulk_in_lock, flags);
	slot->active = false;
	spin_unlock_irqrestore(&ns2_usb->bulk_in_lock, flags);
}

static int switch2_usb_probe(struct usb_interface *intf,
	const struct usb_device_id *id)
{
	struct switch2_controller *ns2;
	struct switch2_usb *ns2_usb;
	struct usb_device *udev;
	struct usb_endpoint_descriptor *bulk_in, *bulk_out;
	char phys[64];
	int ret;
	int i;

	(void)id;

	udev = interface_to_usbdev(intf);
	if (usb_make_path(udev, phys, sizeof(phys)) < 0)
		return -EINVAL;

	ret = usb_find_common_endpoints(intf->cur_altsetting, &bulk_in, &bulk_out,
		NULL, NULL);
	if (ret) {
		dev_err(&intf->dev, "failed to find bulk EPs\n");
		return ret;
	}

	ns2_usb = devm_kzalloc(&intf->dev, sizeof(*ns2_usb), GFP_KERNEL);
	if (!ns2_usb)
		return -ENOMEM;

	ns2_usb->udev = udev;
	ns2_usb->transport = (struct switch2_transport) {
		.type = NS2_TRANS_USB,
		.send_command = switch2_usb_send_command,
	};
	init_usb_anchor(&ns2_usb->bulk_in_anchor);
	spin_lock_init(&ns2_usb->bulk_in_lock);
	init_usb_anchor(&ns2_usb->bulk_out_anchor);
	spin_lock_init(&ns2_usb->bulk_out_lock);

	for (i = 0; i < NS2_IN_URBS; i++) {
		ns2_usb->bulk_in[i].urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!ns2_usb->bulk_in[i].urb) {
			ret = -ENOMEM;
			goto err_free_urbs;
		}

		ns2_usb->bulk_in[i].data = usb_alloc_coherent(udev, NS2_BULK_SIZE,
			GFP_KERNEL, &ns2_usb->bulk_in[i].urb->transfer_dma);
		if (!ns2_usb->bulk_in[i].data) {
			ret = -ENOMEM;
			goto err_free_urbs;
		}

		usb_fill_bulk_urb(ns2_usb->bulk_in[i].urb, udev,
			usb_rcvbulkpipe(udev, bulk_in->bEndpointAddress),
			ns2_usb->bulk_in[i].data, NS2_BULK_SIZE,
			switch2_bulk_in, ns2_usb);
		ns2_usb->bulk_in[i].urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
		INIT_WORK(&ns2_usb->bulk_in[i].work, switch2_usb_input_work);
		ns2_usb->bulk_in[i].owner = ns2_usb;
	}

	for (i = 0; i < NS2_OUT_URBS; i++) {
		ns2_usb->bulk_out[i].urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!ns2_usb->bulk_out[i].urb) {
			ret = -ENOMEM;
			goto err_free_urbs;
		}

		ns2_usb->bulk_out[i].data = usb_alloc_coherent(udev, NS2_BULK_SIZE,
			GFP_KERNEL, &ns2_usb->bulk_out[i].urb->transfer_dma);
		if (!ns2_usb->bulk_out[i].data) {
			ret = -ENOMEM;
			goto err_free_urbs;
		}

		usb_fill_bulk_urb(ns2_usb->bulk_out[i].urb, udev,
			usb_sndbulkpipe(udev, bulk_out->bEndpointAddress),
			ns2_usb->bulk_out[i].data, NS2_BULK_SIZE,
			switch2_bulk_out, ns2_usb);
		ns2_usb->bulk_out[i].urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	}

	/* Start receive I/O only after every callback dependency is initialized.
	 * Anchor the first URB as well so probe failure/disconnect can stop all
	 * input I/O through the same lifetime path. */
	ns2_usb->bulk_in[0].active = true;
	usb_anchor_urb(ns2_usb->bulk_in[0].urb, &ns2_usb->bulk_in_anchor);
	ret = usb_submit_urb(ns2_usb->bulk_in[0].urb, GFP_ATOMIC);
	if (ret < 0) {
		usb_unanchor_urb(ns2_usb->bulk_in[0].urb);
		ns2_usb->bulk_in[0].active = false;
		goto err_free_urbs;
	}

	ns2 = switch2_controller_get(phys);
	if (IS_ERR(ns2)) {
		ret = PTR_ERR(ns2);
		goto err_stop_io;
	}

	ret = switch2_transport_attach(ns2, &ns2_usb->transport);
	if (ret)
		goto err_put_controller;

	usb_set_intfdata(intf, ns2_usb);
	return 0;

err_put_controller:
	switch2_controller_put(ns2);
err_stop_io:
	usb_kill_anchored_urbs(&ns2_usb->bulk_in_anchor);
	switch2_usb_cancel_input_work(ns2_usb);
err_free_urbs:
	switch2_usb_free_urbs(ns2_usb, ns2_usb->bulk_out, NS2_OUT_URBS);
	switch2_usb_free_urbs(ns2_usb, ns2_usb->bulk_in, NS2_IN_URBS);
	return ret;
}

static void switch2_usb_disconnect(struct usb_interface *intf)
{
	struct switch2_usb *ns2_usb = usb_get_intfdata(intf);
	struct switch2_controller *ns2;

	if (!ns2_usb)
		return;

	usb_set_intfdata(intf, NULL);
	ns2 = ns2_usb->transport.controller;
	switch2_transport_detach(&ns2_usb->transport);

	/* Detach first to prevent new controller traffic, then quiesce every USB
	 * callback before freeing its buffers. */
	usb_kill_anchored_urbs(&ns2_usb->bulk_out_anchor);
	usb_kill_anchored_urbs(&ns2_usb->bulk_in_anchor);
	switch2_usb_cancel_input_work(ns2_usb);

	switch2_usb_free_urbs(ns2_usb, ns2_usb->bulk_out, NS2_OUT_URBS);
	switch2_usb_free_urbs(ns2_usb, ns2_usb->bulk_in, NS2_IN_URBS);
	switch2_controller_put(ns2);
}

#define SWITCH2_CONTROLLER(vend, prod) \
	USB_DEVICE_AND_INTERFACE_INFO(vend, prod, USB_CLASS_VENDOR_SPEC, 0, 0)

static const struct usb_device_id switch2_usb_devices[] = {
	{ SWITCH2_CONTROLLER(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_NS2_JOYCONL) },
	{ SWITCH2_CONTROLLER(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_NS2_JOYCONR) },
	{ SWITCH2_CONTROLLER(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_NS2_PROCON) },
	{ SWITCH2_CONTROLLER(USB_VENDOR_ID_NINTENDO, USB_DEVICE_ID_NINTENDO_NS2_GCCON) },
	{ }
};
MODULE_DEVICE_TABLE(usb, switch2_usb_devices);

static struct usb_driver switch2_usb = {
	.name		= "switch2",
	.id_table	= switch2_usb_devices,
	.probe		= switch2_usb_probe,
	.disconnect	= switch2_usb_disconnect,
};
module_usb_driver(switch2_usb);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vicki Pfau <vi@endrift.com>");
MODULE_AUTHOR("Peter Novak");
MODULE_DESCRIPTION("USB transport driver for Nintendo Switch 2 controllers");
