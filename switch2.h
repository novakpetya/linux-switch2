// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * HID driver for Nintendo Switch controllers
 *
 * Copyright (c) 2025 Valve Software
 *
 * Current author and maintainer: Peter Novak
 * Original USB driver implementation: Vicki Pfau <vi@endrift.com>
 *
 * This driver is based on the following work:
 *   https://gist.github.com/shinyquagsire23/66f006b46c56216acbaac6c1e2279b64
 *   https://github.com/ndeadly/switch2_controller_research
 */

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/input.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/rcupdate.h>
#include "hid-ids.h"

/* BTN_GRIPL/BTN_GRIPR landed after some supported kernel branches. */
#ifndef BTN_GRIPL
#define BTN_GRIPL BTN_TRIGGER_HAPPY5
#endif
#ifndef BTN_GRIPR
#define BTN_GRIPR BTN_TRIGGER_HAPPY6
#endif

#define NS2_FLAG_OK	BIT(0)

#define NS2_FLASH_ADDR_SERIAL			0x13002
#define NS2_FLASH_ADDR_BLE_GYRO_CALIB		0x13040
#define NS2_FLASH_ADDR_BLE_ACCEL_CALIB		0x1310c
#define NS2_FLASH_ADDR_FACTORY_PRIMARY_CALIB	0x130a8
#define NS2_FLASH_ADDR_FACTORY_SECONDARY_CALIB	0x130e8
#define NS2_FLASH_ADDR_FACTORY_TRIGGER_CALIB	0x13140
#define NS2_FLASH_ADDR_USER_PRIMARY_CALIB	0x1fc040
#define NS2_FLASH_ADDR_USER_SECONDARY_CALIB	0x1fc080

#define NS2_FLASH_SIZE_SERIAL 0x10
#define NS2_FLASH_SIZE_BLE_GYRO_CALIB 0x10
#define NS2_FLASH_SIZE_BLE_ACCEL_CALIB 0x0c
#define NS2_FLASH_SIZE_FACTORY_AXIS_CALIB 9
#define NS2_FLASH_SIZE_FACTORY_TRIGGER_CALIB 2
#define NS2_FLASH_SIZE_USER_AXIS_CALIB 11

#define NS2_USER_CALIB_MAGIC 0xa1b2

#define NS2_FEATURE_BUTTONS	BIT(0)
#define NS2_FEATURE_ANALOG	BIT(1)
#define NS2_FEATURE_IMU		BIT(2)
#define NS2_FEATURE_MOUSE	BIT(4)
#define NS2_FEATURE_RUMBLE	BIT(5)

#define NS2_MAX_PLAYER_ID	8

#define NS2_BTNR_B	BIT(0)
#define NS2_BTNR_A	BIT(1)
#define NS2_BTNR_Y	BIT(2)
#define NS2_BTNR_X	BIT(3)
#define NS2_BTNR_R	BIT(4)
#define NS2_BTNR_ZR	BIT(5)
#define NS2_BTNR_PLUS	BIT(6)
#define NS2_BTNR_RS	BIT(7)

#define NS2_BTNL_DOWN	BIT(0)
#define NS2_BTNL_RIGHT	BIT(1)
#define NS2_BTNL_LEFT	BIT(2)
#define NS2_BTNL_UP	BIT(3)
#define NS2_BTNL_L	BIT(4)
#define NS2_BTNL_ZL	BIT(5)
#define NS2_BTNL_MINUS	BIT(6)
#define NS2_BTNL_LS	BIT(7)

/* Raw Joy-Con 2 BLE primary button banks. */
#define NS2_BLE_BTN_DOWN	BIT(0)
#define NS2_BLE_BTN_UP		BIT(1)
#define NS2_BLE_BTN_RIGHT	BIT(2)
#define NS2_BLE_BTN_LEFT	BIT(3)
/* Rail buttons occupy the two otherwise-unused primary-bank bits. */
#define NS2_BLE_BTN_SR		BIT(4)
#define NS2_BLE_BTN_SL		BIT(5)
#define NS2_BLE_BTN_SHOULDER	BIT(6)
#define NS2_BLE_BTN_TRIGGER	BIT(7)

/* Raw Joy-Con 2 BLE shared button bank (BLE byte 5). */
#define NS2_BLE_BTN_MINUS	BIT(0)
#define NS2_BLE_BTN_PLUS	BIT(1)
#define NS2_BLE_BTN_RS		BIT(2)
#define NS2_BLE_BTN_LS		BIT(3)
#define NS2_BLE_BTN_HOME	BIT(4)
#define NS2_BLE_BTN_CAPTURE	BIT(5)
#define NS2_BLE_BTN_C		BIT(6)

