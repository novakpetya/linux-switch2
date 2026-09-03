// SPDX-License-Identifier: GPL-2.0-or-later

#include "switch2.h"

#include <linux/hid.h>
#include <linux/device/driver.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#define JC1_VENDOR_ID            0x057e
#define JC1_LEFT_PRODUCT_ID      0x2006
#define JC1_RIGHT_PRODUCT_ID     0x2007

#define JC1_INPUT_SUBCMD_REPLY   0x21
#define JC1_INPUT_FULL_STATE     0x30
#define JC1_OUTPUT_SUBCMD        0x01
#define JC1_OUTPUT_RUMBLE        0x10
#define JC1_OUTPUT_PROPRIETARY   0x80

#define JC1_FACTORY_STICK_ADDR   0x603d
#define JC1_FACTORY_STICK_LEN    0x12
#define JC1_USER_STICK_ADDR      0x8010
#define JC1_USER_STICK_LEN       0x16
#define JC1_FACTORY_IMU_ADDR     0x6020
#define JC1_FACTORY_IMU_LEN      0x18
#define JC1_USER_IMU_ADDR        0x8026
#define JC1_USER_IMU_LEN         0x14

/* The synthetic JC1 factory calibration below advertises a gyro sensitivity
 * span of 13371 raw counts. SDL's classic Nintendo calibration formula uses
 * 936 as the gyro scale multiplier. Native JC2 explicit gyro is signed-15
 * over +/-500 dps, i.e. 16384 counts at full scale. Convert only units; do
 * not filter or reshape the sample.
 *
 *   jc1_raw / jc2_raw = (500 * 13371) / (16384 * 936)
 *                      = 0.435951428...
 *
 * Keep this as an exact rational so the wire samples and the calibration we
 * hand to SDL describe the same physical gain. */
#define JC1_GYRO_CAL_SPAN        13371
#define JC1_GYRO_SCALE_MULT      936
#define JC2_GYRO_FULL_SCALE_DPS  500
#define JC2_GYRO_FULL_SCALE_RAW  16384
#define JC1_GYRO_SCALE_NUM       ((s64)JC2_GYRO_FULL_SCALE_DPS * JC1_GYRO_CAL_SPAN)
#define JC1_GYRO_SCALE_DEN       ((s64)JC2_GYRO_FULL_SCALE_RAW * JC1_GYRO_SCALE_MULT)

/* Common BLE report 0x05 uses the same signed-16 raw gyro domain as physical
 * JC2 USB report 0x05. The synthetic JC1 calibration presented to SDL is
 * 818.48490157 counts/rad/s versus 819.175 counts/rad/s for that USB domain.
 * Convert units only; do not alter timing or reshape the measured sample. */
#define JC1_COMMON_GYRO_Q30      1072837271LL
#define JC1_COMMON_GYRO_SHIFT    30

#define JC1_SAMPLE_FIFO_LEN      3


/* Classic Joy-Con rumble uses a 0..100 nonlinear amplitude code. These are
 * the 16-bit magnitude boundaries represented by those codes; both bands use
 * the same quantization. Decode to the midpoint of the represented bin so the
 * JC1 compatibility boundary preserves the host-requested intensity. */
static const u16 jc1_rumble_magnitude_boundaries[101] = {
        0,   514,   775,   921,  1096,  1303,  1550,  1843,  2192,  2606,
     3100,  3686,  4383,  5213,  6199,  7372,  7698,  8039,  8395,  8767,
     9155,  9560,  9984, 10426, 10887, 11369, 11873, 12398, 12947, 13520,
    14119, 14744, 15067, 15397, 15734, 16079, 16431, 16790, 17158, 17534,
    17918, 18310, 18711, 19121, 19540, 19967, 20405, 20851, 21308, 21775,
    22251, 22739, 23236, 23745, 24265, 24797, 25340, 25894, 26462, 27041,
    27633, 28238, 28856, 29488, 30134, 30794, 31468, 32157, 32861, 33581,
    34316, 35068, 35836, 36620, 37422, 38242, 39079, 39935, 40809, 41703,
    42616, 43549, 44503, 45477, 46473, 47491, 48531, 49593, 50679, 51789,
    52923, 54082, 55266, 56476, 57713, 58977, 60268, 61588, 62936, 64315,
    65535,
};

static u16 switch2_jc1_rumble_magnitude(unsigned int index)
{
    if (!index)
        return 0;
    if (index >= ARRAY_SIZE(jc1_rumble_magnitude_boundaries))
        index = ARRAY_SIZE(jc1_rumble_magnitude_boundaries) - 1;

    return ((u32)jc1_rumble_magnitude_boundaries[index - 1] +
            jc1_rumble_magnitude_boundaries[index] + 1) / 2;
}

