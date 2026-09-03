# linux-switch2

Linux kernel drivers for Nintendo Switch 2 controllers.

The project provides two kernel modules:

- `hid_switch2` — shared controller protocol, HID/input handling, BLE GATT transport endpoint, motion, calibration, rumble, and compatibility frontends;
- `switch2_usb` — native USB transport.

Bluetooth Joy-Con 2 support additionally requires the [`bluez-switch2`](https://github.com/novakpetya/bluez-switch2) built-in BlueZ plugin.

Detailed reverse-engineered protocol information is documented separately in [`PROTOCOL.md`](PROTOCOL.md).

## Supported devices

The current driver contains USB support for:

| Device | USB ID |
| --- | --- |
| Joy-Con 2 (R) | `057e:2066` |
| Joy-Con 2 (L) | `057e:2067` |
| Switch 2 Pro Controller | `057e:2069` |
| Switch 2 GameCube Controller | `057e:2073` |

BLE transport is currently implemented for Joy-Con 2 Left and Right.

## Requirements

You need:

- a Linux kernel with matching kernel headers/build files;
- GNU make;
- a C compiler compatible with the running kernel.

For BLE Joy-Con 2 support you also need [`bluez-switch2`](https://github.com/novakpetya/bluez-switch2).

## Build

Build for the currently running kernel:

```sh
make
```

This produces:

```text
hid-switch2.ko
switch2-usb.ko
```

A different installed kernel can be selected with:

```sh
make KVERSION=<kernel-version>
```

## Install

For a direct installation into the running kernel:

```sh
sudo install -Dm644 hid-switch2.ko \
    /lib/modules/$(uname -r)/updates/switch2/hid-switch2.ko
    
sudo install -Dm644 switch2-usb.ko \
    /lib/modules/$(uname -r)/updates/switch2/switch2-usb.ko

sudo depmod -a
```

Load both modules:

```sh
sudo modprobe hid_switch2
sudo modprobe switch2_usb
```

To completely reload them:

```sh
sudo modprobe -r switch2_usb hid_switch2 2>/dev/null || true
sudo modprobe hid_switch2
sudo modprobe switch2_usb
```

A direct installation must be rebuilt after installing a new kernel.

The repository also contains `dkms.conf` for DKMS-based packaging.

## BLE support

BLE communication is deliberately split between BlueZ and the kernel.

The [`bluez-switch2`](https://github.com/novakpetya/bluez-switch2) plugin owns Bluetooth discovery and ATT/GATT communication and forwards packets through:

```text
/dev/switch2-gatt
```

`hid_switch2` owns controller initialization, protocol handling, calibration, input decoding, motion, rumble, and Linux input/HID frontend creation.

See the BlueZ plugin repository for first-time discovery and connection instructions.

## Module parameters

The following parameters belong to `hid_switch2`.

Current values can be inspected with:

```sh
grep . /sys/module/hid_switch2/parameters/*
```

Available parameters and their descriptions can also be inspected with:

```sh
modinfo hid-switch2.ko
```

Parameters can be supplied when loading the module:

```sh
sudo modprobe hid_switch2 \
    jc1_compat=1 \
    jc1_axis_map=2 \
    jc1_axis_sign=2 \
    jc1_right_sdl_frame=1
```

The defaults are the current known-good compatibility configuration and normally do not need to be changed.

### `jc1_compat`

Default:

```text
1
```

Values:

```text
0 = native Joy-Con 2 frontend
1 = synthetic classic Joy-Con frontend
```

This parameter applies to BLE Joy-Con 2 controllers.

Many applications currently understand the original Nintendo Joy-Con HID interface but not the new Joy-Con 2 interface. With `jc1_compat=1`, the driver therefore exposes an additional classic-compatible Joy-Con frontend:

```text
Left  -> 057e:2006
Right -> 057e:2007
```

With `jc1_compat=0`, the native Joy-Con 2 frontend is used instead.

The mode can be changed while the controller is connected:

```sh
echo 0 | sudo tee /sys/module/hid_switch2/parameters/jc1_compat
```

or:

```sh
echo 1 | sudo tee /sys/module/hid_switch2/parameters/jc1_compat
```

The active BLE frontend is replaced without requiring the controller to reconnect.

### `jc1_axis_map`

Default:

```text
2
```

Controls the IMU axis permutation used by the synthetic classic Joy-Con frontend.

Values:

| Value | Mapping |
| ---: | --- |
| `0` | `xyz` |
| `1` | `xzy` |
| `2` | `yxz` |
| `3` | `yzx` |
| `4` | `zxy` |
| `5` | `zyx` |

SDL deliberately remaps the sensor axes of classic Joy-Con controllers when converting their raw sensor frame into SDL's application coordinate frame.

The default mapping presents the synthetic Joy-Con in the frame expected by SDL's classic Joy-Con implementation.

The parameter is exposed because other applications may use a different coordinate convention, and users may also prefer a different physical controller orientation.

Example:

```sh
echo 2 | sudo tee /sys/module/hid_switch2/parameters/jc1_axis_map
```

This parameter affects only the synthetic BLE Joy-Con 1 compatibility frontend.

It does not alter the native Joy-Con 2 frontend or native USB input. USB Joy-Con 2 controllers are exposed through their native interface and applications such as SDL apply their own Joy-Con 2 handling.

### `jc1_axis_sign`

Default:

```text
2
```

Bit mask controlling axis inversion in the synthetic classic Joy-Con IMU frame:

```text
bit 0 (1) = invert X
bit 1 (2) = invert Y
bit 2 (4) = invert Z
```

Meaningful values are `0` through `7`, and bits can be combined:

| Value | Inverted axes |
| ---: | --- |
| `0` | none |
| `1` | X |
| `2` | Y |
| `3` | X + Y |
| `4` | Z |
| `5` | X + Z |
| `6` | Y + Z |
| `7` | X + Y + Z |

SDL deliberately flips axes when converting the raw classic Joy-Con sensor frame into its application coordinate frame.

The default sign mask is selected for that compatibility path.

Like `jc1_axis_map`, this parameter is also exposed for applications using another coordinate convention or for users who prefer a different physical controller orientation.

Example:

```sh
echo 2 | sudo tee /sys/module/hid_switch2/parameters/jc1_axis_sign
```

This parameter affects only the synthetic BLE Joy-Con 1 compatibility frontend.

It does not alter the native Joy-Con 2 frontend or native USB input.

### `jc1_right_sdl_frame`

Default:

```text
1
```

Values:

```text
0 = disabled
1 = enabled
```

This parameter applies only to the synthetic **right** classic Joy-Con frontend.

SDL's classic `JoyConRight` implementation performs an additional side-specific sensor-frame transformation for a physical right Joy-Con.

The Joy-Con 2 decoder already produces motion in the common internal frame, so when `jc1_right_sdl_frame=1` the synthetic right Joy-Con is presented in the raw classic-Joy-Con frame that SDL expects. SDL's normal right-Joy-Con transformation then produces the intended application-space orientation.

Disable it if the synthetic Joy-Con is being consumed by software that expects a different raw frame:

```sh
echo 0 | sudo tee /sys/module/hid_switch2/parameters/jc1_right_sdl_frame
```

Enable it again with:

```sh
echo 1 | sudo tee /sys/module/hid_switch2/parameters/jc1_right_sdl_frame
```

This parameter affects only the synthetic BLE right Joy-Con compatibility frontend. It does not affect native Joy-Con 2 or USB input.

### Persistent options

To make parameter choices persistent, create for example:

```text
/etc/modprobe.d/switch2.conf
```

containing:

```text
options hid_switch2 jc1_compat=1 jc1_axis_map=2 jc1_axis_sign=2 jc1_right_sdl_frame=1
```

## Diagnostics

Loaded modules:

```sh
lsmod | grep -E 'hid_switch2|switch2_usb'
```

Driver messages:

```sh
sudo journalctl -k | grep -i switch2
```

Input devices:

```sh
grep -A8 -B2 -i 'Joy-Con\|Switch 2' /proc/bus/input/devices
```

BLE transport endpoint:

```sh
ls -l /dev/switch2-gatt
```

## Protocol documentation

[`PROTOCOL.md`](PROTOCOL.md) contains the technical information collected while reverse-engineering Joy-Con 2 support, including:

- USB and BLE controller identities;
- BLE advertisement format;
- GATT services and characteristics;
- controller initialization;
- native side reports `0x07` and `0x08`;
- common report `0x05`;
- buttons and sticks;
- motion sensor formats;
- calibration;
- report and sensor timing;
- BLE connection parameters;
- rumble;
- known protocol behavior and remaining unknown fields.

The README is intended for installation and normal use; protocol-level details belong in `PROTOCOL.md`.

## License

GPL-2.0-or-later.

## Support

This project is provided as-is. It is not actively developed or supported, and bug reports or feature requests are not currently accepted.