#define NS2_BTN3_C	BIT(4)
#define NS2_BTN3_SR	BIT(6)
#define NS2_BTN3_SL	BIT(7)

#define NS2_BTN_JCR_HOME	BIT(0)
#define NS2_BTN_JCR_GR		BIT(2)
#define NS2_BTN_JCR_C		NS2_BTN3_C
#define NS2_BTN_JCR_SR		NS2_BTN3_SR
#define NS2_BTN_JCR_SL		NS2_BTN3_SL

#define NS2_BTN_JCL_CAPTURE	BIT(0)
#define NS2_BTN_JCL_GL		BIT(2)
#define NS2_BTN_JCL_SR		NS2_BTN3_SR
#define NS2_BTN_JCL_SL		NS2_BTN3_SL

#define NS2_BTN_PRO_HOME	BIT(0)
#define NS2_BTN_PRO_CAPTURE	BIT(1)
#define NS2_BTN_PRO_GR		BIT(2)
#define NS2_BTN_PRO_GL		BIT(3)
#define NS2_BTN_PRO_C		NS2_BTN3_C

#define NS2_BTN_GC_HOME		BIT(0)
#define NS2_BTN_GC_CAPTURE	BIT(1)
#define NS2_BTN_GC_C		NS2_BTN3_C

#define NS2_TRIGGER_RANGE	4095
#define NS2_AXIS_MIN		-32768
#define NS2_AXIS_MAX		32767


enum switch2_cmd {
	NS2_CMD_NFC = 0x01,
	NS2_CMD_FLASH = 0x02,
	NS2_CMD_INIT = 0x03,
	NS2_CMD_BLE_SETUP_07 = 0x07,
	NS2_CMD_GRIP = 0x08,
	NS2_CMD_LED = 0x09,
	NS2_CMD_VIBRATE = 0x0a,
	NS2_CMD_FEATSEL = 0x0c,
	NS2_CMD_FW_INFO = 0x10,
	NS2_CMD_BLE_MOTION = 0x11,
	NS2_CMD_BT_PAIR = 0x15,
	NS2_CMD_BLE_SETUP_16 = 0x16,
};

#define NS2_DIR_OUT	0x90

enum switch2_transport_type {
	NS2_TRANS_USB = 0x00,
	NS2_TRANS_BT = 0x01,
};

#define NS2_SUBCMD_FLASH_READ	0x04

enum switch2_subcmd_init {
	NS2_SUBCMD_INIT_SELECT_REPORT = 0xa,
	NS2_SUBCMD_INIT_USB = 0xd,
};

enum switch2_subcmd_feature_select {
	NS2_SUBCMD_FEATSEL_SET_MASK = 0x2,
	NS2_SUBCMD_FEATSEL_ENABLE = 0x4,
};

#define NS2_SUBCMD_GRIP_ENABLE_BUTTONS	0x02

#define NS2_SUBCMD_LED_PATTERN	0x07

#define NS2_SUBCMD_FW_INFO_GET	0x01

enum switch2_controller_type {
	NS2_CTLR_TYPE_JCL = 0x00,
	NS2_CTLR_TYPE_JCR = 0x01,
	NS2_CTLR_TYPE_PRO = 0x02,
	NS2_CTLR_TYPE_GC = 0x03,
};

enum switch2_report_id {
	NS2_REPORT_COMMON = 0x05,
	NS2_REPORT_JCL = 0x07,
	NS2_REPORT_JCR = 0x08,
	NS2_REPORT_PRO = 0x09,
	NS2_REPORT_GC = 0x0a,
};

