// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/unaligned.h>

#include "switch2.h"

/*
 * Native Joy-Con 2 BLE side-report layout 0x28 motion decoder.
 *
 * The GATT transport supplies report ID 0x07/0x08 plus the controller's 63
 * native bytes. Bit offsets below are relative to the native packet (raw_data + 1).
 * Captures establish signed 14-bit accel and signed 15-bit gyro bitstream fields.
 * chronological order:
 *
 *   accel: sample 0 (oldest), sample 1, sample 2 (newest)
 *   gyro:  group 0 (older), group 1 (newer)
 *
 * ABS_X/Y/Z and ABS_RX/RY/RZ expose the newest values.  Full native BLE
 * chronology remains internal to the driver and is not exported as MSC_RAW.
 */
#define NS2_BLE_HID_STATE_SIZE          64
#define NS2_BLE_LAYOUT_COMMON           0x28

#define NS2_BLE_SIDE_MOUSE_OFFSET       9
#define NS2_BLE_COMMON_MOUSE_OFFSET     17
#define NS2_BLE_MOUSE_COVERED_OFFSET    4

#define NS2_MOUSE_SURFACE_ENTER_REPORTS 2
#define NS2_MOUSE_SURFACE_EXIT_REPORTS  3
#define NS2_MOUSE_SCROLL_RANGE          2048
#define NS2_MOUSE_SCROLL_MAX_DT_MS      50

/*
 * Surface classification is entirely optical.  The one-byte side-report
 * field and the low byte of the first common raw status word carry the same
 * lift-off / coveredness signal: uncovered is normally 0xff while contact
 * drives it lower.  The second common raw status word cleanly
 * separates usable desk/fabric surfaces from direct skin occlusion in the
 * validated left/right captures.  Keep the field semantically unnamed until
 * its protocol meaning is established.
 */
#define NS2_MOUSE_COVER_ENTER          0xf0
#define NS2_MOUSE_COVER_EXIT           0xfa
#define NS2_MOUSE_STATUS_ENTER         0x00ff
#define NS2_MOUSE_STATUS_EXIT          0x0140
#define NS2_MOUSE_SCROLL_DEADZONE      512
#define NS2_MOUSE_SCROLL_MAX_HZ        20

static struct input_dev *switch2_mouse_get(struct switch2_controller *ns2)
{
    struct input_dev *mouse;

    rcu_read_lock();
    mouse = rcu_dereference(ns2->mouse_input);
    if (mouse)
        input_get_device(mouse);
    rcu_read_unlock();

    return mouse;
}

static void switch2_ble_mouse_release_buttons(struct switch2_controller *ns2)
{
    struct input_dev *mouse;
    bool sync = false;

    if (!ns2)
        return;

    mouse = switch2_mouse_get(ns2);
    if (mouse) {
        if (READ_ONCE(ns2->mouse_left_down)) {
            input_report_key(mouse, BTN_LEFT, 0);
            sync = true;
        }
        if (READ_ONCE(ns2->mouse_right_down)) {
            input_report_key(mouse, BTN_RIGHT, 0);
            sync = true;
        }
        if (READ_ONCE(ns2->mouse_middle_down)) {
            input_report_key(mouse, BTN_MIDDLE, 0);
            sync = true;
        }
        if (sync)
            input_sync(mouse);
        input_put_device(mouse);
    }

    WRITE_ONCE(ns2->mouse_left_down, false);
    WRITE_ONCE(ns2->mouse_right_down, false);
    WRITE_ONCE(ns2->mouse_middle_down, false);
}

static void switch2_ble_mouse_reset_state(struct switch2_controller *ns2,
                                           bool release_buttons)
{
    if (!ns2)
        return;

    if (release_buttons)
        switch2_ble_mouse_release_buttons(ns2);
    else {
        WRITE_ONCE(ns2->mouse_left_down, false);
        WRITE_ONCE(ns2->mouse_right_down, false);
        WRITE_ONCE(ns2->mouse_middle_down, false);
    }

    WRITE_ONCE(ns2->mouse_surface_active, false);
    WRITE_ONCE(ns2->mouse_surface_enter_count, 0);
    WRITE_ONCE(ns2->mouse_surface_exit_count, 0);
    WRITE_ONCE(ns2->mouse_common_valid, false);
    WRITE_ONCE(ns2->mouse_scroll_x_accum, 0);
    WRITE_ONCE(ns2->mouse_scroll_y_accum, 0);
    WRITE_ONCE(ns2->mouse_scroll_last_ns, 0);
}

static bool switch2_ble_mouse_update_surface(struct switch2_controller *ns2,
                                              u8 coveredness,
                                              bool status_valid, u16 status)
{
    bool active = READ_ONCE(ns2->mouse_surface_active);
    bool enter_ok = coveredness <= NS2_MOUSE_COVER_ENTER &&
                    (!status_valid || status <= NS2_MOUSE_STATUS_ENTER);
    bool exit_bad = coveredness >= NS2_MOUSE_COVER_EXIT ||
                    (status_valid && status >= NS2_MOUSE_STATUS_EXIT);
    bool changed = false;
    u8 count;

    if (!active) {
        WRITE_ONCE(ns2->mouse_surface_exit_count, 0);
        if (enter_ok) {
            count = READ_ONCE(ns2->mouse_surface_enter_count);
            if (count < NS2_MOUSE_SURFACE_ENTER_REPORTS)
                count++;
            WRITE_ONCE(ns2->mouse_surface_enter_count, count);
            if (count >= NS2_MOUSE_SURFACE_ENTER_REPORTS) {
                active = true;
                changed = true;
            }
        } else {
            WRITE_ONCE(ns2->mouse_surface_enter_count, 0);
        }
    } else {
        WRITE_ONCE(ns2->mouse_surface_enter_count, 0);
        if (exit_bad) {
            count = READ_ONCE(ns2->mouse_surface_exit_count);
            if (count < NS2_MOUSE_SURFACE_EXIT_REPORTS)
                count++;
            WRITE_ONCE(ns2->mouse_surface_exit_count, count);
            if (count >= NS2_MOUSE_SURFACE_EXIT_REPORTS) {
                active = false;
                changed = true;
            }
        } else {
            WRITE_ONCE(ns2->mouse_surface_exit_count, 0);
        }
    }

    if (!changed)
        return active;

    WRITE_ONCE(ns2->mouse_surface_active, active);
    WRITE_ONCE(ns2->mouse_surface_enter_count, 0);
    WRITE_ONCE(ns2->mouse_surface_exit_count, 0);
    WRITE_ONCE(ns2->mouse_common_valid, false);
    WRITE_ONCE(ns2->mouse_scroll_x_accum, 0);
    WRITE_ONCE(ns2->mouse_scroll_y_accum, 0);
    WRITE_ONCE(ns2->mouse_scroll_last_ns, 0);

    if (!active)
        switch2_ble_mouse_release_buttons(ns2);

    if (status_valid)
        dev_info(switch2_dev(ns2),
                 "mouse surface %s: coveredness=0x%02x status=0x%04x\n",
                 active ? "on" : "off", coveredness, status);
    else
        dev_info(switch2_dev(ns2),
                 "mouse surface %s: coveredness=0x%02x\n",
                 active ? "on" : "off", coveredness);
    return active;
}

