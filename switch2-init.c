// SPDX-License-Identifier: GPL-2.0-or-later

#include "switch2.h"

#include <linux/unaligned.h>

static const u8 switch2_init_cmd_data[] = {
    /*
     * The last 6 bytes of this packet are the MAC address of
     * the console, but we don't need that for USB/BLE host operation.
     */
    0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

static const u8 switch2_one_data[] = { 0x01, 0x00, 0x00, 0x00 };
static const u8 switch2_zero_data[] = { 0x00, 0x00, 0x00, 0x00 };
static const u8 switch2_ble_finalize_data[] = { 0x00 };

static const u8 switch2_ble_extended_setup[] = {
    0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x35, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00,
};

static const u8 switch2_ble_led_setup[] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* This startup profile is deliberately separate from the normal 0x27 controller
 * feature mask below: the working measured BLE sequence sends 0x94 during
 * transport bring-up and the common driver init subsequently selects ordinary
 * controller features as USB does.
 */
static const u8 switch2_ble_feature_mask[] = { 0x94, 0x00, 0x00, 0x00 };
static const __le32 switch2_ble_input_mode = cpu_to_le32(0x09);

/* Raw GATT control selector sent only after the serial/controller-info read at
 * 0x13002 has completed.  It is not a normal eight-byte switch2 command.
 */
static const u8 switch2_ble_raw_selector[] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x30,
};

static const u8 switch2_player_pattern[] = {
    0x1, 0x3, 0x7, 0xf, 0x9, 0x5, 0xd, 0x6
};

static __le32 switch2_feature_mask(struct switch2_controller *ns2)
{
    u32 features = NS2_FEATURE_BUTTONS | NS2_FEATURE_ANALOG |
        NS2_FEATURE_IMU | NS2_FEATURE_RUMBLE;

    /* The optical sensor is present only on Joy-Con 2.  Enable it for the
     * Bluetooth path where the driver exposes a dedicated relative mouse;
     * leave physical USB feature selection unchanged. */
    if (switch2_controller_is_ble(ns2) &&
        switch2_controller_is_joycon(ns2->type))
        features |= NS2_FEATURE_MOUSE;

    return cpu_to_le32(features);
}

static int switch2_read_flash(struct switch2_controller *ns2,
                              u32 address, u8 size)
{
    u8 message[8] = { size, 0x7e };

    put_unaligned_le32(address, &message[4]);

    return switch2_transport_send(ns2, NS2_CMD_FLASH,
        NS2_SUBCMD_FLASH_READ, message, sizeof(message));
}

static int switch2_set_player_id(struct switch2_controller *ns2, u32 player_id)
{
    u8 message[8];

    player_id %= NS2_MAX_PLAYER_ID;
    memset(message, 0, sizeof(message));
    message[0] = switch2_player_pattern[player_id];

    return switch2_transport_send(ns2, NS2_CMD_LED,
        NS2_SUBCMD_LED_PATTERN, message, sizeof(message));
}

static void switch2_init_work(struct work_struct *work)
{
    struct switch2_controller *ns2 = container_of(to_delayed_work(work),
        struct switch2_controller, init_work);
    int ret;

    mutex_lock(&ns2->lock);
    if (!ns2->transport)
        goto out;

    ret = switch2_init_advance_locked(ns2);
    if (ret)
        hid_err(ns2->hdev,
            "initialization failed at step %u: %d\n",
            ns2->init_step, ret);
out:
    mutex_unlock(&ns2->lock);
}

void switch2_init_work_init(struct switch2_controller *ns2)
{
    INIT_DELAYED_WORK(&ns2->init_work, switch2_init_work);
}

void switch2_init_work_cancel(struct switch2_controller *ns2)
{
    cancel_delayed_work_sync(&ns2->init_work);
}

void switch2_init_schedule_locked(struct switch2_controller *ns2,
                                             unsigned int delay_ms)
{
    lockdep_assert_held(&ns2->lock);
    mod_delayed_work(system_wq, &ns2->init_work,
        msecs_to_jiffies(delay_ms));
}