enum switch2_init_step {
	NS2_INIT_STARTING,
	NS2_INIT_BLE_INITIAL_SETUP,
	NS2_INIT_BLE_SETUP_07,
	NS2_INIT_BLE_SETUP_16,
	NS2_INIT_BLE_FINALIZE,
	NS2_INIT_BLE_SET_FEATURE_MASK,
	NS2_INIT_BLE_MOTION_SETUP,
	NS2_INIT_BLE_EXTENDED_SETUP,
	NS2_INIT_BLE_ENABLE_FEATURES,
	NS2_INIT_BLE_SELECT_REPORT,
	NS2_INIT_BLE_WAIT_INPUT,
	NS2_INIT_BLE_INPUT_READY,
	NS2_INIT_BLE_SETUP_10,
	NS2_INIT_BLE_SETUP_01_0C,
	NS2_INIT_BLE_SETUP_01_01,
	NS2_INIT_BLE_SET_LED,
	NS2_INIT_BLE_READ_GYRO_CALIB,
	NS2_INIT_BLE_READ_ACCEL_CALIB,
	NS2_INIT_READ_SERIAL,
	NS2_INIT_READ_FACTORY_PRIMARY_CALIB,
	NS2_INIT_READ_FACTORY_SECONDARY_CALIB,
	NS2_INIT_READ_FACTORY_TRIGGER_CALIB,
	NS2_INIT_READ_USER_PRIMARY_CALIB,
	NS2_INIT_READ_USER_SECONDARY_CALIB,
	NS2_INIT_BLE_RAW_SELECTOR,
	NS2_INIT_SET_FEATURE_MASK,
	NS2_INIT_ENABLE_FEATURES,
	NS2_INIT_GET_FIRMWARE_INFO,
#ifdef CONFIG_SWITCH2_FF
	NS2_INIT_ENABLE_RUMBLE,
#endif
	NS2_INIT_GRIP_BUTTONS,
	NS2_INIT_SET_PLAYER_LEDS,
	NS2_INIT_USB_FINALIZE,
	NS2_INIT_DONE,
};

struct switch2_cmd_header {
	uint8_t command;
	uint8_t direction;
	uint8_t transport;
	uint8_t subcommand;
	uint8_t unk1;
	uint8_t length;
	uint16_t unk2;
};
static_assert(sizeof(struct switch2_cmd_header) == 8);

#define NS2_CMD_HEADER_SIZE sizeof(struct switch2_cmd_header)
#define NS2_CMD_MAX_PAYLOAD 56
#define NS2_CMD_MAX_SIZE \
(NS2_CMD_HEADER_SIZE + NS2_CMD_MAX_PAYLOAD)

struct switch2_controller;
struct switch2_jc1;

struct switch2_version_info {
	uint8_t major;
	uint8_t minor;
	uint8_t patch;
	uint8_t controller_type;
	__le32 unk;
	int8_t dsp_major;
	int8_t dsp_minor;
	int8_t dsp_patch;
	int8_t dsp_type;
};

struct switch2_axis_calibration {
	uint16_t neutral;
	uint16_t negative;
	uint16_t positive;
};

struct switch2_stick_calibration {
	struct switch2_axis_calibration x;
	struct switch2_axis_calibration y;
};

struct switch2_hd_rumble {
	uint16_t hi_freq : 10;
	uint16_t hi_amp : 10;
	uint16_t lo_freq : 10;
	uint16_t lo_amp : 10;
} __packed;

struct switch2_erm_rumble {
	uint16_t error;
	uint16_t amplitude;
};

struct switch2_transport {
	struct switch2_controller *controller;
	enum switch2_transport_type type;
	int (*send_command)(struct switch2_transport *transport,
		enum switch2_cmd command, uint8_t subcommand,
		const void *message, size_t length);
	int (*send_raw)(struct switch2_transport *transport,
		const void *message, size_t length);

};


struct switch2_controller {
	struct hid_device *hdev;
	struct switch2_jc1 *jc1;
	struct switch2_transport *transport;

	char phys[64];
	struct list_head entry;
	refcount_t refcount;
	struct mutex lock;

	enum switch2_controller_type type;
	enum switch2_init_step init_step;
	struct delayed_work init_work;
	struct input_dev __rcu *input;
	struct input_dev __rcu *mouse_input;
	bool mouse_surface_active;
	u8 mouse_surface_enter_count;
	u8 mouse_surface_exit_count;
	bool mouse_left_down;
	bool mouse_right_down;
	bool mouse_middle_down;
	bool mouse_common_valid;
	u16 mouse_common_x;
	u16 mouse_common_y;
	s64 mouse_scroll_x_accum;
	s64 mouse_scroll_y_accum;
	u64 mouse_scroll_last_ns;
	char serial[NS2_FLASH_SIZE_SERIAL + 1];
	struct switch2_version_info version;

	/* Controller flash calibration.  BLE initialization reads the factory/user
	 * stick blocks before input is activated; never replace their neutral point
	 * with a runtime-learned value. */
	struct switch2_stick_calibration stick_calib[2];

	/* BLE factory IMU calibration used by the synthetic JC1 compatibility HID. */
	bool ble_gyro_calibration_valid;
	bool ble_accel_calibration_valid;
	s16 ble_gyro_zero[3];

	uint8_t lt_zero;
	uint8_t rt_zero;