static void switch2_ble_unpack_stick(const u8 stick[3], u16 *x, u16 *y)
{
    *x = stick[0] | ((stick[1] & 0x0f) << 8);
    *y = (stick[1] >> 4) | (stick[2] << 4);
}

static void switch2_ble_pack_stick(u8 stick[3], u16 x, u16 y)
{
    x &= 0x0fff;
    y &= 0x0fff;
    stick[0] = x;
    stick[1] = (x >> 8) | ((y & 0x000f) << 4);
    stick[2] = y >> 4;
}

static void switch2_ble_mouse_neutral_stick(struct switch2_controller *ns2,
                                             u8 stick[3])
{
    u16 x = ns2->stick_calib[0].x.neutral;
    u16 y = ns2->stick_calib[0].y.neutral;

    if (!x)
        x = 2048;
    if (!y)
        y = 2048;
    switch2_ble_pack_stick(stick, x, y);
}

static int switch2_ble_mouse_scroll_value(int raw, int center)
{
    const unsigned int deadzone = NS2_MOUSE_SCROLL_DEADZONE;
    int value = raw - center;

    if (value > (int)deadzone)
        return value - deadzone;
    if (value < -(int)deadzone)
        return value + deadzone;
    return 0;
}

static int switch2_ble_mouse_scroll_tick(s64 *accum, int value, u64 dt_ns)
{
    const unsigned int deadzone = NS2_MOUSE_SCROLL_DEADZONE;
    const unsigned int max_hz = NS2_MOUSE_SCROLL_MAX_HZ;
    s64 full = NS2_MOUSE_SCROLL_RANGE - deadzone;
    s64 threshold;
    s64 dt_ms;
    s64 ticks;

    if (!max_hz || !value) {
        *accum = 0;
        return 0;
    }

    dt_ms = div_u64(dt_ns, NSEC_PER_MSEC);
    dt_ms = clamp_t(s64, dt_ms, 1, NS2_MOUSE_SCROLL_MAX_DT_MS);

    threshold = div_s64(full * MSEC_PER_SEC, max_hz);
    if (threshold < 1)
        threshold = 1;

    *accum += (s64)value * dt_ms;
    ticks = div_s64(*accum, threshold);
    if (!ticks)
        return 0;

    *accum -= ticks * threshold;
    return clamp_t(s64, ticks, -8, 8);
}

static void switch2_ble_mouse_emit(struct switch2_controller *ns2,
                                    s16 dx, s16 dy, const u8 stick[3],
                                    bool left, bool right, bool middle)
{
    struct input_dev *mouse;
    u64 now;
    u64 last;
    u64 dt_ns = 0;
    u16 raw_x;
    u16 raw_y;
    int center_x;
    int center_y;
    int scroll_x;
    int scroll_y;
    int hwheel = 0;
    int wheel = 0;
    bool sync = false;

    mouse = switch2_mouse_get(ns2);
    if (!mouse)
        return;

    if (dx) {
        input_report_rel(mouse, REL_X, dx);
        sync = true;
    }
    if (dy) {
        input_report_rel(mouse, REL_Y, dy);
        sync = true;
    }

    if (left != READ_ONCE(ns2->mouse_left_down)) {
        input_report_key(mouse, BTN_LEFT, left);
        WRITE_ONCE(ns2->mouse_left_down, left);
        sync = true;
    }
    if (right != READ_ONCE(ns2->mouse_right_down)) {
        input_report_key(mouse, BTN_RIGHT, right);
        WRITE_ONCE(ns2->mouse_right_down, right);
        sync = true;
    }
    if (middle != READ_ONCE(ns2->mouse_middle_down)) {
        input_report_key(mouse, BTN_MIDDLE, middle);
        WRITE_ONCE(ns2->mouse_middle_down, middle);
        sync = true;
    }

    switch2_ble_unpack_stick(stick, &raw_x, &raw_y);
    center_x = ns2->stick_calib[0].x.neutral ?
        ns2->stick_calib[0].x.neutral : 2048;
    center_y = ns2->stick_calib[0].y.neutral ?
        ns2->stick_calib[0].y.neutral : 2048;
    scroll_x = switch2_ble_mouse_scroll_value(raw_x, center_x);
    scroll_y = switch2_ble_mouse_scroll_value(raw_y, center_y);

    now = ktime_get_ns();
    last = READ_ONCE(ns2->mouse_scroll_last_ns);
    WRITE_ONCE(ns2->mouse_scroll_last_ns, now);
    if (last && now > last)
        dt_ns = now - last;

    if (dt_ns) {
        s64 accum_x = READ_ONCE(ns2->mouse_scroll_x_accum);
        s64 accum_y = READ_ONCE(ns2->mouse_scroll_y_accum);

        hwheel = switch2_ble_mouse_scroll_tick(&accum_x, scroll_x, dt_ns);
        wheel = switch2_ble_mouse_scroll_tick(&accum_y, scroll_y, dt_ns);
        WRITE_ONCE(ns2->mouse_scroll_x_accum, accum_x);
        WRITE_ONCE(ns2->mouse_scroll_y_accum, accum_y);
    }

    if (hwheel) {
        input_report_rel(mouse, REL_HWHEEL, hwheel);
        sync = true;
    }
    if (wheel) {
        input_report_rel(mouse, REL_WHEEL, wheel);
        sync = true;
    }

    if (sync)
        input_sync(mouse);
    input_put_device(mouse);
}

static void switch2_ble_mouse_side_buttons(struct switch2_controller *ns2,
                                            const u8 *native,
                                            bool *left, bool *right,
                                            bool *middle)
{
    u8 primary = native[2];

    if (ns2->type == NS2_CTLR_TYPE_JCR) {
        *left = primary & NS2_BTNR_R;
        *right = primary & NS2_BTNR_ZR;
        *middle = primary & NS2_BTNR_RS;
    } else {
        *left = primary & NS2_BTNL_L;
        *right = primary & NS2_BTNL_ZL;
        *middle = primary & NS2_BTNL_LS;
    }
}

static void switch2_ble_mouse_common_buttons(struct switch2_controller *ns2,
                                              const u8 *raw_data,
                                              bool *left, bool *right,
                                              bool *middle)
{
    if (ns2->type == NS2_CTLR_TYPE_JCR) {
        *left = raw_data[5] & BIT(6);
        *right = raw_data[5] & BIT(7);
        *middle = raw_data[6] & NS2_BLE_BTN_RS;
    } else {
        *left = raw_data[7] & NS2_BLE_BTN_SHOULDER;
        *right = raw_data[7] & NS2_BLE_BTN_TRIGGER;
        *middle = raw_data[6] & NS2_BLE_BTN_LS;
    }
}