int switch2_init_advance_locked(struct switch2_controller *ns2)
{
    lockdep_assert_held(&ns2->lock);

    if (!ns2->transport)
        return -ENOTCONN;

    /*
     * The same state machine now owns both the BLE transport preamble and the
     * controller's ordinary USB/BLE initialization.  bluetoothd only makes
     * GATT usable and forwards bytes; it does not understand this sequence.
     */
    if (ns2->init_step == NS2_INIT_DONE) {
        if (switch2_controller_is_ble(ns2)) {
            int ret;

            ret = rcu_access_pointer(ns2->mouse_input) ? 0 :
                switch2_mouse_input_create(ns2);
            if (ret)
                return ret;

            return switch2_ble_frontend_sync_locked(ns2);
        }
        return rcu_access_pointer(ns2->input) ? 0 : switch2_input_create(ns2);
    }

    /* Report selection is acknowledged before the controller starts its live
     * side stream.  Do not advance until the first genuine side notification
     * moves the state to NS2_INIT_BLE_INPUT_READY. */
    if (ns2->init_step == NS2_INIT_BLE_WAIT_INPUT)
        return 0;

    /* USB skips the BLE transport preamble as one block.  The shared serial
     * and calibration reads follow it.  BLE changes its raw input format only
     * after those flash transactions are finished. */
    if (!switch2_controller_is_ble(ns2) &&
        ns2->init_step < NS2_INIT_READ_SERIAL)
        ns2->init_step = NS2_INIT_READ_SERIAL - 1;

    while (ns2->init_step < NS2_INIT_DONE) {
        ns2->init_step++;

        switch (ns2->init_step) {
        case NS2_INIT_BLE_INITIAL_SETUP:
            return switch2_transport_send(ns2, NS2_CMD_INIT,
                NS2_SUBCMD_INIT_USB, switch2_init_cmd_data,
                sizeof(switch2_init_cmd_data));

        case NS2_INIT_BLE_SETUP_07:
            return switch2_transport_send(ns2, NS2_CMD_BLE_SETUP_07,
                0x01, NULL, 0);

        case NS2_INIT_BLE_SETUP_16:
            return switch2_transport_send(ns2, NS2_CMD_BLE_SETUP_16,
                0x01, NULL, 0);

        case NS2_INIT_BLE_FINALIZE:
            return switch2_transport_send(ns2, NS2_CMD_BT_PAIR,
                0x03, switch2_ble_finalize_data,
                sizeof(switch2_ble_finalize_data));

        case NS2_INIT_BLE_SET_FEATURE_MASK:
            return switch2_transport_send(ns2, NS2_CMD_FEATSEL,
                NS2_SUBCMD_FEATSEL_SET_MASK,
                switch2_ble_feature_mask, sizeof(switch2_ble_feature_mask));

        case NS2_INIT_BLE_MOTION_SETUP:
            return switch2_transport_send(ns2, NS2_CMD_BLE_MOTION,
                0x03, NULL, 0);

        case NS2_INIT_BLE_EXTENDED_SETUP:
            return switch2_transport_send(ns2, NS2_CMD_VIBRATE,
                0x08, switch2_ble_extended_setup,
                sizeof(switch2_ble_extended_setup));

        case NS2_INIT_BLE_ENABLE_FEATURES:
            return switch2_transport_send(ns2, NS2_CMD_FEATSEL,
                NS2_SUBCMD_FEATSEL_ENABLE,
                switch2_ble_feature_mask, sizeof(switch2_ble_feature_mask));

        case NS2_INIT_BLE_SELECT_REPORT:
            return switch2_transport_send(ns2, NS2_CMD_INIT,
                NS2_SUBCMD_INIT_SELECT_REPORT,
                &switch2_ble_input_mode, sizeof(switch2_ble_input_mode));

        case NS2_INIT_BLE_SETUP_10:
            return switch2_transport_send(ns2, NS2_CMD_FW_INFO,
                0x01, NULL, 0);

        case NS2_INIT_BLE_SETUP_01_0C:
            return switch2_transport_send(ns2, NS2_CMD_NFC,
                0x0c, NULL, 0);

        case NS2_INIT_BLE_SETUP_01_01:
            return switch2_transport_send(ns2, NS2_CMD_NFC,
                0x01, switch2_zero_data, sizeof(switch2_zero_data));

        case NS2_INIT_BLE_SET_LED:
            return switch2_transport_send(ns2, NS2_CMD_LED,
                NS2_SUBCMD_LED_PATTERN,
                switch2_ble_led_setup, sizeof(switch2_ble_led_setup));

        case NS2_INIT_BLE_READ_GYRO_CALIB:
            return switch2_read_flash(ns2, NS2_FLASH_ADDR_BLE_GYRO_CALIB,
                NS2_FLASH_SIZE_BLE_GYRO_CALIB);

        case NS2_INIT_BLE_READ_ACCEL_CALIB:
            return switch2_read_flash(ns2, NS2_FLASH_ADDR_BLE_ACCEL_CALIB,
                NS2_FLASH_SIZE_BLE_ACCEL_CALIB);

        case NS2_INIT_READ_SERIAL:
            if (switch2_controller_is_ble(ns2) &&
                !switch2_ble_motion_calibration_ready(ns2))
                return -ENODATA;
            return switch2_read_flash(ns2,
                NS2_FLASH_ADDR_SERIAL, NS2_FLASH_SIZE_SERIAL);

        case NS2_INIT_GET_FIRMWARE_INFO:
            return switch2_transport_send(ns2,
                NS2_CMD_FW_INFO, NS2_SUBCMD_FW_INFO_GET, NULL, 0);

        case NS2_INIT_READ_FACTORY_PRIMARY_CALIB:
            return switch2_read_flash(ns2,
                NS2_FLASH_ADDR_FACTORY_PRIMARY_CALIB,
                NS2_FLASH_SIZE_FACTORY_AXIS_CALIB);

        case NS2_INIT_READ_FACTORY_SECONDARY_CALIB:
            if (switch2_controller_is_joycon(ns2->type))
                break;

            return switch2_read_flash(ns2,
                NS2_FLASH_ADDR_FACTORY_SECONDARY_CALIB,
                NS2_FLASH_SIZE_FACTORY_AXIS_CALIB);

        case NS2_INIT_READ_FACTORY_TRIGGER_CALIB:
            if (ns2->type != NS2_CTLR_TYPE_GC)
                break;

            return switch2_read_flash(ns2,
                NS2_FLASH_ADDR_FACTORY_TRIGGER_CALIB,
                NS2_FLASH_SIZE_FACTORY_TRIGGER_CALIB);

        case NS2_INIT_READ_USER_PRIMARY_CALIB:
            return switch2_read_flash(ns2,
                NS2_FLASH_ADDR_USER_PRIMARY_CALIB,
                NS2_FLASH_SIZE_USER_AXIS_CALIB);

        case NS2_INIT_READ_USER_SECONDARY_CALIB:
            if (switch2_controller_is_joycon(ns2->type))
                break;

            return switch2_read_flash(ns2,
                NS2_FLASH_ADDR_USER_SECONDARY_CALIB,
                NS2_FLASH_SIZE_USER_AXIS_CALIB);

        case NS2_INIT_BLE_RAW_SELECTOR: {
            int ret;

            if (!switch2_controller_is_ble(ns2))
                break;

            /* This raw selector has no protocol reply, so preserve the measured
             * BLE command cadence explicitly before advancing. */
            ret = switch2_transport_send_raw(ns2, switch2_ble_raw_selector,
                sizeof(switch2_ble_raw_selector));
            if (ret)
                return ret;

            switch2_init_schedule_locked(ns2, 10);
            return 0;
        }

        case NS2_INIT_SET_FEATURE_MASK: {
            __le32 feature_mask = switch2_feature_mask(ns2);

            return switch2_transport_send(ns2,
                NS2_CMD_FEATSEL, NS2_SUBCMD_FEATSEL_SET_MASK,
                &feature_mask, sizeof(feature_mask));
        }

        case NS2_INIT_ENABLE_FEATURES: {
            __le32 feature_mask = switch2_feature_mask(ns2);

            return switch2_transport_send(ns2,
                NS2_CMD_FEATSEL, NS2_SUBCMD_FEATSEL_ENABLE,
                &feature_mask, sizeof(feature_mask));
        }

#ifdef CONFIG_SWITCH2_FF
        case NS2_INIT_ENABLE_RUMBLE:
            return switch2_transport_send(ns2, NS2_CMD_NFC, 1,
                switch2_zero_data, sizeof(switch2_zero_data));
#endif

        case NS2_INIT_GRIP_BUTTONS:
            if (!switch2_controller_is_joycon(ns2->type))
                break;

            return switch2_transport_send(ns2,
                NS2_CMD_GRIP, NS2_SUBCMD_GRIP_ENABLE_BUTTONS,
                switch2_one_data, sizeof(switch2_one_data));

        case NS2_INIT_SET_PLAYER_LEDS:
            if (ns2->player_id == U32_MAX)
                break;
            return switch2_set_player_id(ns2, ns2->player_id);

        case NS2_INIT_USB_FINALIZE:
            if (switch2_controller_is_ble(ns2))
                break;

            return switch2_transport_send(ns2, NS2_CMD_INIT,
                NS2_SUBCMD_INIT_USB, switch2_init_cmd_data,
                sizeof(switch2_init_cmd_data));

        case NS2_INIT_DONE: {
            int ret;

            if (switch2_controller_is_ble(ns2)) {
                /* BLE always keeps the optical mouse independent.  jc1_compat
                 * selects which controller frontend is published on top of the
                 * real 057e:2066/2067 GATT-backed HID endpoint. */
                ret = switch2_mouse_input_create(ns2);
                if (ret)
                    return ret;

                return switch2_ble_frontend_sync_locked(ns2);
            }

            ret = switch2_input_create(ns2);
            if (ret) {
                ns2->init_step = NS2_INIT_USB_FINALIZE;
                dev_err(switch2_dev(ns2),
                    "failed to create input device: %d\n", ret);
            }
            return ret;
        }

        default:
            WARN_ON_ONCE(1);
        }
    }

    return 0;
}

