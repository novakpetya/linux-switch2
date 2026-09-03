// SPDX-License-Identifier: GPL-2.0-or-later

#include "switch2.h"

#include <linux/jiffies.h>

enum gc_rumble {
    GC_RUMBLE_OFF = 0,
    GC_RUMBLE_ON = 1,
    GC_RUMBLE_STOP = 2,
};

/*
 * The highest rumble level for "HD Rumble" is strong enough to potentially damage the controller,
 * and also leaves your hands feeling like melted jelly, so we set a semi-abitrary scaling factor
 * to artificially limit the maximum for safety and comfort. It is currently unknown if the Switch
 * 2 itself does something similar, but it's quite likely.
 *
 * This value must be between 0 and 1024, otherwise the math below will overflow.
 */
#define RUMBLE_MAX 450u

/*
 * Semi-arbitrary values used to simulate the "rumble" sensation of an eccentric rotating
 * mass type haptic motor on the Switch 2 controllers' linear resonant actuator type haptics.
 *
 * The units used are unknown, but the values must be between 0 and 1023.
 */
#define RUMBLE_HI_FREQ 0x187
#define RUMBLE_LO_FREQ 0x112

/* Native USB can accept the original 4 ms stream. BLE is paced more slowly
 * to avoid starving input while a persistent rumble effect is active. */
#define RUMBLE_NATIVE_INTERVAL_MS 4
#define RUMBLE_BLE_INTERVAL_MS 20
#define RUMBLE_BLE_FRAME_SIZE 5
#define RUMBLE_BLE_FRAME_COUNT 3
#define RUMBLE_BLE_REPORT_SIZE (2 + RUMBLE_BLE_FRAME_SIZE * RUMBLE_BLE_FRAME_COUNT)

#ifdef CONFIG_SWITCH2_FF
static void switch2_encode_rumble(struct switch2_hd_rumble *rumble, uint8_t buffer[5])
{
    buffer[0] = rumble->hi_freq;
    buffer[1] = (rumble->hi_freq >> 8) | (rumble->hi_amp << 2);
    buffer[2] = (rumble->hi_amp >> 6) | (rumble->lo_freq << 4);
    buffer[3] = (rumble->lo_freq >> 4) | (rumble->lo_amp << 6);
    buffer[4] = rumble->lo_amp >> 2;
}

static void switch2_usb_rumble_to_ble(const uint8_t usb_report[64],
                                      uint8_t ble_report[RUMBLE_BLE_REPORT_SIZE])
{
    int i;

    memset(ble_report, 0, RUMBLE_BLE_REPORT_SIZE);
    ble_report[0] = 0x00;
    ble_report[1] = usb_report[1];
    for (i = 0; i < RUMBLE_BLE_FRAME_COUNT; i++)
        memcpy(&ble_report[2 + i * RUMBLE_BLE_FRAME_SIZE],
               &usb_report[2], RUMBLE_BLE_FRAME_SIZE);
}