static void switch2_ble_mouse_report_side(struct switch2_controller *ns2,
                                           const u8 *native)
{
    bool left;
    bool right;
    bool middle;
    s16 dx;
    s16 dy;

    if (!ns2 || !native || !switch2_controller_is_ble(ns2) ||
        !switch2_controller_is_joycon(ns2->type))
        return;

    /* Side reports carry signed relative optical deltas and a one-byte
     * coveredness/lift-off value directly. */
    WRITE_ONCE(ns2->mouse_common_valid, false);
    if (!switch2_ble_mouse_update_surface(
            ns2, native[NS2_BLE_SIDE_MOUSE_OFFSET +
                        NS2_BLE_MOUSE_COVERED_OFFSET], false, 0)) {
        return;
    }

    dx = (s16)get_unaligned_le16(&native[NS2_BLE_SIDE_MOUSE_OFFSET]);
    dy = (s16)get_unaligned_le16(&native[NS2_BLE_SIDE_MOUSE_OFFSET + 2]);
    switch2_ble_mouse_side_buttons(ns2, native, &left, &right, &middle);
    switch2_ble_mouse_emit(ns2, dx, dy, &native[5],
                           left, right, middle);
}

static void switch2_ble_mouse_report_common(struct switch2_controller *ns2,
                                             const u8 *raw_data, int size)
{
    bool left;
    bool right;
    bool middle;
    unsigned int stick_offset;
    u16 status;
    u16 x;
    u16 y;
    s16 dx = 0;
    s16 dy = 0;

    if (!ns2 || !raw_data || size != NS2_BLE_HID_STATE_SIZE ||
        raw_data[0] != NS2_REPORT_COMMON ||
        !switch2_controller_is_ble(ns2) ||
        !switch2_controller_is_joycon(ns2->type))
        return;

    /* Common 0x05 carries cumulative 16-bit optical X/Y.  The low byte of the
     * following unknown word tracks the side report's coveredness field.  The
     * final 16-bit mouse word also separates working surfaces from direct skin
     * coverage in the saved captures, so use it as an additional raw status
     * gate without assigning an unproven protocol meaning to the field. */
    x = get_unaligned_le16(&raw_data[NS2_BLE_COMMON_MOUSE_OFFSET]);
    y = get_unaligned_le16(&raw_data[NS2_BLE_COMMON_MOUSE_OFFSET + 2]);
    status = get_unaligned_le16(&raw_data[NS2_BLE_COMMON_MOUSE_OFFSET + 6]);

    if (!switch2_ble_mouse_update_surface(
            ns2, raw_data[NS2_BLE_COMMON_MOUSE_OFFSET +
                          NS2_BLE_MOUSE_COVERED_OFFSET], true, status)) {
        WRITE_ONCE(ns2->mouse_common_valid, false);
        return;
    }

    if (!READ_ONCE(ns2->mouse_common_valid)) {
        WRITE_ONCE(ns2->mouse_common_x, x);
        WRITE_ONCE(ns2->mouse_common_y, y);
        WRITE_ONCE(ns2->mouse_common_valid, true);
    } else {
        /* Unsigned subtraction provides modulo-65536 wrap; narrowing to s16
         * yields the shortest wrapped relative displacement. */
        dx = (s16)(x - READ_ONCE(ns2->mouse_common_x));
        dy = (s16)(y - READ_ONCE(ns2->mouse_common_y));
        WRITE_ONCE(ns2->mouse_common_x, x);
        WRITE_ONCE(ns2->mouse_common_y, y);
    }

    stick_offset = ns2->type == NS2_CTLR_TYPE_JCR ? 14 : 11;
    switch2_ble_mouse_common_buttons(ns2, raw_data,
                                     &left, &right, &middle);
    switch2_ble_mouse_emit(ns2, dx, dy, &raw_data[stick_offset],
                           left, right, middle);
}

static void switch2_ble_mouse_suppress_controller_controls(
    struct switch2_controller *ns2, u8 buttons[4], u8 stick[3])
{
    if (!READ_ONCE(ns2->mouse_surface_active))
        return;

    if (ns2->type == NS2_CTLR_TYPE_JCR) {
        buttons[0] &= ~(NS2_BLE_BTN_SHOULDER | NS2_BLE_BTN_TRIGGER);
        buttons[1] &= ~NS2_BLE_BTN_RS;
    } else {
        buttons[2] &= ~(NS2_BLE_BTN_SHOULDER | NS2_BLE_BTN_TRIGGER);
        buttons[1] &= ~NS2_BLE_BTN_LS;
    }

    switch2_ble_mouse_neutral_stick(ns2, stick);
}

int switch2_mouse_input_create(struct switch2_controller *ns2)
{
    struct input_dev *mouse;
    int ret;

    if (!ns2 || !ns2->hdev || !switch2_controller_is_ble(ns2) ||
        !switch2_controller_is_joycon(ns2->type))
        return 0;

    if (rcu_access_pointer(ns2->mouse_input))
        return 0;

    mouse = devm_input_allocate_device(&ns2->hdev->dev);
    if (!mouse)
        return -ENOMEM;

    input_set_drvdata(mouse, ns2);
    mouse->dev.parent = &ns2->hdev->dev;
    mouse->id.bustype = ns2->hdev->bus;
    mouse->id.vendor = ns2->hdev->vendor;
    mouse->id.product = ns2->hdev->product;
    mouse->id.version = ns2->hdev->version;
    mouse->name = ns2->type == NS2_CTLR_TYPE_JCR ?
        "Nintendo Joy-Con 2 (R) Mouse" :
        "Nintendo Joy-Con 2 (L) Mouse";
    mouse->phys = ns2->hdev->phys;
    mouse->uniq = ns2->serial;

    __set_bit(INPUT_PROP_POINTER, mouse->propbit);
    input_set_capability(mouse, EV_KEY, BTN_LEFT);
    input_set_capability(mouse, EV_KEY, BTN_RIGHT);
    input_set_capability(mouse, EV_KEY, BTN_MIDDLE);
    input_set_capability(mouse, EV_REL, REL_X);
    input_set_capability(mouse, EV_REL, REL_Y);
    input_set_capability(mouse, EV_REL, REL_WHEEL);
    input_set_capability(mouse, EV_REL, REL_HWHEEL);
    input_set_events_per_packet(mouse, 10);

    ret = input_register_device(mouse);
    if (ret)
        return ret;

    switch2_ble_mouse_reset_state(ns2, false);
    rcu_assign_pointer(ns2->mouse_input, mouse);
    return 0;
}