static void switch2_jc1_decode_rumble(const u8 rumble[4],
                                      u16 *strong_magnitude,
                                      u16 *weak_magnitude)
{
    unsigned int high_index;
    unsigned int low_index;

    /* Byte 1 bit 0 belongs to the 9-bit high frequency, not amplitude. */
    high_index = (rumble[1] & 0xfe) >> 1;

    /* Low amplitude alternates 0x0040, 0x8040, 0x0041, 0x8041, ... .
     * Its high bit is carried in rumble[2] bit 7. */
    if (rumble[3] < 0x40)
        low_index = 0;
    else
        low_index = (rumble[3] - 0x40) * 2 + !!(rumble[2] & 0x80);

    *weak_magnitude = switch2_jc1_rumble_magnitude(high_index);
    *strong_magnitude = switch2_jc1_rumble_magnitude(low_index);
}

static bool jc1_compat = true;
static unsigned int jc1_axis_map = 2;
static unsigned int jc1_axis_sign = 2;
static bool jc1_right_sdl_frame = true;

static int switch2_jc1_compat_set(const char *value,
                                  const struct kernel_param *kp)
{
    bool old = READ_ONCE(jc1_compat);
    int ret;

    ret = param_set_bool(value, kp);
    if (ret)
        return ret;

    if (old != READ_ONCE(jc1_compat))
        switch2_controller_refresh_ble_frontends();

    return 0;
}

static const struct kernel_param_ops switch2_jc1_compat_ops = {
    .set = switch2_jc1_compat_set,
    .get = param_get_bool,
};

module_param_cb(jc1_compat, &switch2_jc1_compat_ops, &jc1_compat, 0644);
MODULE_PARM_DESC(jc1_compat,
    "Select BLE Joy-Con 2 controller frontend: 1=classic JC1 compatibility, 0=native JC2");
module_param_named(jc1_axis_map, jc1_axis_map, uint, 0644);
MODULE_PARM_DESC(jc1_axis_map,
    "Synthetic JC1 IMU axis permutation: 0=xyz 1=xzy 2=yxz 3=yzx 4=zxy 5=zyx");
module_param_named(jc1_axis_sign, jc1_axis_sign, uint, 0644);
MODULE_PARM_DESC(jc1_axis_sign,
    "Synthetic JC1 IMU axis sign mask: bit0=X bit1=Y bit2=Z");
module_param_named(jc1_right_sdl_frame, jc1_right_sdl_frame, bool, 0644);
MODULE_PARM_DESC(jc1_right_sdl_frame,
    "Apply SDL JoyConRight Y/Z frame compensation to synthetic JC1 IMU data");

struct switch2_jc1_sample {
    s16 accel[3];
    s16 gyro[3];
};

struct switch2_jc1 {
    struct switch2_controller *controller;
    struct hid_device *hdev;

    u8 timer;
    u8 latest_buttons[3];
    u8 latest_stick[3];
    u8 native_battery_status;
    bool common_motion_active;

    struct switch2_jc1_sample fifo[JC1_SAMPLE_FIFO_LEN];
    u8 fifo_count;

};

static int switch2_jc1_forward_rumble(struct switch2_jc1 *jc1,
                                      const u8 *buf, size_t len)
{
    const u8 *rumble;
    u16 strong_magnitude;
    u16 weak_magnitude;

    /* Report 0x10 is: id, packet number, left[4], right[4], padding. */
    if (!jc1 || !jc1->controller || len < 10)
        return -EINVAL;

    rumble = jc1->controller->type == NS2_CTLR_TYPE_JCR ? &buf[6] : &buf[2];
    switch2_jc1_decode_rumble(rumble, &strong_magnitude, &weak_magnitude);

    return switch2_ff_set_rumble(jc1->controller,
                                 strong_magnitude, weak_magnitude);
}


/* Minimal classic Joy-Con HID descriptor: 0x21 and 0x30 input,
 * 0x01/0x10/0x80 output. */
static const u8 switch2_jc1_rdesc[] = {
    0x05,0x01,0x09,0x05,0xa1,0x01,
    0x85,0x21,0x06,0x00,0xff,0x09,0x01,0x15,0x00,0x26,0xff,0x00,
    0x75,0x08,0x95,0x31,0x09,0x01,0x81,0x02,
    0x85,0x30,0x06,0x00,0xff,0x09,0x01,0x15,0x00,0x26,0xff,0x00,
    0x75,0x08,0x95,0x30,0x09,0x01,0x81,0x02,
    0x85,0x01,0x06,0x00,0xff,0x09,0x01,0x15,0x00,0x26,0xff,0x00,
    0x75,0x08,0x95,0x30,0x09,0x01,0x91,0x02,
    0x85,0x10,0x06,0x00,0xff,0x09,0x01,0x15,0x00,0x26,0xff,0x00,
    0x75,0x08,0x95,0x30,0x09,0x01,0x91,0x02,
    0x85,0x80,0x06,0x00,0xff,0x09,0x01,0x15,0x00,0x26,0xff,0x00,
    0x75,0x08,0x95,0x30,0x09,0x01,0x91,0x02,
    0xc0,
};