	uint32_t player_id;

#ifdef CONFIG_SWITCH2_FF
	spinlock_t rumble_lock;
	uint8_t rumble_seq;
	union {
		struct switch2_hd_rumble hd;
		struct switch2_erm_rumble sd;
	} rumble;
	unsigned long last_rumble_jiffies;
	struct delayed_work rumble_work;
#endif
};

static inline bool switch2_controller_is_ble(const struct switch2_controller *ns2)
{
	return ns2 && ns2->transport && ns2->transport->type == NS2_TRANS_BT;
}

static inline bool switch2_ble_motion_calibration_ready(
	const struct switch2_controller *ns2)
{
	return ns2 && ns2->ble_gyro_calibration_valid &&
	       ns2->ble_accel_calibration_valid;
}

struct switch2_controller *switch2_controller_get(const char *phys);
void switch2_controller_put(struct switch2_controller *controller);

void switch2_init_work_init(struct switch2_controller *controller);
void switch2_init_work_cancel(struct switch2_controller *controller);
void switch2_init_schedule_locked(struct switch2_controller *controller,
	unsigned int delay_ms);

int switch2_transport_attach(struct switch2_controller *controller,
	struct switch2_transport *transport);

void switch2_transport_detach(struct switch2_transport *transport);

int switch2_transport_build_command(
	const struct switch2_transport *transport,
	enum switch2_cmd command, uint8_t subcommand,
	const void *payload, size_t payload_size,
	uint8_t *buffer, size_t buffer_size);

int switch2_transport_send_raw(struct switch2_controller *controller,
	const void *payload, size_t payload_size);
int switch2_transport_send(struct switch2_controller *controller,
	enum switch2_cmd command, uint8_t subcommand,
	const void *payload, size_t payload_size);

int switch2_transport_receive(struct switch2_transport *transport,
	const uint8_t *packet, size_t packet_size);

int switch2_protocol_handle_reply(struct switch2_controller *controller,
	const uint8_t *message, size_t length);

int switch2_input_create(struct switch2_controller *ns2);
int switch2_mouse_input_create(struct switch2_controller *ns2);

int switch2_input_receive_state(struct switch2_controller *controller,
	const uint8_t *report, int report_size);

int switch2_input_receive_ble_side(struct switch2_controller *controller,
	const uint8_t *report, int report_size);

void switch2_input_disconnect_locked(struct switch2_controller *controller);

int switch2_init_advance_locked(struct switch2_controller *controller);


void switch2_input_destroy_gamepad(struct switch2_controller *controller);
void switch2_input_destroy(struct switch2_controller *controller);

bool switch2_jc1_compat_enabled(void);
int switch2_jc1_create(struct switch2_controller *controller);
void switch2_jc1_destroy(struct switch2_controller *controller);
int switch2_ble_frontend_sync_locked(struct switch2_controller *controller);
void switch2_jc1_push_common(struct switch2_controller *controller,
	const s16 accel[3], const s16 gyro[3],
	const u8 buttons[4], const u8 stick[3]);
void switch2_jc1_push_native(struct switch2_controller *controller,
	const s16 accel[3][3], const s16 gyro[2][3],
	const u8 buttons[4], const u8 stick[3],
	u8 native_battery_status);

int switch2_controller_activate_locked(struct switch2_controller *ns2);
void switch2_controller_deactivate(struct switch2_controller *ns2);


void switch2_controller_cleanup(void);
void switch2_controller_refresh_ble_frontends(void);


static inline bool switch2_controller_is_joycon(enum switch2_controller_type type)
{
	return type == NS2_CTLR_TYPE_JCL || type == NS2_CTLR_TYPE_JCR;
}

static inline struct device *switch2_dev(struct switch2_controller *ns2)
{
	return ns2 && ns2->hdev ? &ns2->hdev->dev : NULL;
}

#ifdef CONFIG_SWITCH2_FF
int switch2_ff_init(struct switch2_controller *ns2);
void switch2_ff_destroy(struct switch2_controller *ns2);
int switch2_ff_configure_input(struct switch2_controller *ns2,
							   struct input_dev *input);
int switch2_ff_set_rumble(struct switch2_controller *ns2,
			 u16 strong_magnitude, u16 weak_magnitude);
#else
static inline int switch2_ff_init(struct switch2_controller *ns2)
{
	return 0;
}

static inline void switch2_ff_destroy(struct switch2_controller *ns2)
{
}

static inline int switch2_ff_configure_input(
	struct switch2_controller *ns2, struct input_dev *input)
{
	return 0;
}

static inline int switch2_ff_set_rumble(struct switch2_controller *ns2,
					u16 strong_magnitude,
					u16 weak_magnitude)
{
	return -EOPNOTSUPP;
}

#endif
