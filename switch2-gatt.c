// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Joy-Con 2 BLE GATT transport -> kernel HID transport.
 *
 * BlueZ owns only ATT/GATT mechanics.  This file owns the kernel-visible
 * Joy-Con 2 Bluetooth HID and implements struct switch2_transport directly.
 * Controller protocol, initialization, motion conversion and JC1 compatibility
 * remain in the shared hid_switch2 core exactly as for USB.
 */

#include "switch2.h"
#include "switch2-gatt.h"

#include <linux/fs.h>
#include <linux/hid.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define NS2_GATT_ABI_VERSION          1
#define NS2_GATT_MAX_PAYLOAD          255
#define NS2_GATT_RECORD_MAX \
	(sizeof(struct switch2_gatt_header) + NS2_GATT_MAX_PAYLOAD)
#define NS2_GATT_TX_LIMIT             64


/* Userspace -> kernel. */
enum switch2_gatt_rx_type {
	NS2_GATT_RX_ATTACH = 0x01,
	NS2_GATT_RX_STATE  = 0x02,
	NS2_GATT_RX_REPLY  = 0x03,
};

/* Kernel -> userspace. */
enum switch2_gatt_tx_type {
	NS2_GATT_TX_COMMAND = 0x81,
	NS2_GATT_TX_RAW     = 0x82,
	NS2_GATT_TX_OUTPUT  = 0x83,
};

struct switch2_gatt_header {
	u8 abi;
	u8 type;
	__le16 length;
} __packed;

struct switch2_gatt_attach {
	__le16 product;
	u8 report_id;
	char address[18];
} __packed;

struct switch2_gatt_packet {
	struct list_head entry;
	size_t size;
	u8 data[];
};

struct switch2_gatt_client {
	struct hid_device *hdev;
	struct switch2_transport transport;
	spinlock_t tx_lock;
	struct list_head tx_queue;
	wait_queue_head_t read_wait;
	unsigned int tx_count;
	bool closing;
};

static struct miscdevice switch2_gatt_misc;

static u8 switch2_gatt_side_report_id(u16 product)
{
	switch (product) {
	case USB_DEVICE_ID_NINTENDO_NS2_JOYCONL:
		return NS2_REPORT_JCL;
	case USB_DEVICE_ID_NINTENDO_NS2_JOYCONR:
		return NS2_REPORT_JCR;
	default:
		return 0;
	}
}

static bool switch2_gatt_tx_pending(const struct switch2_gatt_client *client)
{
	return READ_ONCE(client->tx_count) != 0;
}

static void switch2_gatt_purge_tx(struct switch2_gatt_client *client)
{
	struct switch2_gatt_packet *packet, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&client->tx_lock, flags);
	list_for_each_entry_safe(packet, tmp, &client->tx_queue, entry) {
		list_del(&packet->entry);
		kfree(packet);
	}
	client->tx_count = 0;
	spin_unlock_irqrestore(&client->tx_lock, flags);
}

/* The kernel-created native JC2 HID carries BLE input/output reports. Commands
 * and replies travel directly through struct switch2_transport. Byte 8 is
 * patched from 0x07 to 0x08 for the right Joy-Con 2. Common 0x05 remains a
 * supported compatibility input alongside the side-native report. */
static const u8 switch2_gatt_rdesc_template[] = {
	0x06,0x00,0xff,0x09,0x01,0xa1,0x01,0x85,0x07,0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x3f,0x09,0x01,0x81,0x02,0xc0,
	0x06,0x00,0xff,0x09,0x01,0xa1,0x01,0x85,0x05,0x15,0x00,0x26,0xff,0x00,0x75,0x08,0x95,0x3f,0x09,0x01,0x81,0x02,0xc0,
};