static int switch2_jc1_imu_source_axis(int out_axis)
{
    static const u8 maps[6][3] = {
        { 0, 1, 2 }, /* xyz */
        { 0, 2, 1 }, /* xzy */
        { 1, 0, 2 }, /* yxz */
        { 1, 2, 0 }, /* yzx */
        { 2, 0, 1 }, /* zxy */
        { 2, 1, 0 }, /* zyx */
    };
    unsigned int map = READ_ONCE(jc1_axis_map);

    if (map >= ARRAY_SIZE(maps))
        map = 0;
    return maps[map][out_axis];
}

/* SDL's classic JoyConRight HID path applies a right-side sensor-frame
 * transform.  The JC2 common 0x05 data is already in the common/canonical
 * frame, so present synthetic 057e:2007 IMU data in the raw JC1 frame SDL
 * expects.  Keep this strictly at the synthetic-JC1 boundary. */
static s32 switch2_jc1_imu_frame_axis(struct switch2_controller *ns2,
                                       const s32 native[3], int out_axis)
{
    s32 value = native[switch2_jc1_imu_source_axis(out_axis)];

    if (READ_ONCE(jc1_axis_sign) & BIT(out_axis))
        value = -value;

    if (READ_ONCE(jc1_right_sdl_frame) && ns2 &&
        ns2->type == NS2_CTLR_TYPE_JCR &&
        (BIT(out_axis) & (BIT(1) | BIT(2))))
        value = -value;

    return value;
}

static s16 switch2_jc1_gyro_raw(s32 native)
{
    s64 numerator = (s64)native * JC1_GYRO_SCALE_NUM;
    s64 scaled;

    /* Round symmetrically about zero.  Do not use a signed right shift here:
     * arithmetic shift rounds negative values toward -infinity and gives the
     * old translator a one-count negative bias near zero. */
    if (numerator < 0)
        scaled = -div_s64(-numerator + JC1_GYRO_SCALE_DEN / 2,
                          JC1_GYRO_SCALE_DEN);
    else
        scaled = div_s64(numerator + JC1_GYRO_SCALE_DEN / 2,
                         JC1_GYRO_SCALE_DEN);

    return clamp_t(s64, scaled, S16_MIN, S16_MAX);
}

static s16 switch2_jc1_common_gyro_raw(s32 native)
{
    s64 scaled = (s64)native * JC1_COMMON_GYRO_Q30;
    s64 rounded;

    if (scaled < 0)
        rounded = -div_s64(-scaled + (1LL << (JC1_COMMON_GYRO_SHIFT - 1)),
                           1LL << JC1_COMMON_GYRO_SHIFT);
    else
        rounded = div_s64(scaled + (1LL << (JC1_COMMON_GYRO_SHIFT - 1)),
                          1LL << JC1_COMMON_GYRO_SHIFT);

    return clamp_t(s64, rounded, S16_MIN, S16_MAX);
}


static void switch2_jc1_neutral_stick(u8 stick[3])
{
    stick[0] = 0x00;
    stick[1] = 0x08;
    stick[2] = 0x80;
}

/*
 * Native JC2 report byte 1 is Power Info:
 *   bit 0    external power
 *   bit 1    charging
 *   bits 2:5 battery level (0..9)
 *
 * The archived BLE corpus independently shows a slow discharge sequence
 * 0x20 -> 0x1c -> 0x18 -> 0x14, i.e. native levels 8 -> 7 -> 6 -> 5.
 * Classic Joy-Con reports use the battery nibble in two-step levels
 * 8/6/4/2/0, with its low bit indicating charging.  Quantize only the level;
 * preserve the controller's real charging flag.  Keep the classic connection
 * low nibble at zero as before: native external power is not the synthetic
 * Bluetooth transport's connection type.
 */
static u8 switch2_jc1_battery_level(u8 power_info)
{
    u8 level = (power_info >> 2) & 0x0f;

    /* Native has ten states (0..9); classic JC1 only has five (0/2/4/6/8).
     * Do not map native 8/9 both to classic 8: that turns 8/9 (~89%) into
     * a false 100% reading.  Use the lower classic bucket so only native 9
     * is advertised as full. */
    if (level >= 9)
        level = 8;
    else if (level >= 7)
        level = 6;
    else if (level >= 5)
        level = 4;
    else if (level >= 3)
        level = 2;
    else
        level = 0;

    if (power_info & BIT(1))
        level |= 1;

    return level << 4;
}

