// SPDX-License-Identifier: GPL-2.0-or-later

#include "switch2.h"

#include <linux/unaligned.h>

static bool switch2_float_bits_finite(u32 bits)
{
    return (bits & 0x7f800000U) != 0x7f800000U;
}

/* Convert an IEEE-754 binary32 bit pattern to round-to-nearest-even of
 * value * numerator / denominator without using floating point in the kernel.
 * The calibration values used here are small, but the helper still fails
 * closed on shifts that cannot be represented safely in u64.
 */
static int switch2_float_bits_scale_round(u32 bits, u32 numerator,
                                          u32 denominator, s32 *result)
{
    bool negative = bits >> 31;
    u32 exponent = (bits >> 23) & 0xff;
    u32 fraction = bits & 0x7fffff;
    u64 mantissa;
    int exp2;
    u64 scaled;
    u64 divisor;
    u64 quotient;
    u64 remainder;
    s64 signed_result;

    if (!result || !denominator || exponent == 0xff)
        return -EINVAL;

    if (!exponent && !fraction) {
        *result = 0;
        return 0;
    }

    if (exponent) {
        mantissa = BIT_ULL(23) | fraction;
        exp2 = (int)exponent - 127 - 23;
    } else {
        mantissa = fraction;
        exp2 = -126 - 23;
    }

    if (mantissa > div_u64(U64_MAX, numerator))
        return -ERANGE;
    scaled = mantissa * numerator;

    if (exp2 >= 0) {
        if (exp2 >= 63 || scaled > (U64_MAX >> exp2))
            return -ERANGE;
        scaled <<= exp2;
        divisor = denominator;
    } else {
        unsigned int shift = -exp2;

        /* Values below half a destination unit round to zero. */
        if (shift >= 63) {
            *result = 0;
            return 0;
        }
        if ((u64)denominator > (U64_MAX >> shift))
            return -ERANGE;
        divisor = (u64)denominator << shift;
    }

    quotient = div64_u64_rem(scaled, divisor, &remainder);
    if (remainder > divisor / 2 ||
        (remainder == divisor / 2 && (divisor & 1) == 0 &&
         (quotient & 1)))
        quotient++;

    if (quotient > S32_MAX)
        return -ERANGE;
    signed_result = negative ? -(s64)quotient : (s64)quotient;
    *result = signed_result;
    return 0;
}

static void switch2_log_ble_motion_calibration_ready(
    struct switch2_controller *ns2, bool was_ready)
{
    if (was_ready || !switch2_ble_motion_calibration_ready(ns2))
        return;

    hid_info(ns2->hdev,
        "BLE motion calibration ready: gyro zero=%d,%d,%d\n",
        ns2->ble_gyro_zero[0],
        ns2->ble_gyro_zero[1],
        ns2->ble_gyro_zero[2]);
}

static bool switch2_parse_ble_gyro_calibration(struct switch2_controller *ns2,
                                                const u8 *data, u8 size)
{
    bool was_ready = switch2_ble_motion_calibration_ready(ns2);
    int axis;

    if (size != NS2_FLASH_SIZE_BLE_GYRO_CALIB)
        return false;

    for (axis = 0; axis < 3; axis++) {
        u32 bits = get_unaligned_le32(&data[4 + axis * 4]);
        s32 zero;
        int ret;

        if (!switch2_float_bits_finite(bits))
            return false;
        ret = switch2_float_bits_scale_round(bits, 32767, 40, &zero);
        if (ret || zero < S16_MIN || zero > S16_MAX)
            return false;

        ns2->ble_gyro_zero[axis] = zero;
    }

    ns2->ble_gyro_calibration_valid = true;
    switch2_log_ble_motion_calibration_ready(ns2, was_ready);
    return true;
}

static bool switch2_parse_ble_accel_calibration(struct switch2_controller *ns2,
                                                 const u8 *data, u8 size)
{
    bool was_ready = switch2_ble_motion_calibration_ready(ns2);
    int axis;

    if (size != NS2_FLASH_SIZE_BLE_ACCEL_CALIB)
        return false;

    for (axis = 0; axis < 3; axis++) {
        u32 bits = get_unaligned_le32(&data[axis * 4]);

        if (!switch2_float_bits_finite(bits))
            return false;
    }

    ns2->ble_accel_calibration_valid = true;
    switch2_log_ble_motion_calibration_ready(ns2, was_ready);
    return true;
}