static const unsigned int switch2_ble_accel_starts[9] = {
    220, 234, 248, 310, 323, 336, 397, 411, 425
};
static const unsigned int switch2_ble_gyro_starts[6] = {
    263, 279, 295, 350, 366, 382
};
static u16 switch2_ble_get_raw_bits(const u8 *data, unsigned int start,
                                      unsigned int width)
{
    u32 word;
    u32 mask;

    word = (u32)data[start / 8] |
           ((u32)data[start / 8 + 1] << 8) |
           ((u32)data[start / 8 + 2] << 16);
    mask = (1U << width) - 1;

    return (u16)((word >> (start & 7)) & mask);
}

static s16 switch2_ble_sign_extend_13(u16 raw13)
{
    raw13 &= 0x1fff;

    if (raw13 & BIT(12))
        raw13 |= 0xe000;

    return (s16)raw13;
}

static s16 switch2_ble_sign_extend_14(u16 raw14)
{
    raw14 &= 0x3fff;

    /* Bit 13 is the sign bit of a 14-bit two's-complement value. */
    if (raw14 & BIT(13))
        raw14 |= 0xc000;

    return (s16)raw14;
}

static s16 switch2_ble_sign_extend_15(u16 raw15)
{
    raw15 &= 0x7fff;

    /* Bit 14 is the sign bit of a 15-bit two's-complement value. */
    if (raw15 & BIT(14))
        raw15 |= 0x8000;

    return (s16)raw15;
}


/* Convert the native BLE control banks into the internal common Format-0
 * button area consumed by the synthetic classic-Joy-Con compatibility HID.
 *
 * Native BLE layout 0x28 uses the same primary byte position on both sides,
 * but the meanings are side-specific.  native[5..7] is the active side's
 * packed 12-bit stick.
 *
 * Common Format-0 payload:
 *   buttons[0] = right-side bank (Y/X/B/A, SR/SL, R/ZR)
 *   buttons[1] = shared bank (-,+,RS,LS,Home,Capture,C,...)
 *   buttons[2] = left-side bank (D-pad, SR/SL, L/ZL)
 *   buttons[3] = grip/status bank (GR/GL plus neutral/status bit 5)
 */
static void switch2_ble_side_controls(struct switch2_controller *ns2,
                                       const u8 *native,
                                       u8 buttons[4], u8 stick[3])
{
    const u8 primary = native[2];
    const u8 secondary = native[3];

    memset(buttons, 0, 4);
    buttons[3] = BIT(5);
    memcpy(stick, &native[5], 3);

    if (ns2->type == NS2_CTLR_TYPE_JCR) {
        /* Common right bank follows Nintendo's Y/X/B/A, SR/SL, R/ZR bit
         * positions.  C and Home were already proven on the native evdev
         * path; preserve those exact physical controls in the synthetic JC1 state here. */
        if (primary & NS2_BTNR_Y)
            buttons[0] |= BIT(0);
        if (primary & NS2_BTNR_X)
            buttons[0] |= BIT(1);
        if (primary & NS2_BTNR_B)
            buttons[0] |= BIT(2);
        if (primary & NS2_BTNR_A)
            buttons[0] |= BIT(3);
        if (secondary & NS2_BTN_JCR_SR)
            buttons[0] |= BIT(4);
        if (secondary & NS2_BTN_JCR_SL)
            buttons[0] |= BIT(5);
        if (primary & NS2_BTNR_R)
            buttons[0] |= BIT(6);
        if (primary & NS2_BTNR_ZR)
            buttons[0] |= BIT(7);

        if (primary & NS2_BTNR_PLUS)
            buttons[1] |= NS2_BLE_BTN_PLUS;
        if (primary & NS2_BTNR_RS)
            buttons[1] |= NS2_BLE_BTN_RS;
        if (secondary & NS2_BTN_JCR_HOME)
            buttons[1] |= NS2_BLE_BTN_HOME;
        if (secondary & NS2_BTN_JCR_C)
            buttons[1] |= NS2_BLE_BTN_C;
        if (secondary & NS2_BTN_JCR_GR)
            buttons[3] |= BIT(0);
        return;
    }

    /* Left-side mapping is the already-validated v36.5.60 common mapping.
     * In this internal representation O/Capture remains shared-bank bit 5 so
     * the synthetic JC1 compatibility HID is byte-for-byte unchanged. */
    if (primary & NS2_BTNL_MINUS)
        buttons[1] |= NS2_BLE_BTN_MINUS;
    if (primary & NS2_BTNL_LS)
        buttons[1] |= NS2_BLE_BTN_LS;
    if (secondary & NS2_BTN_JCL_CAPTURE)
        buttons[1] |= NS2_BLE_BTN_CAPTURE;

    if (primary & NS2_BTNL_DOWN)
        buttons[2] |= NS2_BLE_BTN_DOWN;
    if (primary & NS2_BTNL_UP)
        buttons[2] |= NS2_BLE_BTN_UP;
    if (primary & NS2_BTNL_RIGHT)
        buttons[2] |= NS2_BLE_BTN_RIGHT;
    if (primary & NS2_BTNL_LEFT)
        buttons[2] |= NS2_BLE_BTN_LEFT;
    if (secondary & NS2_BTN_JCL_SR)
        buttons[2] |= NS2_BLE_BTN_SR;
    if (secondary & NS2_BTN_JCL_SL)
        buttons[2] |= NS2_BLE_BTN_SL;
    if (primary & NS2_BTNL_L)
        buttons[2] |= NS2_BLE_BTN_SHOULDER;
    if (primary & NS2_BTNL_ZL)
        buttons[2] |= NS2_BLE_BTN_TRIGGER;
    if (secondary & NS2_BTN_JCL_GL)
        buttons[3] |= BIT(1);
}
static void switch2_ble_side_report_jc1(struct switch2_controller *ns2,
                                      const u8 *native)
{
    s16 accel[3][3];
    s16 gyro[2][3];
    u8 buttons[4];
    u8 stick[3];
    unsigned int sample;
    unsigned int axis;

    if (native[14] != NS2_BLE_LAYOUT_COMMON)
        return;

    switch2_ble_side_controls(ns2, native, buttons, stick);
    switch2_ble_mouse_suppress_controller_controls(ns2, buttons, stick);

    for (sample = 0; sample < ARRAY_SIZE(accel); sample++) {
        unsigned int width = sample == 1 ? 13 : 14;

        for (axis = 0; axis < ARRAY_SIZE(accel[sample]); axis++) {
            u16 raw = switch2_ble_get_raw_bits(
                native, switch2_ble_accel_starts[sample * 3 + axis], width);

            if (sample == 1)
                accel[sample][axis] = 2 * switch2_ble_sign_extend_13(raw);
            else
                accel[sample][axis] = switch2_ble_sign_extend_14(raw);
        }
    }

    for (sample = 0; sample < ARRAY_SIZE(gyro); sample++) {
        for (axis = 0; axis < ARRAY_SIZE(gyro[sample]); axis++) {
            u16 raw = switch2_ble_get_raw_bits(
                native, switch2_ble_gyro_starts[sample * 3 + axis], 15);

            gyro[sample][axis] = switch2_ble_sign_extend_15(raw);
        }
    }

    switch2_jc1_push_native(
        ns2, accel, gyro, buttons, stick, native[1]);
}