static void switch2_jc1_state(struct switch2_jc1 *jc1, u8 state[12])
{
    u8 neutral[3];

    memset(state, 0, 12);
    state[0] = jc1->timer;
    state[1] = switch2_jc1_battery_level(
        READ_ONCE(jc1->native_battery_status));
    memcpy(&state[2], jc1->latest_buttons, 3);
    switch2_jc1_neutral_stick(neutral);

    if (jc1->controller->type == NS2_CTLR_TYPE_JCR) {
        memcpy(&state[5], neutral, 3);
        memcpy(&state[8], jc1->latest_stick, 3);
    } else {
        memcpy(&state[5], jc1->latest_stick, 3);
        memcpy(&state[8], neutral, 3);
    }
    state[11] = 0;
}

static s16 switch2_jc1_gyro_zero(struct switch2_controller *ns2, int axis)
{
    int source = switch2_jc1_imu_source_axis(axis);
    s32 zero = ns2->ble_gyro_zero[source];

    /* Advertise the controller's real BLE gyro zero in the same synthetic-JC1
     * frame used by the live IMU stream.  SDL applies this zero itself. */
    if (READ_ONCE(jc1_axis_sign) & BIT(axis))
        zero = -zero;

    if (READ_ONCE(jc1_right_sdl_frame) &&
        ns2->type == NS2_CTLR_TYPE_JCR &&
        (BIT(axis) & (BIT(1) | BIT(2))))
        zero = -zero;

    return clamp_t(s32, zero, S16_MIN, S16_MAX);
}

static bool switch2_jc1_stick_cal_valid(
    const struct switch2_stick_calibration *calib)
{
    if (!calib)
        return false;

    return calib->x.neutral && calib->x.neutral <= 0x0fff &&
           calib->x.negative && calib->x.negative <= 0x0fff &&
           calib->x.positive && calib->x.positive <= 0x0fff &&
           calib->y.neutral && calib->y.neutral <= 0x0fff &&
           calib->y.negative && calib->y.negative <= 0x0fff &&
           calib->y.positive && calib->y.positive <= 0x0fff;
}

static void switch2_jc1_pack_stick_pair(u8 out[3], u16 first, u16 second)
{
    first &= 0x0fff;
    second &= 0x0fff;

    out[0] = first & 0xff;
    out[1] = ((first >> 8) & 0x0f) | ((second & 0x0f) << 4);
    out[2] = (second >> 4) & 0xff;
}

static void switch2_jc1_factory_stick_data(struct switch2_jc1 *jc1, u8 out[18])
{
    const struct switch2_stick_calibration *calib =
        &jc1->controller->stick_calib[0];
    u8 *active;

    /* 0xff is the real JC1 "no calibration in this slot" encoding.  Keep the
     * inactive synthetic stick absent instead of inventing a second stick's
     * calibration. */
    memset(out, 0xff, JC1_FACTORY_STICK_LEN);
    if (!switch2_jc1_stick_cal_valid(calib))
        return;

    if (jc1->controller->type == NS2_CTLR_TYPE_JCR) {
        /* JC1 right-stick factory order:
         * center, negative range, positive range. */
        active = &out[9];
        switch2_jc1_pack_stick_pair(&active[0],
                                    calib->x.neutral, calib->y.neutral);
        switch2_jc1_pack_stick_pair(&active[3],
                                    calib->x.negative, calib->y.negative);
        switch2_jc1_pack_stick_pair(&active[6],
                                    calib->x.positive, calib->y.positive);
    } else {
        /* JC1 left-stick factory order:
         * positive range, center, negative range. */
        active = &out[0];
        switch2_jc1_pack_stick_pair(&active[0],
                                    calib->x.positive, calib->y.positive);
        switch2_jc1_pack_stick_pair(&active[3],
                                    calib->x.neutral, calib->y.neutral);
        switch2_jc1_pack_stick_pair(&active[6],
                                    calib->x.negative, calib->y.negative);
    }
}