static int switch2_gatt_queue(struct switch2_gatt_client *client, u8 type,
			      const void *payload, size_t length)
{
	struct switch2_gatt_packet *packet;
	struct switch2_gatt_header *header;
	unsigned long flags;

	if (!client || !payload || !length || length > NS2_GATT_MAX_PAYLOAD)
		return -EINVAL;

	packet = kmalloc(sizeof(*packet) + sizeof(*header) + length, GFP_ATOMIC);
	if (!packet)
		return -ENOMEM;

	packet->size = sizeof(*header) + length;
	header = (struct switch2_gatt_header *)packet->data;
	header->abi = NS2_GATT_ABI_VERSION;
	header->type = type;
	header->length = cpu_to_le16(length);
	memcpy(packet->data + sizeof(*header), payload, length);

	spin_lock_irqsave(&client->tx_lock, flags);
	if (client->closing || client->tx_count >= NS2_GATT_TX_LIMIT) {
		spin_unlock_irqrestore(&client->tx_lock, flags);
		kfree(packet);
		return client->closing ? -ENODEV : -ENOSPC;
	}
	list_add_tail(&packet->entry, &client->tx_queue);
	client->tx_count++;
	spin_unlock_irqrestore(&client->tx_lock, flags);
	wake_up_interruptible(&client->read_wait);
	return 0;
}

static int switch2_gatt_transport_send_command(
	struct switch2_transport *transport, enum switch2_cmd command,
	u8 subcommand, const void *message, size_t length)
{
	struct switch2_gatt_client *client =
		container_of(transport, struct switch2_gatt_client, transport);
	u8 packet[NS2_CMD_MAX_SIZE];
	int packet_size;

	packet_size = switch2_transport_build_command(transport, command,
		subcommand, message, length, packet, sizeof(packet));
	if (packet_size < 0)
		return packet_size;

	return switch2_gatt_queue(client, NS2_GATT_TX_COMMAND, packet,
		packet_size);
}

static int switch2_gatt_transport_send_raw(struct switch2_transport *transport,
	const void *message, size_t length)
{
	struct switch2_gatt_client *client =
		container_of(transport, struct switch2_gatt_client, transport);

	return switch2_gatt_queue(client, NS2_GATT_TX_RAW, message, length);
}

static int switch2_gatt_hid_parse(struct hid_device *hdev)
{
	u8 descriptor[sizeof(switch2_gatt_rdesc_template)];
	u8 report_id = switch2_gatt_side_report_id(hdev->product);

	if (!report_id)
		return -ENODEV;

	memcpy(descriptor, switch2_gatt_rdesc_template, sizeof(descriptor));
	descriptor[8] = report_id;
	return hid_parse_report(hdev, descriptor, sizeof(descriptor));
}

static int switch2_gatt_hid_start(struct hid_device *hdev) { return 0; }
static void switch2_gatt_hid_stop(struct hid_device *hdev) { }
static int switch2_gatt_hid_open(struct hid_device *hdev) { return 0; }
static void switch2_gatt_hid_close(struct hid_device *hdev) { }

static int switch2_gatt_hid_output_report(struct hid_device *hdev, u8 *buf,
					 size_t len)
{
	struct switch2_gatt_client *client = dev_get_platdata(&hdev->dev);
	int ret;

	if (!client || client->hdev != hdev)
		return -ENOTCONN;
	if (!buf || !len)
		return -EINVAL;

	ret = switch2_gatt_queue(client, NS2_GATT_TX_OUTPUT, buf, len);
	return ret ? ret : len;
}

static int switch2_gatt_hid_raw_request(struct hid_device *hdev,
				       unsigned char reportnum, u8 *buf,
				       size_t len, unsigned char rtype,
				       int reqtype)
{
	if (reqtype != HID_REQ_SET_REPORT || rtype != HID_OUTPUT_REPORT)
		return -EOPNOTSUPP;
	if (!len || buf[0] != reportnum)
		return -EINVAL;
	return switch2_gatt_hid_output_report(hdev, buf, len);
}

static const struct hid_ll_driver switch2_gatt_hid_driver = {
	.start = switch2_gatt_hid_start,
	.stop = switch2_gatt_hid_stop,
	.open = switch2_gatt_hid_open,
	.close = switch2_gatt_hid_close,
	.parse = switch2_gatt_hid_parse,
	.raw_request = switch2_gatt_hid_raw_request,
	.output_report = switch2_gatt_hid_output_report,
};