struct switch2_ctlr_button_mapping {
    uint32_t code;
    int byte;
    uint32_t bit;
};

static const struct switch2_ctlr_button_mapping left_joycon_button_mappings[] = {
    { BTN_TL,	0, NS2_BTNL_L,		},
    { BTN_TL2,	0, NS2_BTNL_ZL,		},
    { BTN_SELECT,	0, NS2_BTNL_MINUS,	},
    { BTN_THUMBL,	0, NS2_BTNL_LS,		},
    { BTN_GRIPL,	1, NS2_BTN_JCL_GL,	},
    { BTN_TRIGGER_HAPPY1, 1, NS2_BTN_JCL_SL, },
    { BTN_TRIGGER_HAPPY2, 1, NS2_BTN_JCL_SR, },
    { KEY_RECORD,	1, NS2_BTN_JCL_CAPTURE,	},
    { /* sentinel */ },
};

static const struct switch2_ctlr_button_mapping right_joycon_button_mappings[] = {
    { BTN_EAST,	0, NS2_BTNR_A,		},
    { BTN_SOUTH,	0, NS2_BTNR_B,		},
    { BTN_NORTH,	0, NS2_BTNR_X,		},
    { BTN_WEST,	0, NS2_BTNR_Y,		},
    { BTN_TR,	0, NS2_BTNR_R,		},
    { BTN_TR2,	0, NS2_BTNR_ZR,		},
    { BTN_START,	0, NS2_BTNR_PLUS,	},
    { BTN_THUMBR,	0, NS2_BTNR_RS,		},
    { BTN_C,	1, NS2_BTN_JCR_C,	},
    { BTN_GRIPR,	1, NS2_BTN_JCR_GR,	},
    { BTN_MODE,	1, NS2_BTN_JCR_HOME,	},
    { BTN_TRIGGER_HAPPY1, 1, NS2_BTN_JCR_SL, },
    { BTN_TRIGGER_HAPPY2, 1, NS2_BTN_JCR_SR, },
    { /* sentinel */ },
};

static const struct switch2_ctlr_button_mapping procon_mappings[] = {
    { BTN_EAST,	0, NS2_BTNR_A,		},
    { BTN_SOUTH,	0, NS2_BTNR_B,		},
    { BTN_NORTH,	0, NS2_BTNR_X,		},
    { BTN_WEST,	0, NS2_BTNR_Y,		},
    { BTN_TL,	1, NS2_BTNL_L,		},
    { BTN_TR,	0, NS2_BTNR_R,		},
    { BTN_TL2,	1, NS2_BTNL_ZL,		},
    { BTN_TR2,	0, NS2_BTNR_ZR,		},
    { BTN_SELECT,	1, NS2_BTNL_MINUS,	},
    { BTN_START,	0, NS2_BTNR_PLUS,	},
    { BTN_THUMBL,	1, NS2_BTNL_LS,		},
    { BTN_THUMBR,	0, NS2_BTNR_RS,		},
    { BTN_MODE,	2, NS2_BTN_PRO_HOME	},
    { KEY_RECORD,	2, NS2_BTN_PRO_CAPTURE	},
    { BTN_GRIPR,	2, NS2_BTN_PRO_GR	},
    { BTN_GRIPL,	2, NS2_BTN_PRO_GL	},
    { BTN_C,	2, NS2_BTN_PRO_C	},
    { /* sentinel */ },
};

static const struct switch2_ctlr_button_mapping gccon_mappings[] = {
    { BTN_SOUTH,	0, NS2_BTNR_A,		},
    { BTN_EAST,	0, NS2_BTNR_B,		},
    { BTN_NORTH,	0, NS2_BTNR_X,		},
    { BTN_WEST,	0, NS2_BTNR_Y,		},
    { BTN_TL,	1, NS2_BTNL_L,		},
    { BTN_TR,	0, NS2_BTNR_R,		},
    { BTN_TL2,	1, NS2_BTNL_ZL,		},
    { BTN_TR2,	0, NS2_BTNR_ZR,		},
    { BTN_SELECT,	1, NS2_BTNL_MINUS,	},
    { BTN_START,	0, NS2_BTNR_PLUS,	},
    { BTN_MODE,	2, NS2_BTN_GC_HOME	},
    { KEY_RECORD,	2, NS2_BTN_GC_CAPTURE	},
    { BTN_C,	2, NS2_BTN_GC_C		},
    { /* sentinel */ },
};

static void switch2_config_buttons(struct input_dev *idev,
                                   const struct switch2_ctlr_button_mapping button_mappings[])
{
    const struct switch2_ctlr_button_mapping *button;

    for (button = button_mappings; button->code; button++)
        input_set_capability(idev, EV_KEY, button->code);
}

static void switch2_report_buttons(struct input_dev *input, const uint8_t *bytes,
                                   const struct switch2_ctlr_button_mapping button_mappings[])
{
    const struct switch2_ctlr_button_mapping *button;

    for (button = button_mappings; button->code; button++)
        input_report_key(input, button->code, bytes[button->byte] & button->bit);
}

static void switch2_report_axis(struct input_dev *input, struct switch2_axis_calibration *calib,
                                int axis, int value, bool negate)
{
    if (calib && calib->neutral && calib->negative && calib->positive) {
        value -= calib->neutral;
        value *= NS2_AXIS_MAX + 1;
        if (value < 0)
            value /= calib->negative;
        else
            value /= calib->positive;
    } else {
        value = (value - 2048) * 16;
    }

    if (negate)
        value = -value;
    input_report_abs(input, axis,
                     clamp(value, NS2_AXIS_MIN, NS2_AXIS_MAX));
}

static void switch2_report_stick(struct input_dev *input, struct switch2_stick_calibration *calib,
                                 int x, int y, const uint8_t *data)
{
    switch2_report_axis(input, &calib->x, x, data[0] | ((data[1] & 0x0F) << 8), false);
    switch2_report_axis(input, &calib->y, y, (data[1] >> 4) | (data[2] << 4), true);
}

static void switch2_report_trigger(struct input_dev *input, uint8_t zero, int abs, uint8_t data)
{
    int value = (NS2_TRIGGER_RANGE + 1) * (data - zero) / (232 - zero);

    input_report_abs(input, abs, clamp(value, 0, NS2_TRIGGER_RANGE));
}