static void switch2_jc1_spi_data(struct switch2_jc1 *jc1, u32 address,
                                 u8 length, u8 *out)
{
    struct switch2_controller *ns2 = jc1->controller;
    int axis;

    memset(out, 0, length);

    if (address == JC1_FACTORY_STICK_ADDR && length == JC1_FACTORY_STICK_LEN) {
        switch2_jc1_factory_stick_data(jc1, out);
        return;
    }
    if (address == JC1_USER_STICK_ADDR && length == JC1_USER_STICK_LEN)
        return;
    if (address == JC1_USER_IMU_ADDR && length == JC1_USER_IMU_LEN)
        return;

    if (address == JC1_FACTORY_IMU_ADDR && length == JC1_FACTORY_IMU_LEN) {
        /* accel offset[3], accel scale[3], gyro offset[3], gyro scale[3] */
        for (axis = 0; axis < 3; axis++)
            put_unaligned_le16(0, &out[axis * 2]);
        for (axis = 0; axis < 3; axis++)
            put_unaligned_le16(16384, &out[6 + axis * 2]);
        for (axis = 0; axis < 3; axis++) {
            s16 zero = switch2_jc1_gyro_zero(ns2, axis);
            put_unaligned_le16(zero, &out[12 + axis * 2]);
            put_unaligned_le16(clamp_t(s32, (s32)zero + JC1_GYRO_CAL_SPAN,
                                       S16_MIN, S16_MAX),
                               &out[18 + axis * 2]);
        }
    }
}