static int switch2_gatt_attach(struct switch2_gatt_client *client,
			       const void *payload, size_t length)
{
	const struct switch2_gatt_attach *attach = payload;
	struct hid_device *hdev;
	u16 product;
	u8 report_id;
	int ret;

	if (client->hdev || length != sizeof(*attach))
		return -EINVAL;

	product = le16_to_cpu(attach->product);
	report_id = switch2_gatt_side_report_id(product);
	if (!report_id || attach->report_id != report_id)
		return -EINVAL;
	if (!memchr(attach->address, '\0', sizeof(attach->address)))
		return -EINVAL;

	hdev = hid_allocate_device();
	if (IS_ERR(hdev))
		return PTR_ERR(hdev);

	client->hdev = hdev;
	hdev->ll_driver = &switch2_gatt_hid_driver;
	hdev->bus = BUS_BLUETOOTH;
	hdev->vendor = USB_VENDOR_ID_NINTENDO;
	hdev->product = product;
	hdev->version = 0x0100;
	hdev->dev.parent = switch2_gatt_misc.this_device;
	hdev->dev.platform_data = client;
	snprintf(hdev->name, sizeof(hdev->name), "Nintendo Joy-Con 2 (%c)",
		 report_id == NS2_REPORT_JCR ? 'R' : 'L');
	snprintf(hdev->phys, sizeof(hdev->phys), "switch2-gatt/%s", attach->address);
	snprintf(hdev->uniq, sizeof(hdev->uniq), "%s", attach->address);

	/* Publish the native JC2 HID first. hid_switch2's probe creates/fetches the
	 * shared controller object keyed by hdev->phys; the BLE transport is then
	 * attached to that controller below. */
	ret = hid_add_device(hdev);
	if (ret)
		goto err_hid;

	{
		struct switch2_controller *controller = hid_get_drvdata(hdev);

		if (!controller) {
			ret = -ENODEV;
			goto err_hid;
		}

		/* This is the same shared transport attachment used by switch2-usb.c.
		 * Initialization starts here and command replies come back through
		 * switch2_transport_receive(). */
		ret = switch2_transport_attach(controller, &client->transport);
		if (ret)
			goto err_hid;
	}

	return 0;

err_hid:
	client->hdev = NULL;
	hid_destroy_device(hdev);
	return ret;
}

static void switch2_gatt_detach(struct switch2_gatt_client *client)
{
	struct hid_device *hdev;

	if (!client || !client->hdev)
		return;

	hdev = client->hdev;
	switch2_transport_detach(&client->transport);
	client->hdev = NULL;
	hid_destroy_device(hdev);
}

static int switch2_gatt_receive_state(struct switch2_gatt_client *client,
				      const void *payload, size_t length)
{
	const u8 *report = payload;
	u8 side_report_id;

	if (!client->hdev || !report || length != 64)
		return -EINVAL;

	side_report_id = switch2_gatt_side_report_id(client->hdev->product);
	if (report[0] != NS2_REPORT_COMMON && report[0] != side_report_id)
		return -EINVAL;

	return hid_input_report(client->hdev, HID_INPUT_REPORT,
				(u8 *)report, length, 1);
}

static int switch2_gatt_receive_reply(struct switch2_gatt_client *client,
			      const void *payload, size_t length)
{
	if (!client->transport.controller || !payload || !length ||
	    length > NS2_GATT_MAX_PAYLOAD)
		return -EINVAL;

	return switch2_transport_receive(&client->transport, payload, length);
}

static int switch2_gatt_open(struct inode *inode, struct file *file)
{
	struct switch2_gatt_client *client;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;
	client->transport = (struct switch2_transport) {
		.type = NS2_TRANS_BT,
		.send_command = switch2_gatt_transport_send_command,
		.send_raw = switch2_gatt_transport_send_raw,
	};
	spin_lock_init(&client->tx_lock);
	INIT_LIST_HEAD(&client->tx_queue);
	init_waitqueue_head(&client->read_wait);
	file->private_data = client;
	return 0;
}