static bool switch2_parse_stick_calibration(struct switch2_stick_calibration *calib,
                                            const u8 *data)
{
    static const u8 uncalibrated[9] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };

    if (!memcmp(uncalibrated, data, sizeof(uncalibrated)))
        return false;

    calib->x.neutral = data[0];
    calib->x.neutral |= (data[1] & 0x0f) << 8;

    calib->y.neutral = data[1] >> 4;
    calib->y.neutral |= data[2] << 4;

    calib->x.positive = data[3];
    calib->x.positive |= (data[4] & 0x0f) << 8;

    calib->y.positive = data[4] >> 4;
    calib->y.positive |= data[5] << 4;

    calib->x.negative = data[6];
    calib->x.negative |= (data[7] & 0x0f) << 8;

    calib->y.negative = data[7] >> 4;
    calib->y.negative |= data[8] << 4;

    return true;
}

static void switch2_log_stick_calibration(struct switch2_controller *ns2,
                                          unsigned int index,
                                          const char *source)
{
    const struct switch2_stick_calibration *calib = &ns2->stick_calib[index];
    const char *stick = index ? "secondary" : "primary";

    hid_dbg(ns2->hdev, "Got %s %s stick calibration:\n", source, stick);
    hid_dbg(ns2->hdev, "Left max: %i, neutral: %i, right max: %i\n",
            calib->x.negative, calib->x.neutral, calib->x.positive);
    hid_dbg(ns2->hdev, "Down max: %i, neutral: %i, up max: %i\n",
            calib->y.negative, calib->y.neutral, calib->y.positive);
}

static void switch2_handle_stick_calibration(struct switch2_controller *ns2,
                                             unsigned int index,
                                             const u8 *data, bool user)
{
    const char *stick = index ? "secondary" : "primary";

    if (user) {
        if (get_unaligned_le16(data) != NS2_USER_CALIB_MAGIC) {
            hid_dbg(ns2->hdev, "No user %s stick calibration present\n", stick);
            return;
        }
        data += 2;
    }

    if (!switch2_parse_stick_calibration(&ns2->stick_calib[index], data)) {
        if (user)
            hid_dbg(ns2->hdev, "No user %s stick calibration present\n", stick);
        else
            hid_dbg(ns2->hdev, "Factory %s stick calibration not present\n",
                    stick);
        return;
    }

    switch2_log_stick_calibration(ns2, index, user ? "user" : "factory");
}

static void switch2_handle_flash_read(struct switch2_controller *ns2, u8 size,
                                      u32 address, const u8 *data)
{
    switch (address) {
    case NS2_FLASH_ADDR_BLE_GYRO_CALIB:
        if (!switch2_parse_ble_gyro_calibration(ns2, data, size))
            hid_warn(ns2->hdev,
                "invalid BLE gyro calibration at 0x%05x\n", address);
        break;

    case NS2_FLASH_ADDR_BLE_ACCEL_CALIB:
        if (!switch2_parse_ble_accel_calibration(ns2, data, size))
            hid_warn(ns2->hdev,
                "invalid BLE accel calibration at 0x%05x\n", address);
        break;

    case NS2_FLASH_ADDR_SERIAL:
        if (size == NS2_FLASH_SIZE_SERIAL)
            memcpy(ns2->serial, data, size);
        break;

    case NS2_FLASH_ADDR_FACTORY_PRIMARY_CALIB:
        if (size == NS2_FLASH_SIZE_FACTORY_AXIS_CALIB)
            switch2_handle_stick_calibration(ns2, 0, data, false);
        break;

    case NS2_FLASH_ADDR_FACTORY_SECONDARY_CALIB:
        if (size == NS2_FLASH_SIZE_FACTORY_AXIS_CALIB)
            switch2_handle_stick_calibration(ns2, 1, data, false);
        break;

    case NS2_FLASH_ADDR_FACTORY_TRIGGER_CALIB:
        if (size != NS2_FLASH_SIZE_FACTORY_TRIGGER_CALIB)
            break;
        if (data[0] == 0xff || data[1] == 0xff) {
            hid_dbg(ns2->hdev, "Factory trigger calibration not present\n");
            break;
        }

        ns2->lt_zero = data[0];
        ns2->rt_zero = data[1];
        hid_dbg(ns2->hdev, "Got factory trigger calibration:\n");
        hid_dbg(ns2->hdev, "Left zero point: %i\n", ns2->lt_zero);
        hid_dbg(ns2->hdev, "Right zero point: %i\n", ns2->rt_zero);
        break;

    case NS2_FLASH_ADDR_USER_PRIMARY_CALIB:
        if (size == NS2_FLASH_SIZE_USER_AXIS_CALIB)
            switch2_handle_stick_calibration(ns2, 0, data, true);
        break;

    case NS2_FLASH_ADDR_USER_SECONDARY_CALIB:
        if (size == NS2_FLASH_SIZE_USER_AXIS_CALIB)
            switch2_handle_stick_calibration(ns2, 1, data, true);
        break;
    }
}