int switch2_input_create(struct switch2_controller *ns2)
{
    struct input_dev *input;
    int ret;

    if (!ns2->hdev)
        return -ENODEV;

    if (rcu_access_pointer(ns2->input))
        return 0;

    /* The BLE frontend is switchable at runtime through jc1_compat, so this
     * gamepad must have an independent lifetime rather than being tied to the
     * parent HID's devres list. */
    input = input_allocate_device();
    if (!input)
        return -ENOMEM;

    input_set_drvdata(input, ns2);
    input->dev.parent = &ns2->hdev->dev;
    input->id.bustype = ns2->hdev->bus;
    input->id.vendor = ns2->hdev->vendor;
    input->id.product = ns2->hdev->product;
    input->id.version = ns2->hdev->version;
    input->name = ns2->hdev->name;
    input->phys = ns2->hdev->phys;
    input->uniq = ns2->serial;

    switch (ns2->type) {
        case NS2_CTLR_TYPE_JCL:
            input_set_abs_params(input, ABS_X, NS2_AXIS_MIN, NS2_AXIS_MAX, 32, 128);
            input_set_abs_params(input, ABS_Y, NS2_AXIS_MIN, NS2_AXIS_MAX, 32, 128);
            input_set_abs_params(input, ABS_HAT0X, -1, 1, 0, 0);
            input_set_abs_params(input, ABS_HAT0Y, -1, 1, 0, 0);
            switch2_config_buttons(input, left_joycon_button_mappings);
            if (switch2_controller_is_ble(ns2)) {
                /* Left stick owns ABS_X/Y. Expose the real common-0x05 IMU
                 * sample on otherwise-unused axes of this same JC2 device. */
                input_set_abs_params(input, ABS_Z, -32768, 32767, 0, 0);
                input_set_abs_params(input, ABS_THROTTLE, -32768, 32767, 0, 0);
                input_set_abs_params(input, ABS_RUDDER, -32768, 32767, 0, 0);
                input_set_abs_params(input, ABS_RX, -32768, 32767, 0, 0);
                input_set_abs_params(input, ABS_RY, -32768, 32767, 0, 0);
                input_set_abs_params(input, ABS_RZ, -32768, 32767, 0, 0);
                input_set_events_per_packet(input, 32);
            }
            break;
        case NS2_CTLR_TYPE_JCR:
            input_set_abs_params(input, ABS_RX, NS2_AXIS_MIN, NS2_AXIS_MAX, 32, 128);
            input_set_abs_params(input, ABS_RY, NS2_AXIS_MIN, NS2_AXIS_MAX, 32, 128);
            switch2_config_buttons(input, right_joycon_button_mappings);
            if (switch2_controller_is_ble(ns2)) {
                /* Right stick owns ABS_RX/RY. Keep those axes exclusive to the
                 * stick and expose the IMU on the remaining free ABS axes. */
                input_set_abs_params(input, ABS_X, -32768, 32767, 0, 0);
                input_set_abs_params(input, ABS_Y, -32768, 32767, 0, 0);
                input_set_abs_params(input, ABS_Z, -32768, 32767, 0, 0);
                input_set_abs_params(input, ABS_THROTTLE, -32768, 32767, 0, 0);
                input_set_abs_params(input, ABS_RUDDER, -32768, 32767, 0, 0);
                input_set_abs_params(input, ABS_RZ, -32768, 32767, 0, 0);
                input_set_events_per_packet(input, 32);
            }
            break;
        case NS2_CTLR_TYPE_GC:
            input_set_abs_params(input, ABS_X, NS2_AXIS_MIN, NS2_AXIS_MAX, 32, 128);
            input_set_abs_params(input, ABS_Y, NS2_AXIS_MIN, NS2_AXIS_MAX, 32, 128);
            input_set_abs_params(input, ABS_RX, NS2_AXIS_MIN, NS2_AXIS_MAX, 32, 128);
            input_set_abs_params(input, ABS_RY, NS2_AXIS_MIN, NS2_AXIS_MAX, 32, 128);
            input_set_abs_params(input, ABS_Z, 0, NS2_TRIGGER_RANGE, 32, 128);
            input_set_abs_params(input, ABS_RZ, 0, NS2_TRIGGER_RANGE, 32, 128);
            input_set_abs_params(input, ABS_HAT0X, -1, 1, 0, 0);
            input_set_abs_params(input, ABS_HAT0Y, -1, 1, 0, 0);
            switch2_config_buttons(input, gccon_mappings);
            break;
        case NS2_CTLR_TYPE_PRO:
            input_set_abs_params(input, ABS_X, NS2_AXIS_MIN, NS2_AXIS_MAX, 32, 128);
            input_set_abs_params(input, ABS_Y, NS2_AXIS_MIN, NS2_AXIS_MAX, 32, 128);
            input_set_abs_params(input, ABS_RX, NS2_AXIS_MIN, NS2_AXIS_MAX, 32, 128);
            input_set_abs_params(input, ABS_RY, NS2_AXIS_MIN, NS2_AXIS_MAX, 32, 128);
            input_set_abs_params(input, ABS_HAT0X, -1, 1, 0, 0);
            input_set_abs_params(input, ABS_HAT0Y, -1, 1, 0, 0);
            switch2_config_buttons(input, procon_mappings);
            break;
        default:
            ret = -EINVAL;
            goto err_free;
    }

    ret = switch2_ff_configure_input(ns2, input);
    if (ret)
        goto err_free;

    ret = input_register_device(input);
    if (ret)
        goto err_free;

    rcu_assign_pointer(ns2->input, input);


    dev_info(switch2_dev(ns2), "Firmware version %u.%u.%u (%i)\n",
        ns2->version.major, ns2->version.minor,
        ns2->version.patch, ns2->version.controller_type);

    if (ns2->version.dsp_type >= 0) {
        dev_info(switch2_dev(ns2),
            "DSP version %u.%u.%u\n",
            ns2->version.dsp_major, ns2->version.dsp_minor,
            ns2->version.dsp_patch);
    }

    return 0;

err_free:
    input_free_device(input);
    return ret;
}

int switch2_input_receive_ble_side(struct switch2_controller *ns2,
                                    const u8 *raw_data, int size)
{
    u8 right_motion[NS2_BLE_HID_STATE_SIZE - 1];
    const u8 *motion_native = raw_data + 1;

    if (!ns2 || !raw_data)
        return -EINVAL;

    if (size != NS2_BLE_HID_STATE_SIZE ||
        (raw_data[0] != NS2_REPORT_JCL &&
        raw_data[0] != NS2_REPORT_JCR))
        return -EINVAL;

    /*
     * The SELECT_REPORT reply path deliberately waits for one subsequent
     * native live notification before advancing BLE initialization.  This is
     * only a transport/data-flow synchronization point: either Joy-Con side
     * wakes the same state machine, regardless of the live packet layout.
     */
    mutex_lock(&ns2->lock);
    if (ns2->init_step == NS2_INIT_BLE_WAIT_INPUT) {
        ns2->init_step = NS2_INIT_BLE_INPUT_READY;
        switch2_init_schedule_locked(ns2, 10);
    }
    mutex_unlock(&ns2->lock);

    if ((ns2->type != NS2_CTLR_TYPE_JCL &&
        ns2->type != NS2_CTLR_TYPE_JCR) ||
        !switch2_controller_is_ble(ns2))
        return 0;

    switch2_ble_mouse_report_side(ns2, raw_data + 1);

    /*
     * Right Joy-Con 2 ordinary motion reports contain one extra byte before
     * the common 0x28 motion block:
     *
     *   left:  ... ff 28 ...
     *   right: ... ff 00 28 ...
     *
     * The established motion decoder uses offsets relative to the left/common
     * representation.  Normalize only the right ordinary-motion packet here
     * by removing that extra byte.  Keep the original packet unchanged for
     * buttons/stick normalization below.
     */
    if (raw_data[0] == NS2_REPORT_JCR &&
        motion_native[14] == 0x00 &&
        motion_native[15] == NS2_BLE_LAYOUT_COMMON) {
        memcpy(right_motion, motion_native, 14);
        memcpy(&right_motion[14], &motion_native[15],
               sizeof(right_motion) - 15);
        right_motion[sizeof(right_motion) - 1] = 0;
        motion_native = right_motion;
    }

    switch2_ble_side_report_jc1(ns2, motion_native);
    return 0;
}