static void switch2_jc1_send_reply(struct switch2_jc1 *jc1,
                                   const u8 *command, size_t len)
{
    u8 reply[50] = { 0 };
    u8 state[12];
    u8 subcmd;
    u8 *data = &reply[15];

    if (!jc1->hdev || !command || len < 11)
        return;

    subcmd = command[10];
    switch2_jc1_state(jc1, state);
    reply[0] = JC1_INPUT_SUBCMD_REPLY;
    memcpy(&reply[1], state, sizeof(state));
    reply[13] = 0x80;
    reply[14] = subcmd;

    switch (subcmd) {
    case 0x02: { /* request device info */
        const char *phys = jc1->controller->phys;
        const char *mac_text = phys ? strrchr(phys, '/') : NULL;
        unsigned int mac[6];
        int i;

        if (mac_text)
            mac_text++;
        else
            mac_text = phys;

        data[0] = 0x04;
        data[1] = 0x00;
        data[2] = jc1->controller->type == NS2_CTLR_TYPE_JCR ? 0x02 : 0x01;
        data[3] = 0x02;
        if (mac_text &&
            sscanf(mac_text, "%02x:%02x:%02x:%02x:%02x:%02x",
                   &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
            for (i = 0; i < 6; i++)
                data[4 + i] = mac[i];
        }
        break;
    }
    case 0x10: /* SPI flash read */
        if (len >= 16) {
            u32 address = get_unaligned_le32(&command[11]);
            u8 requested = min_t(u8, command[15], 30);

            memcpy(data, &command[11], 5);
            switch2_jc1_spi_data(jc1, address, requested, &data[5]);
        }
        break;
    case 0x03: /* set input report mode */
    case 0x40: /* enable IMU */
        /* The compatibility HID always emits its fixed full-state format. */
        break;
    default:
        /* Player LEDs, vibration enable, IMU sensitivity and other benign
         * classic setup commands are host-facing compatibility state and are
         * ACKed without changing the physical controller. */
        break;
    }

    hid_input_report(jc1->hdev, HID_INPUT_REPORT, reply, sizeof(reply), 1);
}

static int switch2_jc1_handle_output(struct switch2_jc1 *jc1,
                                     const u8 *buf, size_t len)
{
    if (!jc1 || !buf || !len)
        return -EINVAL;

    switch (buf[0]) {
    case JC1_OUTPUT_SUBCMD:
        switch2_jc1_send_reply(jc1, buf, len);
        break;
    case JC1_OUTPUT_RUMBLE: {
        int ret = switch2_jc1_forward_rumble(jc1, buf, len);

        return ret ? ret : (int)len;
    }
    case JC1_OUTPUT_PROPRIETARY:
        /* Bluetooth identity should not need USB proprietary commands. */
        break;
    default:
        break;
    }

    return len;
}

static int switch2_jc1_hid_parse(struct hid_device *hdev)
{
    return hid_parse_report(hdev, switch2_jc1_rdesc,
                            sizeof(switch2_jc1_rdesc));
}

static int switch2_jc1_hid_start(struct hid_device *hdev)
{
    return 0;
}

static void switch2_jc1_hid_stop(struct hid_device *hdev)
{
}

static int switch2_jc1_hid_open(struct hid_device *hdev)
{
    return 0;
}

static void switch2_jc1_hid_close(struct hid_device *hdev)
{
}

static int switch2_jc1_hid_output_report(struct hid_device *hdev, u8 *buf,
                                        size_t len)
{
    struct switch2_jc1 *jc1 = dev_get_platdata(&hdev->dev);

    return switch2_jc1_handle_output(jc1, buf, len);
}

static int switch2_jc1_hid_raw_request(struct hid_device *hdev,
                                      unsigned char reportnum, u8 *buf,
                                      size_t len, unsigned char rtype,
                                      int reqtype)
{
    struct switch2_jc1 *jc1 = dev_get_platdata(&hdev->dev);

    if (reqtype == HID_REQ_SET_REPORT) {
        if (len && buf[0] != reportnum)
            buf[0] = reportnum;
        return switch2_jc1_handle_output(jc1, buf, len);
    }

    return -EOPNOTSUPP;
}

static const struct hid_ll_driver switch2_jc1_hid_driver = {
    .start = switch2_jc1_hid_start,
    .stop = switch2_jc1_hid_stop,
    .open = switch2_jc1_hid_open,
    .close = switch2_jc1_hid_close,
    .parse = switch2_jc1_hid_parse,
    .raw_request = switch2_jc1_hid_raw_request,
    .output_report = switch2_jc1_hid_output_report,
};

int switch2_jc1_create(struct switch2_controller *ns2)
{
    struct switch2_jc1 *jc1;
    struct hid_device *hdev;
    int ret;

    if (ns2->jc1)
        return 0;

    jc1 = kzalloc(sizeof(*jc1), GFP_KERNEL);
    if (!jc1)
        return -ENOMEM;

    hdev = hid_allocate_device();
    if (IS_ERR(hdev)) {
        ret = PTR_ERR(hdev);
        kfree(jc1);
        return ret;
    }

    jc1->controller = ns2;
    jc1->hdev = hdev;
    /* Preserve the old full-battery facade only until the first ordinary
     * native packet supplies the controller's housekeeping value. */
    jc1->native_battery_status = 0x20;
    switch2_jc1_neutral_stick(jc1->latest_stick);

    hdev->ll_driver = &switch2_jc1_hid_driver;
    hdev->bus = BUS_BLUETOOTH;
    hdev->vendor = JC1_VENDOR_ID;
    hdev->product = ns2->type == NS2_CTLR_TYPE_JCR ?
        JC1_RIGHT_PRODUCT_ID : JC1_LEFT_PRODUCT_ID;
    hdev->version = 0x0100;
    hdev->dev.parent = switch2_dev(ns2);
    hdev->dev.platform_data = jc1;
    snprintf(hdev->name, sizeof(hdev->name), "Nintendo Switch Joy-Con (%c)",
             ns2->type == NS2_CTLR_TYPE_JCR ? 'R' : 'L');
    snprintf(hdev->phys, sizeof(hdev->phys), "switch2-jc1/%.*s",
             (int)sizeof(hdev->phys) - 13, ns2->phys);
    snprintf(hdev->uniq, sizeof(hdev->uniq), "%s", ns2->serial);

    /* This is a host-facing JC1 protocol facade implemented by this driver.
     * Keep the genuine 057e:2006/2007 Bluetooth identity so SDL selects its
     * classic Joy-Con HIDAPI path, but tell HID core that this transport must
     * stay on hid-generic rather than a special Nintendo driver.
     *
     * Use initial_quirks, not hdev->quirks: hid_add_device() rebuilds quirks
     * with hid_lookup_quirk(), and that lookup deliberately preserves the
     * transport-supplied initial_quirks. */
    hdev->initial_quirks |= HID_QUIRK_IGNORE_SPECIAL_DRIVER;

    /* Publish before hid_add_device(): hid-generic/hidraw can become visible
     * synchronously while the device is registered. */
    ns2->jc1 = jc1;
    ret = hid_add_device(hdev);
    if (ret) {
        ns2->jc1 = NULL;
        hid_destroy_device(hdev);
        kfree(jc1);
        return ret;
    }

    /* HID's bus does not support driver_override in 6.12.  Normally the
     * IGNORE_SPECIAL_DRIVER initial quirk lets hid-generic win immediately.
     * If a special driver was already bound first, explicitly detach it and
     * attach hid-generic.  This is scoped to this synthetic hid_device only. */
    if (!hdev->dev.driver || strcmp(hdev->dev.driver->name, "hid-generic")) {
        struct device_driver *generic;

        generic = driver_find("hid-generic", &hid_bus_type);
        if (!generic) {
            ret = -ENODEV;
        } else {
            if (hdev->dev.driver)
                device_release_driver(&hdev->dev);
            ret = device_driver_attach(generic, &hdev->dev);
        }

        if (ret) {
            dev_err(switch2_dev(ns2),
                    "failed to bind classic compatibility HID to hid-generic: %d\n",
                    ret);
            ns2->jc1 = NULL;
            hid_destroy_device(hdev);
            kfree(jc1);
            return ret;
        }
    }

    dev_info(switch2_dev(ns2),
             "classic compatibility HID created: 0005:057E:%04X (%c)\n",
             hdev->product, ns2->type == NS2_CTLR_TYPE_JCR ? 'R' : 'L');
    return 0;
}


void switch2_jc1_destroy(struct switch2_controller *ns2)
{
    struct switch2_jc1 *jc1;

    if (!ns2)
        return;
    jc1 = ns2->jc1;
    if (!jc1)
        return;

    ns2->jc1 = NULL;
    if (jc1->hdev)
        hid_destroy_device(jc1->hdev);
    kfree(jc1);
}

bool switch2_jc1_compat_enabled(void)
{
    return READ_ONCE(jc1_compat);
}

int switch2_ble_frontend_sync_locked(struct switch2_controller *ns2)
{
    int ret;

    lockdep_assert_held(&ns2->lock);

    if (!switch2_controller_is_ble(ns2) ||
        ns2->init_step != NS2_INIT_DONE ||
        !switch2_controller_is_joycon(ns2->type))
        return 0;

    if (switch2_jc1_compat_enabled()) {
        bool had_native = rcu_access_pointer(ns2->input);

        if (ns2->jc1 && !had_native)
            return 0;

        ret = switch2_jc1_create(ns2);
        if (ret)
            return ret;

        /* Do not let a rumble request owned by the disappearing frontend keep
         * refreshing after the runtime mode switch. */
        if (had_native)
            switch2_ff_set_rumble(ns2, 0, 0);
        switch2_input_destroy_gamepad(ns2);
        dev_info(switch2_dev(ns2),
                 "BLE controller frontend: classic JC1 compatibility\n");
        return 0;
    }

    if (rcu_access_pointer(ns2->input) && !ns2->jc1)
        return 0;

    ret = switch2_input_create(ns2);
    if (ret)
        return ret;

    if (ns2->jc1)
        switch2_ff_set_rumble(ns2, 0, 0);
    switch2_jc1_destroy(ns2);
    dev_info(switch2_dev(ns2),
             "BLE controller frontend: native JC2 (common 0x05)\n");
    return 0;
}

static void switch2_jc1_emit_samples(struct switch2_jc1 *jc1,
                                     const struct switch2_jc1_sample samples[3])
{
    u8 report[49] = { 0 };
    u8 state[12];
    unsigned int wire;

    if (!jc1->hdev)
        return;

    jc1->timer += 3;
    switch2_jc1_state(jc1, state);
    report[0] = JC1_INPUT_FULL_STATE;
    memcpy(&report[1], state, sizeof(state));

    /* SDL consumes classic Joy-Con IMU slots 2,1,0 chronologically. */
    for (wire = 0; wire < 3; wire++) {
        const struct switch2_jc1_sample *sample = &samples[2 - wire];
        u8 *dst = &report[13 + wire * 12];
        int axis;

        for (axis = 0; axis < 3; axis++)
            put_unaligned_le16(sample->accel[axis], &dst[axis * 2]);
        for (axis = 0; axis < 3; axis++)
            put_unaligned_le16(sample->gyro[axis], &dst[6 + axis * 2]);
    }

    hid_input_report(jc1->hdev, HID_INPUT_REPORT, report, sizeof(report), 1);
}

static void switch2_jc1_emit_full(struct switch2_jc1 *jc1)
{
    if (!jc1->hdev || jc1->fifo_count != JC1_SAMPLE_FIFO_LEN)
        return;

    switch2_jc1_emit_samples(jc1, jc1->fifo);
    jc1->fifo_count = 0;
}

static void switch2_jc1_push_sample(struct switch2_jc1 *jc1,
                                    const s16 accel[3], const s16 gyro[3])
{
    struct switch2_jc1_sample *sample;

    if (jc1->fifo_count >= JC1_SAMPLE_FIFO_LEN)
        switch2_jc1_emit_full(jc1);

    sample = &jc1->fifo[jc1->fifo_count++];
    memcpy(sample->accel, accel, sizeof(sample->accel));
    memcpy(sample->gyro, gyro, sizeof(sample->gyro));

    if (jc1->fifo_count == JC1_SAMPLE_FIFO_LEN)
        switch2_jc1_emit_full(jc1);
}



void switch2_jc1_push_common(struct switch2_controller *ns2,
                              const s16 accel[3], const s16 gyro[3],
                              const u8 buttons[4], const u8 stick[3])
{
    struct switch2_jc1 *jc1;
    s32 native_accel[3];
    s32 native_gyro[3];
    s16 converted_accel[3];
    s16 converted_gyro[3];
    int axis;

    if (!READ_ONCE(jc1_compat) || !ns2 ||
        !switch2_controller_is_ble(ns2) ||
        ns2->init_step != NS2_INIT_DONE ||
        !switch2_ble_motion_calibration_ready(ns2))
        return;

    if (!ns2->jc1 && switch2_jc1_create(ns2))
        return;
    jc1 = ns2->jc1;
    if (!jc1)
        return;

    /* Never mix a partial sparse-side batch with the common stream.  This is
     * packetization state only; no common sample is discarded after entry. */
    if (!jc1->common_motion_active) {
        jc1->fifo_count = 0;
        jc1->common_motion_active = true;
        dev_info(switch2_dev(ns2),
                 "JC1 motion source: BLE common 0x05\n");
    }

    memcpy(jc1->latest_buttons, buttons, sizeof(jc1->latest_buttons));
    memcpy(jc1->latest_stick, stick, sizeof(jc1->latest_stick));

    for (axis = 0; axis < 3; axis++) {
        native_accel[axis] = accel[axis];
        native_gyro[axis] = gyro[axis];
    }

    for (axis = 0; axis < 3; axis++) {
        converted_accel[axis] = clamp_t(
            s32, switch2_jc1_imu_frame_axis(jc1->controller, native_accel, axis),
            S16_MIN, S16_MAX);
        converted_gyro[axis] = switch2_jc1_common_gyro_raw(
            switch2_jc1_imu_frame_axis(jc1->controller, native_gyro, axis));
    }

    /* One common notification is one genuine synchronized IMU state.  The
     * existing FIFO merely packs three consecutive real states into the three
     * structural IMU slots of one classic-JC1 0x30 report. */
    switch2_jc1_push_sample(jc1, converted_accel, converted_gyro);
}

void switch2_jc1_push_native(struct switch2_controller *ns2,
                             const s16 accel[3][3], const s16 gyro[2][3],
                             const u8 buttons[4], const u8 stick[3],
                             u8 native_battery_status)
{
    struct switch2_jc1 *jc1;
    s16 converted_accel[3];
    s16 converted[3];
    int axis;

    if (!READ_ONCE(jc1_compat) || !ns2 ||
        !switch2_controller_is_ble(ns2) ||
        ns2->init_step != NS2_INIT_DONE ||
        !switch2_ble_motion_calibration_ready(ns2))
        return;

    if (!ns2->jc1 && switch2_jc1_create(ns2))
        return;
    jc1 = ns2->jc1;
    if (!jc1)
        return;

    if (jc1->common_motion_active) {
        jc1->fifo_count = 0;
        jc1->common_motion_active = false;
        dev_info(switch2_dev(ns2),
                 "JC1 motion source: BLE side fallback\n");
    }

    WRITE_ONCE(jc1->native_battery_status, native_battery_status);
    memcpy(jc1->latest_buttons, buttons, sizeof(jc1->latest_buttons));
    memcpy(jc1->latest_stick, stick, sizeof(jc1->latest_stick));


    /*
     * Side-report fallback: deliver exactly the two explicit motion states
     * present in this ordinary JC2 packet:
     *
     *   A1(n) + G2(n)
     *   A2(n) + G3(n)
     *
     * No quaternion correction, next-packet pairing, accumulator-derived
     * sample, interpolation, prediction, repetition, gain shaping or pacing.
     * JC1 report 0x30 requires three IMU slots, so consecutive genuine pairs
     * are only packetized together; no sensor sample is fabricated.
     */
    {
        s32 native_accel[3];
        s32 native_gyro[3];

        for (axis = 0; axis < 3; axis++) {
            native_accel[axis] = accel[1][axis];
            native_gyro[axis] = gyro[0][axis];
        }
        for (axis = 0; axis < 3; axis++) {
            converted_accel[axis] = clamp_t(
                s32, switch2_jc1_imu_frame_axis(jc1->controller, native_accel, axis),
                S16_MIN, S16_MAX);
            converted[axis] = switch2_jc1_gyro_raw(
                switch2_jc1_imu_frame_axis(jc1->controller, native_gyro, axis));
        }
        switch2_jc1_push_sample(jc1, converted_accel, converted);

        for (axis = 0; axis < 3; axis++) {
            native_accel[axis] = accel[2][axis];
            native_gyro[axis] = gyro[1][axis];
        }
        for (axis = 0; axis < 3; axis++) {
            converted_accel[axis] = clamp_t(
                s32, switch2_jc1_imu_frame_axis(jc1->controller, native_accel, axis),
                S16_MIN, S16_MAX);
            converted[axis] = switch2_jc1_gyro_raw(
                switch2_jc1_imu_frame_axis(jc1->controller, native_gyro, axis));
        }
        switch2_jc1_push_sample(jc1, converted_accel, converted);
    }
}
