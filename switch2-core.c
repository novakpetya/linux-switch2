// SPDX-License-Identifier: GPL-2.0-or-later

#include "switch2.h"

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

static LIST_HEAD(switch2_controllers);
static DEFINE_MUTEX(switch2_controllers_lock);

int switch2_transport_build_command(
    const struct switch2_transport *transport,
    enum switch2_cmd command, uint8_t subcommand,
    const void *payload, size_t payload_size,
    uint8_t *buffer, size_t buffer_size)
{
    struct switch2_cmd_header header;

    if (!transport || !buffer)
        return -EINVAL;

    if (payload_size > NS2_CMD_MAX_PAYLOAD)
        return -EINVAL;

    if (payload_size && !payload)
        return -EINVAL;

    if (buffer_size < NS2_CMD_HEADER_SIZE + payload_size)
        return -ENOSPC;

    header = (struct switch2_cmd_header) {
        .command = command,
        .direction = NS2_DIR_OUT | NS2_FLAG_OK,
        .transport = transport->type,
        .subcommand = subcommand,
        .unk1 = 0,
        .length = payload_size,
        .unk2 = 0,
    };

    memcpy(buffer, &header, NS2_CMD_HEADER_SIZE);
    if (payload_size)
        memcpy(buffer + NS2_CMD_HEADER_SIZE, payload, payload_size);

    return NS2_CMD_HEADER_SIZE + payload_size;
}
EXPORT_SYMBOL_GPL(switch2_transport_build_command);

int switch2_transport_send_raw(struct switch2_controller *controller,
    const void *payload, size_t payload_size)
{
    struct switch2_transport *transport;

    if (!controller || !payload || !payload_size)
        return -EINVAL;

    transport = controller->transport;
    if (!transport)
        return -ENOTCONN;

    if (!transport->send_raw)
        return -EOPNOTSUPP;

    return transport->send_raw(transport, payload, payload_size);
}

int switch2_transport_send(struct switch2_controller *controller,
    enum switch2_cmd command, uint8_t subcommand,
    const void *payload, size_t payload_size)
{
    struct switch2_transport *transport;

    if (!controller)
        return -EINVAL;

    transport = controller->transport;
    if (!transport)
        return -ENOTCONN;

    if (!transport->send_command)
        return -EOPNOTSUPP;

    return transport->send_command(transport,
        command, subcommand, payload, payload_size);
}

int switch2_transport_receive(struct switch2_transport *transport,
    const uint8_t *packet, size_t packet_size)
{
    if (!transport || !packet)
        return -EINVAL;

    if (!transport->controller)
        return -ENOTCONN;

    return switch2_protocol_handle_reply(transport->controller,
        packet, packet_size);
}
EXPORT_SYMBOL_GPL(switch2_transport_receive);


struct switch2_controller *switch2_controller_get(const char *phys)
{
    struct switch2_controller *ns2;

    guard(mutex)(&switch2_controllers_lock);
    list_for_each_entry(ns2, &switch2_controllers, entry) {
        if (strncmp(ns2->phys, phys, sizeof(ns2->phys)) == 0) {
            refcount_inc(&ns2->refcount);
            return ns2;
        }
    }

    ns2 = kzalloc(sizeof(*ns2), GFP_KERNEL);
    if (!ns2)
        return ERR_PTR(-ENOMEM);

    mutex_init(&ns2->lock);
    switch2_init_work_init(ns2);
    INIT_LIST_HEAD(&ns2->entry);
    refcount_set(&ns2->refcount, 1);
    strscpy(ns2->phys, phys, sizeof(ns2->phys));
    list_add(&ns2->entry, &switch2_controllers);
    return ns2;
}
EXPORT_SYMBOL_GPL(switch2_controller_get);

void switch2_controller_put(struct switch2_controller *ns2)
{
    bool release;

    if (!ns2)
        return;

    mutex_lock(&switch2_controllers_lock);
    release = refcount_dec_and_test(&ns2->refcount);
    if (release)
        list_del_init(&ns2->entry);
    mutex_unlock(&switch2_controllers_lock);

    if (!release)
        return;

    switch2_init_work_cancel(ns2);
    switch2_input_destroy(ns2);
    mutex_destroy(&ns2->lock);
    kfree(ns2);
}
EXPORT_SYMBOL_GPL(switch2_controller_put);

void switch2_controller_refresh_ble_frontends(void)
{
    struct switch2_controller *ns2;

    mutex_lock(&switch2_controllers_lock);
    list_for_each_entry(ns2, &switch2_controllers, entry) {
        int ret;

        mutex_lock(&ns2->lock);
        ret = switch2_ble_frontend_sync_locked(ns2);
        if (ret && ns2->hdev)
            dev_err(switch2_dev(ns2),
                    "failed to switch BLE controller frontend: %d\n", ret);
        mutex_unlock(&ns2->lock);
    }
    mutex_unlock(&switch2_controllers_lock);
}

int switch2_transport_attach(struct switch2_controller *ns2,
                             struct switch2_transport *transport)
{
    int ret = 0;

    if (!ns2 || !transport)
        return -EINVAL;

    mutex_lock(&ns2->lock);

    if ((ns2->transport && ns2->transport != transport) ||
        (transport->controller && transport->controller != ns2)) {
        ret = -EBUSY;
        goto out;
    }

    transport->controller = ns2;
    ns2->transport = transport;

    if (ns2->hdev) {
        ret = switch2_init_advance_locked(ns2);
        if (ret) {
            ns2->transport = NULL;
            transport->controller = NULL;
        }
    }

out:
    mutex_unlock(&ns2->lock);
    return ret;
}
EXPORT_SYMBOL_GPL(switch2_transport_attach);

void switch2_transport_detach(struct switch2_transport *transport)
{
    struct switch2_controller *ns2;

    if (!transport)
        return;

    ns2 = transport->controller;
    if (!ns2)
        return;

    mutex_lock(&ns2->lock);

    if (ns2->transport == transport)
        ns2->transport = NULL;

    if (transport->controller == ns2)
        transport->controller = NULL;

    mutex_unlock(&ns2->lock);
}
EXPORT_SYMBOL_GPL(switch2_transport_detach);