static void switch2_ble_common_report_native(struct switch2_controller *ns2,
                                              struct input_dev *input,
                                              const u8 *raw_data, int size)
{
    s16 accel[3];
    s16 gyro[3];
    u8 buttons[4];
    u8 stick[3];
    unsigned int stick_offset;
    int axis;

    if (!ns2 || !input || !raw_data || size != NS2_BLE_HID_STATE_SIZE ||
        raw_data[0] != NS2_REPORT_COMMON ||
        !switch2_controller_is_ble(ns2) ||
        !switch2_controller_is_joycon(ns2->type) ||
        switch2_jc1_compat_enabled())
        return;

    /* Common 0x05 carries one synchronized signed-16 accel XYZ followed by
     * gyro XYZ at HID report offsets 49..60. Mirror that real sample onto the
     * native JC2 gamepad node so evdev clients can observe motion without a
     * second synthetic Motion device. */
    for (axis = 0; axis < 3; axis++) {
        accel[axis] = (s16)get_unaligned_le16(&raw_data[49 + axis * 2]);
        gyro[axis] = (s16)get_unaligned_le16(&raw_data[55 + axis * 2]);
    }

    if (ns2->type == NS2_CTLR_TYPE_JCR) {
        input_report_abs(input, ABS_X, accel[0]);
        input_report_abs(input, ABS_Y, accel[1]);
        input_report_abs(input, ABS_Z, accel[2]);
        input_report_abs(input, ABS_THROTTLE, gyro[0]);
        input_report_abs(input, ABS_RUDDER, gyro[1]);
        input_report_abs(input, ABS_RZ, gyro[2]);
    } else {
        input_report_abs(input, ABS_Z, accel[0]);
        input_report_abs(input, ABS_THROTTLE, accel[1]);
        input_report_abs(input, ABS_RUDDER, accel[2]);
        input_report_abs(input, ABS_RX, gyro[0]);
        input_report_abs(input, ABS_RY, gyro[1]);
        input_report_abs(input, ABS_RZ, gyro[2]);
    }

    memcpy(buttons, &raw_data[5], sizeof(buttons));
    stick_offset = ns2->type == NS2_CTLR_TYPE_JCR ? 14 : 11;
    memcpy(stick, &raw_data[stick_offset], sizeof(stick));
    switch2_ble_mouse_suppress_controller_controls(ns2, buttons, stick);

    if (ns2->type == NS2_CTLR_TYPE_JCR) {
        switch2_report_stick(input, &ns2->stick_calib[0],
                             ABS_RX, ABS_RY, stick);
        input_report_key(input, BTN_EAST,  !!(buttons[0] & BIT(3)));
        input_report_key(input, BTN_SOUTH, !!(buttons[0] & BIT(2)));
        input_report_key(input, BTN_NORTH, !!(buttons[0] & BIT(1)));
        input_report_key(input, BTN_WEST,  !!(buttons[0] & BIT(0)));
        input_report_key(input, BTN_TR,
                         !!(buttons[0] & NS2_BLE_BTN_SHOULDER));
        input_report_key(input, BTN_TR2,
                         !!(buttons[0] & NS2_BLE_BTN_TRIGGER));
        input_report_key(input, BTN_START,
                         !!(buttons[1] & NS2_BLE_BTN_PLUS));
        input_report_key(input, BTN_THUMBR,
                         !!(buttons[1] & NS2_BLE_BTN_RS));
        input_report_key(input, BTN_MODE,
                         !!(buttons[1] & NS2_BLE_BTN_HOME));
        input_report_key(input, BTN_C,
                         !!(buttons[1] & NS2_BLE_BTN_C));
        input_report_key(input, BTN_GRIPR, !!(buttons[3] & BIT(0)));
        input_report_key(input, BTN_TRIGGER_HAPPY1,
                         !!(buttons[0] & NS2_BLE_BTN_SL));
        input_report_key(input, BTN_TRIGGER_HAPPY2,
                         !!(buttons[0] & NS2_BLE_BTN_SR));
        return;
    }

    input_report_abs(input, ABS_HAT0X,
                     !!(buttons[2] & NS2_BLE_BTN_RIGHT) -
                     !!(buttons[2] & NS2_BLE_BTN_LEFT));
    input_report_abs(input, ABS_HAT0Y,
                     !!(buttons[2] & NS2_BLE_BTN_DOWN) -
                     !!(buttons[2] & NS2_BLE_BTN_UP));
    switch2_report_stick(input, &ns2->stick_calib[0], ABS_X, ABS_Y, stick);
    input_report_key(input, BTN_TL,
                     !!(buttons[2] & NS2_BLE_BTN_SHOULDER));
    input_report_key(input, BTN_TL2,
                     !!(buttons[2] & NS2_BLE_BTN_TRIGGER));
    input_report_key(input, BTN_SELECT,
                     !!(buttons[1] & NS2_BLE_BTN_MINUS));
    input_report_key(input, BTN_THUMBL,
                     !!(buttons[1] & NS2_BLE_BTN_LS));
    input_report_key(input, KEY_RECORD,
                     !!(buttons[1] & NS2_BLE_BTN_CAPTURE));
    input_report_key(input, BTN_GRIPL, !!(buttons[3] & BIT(1)));
    input_report_key(input, BTN_TRIGGER_HAPPY1,
                     !!(buttons[2] & NS2_BLE_BTN_SL));
    input_report_key(input, BTN_TRIGGER_HAPPY2,
                     !!(buttons[2] & NS2_BLE_BTN_SR));
}