static void switch2_rumble_work(struct work_struct *work)
{
    struct switch2_controller *ns2 = container_of(to_delayed_work(work),
                                                  struct switch2_controller, rumble_work);
    unsigned long now = jiffies;
    uint8_t *buffer = kzalloc(64, GFP_KERNEL);
    unsigned long flags;
    bool active;
    int ret;

    if (!buffer)
        return;

    spin_lock_irqsave(&ns2->rumble_lock, flags);
    buffer[0x1] = 0x50 | ns2->rumble_seq;
    if (ns2->type == NS2_CTLR_TYPE_GC) {
        buffer[0] = 3;
        if (ns2->rumble.sd.amplitude == 0) {
            buffer[2] = GC_RUMBLE_STOP;
            ns2->rumble.sd.error = 0;
            active = false;
        } else {
            if (ns2->rumble.sd.error < ns2->rumble.sd.amplitude) {
                buffer[2] = GC_RUMBLE_ON;
                ns2->rumble.sd.error += U16_MAX - ns2->rumble.sd.amplitude;
            } else {
                buffer[2] = GC_RUMBLE_OFF;
                ns2->rumble.sd.error -= ns2->rumble.sd.amplitude;
            }
            active = true;
        }
    } else {
        buffer[0] = 1;
        switch2_encode_rumble(&ns2->rumble.hd, &buffer[0x2]);
        active = ns2->rumble.hd.hi_amp || ns2->rumble.hd.lo_amp;
        if (ns2->type == NS2_CTLR_TYPE_PRO) {
            /*
             * The Pro Controller contains separate LRAs on each
             * side that can be controlled individually.
             */
            buffer[0] = 2;
            buffer[0x11] = 0x50 | ns2->rumble_seq;
            switch2_encode_rumble(&ns2->rumble.hd, &buffer[0x12]);
        }
    }
    ns2->rumble_seq = (ns2->rumble_seq + 1) & 0xF;

    if (active) {
        unsigned int interval_ms = switch2_controller_is_ble(ns2) ?
            RUMBLE_BLE_INTERVAL_MS : RUMBLE_NATIVE_INTERVAL_MS;
        unsigned long interval = msecs_to_jiffies(interval_ms);

        unsigned long next;

        if (!ns2->last_rumble_jiffies)
            ns2->last_rumble_jiffies = now;
        else
            ns2->last_rumble_jiffies += interval;

        next = ns2->last_rumble_jiffies + interval;
        schedule_delayed_work(&ns2->rumble_work,
                              time_after(next, now) ? next - now : 0);
    } else {
        ns2->last_rumble_jiffies = 0;
    }
    spin_unlock_irqrestore(&ns2->rumble_lock, flags);

    if (switch2_controller_is_ble(ns2) &&
        switch2_controller_is_joycon(ns2->type)) {
        uint8_t ble_report[RUMBLE_BLE_REPORT_SIZE];

        /* Joy-Con 2 BLE haptics use the same encoded 5-byte frame as USB,
         * repeated three times behind a BLE transport byte. */
        switch2_usb_rumble_to_ble(buffer, ble_report);
        if (!ns2->hdev)
            ret = -ENODEV;
        else
            ret = hid_hw_output_report(ns2->hdev, ble_report,
                                       sizeof(ble_report));
    } else {
        if (!ns2->hdev)
            ret = -ENODEV;
        else
            ret = hid_hw_output_report(ns2->hdev, buffer, 64);
    }
    if (ret == -EOPNOTSUPP || ret == -ENODEV)
        cancel_delayed_work(&ns2->rumble_work);

    kfree(buffer);
    if (ret < 0 && ns2->hdev)
        hid_dbg(ns2->hdev, "Failed to send output report ret=%d\n", ret);
}
#endif

int switch2_ff_init(struct switch2_controller *ns2)
{
    memset(&ns2->rumble, 0, sizeof(ns2->rumble));
    ns2->rumble_seq = 0;
    ns2->last_rumble_jiffies = 0;

    if (ns2->type != NS2_CTLR_TYPE_GC) {
        ns2->rumble.hd.hi_freq = RUMBLE_HI_FREQ;
        ns2->rumble.hd.lo_freq = RUMBLE_LO_FREQ;
    }

    spin_lock_init(&ns2->rumble_lock);
    INIT_DELAYED_WORK(&ns2->rumble_work, switch2_rumble_work);

    return 0;
}

void switch2_ff_destroy(struct switch2_controller *ns2)
{
    cancel_delayed_work_sync(&ns2->rumble_work);
}

int switch2_ff_set_rumble(struct switch2_controller *ns2,
    u16 strong_magnitude, u16 weak_magnitude)
{
    if (!ns2)
        return -EINVAL;

    guard(spinlock_irqsave)(&ns2->rumble_lock);
    if (ns2->type == NS2_CTLR_TYPE_GC) {
        ns2->rumble.sd.amplitude = max(strong_magnitude,
            weak_magnitude >> 1);
    } else {
        ns2->rumble.hd.hi_amp =
            (u32)weak_magnitude * RUMBLE_MAX >> 16;
        ns2->rumble.hd.lo_amp =
            (u32)strong_magnitude * RUMBLE_MAX >> 16;
    }

    schedule_delayed_work(&ns2->rumble_work, 0);
    return 0;
}

static int switch2_play_effect(struct input_dev *dev, void *data,
    struct ff_effect *effect)
{
    struct switch2_controller *ns2 = input_get_drvdata(dev);

    if (effect->type != FF_RUMBLE)
        return 0;

    return switch2_ff_set_rumble(ns2, effect->u.rumble.strong_magnitude,
        effect->u.rumble.weak_magnitude);
}

int switch2_ff_configure_input(struct switch2_controller *ns2,
    struct input_dev *input)
{
    if (!ns2 || !input)
        return -EINVAL;

    input_set_capability(input, EV_FF, FF_RUMBLE);
    return input_ff_create_memless(input, NULL, switch2_play_effect);
}