static int switch2_parse_reply_locked(struct switch2_controller *ns2,
                                      const u8 *packet, size_t packet_size)
{
    const struct switch2_cmd_header *header;
    const u8 *payload;
    size_t payload_size;

    lockdep_assert_held(&ns2->lock);

    if (packet_size < NS2_CMD_HEADER_SIZE)
        return -EINVAL;

    header = (const struct switch2_cmd_header *)packet;
    payload = packet + NS2_CMD_HEADER_SIZE;
    payload_size = packet_size - NS2_CMD_HEADER_SIZE;

    switch (header->command) {
    case NS2_CMD_FLASH:
        if (header->subcommand == NS2_SUBCMD_FLASH_READ) {
            u8 read_size;
            u32 read_address;

            /* Flash-read replies have an eight-byte metadata prefix followed
             * by exactly read_size bytes of flash data. */
            if (payload_size < 8)
                return -EINVAL;

            read_size = payload[0];
            read_address = get_unaligned_le32(&payload[4]);
            if (payload_size < 8 + read_size)
                return -EINVAL;

            switch2_handle_flash_read(ns2, read_size, read_address,
                                      &payload[8]);
        }
        break;

    case NS2_CMD_FW_INFO:
        if (header->subcommand == NS2_SUBCMD_FW_INFO_GET) {
            if (payload_size < sizeof(ns2->version))
                return -EINVAL;

            memcpy(&ns2->version, payload, sizeof(ns2->version));
            if (ns2->version.controller_type != ns2->type) {
                hid_warn(ns2->hdev,
                    "firmware controller type %u disagrees with HID product type %u\n",
                    ns2->version.controller_type, ns2->type);
                return -EPROTO;
            }
        }
        break;

    default:
        break;
    }

    return 0;
}

static int switch2_continue_initialization_locked(struct switch2_controller *ns2)
{
    int ret;

    lockdep_assert_held(&ns2->lock);

    if (ns2->init_step >= NS2_INIT_DONE)
        return 0;

    if (switch2_controller_is_ble(ns2)) {
        /* Match the measured BLE startup cadence.  After each acknowledged
         * startup command, leave a 10 ms guard before issuing the next one.
         * Report selection additionally waits for the first genuine native
         * state notification before continuing. */
        if (ns2->init_step == NS2_INIT_BLE_SELECT_REPORT) {
            ns2->init_step = NS2_INIT_BLE_WAIT_INPUT;
            return 0;
        }

        if (ns2->init_step == NS2_INIT_BLE_WAIT_INPUT ||
            ns2->init_step == NS2_INIT_BLE_INPUT_READY)
            return 0;

        switch2_init_schedule_locked(ns2, 10);
        return 0;
    }

    ret = switch2_init_advance_locked(ns2);
    if (ret)
        hid_err(ns2->hdev, "initialization failed at step %u: %d\n",
                ns2->init_step, ret);
    return ret;
}

int switch2_protocol_handle_reply(struct switch2_controller *ns2,
                                  const u8 *packet, size_t packet_size)
{
    int ret;

    if (!ns2 || !packet)
        return -EINVAL;

    print_hex_dump_debug("got cmd: ", DUMP_PREFIX_OFFSET, 16, 1,
                         packet, packet_size, false);

    guard(mutex)(&ns2->lock);

    /* Parse the reply before advancing the state machine. Firmware info
     * confirms the controller type established from the HID product ID. */
    ret = switch2_parse_reply_locked(ns2, packet, packet_size);
    if (ret)
        return ret;

    return switch2_continue_initialization_locked(ns2);
}