static void switch2_ble_common_report_jc1(struct switch2_controller *ns2,
                                             const u8 *raw_data, int size)
{
    s16 accel[3];
    s16 gyro[3];
    u8 buttons[4];
    u8 stick[3];
    unsigned int stick_offset;
    int axis;

    if (!ns2 || !raw_data || size != NS2_BLE_HID_STATE_SIZE ||
        raw_data[0] != NS2_REPORT_COMMON ||
        (ns2->type != NS2_CTLR_TYPE_JCL &&
         ns2->type != NS2_CTLR_TYPE_JCR) ||
        !switch2_controller_is_ble(ns2))
        return;

    /* Common GATT report 0x05 is already the plain USB-style representation:
     * one synchronized signed-16 accel XYZ plus gyro XYZ at report[49..60]. */
    for (axis = 0; axis < 3; axis++) {
        accel[axis] = (s16)get_unaligned_le16(&raw_data[49 + axis * 2]);
        gyro[axis] = (s16)get_unaligned_le16(&raw_data[55 + axis * 2]);
    }

    /* Common 0x05 carries the same four-byte common control bank used by our
     * existing JC1 state builder. Keep controls live after the side CCC is
     * disabled; battery remains on the already-working side-derived path. */
    memcpy(buttons, &raw_data[5], sizeof(buttons));
    stick_offset = ns2->type == NS2_CTLR_TYPE_JCR ? 14 : 11;
    memcpy(stick, &raw_data[stick_offset], sizeof(stick));
    switch2_ble_mouse_suppress_controller_controls(ns2, buttons, stick);

    switch2_jc1_push_common(ns2, accel, gyro, buttons, stick);
}

int switch2_input_receive_state(struct switch2_controller *ns2,
                          const uint8_t *raw_data, int size)
{
    struct input_dev *input;

    if (!ns2 || !raw_data)
        return -EINVAL;

    if (raw_data[0] == NS2_REPORT_COMMON) {
        if (size != NS2_BLE_HID_STATE_SIZE)
            return -EINVAL;

        switch2_ble_mouse_report_common(ns2, raw_data, size);
        switch2_ble_common_report_jc1(ns2, raw_data, size);

        rcu_read_lock();
        input = rcu_dereference(ns2->input);
        if (input)
            input_get_device(input);
        rcu_read_unlock();

        if (!input)
            return 0;

        switch2_ble_common_report_native(ns2, input, raw_data, size);
        input_sync(input);
        input_put_device(input);
        return 0;
    }

    if (size < 15)
        return -EINVAL;

    rcu_read_lock();
    input = rcu_dereference(ns2->input);
    if (input)
        input_get_device(input);
    rcu_read_unlock();

    if (!input)
        return 0;

    switch (raw_data[0]) {
        case NS2_REPORT_JCL:
            input_report_abs(input, ABS_HAT0X,
                             !!(raw_data[3] & NS2_BTNL_RIGHT) -
                             !!(raw_data[3] & NS2_BTNL_LEFT));
            input_report_abs(input, ABS_HAT0Y,
                             !!(raw_data[3] & NS2_BTNL_DOWN) -
                             !!(raw_data[3] & NS2_BTNL_UP));
            switch2_report_stick(input, &ns2->stick_calib[0], ABS_X, ABS_Y, &raw_data[6]);
            switch2_report_buttons(input, &raw_data[3], left_joycon_button_mappings);
            if (switch2_controller_is_ble(ns2))
                input_report_key(input, BTN_C,
                                 raw_data[4] & NS2_BTN_JCL_CAPTURE);
            break;
        case NS2_REPORT_JCR:
            switch2_report_stick(input, &ns2->stick_calib[0], ABS_RX, ABS_RY, &raw_data[6]);
            switch2_report_buttons(input, &raw_data[3], right_joycon_button_mappings);
            break;
        case NS2_REPORT_GC:
            input_report_abs(input, ABS_HAT0X,
                             !!(raw_data[4] & NS2_BTNL_RIGHT) -
                             !!(raw_data[4] & NS2_BTNL_LEFT));
            input_report_abs(input, ABS_HAT0Y,
                             !!(raw_data[4] & NS2_BTNL_DOWN) -
                             !!(raw_data[4] & NS2_BTNL_UP));
            switch2_report_buttons(input, &raw_data[3], gccon_mappings);
            switch2_report_stick(input, &ns2->stick_calib[0], ABS_X, ABS_Y, &raw_data[6]);
            switch2_report_stick(input, &ns2->stick_calib[1], ABS_RX, ABS_RY, &raw_data[9]);
            switch2_report_trigger(input, ns2->lt_zero, ABS_Z, raw_data[13]);
            switch2_report_trigger(input, ns2->rt_zero, ABS_RZ, raw_data[14]);
            break;
        case NS2_REPORT_PRO:
            input_report_abs(input, ABS_HAT0X,
                             !!(raw_data[4] & NS2_BTNL_RIGHT) -
                             !!(raw_data[4] & NS2_BTNL_LEFT));
            input_report_abs(input, ABS_HAT0Y,
                             !!(raw_data[4] & NS2_BTNL_DOWN) -
                             !!(raw_data[4] & NS2_BTNL_UP));
            switch2_report_buttons(input, &raw_data[3], procon_mappings);
            switch2_report_stick(input, &ns2->stick_calib[0], ABS_X, ABS_Y, &raw_data[6]);
            switch2_report_stick(input, &ns2->stick_calib[1], ABS_RX, ABS_RY, &raw_data[9]);
            break;
        default:
            input_put_device(input);
            return -EINVAL;
    }

    input_sync(input);
    input_put_device(input);
    return 0;
}

void switch2_input_destroy_gamepad(struct switch2_controller *ns2)
{
    struct input_dev *input;

    if (!ns2)
        return;

    input = rcu_replace_pointer(ns2->input, NULL, true);
    if (!input)
        return;

    synchronize_rcu();
    input_unregister_device(input);
}

void switch2_input_destroy(struct switch2_controller *ns2)
{
    struct input_dev *mouse;

    if (!ns2)
        return;

    switch2_jc1_destroy(ns2);
    switch2_ble_mouse_reset_state(ns2, true);

    mouse = rcu_replace_pointer(ns2->mouse_input, NULL, true);
    if (mouse) {
        synchronize_rcu();
        input_unregister_device(mouse);
    }

    switch2_input_destroy_gamepad(ns2);
}

void switch2_input_disconnect_locked(struct switch2_controller *ns2)
{
    static const unsigned int neutral_axes[] = {
        ABS_X,
        ABS_Y,
        ABS_RX,
        ABS_RY,
        ABS_Z,
        ABS_RZ,
        ABS_THROTTLE,
        ABS_RUDDER,
        ABS_HAT0X,
        ABS_HAT0Y,
    };
    struct input_dev *input;
    unsigned int i;

    switch2_ble_mouse_reset_state(ns2, true);

    rcu_read_lock();
    input = rcu_dereference(ns2->input);
    if (input)
        input_get_device(input);
    rcu_read_unlock();

    if (!input)
        return;

    input_reset_device(input);

    for (i = 0; i < ARRAY_SIZE(neutral_axes); i++) {
        if (test_bit(neutral_axes[i], input->absbit))
            input_report_abs(input, neutral_axes[i], 0);
    }

    input_sync(input);
    input_put_device(input);
}