static ssize_t switch2_gatt_write(struct file *file, const char __user *buffer,
				  size_t count, loff_t *ppos)
{
	struct switch2_gatt_client *client = file->private_data;
	struct switch2_gatt_header *header;
	u8 record[NS2_GATT_RECORD_MAX];
	u16 length;
	int ret;

	if (!client || count < sizeof(*header) || count > sizeof(record))
		return -EINVAL;
	if (copy_from_user(record, buffer, count))
		return -EFAULT;
	header = (struct switch2_gatt_header *)record;
	if (header->abi != NS2_GATT_ABI_VERSION)
		return -EPROTO;
	length = le16_to_cpu(header->length);
	if (!length || count != sizeof(*header) + length)
		return -EINVAL;

	switch (header->type) {
	case NS2_GATT_RX_ATTACH:
		ret = switch2_gatt_attach(client, record + sizeof(*header), length);
		break;
	case NS2_GATT_RX_STATE:
		ret = switch2_gatt_receive_state(client, record + sizeof(*header), length);
		break;
	case NS2_GATT_RX_REPLY:
		ret = switch2_gatt_receive_reply(client, record + sizeof(*header), length);
		break;
	default:
		return -EINVAL;
	}

	return ret < 0 ? ret : count;
}

static ssize_t switch2_gatt_read(struct file *file, char __user *buffer,
				 size_t count, loff_t *ppos)
{
	struct switch2_gatt_client *client = file->private_data;
	struct switch2_gatt_packet *packet;
	unsigned long flags;
	int ret;

	if (!client)
		return -ENODEV;

	for (;;) {
		spin_lock_irqsave(&client->tx_lock, flags);
		if (!list_empty(&client->tx_queue)) {
			packet = list_first_entry(&client->tx_queue,
						  struct switch2_gatt_packet, entry);
			if (count < packet->size) {
				spin_unlock_irqrestore(&client->tx_lock, flags);
				return -EMSGSIZE;
			}
			list_del(&packet->entry);
			client->tx_count--;
			spin_unlock_irqrestore(&client->tx_lock, flags);
			break;
		}
		if (client->closing) {
			spin_unlock_irqrestore(&client->tx_lock, flags);
			return 0;
		}
		spin_unlock_irqrestore(&client->tx_lock, flags);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(client->read_wait,
			READ_ONCE(client->closing) || switch2_gatt_tx_pending(client));
		if (ret)
			return ret;
	}

	if (copy_to_user(buffer, packet->data, packet->size)) {
		kfree(packet);
		return -EFAULT;
	}
	ret = packet->size;
	kfree(packet);
	return ret;
}

static __poll_t switch2_gatt_poll(struct file *file, poll_table *wait)
{
	struct switch2_gatt_client *client = file->private_data;
	__poll_t mask = 0;

	if (!client)
		return EPOLLERR;
	poll_wait(file, &client->read_wait, wait);
	if (switch2_gatt_tx_pending(client))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (READ_ONCE(client->closing))
		mask |= EPOLLHUP;
	return mask;
}

static int switch2_gatt_release(struct inode *inode, struct file *file)
{
	struct switch2_gatt_client *client = file->private_data;
	unsigned long flags;

	if (!client)
		return 0;
	spin_lock_irqsave(&client->tx_lock, flags);
	client->closing = true;
	spin_unlock_irqrestore(&client->tx_lock, flags);
	wake_up_interruptible(&client->read_wait);

	switch2_gatt_detach(client);
	switch2_gatt_purge_tx(client);
	kfree(client);
	file->private_data = NULL;
	return 0;
}

static const struct file_operations switch2_gatt_fops = {
	.owner = THIS_MODULE,
	.open = switch2_gatt_open,
	.read = switch2_gatt_read,
	.write = switch2_gatt_write,
	.poll = switch2_gatt_poll,
	.release = switch2_gatt_release,
	.llseek = noop_llseek,
};

static struct miscdevice switch2_gatt_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "switch2-gatt",
	.fops = &switch2_gatt_fops,
	.mode = 0600,
};

int switch2_gatt_init(void)
{
	return misc_register(&switch2_gatt_misc);
}

void switch2_gatt_exit(void)
{
	misc_deregister(&switch2_gatt_misc);
}
