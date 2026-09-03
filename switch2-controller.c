// SPDX-License-Identifier: GPL-2.0-or-later

#include "switch2.h"

static DEFINE_IDA(switch2_player_id_allocator);

static int switch2_allocate_player_id(struct switch2_controller *ns2)
{
    int ret;

    ns2->player_id = U32_MAX;

    ret = ida_alloc(&switch2_player_id_allocator, GFP_KERNEL);
    if (ret < 0)
        return ret;

    ns2->player_id = ret;
    return 0;
}

static void switch2_release_player_id(struct switch2_controller *ns2)
{
    if (!ns2)
        return;

    if (ns2->player_id == U32_MAX)
        return;

    dev_info(switch2_dev(ns2),
        "player release: bus=%04x product=%04x player_id=%u phys=%s\n",
        ns2->hdev->bus, ns2->hdev->product, ns2->player_id,
        ns2->hdev->phys[0] ? ns2->hdev->phys : "-");
    ida_free(&switch2_player_id_allocator, ns2->player_id);
    ns2->player_id = U32_MAX;
}

void switch2_controller_cleanup(void)
{
    ida_destroy(&switch2_player_id_allocator);
}

int switch2_controller_activate_locked(struct switch2_controller *ns2)
{
    int ret;

    lockdep_assert_held(&ns2->lock);

    ret = switch2_allocate_player_id(ns2);
    if (ret < 0) {
        dev_warn(switch2_dev(ns2),
            "Failed to allocate player ID, skipping; ret=%d\n", ret);
        ret = 0;
    }

    ret = switch2_ff_init(ns2);
    if (ret)
        goto err_player;

    if (ns2->transport) {
        ret = switch2_init_advance_locked(ns2);
        if (ret)
            goto err_ff;
    }

    return 0;

err_ff:
    switch2_ff_destroy(ns2);

err_player:
    switch2_release_player_id(ns2);

    return ret;
}

void switch2_controller_deactivate(struct switch2_controller *ns2)
{
    if (!ns2)
        return;

    /* The initialization worker also takes ns2->lock, so stop it before the
     * native HID device starts disappearing. This matters when the USB transport
     * interface outlives the HID interface. */
    switch2_init_work_cancel(ns2);

    mutex_lock(&ns2->lock);
    switch2_input_disconnect_locked(ns2);
    mutex_unlock(&ns2->lock);

    switch2_input_destroy(ns2);
    switch2_ff_destroy(ns2);
    switch2_release_player_id(ns2);

    mutex_lock(&ns2->lock);
    ns2->hdev = NULL;
    mutex_unlock(&ns2->lock);
}
