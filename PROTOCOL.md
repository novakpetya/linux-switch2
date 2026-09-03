# Nintendo Switch 2 controller protocol reference

This document collects the protocol information established while developing `linux-switch2` and `bluez-switch2`, with a primary focus on the Nintendo Joy-Con 2 Left and Right controllers.

It documents observed and reverse-engineered behavior rather than an official Nintendo specification.

The following terminology is used throughout:

- **Established** — directly validated against captured hardware behavior, generally over many packets or repeated experiments.
- **Observed** — seen on hardware, but not necessarily proven universal across firmware revisions or controller types.
- **Cross-checked** — independently consistent with other public Switch 2 controller reverse engineering.
- **Opaque** — position or structure is known, but its semantic meaning is not.
- **Host policy** — behavior implemented by `linux-switch2`; it is not itself part of the controller wire protocol.

Unless otherwise stated:

- byte offsets are zero-based;
- bit offsets are zero-based and LSB-first;
- multi-byte integers are little-endian;
- BLE GATT report values do **not** include a HID report ID;
- HID/USB report offsets **do** include the report ID.

---

# 1. Controller identities

Nintendo USB vendor ID:

```text
057e
```

Known Switch 2 controller product IDs:

| Controller | USB VID:PID | Side/native report | Common report | Output report |
| --- | --- | --- | --- | --- |
| Joy-Con 2 Left | `057e:2067` | `0x07` | `0x05` | `0x01` |
| Joy-Con 2 Right | `057e:2066` | `0x08` | `0x05` | `0x01` |
| Switch 2 Pro Controller | `057e:2069` | `0x09` | `0x05` | `0x02` |
| NSO GameCube Controller | `057e:2073` | `0x0a` | `0x05` | `0x03` |

Controller type values returned in firmware information:

```text
0 = Joy-Con 2 Left
1 = Joy-Con 2 Right
2 = Switch 2 Pro Controller
3 = NSO GameCube Controller
```

The Joy-Con 2 side reports `0x07` and `0x08` remain the native side-specific formats. Report `0x05` is a second, common report format that can be selected and is shared by all Switch 2 controller types.

---

# 2. BLE advertisement identity

Joy-Con 2 controllers advertise Nintendo manufacturer data using company ID:

```text
0x0553
```

The manufacturer-specific payload used by `bluez-switch2` contains:

```text
offset 0..2 : 01 00 03
offset 3..4 : Nintendo vendor ID, little-endian = 7e 05
offset 5..6 : product ID, little-endian
```

Therefore:

```text
Joy-Con 2 Left:
01 00 03 7e 05 67 20

Joy-Con 2 Right:
01 00 03 7e 05 66 20
```

This is sufficient to identify a Joy-Con 2 during Bluetooth discovery before a GATT session has been established.

Normal Bluetooth SMP pairing is not required for PC communication. `bluez-switch2` deliberately uses discovery and auto-connection without pairing.

The controllers also implement Nintendo's own pairing/key-exchange commands, but these are not required by the current Linux implementation.

---

# 3. BLE connection parameters

The known-good connection parameters are:

```text
minimum interval : 6
maximum interval : 6
latency          : 0
supervision timeout : 42
```

BLE interval units are 1.25 ms, therefore:

```text
6 × 1.25 ms = 7.5 ms
```

The supervision timeout is:

```text
42 × 10 ms = 420 ms
```

Thus the working configuration is:

```text
interval = 7.5 ms fixed
latency  = 0
timeout  = 420 ms
```

Incorrect/default connection parameters were experimentally shown to cause severely reduced effective motion delivery, in some cases producing multi-second apparent motion intervals.

`bluez-switch2` installs these parameters in BlueZ's connection-parameter cache before auto-connect.

---

# 4. BLE GATT database

## 4.1 Bootstrap service

Primary service:

```text
UUID    00c5af5d-1964-4e30-8f51-1956f96bd280
handles 0x0001..0x0007
```

Known characteristics:

| Handle | UUID | Properties | Meaning |
| ---: | --- | --- | --- |
| `0x0003` | `00c5af5d-1964-4e30-8f51-1956f96bd281` | READ | unknown |
| `0x0005` | `00c5af5d-1964-4e30-8f51-1956f96bd282` | WRITE | unknown |
| `0x0007` | `00c5af5d-1964-4e30-8f51-1956f96bd283` | READ | unknown |

The current Linux transport does not need to assign semantics to these attributes.

## 4.2 Nintendo controller service

Primary service:

```text
UUID    ab7de9be-89fe-49ad-828f-118f09df7fd0
handles 0x0008..0x002a
```

### Common input report

```text
declaration  0x0009
value        0x000a
CCC          0x000b
rate desc.   0x000c
UUID         ab7de9be-89fe-49ad-828f-118f09df7fd2
```

This carries input report `0x05` without the report-ID byte.

### Side/native input report

Value handle:

```text
0x000e
```

CCC:

```text
0x000f
```

Report-rate descriptor:

```text
0x0010
```

Left UUID:

```text
cc1bbbb5-7354-4d32-a716-a81cb241a32a
```

Right UUID:

```text
d5a9e01e-2ffc-4cca-b20c-8b67142bf442
```

These carry native side report `0x07` or `0x08` without the HID report-ID byte.

Other known controller-specific UUIDs for this handle are:

```text
Pro Controller:
7492866c-ec3e-4619-8258-32755ffcc0f8

GameCube:
8261cba1-9435-420c-84d6-f0c75a2c8e4d
```

### Vibration output

Value handle:

```text
0x0012
```

Left UUID:

```text
289326cb-a471-485d-a8f4-240c14f18241
```

Right UUID:

```text
fa19b0fb-cd1f-46a7-84a1-bbb09e00c149
```

Property:

```text
WRITE WITHOUT RESPONSE
```

### Basic command output

```text
handle 0x0014
UUID   649d4ac9-8eb7-4e6c-af44-1ea54fe5f005
```

The payload is:

```text
8-byte command header
variable command payload
```

### Vibration + command output

Handle:

```text
0x0016
```

Left:

```text
ce49a830-dced-48ae-931e-c8cf88aadbea
```

Right:

```text
65a724b3-f1e7-4a61-8078-a342376b27ff
```

Joy-Con format:

```text
offset 0x00 : Bluetooth report ID, 0x00
offset 0x01 : 16 bytes HD-rumble data
offset 0x11 : 8-byte command header
offset 0x19 : command payload
```

The current Linux implementation uses the simpler separate command and vibration characteristics rather than requiring this combined form.

### Large / firmware-update command

```text
handle 0x0018
UUID   4147423d-fdae-4df7-a4f7-d23e5df59f8d
```

Known framing:

```text
byte 0 : chunk type
         0x01 = beginning
         0x02 = continuation
byte 1 : chunk number
byte 2 : chunk size
byte 3 : 0x00 / unused
byte 4.. : chunk data
```

This is used for commands larger than a normal GATT transaction, particularly firmware updates.

### Basic command reply

```text
handle 0x001a
CCC    0x001b
UUID   c765a961-d9d8-4d36-a20a-5315b111836a
```

Payload:

```text
8-byte command response header
variable response data
```

This is the response path used by `bluez-switch2`.

### Extended command reply

Handle:

```text
0x001e
```

Left UUID:

```text
63a3810f-aec7-474b-9010-3d52403cb996
```

Right UUID:

```text
640ca58e-0e88-410c-a7f3-426faf2b690b
```

CCC:

```text
0x001f
```

Observed structure:

```text
leading zero/unknown region
command header
command response data
```

The current Linux transport does not require this response path.

### Unknown attributes

The following attributes are structurally known but semantically unresolved:

| Handle | UUID | Properties | Status |
| ---: | --- | --- | --- |
| `0x001c` | `b746df8c-f358-495b-9cd2-e3bbeda4f979` | descriptor | unknown |
| `0x0020` | same | descriptor | unknown |
| `0x0022` | `d3bd69d2-841c-4241-ab15-f86f406d2a80` | NOTIFY | unknown input |
| `0x0024` | `b746df8c-f358-495b-9cd2-e3bbeda4f979` | descriptor | unknown |
| `0x0026` | `ab7de9be-89fe-49ad-828f-118f09df7fde` | READ/NOTIFY | unknown input |
| `0x0027` | `0x2902` | CCC | for `0x0026` |
| `0x0028` | `679d5510-5a24-4dee-9557-95df80486ecb` | descriptor | report rate? |
| `0x002a` | `ab7de9be-89fe-49ad-828f-118f09df7fdf` | WRITE-NR | unknown output |

The UUID:

```text
679d5510-5a24-4dee-9557-95df80486ecb
```

appears repeatedly next to input characteristics and is strongly suspected to configure report rate, but its exact value encoding remains unresolved.

## 4.3 Standard services

Joy-Con 2 also exposes:

```text
Generic Access  0x1800
Generic Attribute 0x1801
```

The controller's unusual GATT implementation is important: normal primary discovery succeeds, but secondary-service discovery has been observed to stall. `bluez-switch2` therefore seeds the known fixed attribute layout before constructing the normal BlueZ GATT client.

---

# 5. BLE/HID framing

A side GATT notification contains:

```text
63 bytes
```

It does **not** contain `0x07` or `0x08`.

For the Linux HID-facing representation:

```text
Left:
byte 0     = 0x07
bytes 1..63 = GATT bytes 0..62

Right:
byte 0     = 0x08
bytes 1..63 = GATT bytes 0..62
```

Likewise, common GATT input contains 63 bytes:

```text
HID byte 0 = 0x05
HID bytes 1..63 = common GATT bytes 0..62
```

This distinction is important when comparing offsets from Bluetooth captures with physical USB captures.

---

# 6. Command protocol

## 6.1 Command header

Commands use an 8-byte header:

| Offset | Size | Field |
| ---: | ---: | --- |
| `0` | 1 | command |
| `1` | 1 | direction/status |
| `2` | 1 | transport |
| `3` | 1 | subcommand |
| `4` | 1 | unknown/status |
| `5` | 1 | payload length or response status depending on direction |
| `6` | 2 | unknown/reserved |

Linux host requests use:

```text
byte 1 = 0x91
```

which is:

```text
0x90 direction-out
+
0x01 OK/valid flag
```

Transport values:

```text
0x00 USB
0x01 Bluetooth
```

The current Linux command builder sends:

```text
byte 4 = 0x00
bytes 6..7 = 00 00
```

unless a raw protocol packet is deliberately used.

Typical BLE request:

```text
03 91 01 0a 00 04 00 00 ...
```

Typical reply header begins:

```text
03 01 01 0a ...
```

Frequently observed successful response/status bytes include:

```text
BLE: 0x78
USB: 0xf8
```

Their bit-level semantics have not been fully established and the current driver does not depend on decoding them.

Maximum normal command payload in the current implementation:

```text
56 bytes
```

## 6.2 Known command IDs

| ID | Meaning |
| ---: | --- |
| `0x01` | NFC |
| `0x02` | flash memory |
| `0x03` | initialization |
| `0x04` | unknown |
| `0x05` | unknown |
| `0x06` | unknown |
| `0x07` | BLE setup / exact semantic unknown |
| `0x08` | charging grip / grip controls |
| `0x09` | player LEDs |
| `0x0a` | vibration |
| `0x0b` | battery |
| `0x0c` | feature select |
| `0x0d` | firmware update |
| `0x0e` | unknown |
| `0x0f` | unknown |
| `0x10` | firmware information |
| `0x11` | BLE/motion setup, exact semantic unknown |
| `0x12` | unknown |
| `0x13` | unknown |
| `0x14` | unknown |
| `0x15` | Bluetooth pairing |
| `0x16` | BLE setup / exact semantic unknown |
| `0x17` | unknown |
| `0x18` | unknown |

---

# 7. Feature flags

Command `0x0c` controls controller feature selection.

Known feature-mask bits:

| Bit | Mask | Feature |
| ---: | ---: | --- |
| 0 | `0x01` | buttons |
| 1 | `0x02` | analog sticks |
| 2 | `0x04` | IMU |
| 3 | `0x08` | unused / unknown |
| 4 | `0x10` | optical mouse, Joy-Con only |
| 5 | `0x20` | rumble; also enables a battery-current field in common `0x05` |
| 6 | `0x40` | unused / unknown |
| 7 | `0x80` | magnetometer |

The ordinary `linux-switch2` feature mask is:

```text
buttons + sticks + IMU + rumble
0x01 + 0x02 + 0x04 + 0x20
= 0x27
```

BLE Joy-Con 2 additionally enables the optical sensor:

```text
0x27 + 0x10 = 0x37
```

The measured BLE startup procedure temporarily uses:

```text
0x94
```

which corresponds to:

```text
IMU          0x04
optical      0x10
magnetometer 0x80
```

before the normal controller feature set is selected.

Feature operations used by the driver:

```text
command 0x0c subcommand 0x02 = set/initialise mask
command 0x0c subcommand 0x04 = enable/confirm mask
```

---

# 8. Current working BLE initialization sequence

The following is the working sequence implemented by `linux-switch2`.

It should be treated as an observed working initialization profile, not as proof that every apparently opaque command is strictly required.

## 8.1 Initial BLE preamble

### 1. Initial setup

```text
command    0x03
subcommand 0x0d
payload:
01 00 ff ff ff ff ff ff
```

The final six bytes correspond to the position normally occupied by a host Bluetooth address. A valid console address is unnecessary for the PC USB/BLE path.

### 2. Setup command

```text
command    0x07
subcommand 0x01
payload    none
```

Exact meaning unknown.

### 3. Setup command

```text
command    0x16
subcommand 0x01
payload    none
```

Exact meaning unknown.

### 4. BLE finalize/pair-related command

```text
command    0x15
subcommand 0x03
payload:
00
```

The normal Nintendo protocol associates command `0x15` with its proprietary Bluetooth pairing process. The Linux path does not perform standard controller pairing.

### 5. Initial feature mask

```text
command    0x0c
subcommand 0x02
payload:
94 00 00 00
```

### 6. BLE motion setup

```text
command    0x11
subcommand 0x03
payload    none
```

Exact command semantics remain unknown.

### 7. Extended vibration/setup packet

```text
command    0x0a
subcommand 0x08
payload:
01 ff ff ff ff ff ff ff ff
35 00 46 00
00 00 00 00 00 00 00 00
```

This is a 21-byte setup payload. Some bytes clearly belong to vibration setup, but its complete semantic decomposition is not established.

### 8. Enable initial features

```text
command    0x0c
subcommand 0x04
payload:
94 00 00 00
```

### 9. Select/start BLE input mode

```text
command    0x03
subcommand 0x0a
payload:
09 00 00 00
```

Public protocol work describes this subcommand as selecting an input report. The known-good BLE startup sequence uses value `0x09` even for Joy-Con 2, so this value should not be blindly interpreted as a literal Joy-Con HID report ID in this context.

After its acknowledgement, initialization deliberately waits until a real native side notification arrives.

### 10. First-live-report synchronization

The driver does not continue merely because the preceding command was acknowledged.

It waits for:

```text
one genuine side input notification
```

then waits another:

```text
10 ms
```

before continuing.

This proved important for stable BLE initialization.

## 8.2 Post-input setup

### Firmware information

```text
10 91 01 01 ...
```

### NFC setup

```text
command 0x01 subcommand 0x0c
```

followed by:

```text
command 0x01 subcommand 0x01
payload:
00 00 00 00
```

### LED setup

```text
command 0x09
subcommand 0x07
payload:
01 00 00 00 00 00 00 00
```

### BLE calibration reads

Gyroscope:

```text
address 0x13040
size    0x10
```

Accelerometer:

```text
address 0x1310c
size    0x0c
```

### Serial/controller information

```text
address 0x13002
size    0x10
```

## 8.3 Shared calibration/setup sequence

The driver then reads applicable:

- factory primary stick calibration;
- factory secondary stick calibration;
- factory GameCube trigger calibration;
- user primary stick calibration;
- user secondary stick calibration.

Joy-Con 2 has only its one physical stick, so secondary-stick reads are skipped for Joy-Con.

## 8.4 Raw BLE selector

After the serial/calibration transactions, the known-good sequence sends this raw GATT control payload:

```text
01 00 00 00 00 00 00 00 00 03 30
```

This is **not** wrapped in the normal eight-byte command header.

No protocol reply is expected.

The driver inserts a:

```text
10 ms
```

delay after it.

The detailed semantics of this selector remain unknown.

## 8.5 Final feature setup

Joy-Con BLE:

```text
feature mask 0x37
```

sent with:

```text
0x0c / 0x02
0x0c / 0x04
```

Firmware information is then queried again.

Force-feedback-enabled builds perform the additional working rumble/NFC setup command, grip-button support is enabled, and the player LED is assigned.

## 8.6 BLE command cadence

For acknowledged BLE startup commands, the known-good driver intentionally leaves approximately:

```text
10 ms
```

between the reply and the next command.

This is a host-side timing requirement established experimentally.

---

# 9. Flash memory reads

Flash read command:

```text
command    0x02
subcommand 0x04
```

Request payload:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| `0` | 1 | requested length |
| `1` | 1 | `0x7e` |
| `2..3` | 2 | zero / unknown |
| `4..7` | 4 | flash address, LE32 |

Example:

```text
10 7e 00 00 40 30 01 00
```

means:

```text
read 0x10 bytes from 0x13040
```

Observed/documented transport limits are approximately:

```text
USB max read: 0x50
BLE max read: 0x4f
```

Flash response payload:

```text
byte 0       read length
bytes 1..3   metadata/unknown, commonly zero
bytes 4..7   returned address LE32
bytes 8..    flash data
```

---

# 10. Known flash locations

| Address | Size | Meaning |
| ---: | ---: | --- |
| `0x13002` | `0x10` | serial/controller information |
| `0x13040` | `0x10` | BLE gyro calibration |
| `0x130a8` | `9` | factory primary stick calibration |
| `0x130e8` | `9` | factory secondary stick calibration |
| `0x1310c` | `0x0c` | BLE accelerometer calibration |
| `0x13140` | `2` | factory GameCube trigger calibration |
| `0x1fc040` | `11` | user primary stick calibration |
| `0x1fc080` | `11` | user secondary stick calibration |

User stick-calibration magic:

```text
0xa1b2 little-endian
```

---

# 11. Stick calibration encoding

Factory stick calibration uses 9 bytes.

For each two-dimensional quantity:

```text
X = byte0 | ((byte1 & 0x0f) << 8)
Y = (byte1 >> 4) | (byte2 << 4)
```

Layout:

```text
bytes 0..2 : neutral X/Y
bytes 3..5 : positive X/Y range
bytes 6..8 : negative X/Y range
```

Thus each coordinate is a packed 12-bit value.

An all-`0xff` 9-byte block means calibration is absent.

User calibration is:

```text
bytes 0..1 : magic 0xa1b2
bytes 2..10: same 9-byte calibration structure
```

---

# 12. BLE motion calibration

## 12.1 Gyroscope block

Address:

```text
0x13040
```

Size:

```text
16 bytes
```

Current established structure:

```text
bytes 0..3   unknown / currently unused
bytes 4..7   IEEE-754 float32 LE, axis X
bytes 8..11  IEEE-754 float32 LE, axis Y
bytes 12..15 IEEE-754 float32 LE, axis Z
```

The driver verifies that all three floats are finite.

It converts each float into the raw common-report gyro domain using:

```text
zero_raw = round(float × 32767 / 40)
```

This strongly indicates that these values are gyro zero/bias values expressed in the same physical unit underlying the common report's:

```text
40 rad/s full-scale
```

conversion.

The first four bytes remain opaque.

## 12.2 Accelerometer block

Address:

```text
0x1310c
```

Size:

```text
12 bytes
```

Structure:

```text
three little-endian float32 values
```

All three have been validated as finite calibration values.

Their exact physical interpretation — offset, scale, or another calibration quantity — has not yet been established sufficiently to apply them in the driver.

---

# 13. Native side reports `0x07` and `0x08`

The native BLE GATT payload is 63 bytes.

Report ID is supplied externally by the transport:

```text
Left  = 0x07
Right = 0x08
```

The following offsets refer to the 63-byte native payload, not the added report ID.

## 13.1 Shared prefix

| Byte(s) | Meaning | Status |
| --- | --- | --- |
| `0` | rolling 8-bit report counter | established |
| `1` | power information | established |
| `2` | primary button bank | established |
| `3` | secondary button bank | established |
| `4` | status/unknown, commonly `0x07` | opaque |
| `5..7` | active stick, packed 12-bit X/Y | established |
| `8` | status/unknown, values including `0x30` / `0x38` | opaque |
| `9..10` | optical relative X, signed LE16 | established |
| `11..12` | optical relative Y, signed LE16 | established |
| `13` | optical coveredness/lift-off-like value | established structurally; exact physical meaning unresolved |

## 13.2 Side report counter

Byte:

```text
native[0]
```

is an 8-bit rolling counter.

It advances through native report traffic and wraps modulo 256.

Important:

It is a transport/report counter, not a direct motion sample timestamp.

It can also advance across compact or other interposed report formats, so:

```text
counter +1
```

must not be used as the sole criterion for continuity between two ordinary motion packets.

---

# 14. Native Power Info byte

Native:

```text
byte 1
```

Bit layout:

| Bits | Meaning |
| --- | --- |
| 0 | external power |
| 1 | charging |
| 2..5 | battery level `0..9` |
| 6..7 | reserved |

Battery level extraction:

```text
level = (power_info >> 2) & 0x0f
```

## 14.1 Battery evidence from our capture corpus

Across 33 archived left-controller capture runs and 41,352 native ordinary/compact packets, the field followed the long-term discharge progression:

```text
0x20 -> native level 8
0x1c -> native level 7
0x18 -> native level 6
0x14 -> native level 5
```

Two recordings contained a level transition inside one uninterrupted stream.

Example:

```text
0x1c -> 0x18
level 7 -> 6
```

while the normal packet counter and motion time continued without reconnect or reinitialization.

This rules out session state, report mode, or connection state as the primary meaning of these bits.

No packet in the archived corpus had:

```text
external-power bit = 1
charging bit       = 1
```

so those two bits are cross-checked with independent research rather than proven by our own charging capture.

## 14.2 Fields rejected as battery indicators

The following were explicitly investigated and rejected as battery level:

```text
native byte 4
native byte 8
motion singleton bits 262, 278, 294, 349, 365, 381
motion bit 156
compact slow_u10
compact U0
compact U1
```

Reasons include constancy, rapid roughly 50/50 activity, high entropy, or session-local reset behavior inconsistent with battery discharge.

---

# 15. Native Joy-Con 2 Left buttons

Native byte `2`:

| Bit | Button |
| ---: | --- |
| 0 | Down |
| 1 | Right |
| 2 | Left |
| 3 | Up |
| 4 | L |
| 5 | ZL |
| 6 | Minus |
| 7 | Left stick click |

Native byte `3`:

| Bit | Button |
| ---: | --- |
| 0 | Capture / O |
| 1 | reserved / unknown |
| 2 | GL |
| 3 | reserved / unknown |
| 4 | reserved / unknown |
| 5 | reserved / unknown |
| 6 | SR |
| 7 | SL |

---

# 16. Native Joy-Con 2 Right buttons

Native byte `2`:

| Bit | Button |
| ---: | --- |
| 0 | B |
| 1 | A |
| 2 | Y |
| 3 | X |
| 4 | R |
| 5 | ZR |
| 6 | Plus |
| 7 | Right stick click |

Native byte `3`:

| Bit | Button |
| ---: | --- |
| 0 | Home |
| 1 | reserved / unknown |
| 2 | GR |
| 3 | reserved / unknown |
| 4 | C |
| 5 | reserved / unknown |
| 6 | SR |
| 7 | SL |

---

# 17. Native stick encoding

Native side report bytes:

```text
5..7
```

contain the active Joy-Con stick.

Packing:

```text
X = b0 | ((b1 & 0x0f) << 8)
Y = (b1 >> 4) | (b2 << 4)
```

Both are unsigned 12-bit values:

```text
0..4095
```

Typical neutral is around:

```text
~2048
```

but actual calibrated center must be taken from the controller calibration block.

---

# 18. Optical / mouse sensor

Joy-Con 2 contains a surface-tracking optical sensor.

It is enabled through feature bit:

```text
0x10
```

## 18.1 Native side-report optical block

Native bytes:

```text
9..13
```

Layout:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| `9` | 2 | relative X, signed LE16 |
| `11` | 2 | relative Y, signed LE16 |
| `13` | 1 | coveredness / lift-off / surface-quality-like value |

The first four bytes are true relative deltas.

The final byte is not motion. It strongly tracks whether the optical aperture is exposed to a usable surface.

Observed behavior:

```text
uncovered / lifted: normally close to 0xff
surface contact:    significantly lower
```

The exact physical quantity and scale remain unknown.

It should not yet be named definitively as distance or image quality.

## 18.2 Current driver surface heuristic

This is **host policy**, not a wire-protocol constant.

`linux-switch2` currently considers surface entry after two consecutive reports satisfying approximately:

```text
coveredness <= 0xf0
```

and exits after three reports with:

```text
coveredness >= 0xfa
```

Common-report optical status supplies an additional gate.

These thresholds were derived empirically from actual desk/fabric/skin tests and should not be interpreted as Nintendo-defined values.

---

# 19. Left/right motion-header asymmetry

This deserves explicit documentation because it was one of the more confusing parts of the reverse engineering.

Our actual BLE captures show:

```text
Left:
... optical-coveredness  0x28 ...

Right:
... optical-coveredness  0x00  0x28 ...
```

For ordinary motion.

Likewise, the compact value can appear as:

```text
0x1e
```

instead of `0x28`.

The important correction is:

```text
0x28 = 40 decimal
0x1e = 30 decimal
```

These values are best understood as **motion-data lengths**, not arbitrary format-selector magic values.

### Our observed left framing

```text
native[13] = optical status
native[14] = motion length
native[15] = motion payload start
```

### Our observed right framing

```text
native[13] = optical status
native[14] = 0x00
native[15] = motion length
native[16] = motion payload start
```

The extra right-side `0x00` is strongly consistent with an NFC-state byte:

```text
0x00 = NFC idle
```

because the Right Joy-Con contains the NFC reader.

Current public protocol tables place an NFC-state byte at this position on **both** Joy-Cons, with the Left value always zero, followed by motion length one byte later.

That does not match the actual Left BLE stream used throughout this project, where `0x28`/`0x1e` directly occupies native byte 14.

Therefore the current status is:

- Right byte 14 being NFC state is strongly supported.
- Left-side NFC-byte presence is **not universal in our observed BLE stream**.
- This may be firmware-dependent, transport-dependent, or an outstanding discrepancy in existing public report maps.
- Implementations must not blindly assume identical left/right prefix length.

`linux-switch2` consequently removes the extra right `0x00` only for motion decoding and leaves the original controls packet unchanged.

---

# 20. Ordinary native motion payload — 40 bytes

The ordinary motion form has length:

```text
0x28 = 40 bytes
```

For the established decoder, right reports are first normalized to the same canonical byte positions as the observed left format.

Bit offsets below refer to this normalized 63-byte native representation.

## 20.1 Complete established map

| Bits | Field | Encoding |
| ---: | --- | --- |
| `120..131` | sample time | unsigned 12-bit |
| `132..139` | delta sample time | unsigned 8-bit |
| `140..147` | fixed prefix | `0xf0` |
| `148..152` | stable fixed pattern | opaque |
| `153` | rare status bit | opaque |
| `154..155` | quaternion omitted-component selector | unsigned 2-bit |
| `156` | structural zero | `0` |
| `157..177` | quaternion component 0 | unsigned 21-bit |
| `178..198` | quaternion component 1 | unsigned 21-bit |
| `199..219` | quaternion component 2 | unsigned 21-bit |
| `220..233` | A0.X | signed 14-bit |
| `234..247` | A0.Y | signed 14-bit |
| `248..261` | A0.Z | signed 14-bit |
| `262` | singleton side-channel bit | unknown |
| `263..277` | G2.X | signed 15-bit |
| `278` | singleton side-channel bit | unknown |
| `279..293` | G2.Y | signed 15-bit |
| `294` | singleton side-channel bit | unknown |
| `295..309` | G2.Z | signed 15-bit |
| `310..322` | A1.X | signed 13-bit × 2 |
| `323..335` | A1.Y | signed 13-bit × 2 |
| `336..348` | A1.Z | signed 13-bit × 2 |
| `349` | singleton side-channel bit | unknown |
| `350..364` | G3.X | signed 15-bit |
| `365` | singleton side-channel bit | unknown |
| `366..380` | G3.Y | signed 15-bit |
| `381` | singleton side-channel bit | unknown |
| `382..396` | G3.Z | signed 15-bit |
| `397..410` | A2.X | signed 14-bit |
| `411..424` | A2.Y | signed 14-bit |
| `425..438` | A2.Z | signed 14-bit |
| `439..503` | padding/reserved | observed zero |

Chronology:

```text
accelerometer:
A0 oldest
A1 middle
A2 newest

gyroscope:
G2 older
G3 newer
```

The strange singleton bits:

```text
262
278
294
349
365
381
```

are **not** missing sign or least-significant bits of the neighboring gyro values.

They were tested independently and behave approximately like one-bit controller-local side channels.

They must remain separate until their meaning is established.

Bit:

```text
156
```

is a structural zero in the validated ordinary corpus.

Bit:

```text
153
```

is normally set but was observed to drop rarely. It is not required to decode motion and remains semantically unknown.

The tail:

```text
439..503
```

was zero in every ordinary packet in the motion-closure validation corpora.

---

# 21. Ordinary motion scheduler time

`sample_time12`:

```text
bits 120..131
```

is a 12-bit modulo-4096 scheduler/sample time.

`delta_sample_time8`:

```text
bits 132..139
```

was proven to obey:

```text
delta_sample_time8
=
(current_sample_time12 - previous_sample_time12) mod 4096
```

for contiguous ordinary packets.

This equality held for:

```text
93,717 / 93,717
```

discovery-corpus transitions and:

```text
10,817 / 10,817
```

frozen archival transitions.

Typical deltas:

```text
46
47
```

native ticks.

This corresponds to an ordinary packet period of approximately:

```text
48.75 ms
```

in the nominal ~960 Hz clock family.

A practical continuity window established during analysis was approximately:

```text
40..55 ticks
```

A missing ordinary packet usually produces roughly double the normal delta and should not be bridged as if continuous.

The outer 8-bit report counter should not replace this timing check because compact/other reports can advance it between ordinary packets.

---

# 22. Accelerometer encoding

Physical scale:

```text
~4096 counts/g
```

Native axes:

```text
X Y Z
signs + + +
```

## A0

Three signed 14-bit values:

```text
bits 220..261
```

## A1

Three signed 13-bit values:

```text
310..348
```

After sign extension:

```text
A1 = raw_s13 × 2
```

This gives the same physical range as the 14-bit samples but with two-count resolution.

The isolated bit immediately following A1:

```text
349
```

must not be treated as its missing LSB.

## A2

Three signed 14-bit values:

```text
397..438
```

---

# 23. Gyroscope encoding

Native ordinary side report contains two explicit gyro vectors per ordinary packet:

```text
G2
G3
```

Each axis is signed 15-bit.

Physical range:

```text
approximately ±500 degrees/second
```

Conversion:

```text
rad/s per native count
=
radians(500) / 16384
≈ 0.000532632218
```

Inverse:

```text
native counts per rad/s
≈ 1877.468103
```

Native axis order:

```text
X Y Z
```

Native sign convention established by physical testing:

```text
+ + +
```

No interpolation or prediction is required to decode these values; they are explicit controller-produced angular-rate states.

---

# 24. Quaternion representation

Ordinary native motion carries three 21-bit components plus a two-bit selector.

Encoding is a smallest-three style affine representation.

For each stored 21-bit value:

```text
c =
(2 × raw / (2^21 - 1) - 1) / sqrt(2)
```

Three components are stored and one component is reconstructed.

Reconstructed component:

```text
q_missing =
sqrt(max(0, 1 - q0² - q1² - q2²))
```

The omitted component is taken positive in the encoded gauge.

Selector mapping:

```text
selector 0:
stored q0 q1 q2
omit   q3

selector 1:
stored q1 q2 q3
omit   q0

selector 2:
stored q2 q3 q0
omit   q1

selector 3:
stored q3 q0 q1
omit   q2
```

A useful body-space naming established during the motion work is:

```text
[w, x, y, z] = [q3, q0, q1, q2]
```

Quaternion sign is globally redundant:

```text
q
and
-q
```

represent the same orientation.

For continuity analysis it is therefore valid to flip the entire reconstructed quaternion when its dot product with the previous quaternion is negative.

Body-space incremental rotation:

```text
dq = conjugate(q_previous) × q_current
```

The resulting rotational signs match the established native gyro XYZ axes.

---

# 25. Native intra-packet motion timing

The accelerometer samples are **not uniformly spaced**.

The established topology is long-short-short:

```text
A0 -> A1 ≈ 22.75 ms
A1 -> A2 ≈ 13 ms
A2 -> next A0 ≈ 13 ms
```

Total:

```text
≈48.75 ms
```

This produces an average accelerometer sample rate of approximately:

```text
61.5 Hz
```

but treating it as a uniform 16.25 ms cadence is incorrect.

A simple internal-divider model close to:

```text
7 : 4 : 4
```

fits the measured topology well, although the exact hardware divider has not been proven.

Approximate measured phases relative to G2:

```text
A0           -27.802 ms
A1            -5.052 ms
G2             0
quaternion    +3.656 ms
A2            +7.948 ms
next A0      +20.948 ms
G3           +24.375 ms
report/time  +26.961 ms
```

The exact decimal values are empirical fit results.

The ordering/topology is more strongly established than every tenth of a millisecond.

Important consequence:

`sample_time12` is a scheduler/report timing reference. It is **not** the physical timestamp of every sensor sample inside the packet.

---

# 26. Compact native motion payload — 30 bytes

Compact motion length:

```text
0x1e = 30 bytes
```

Compact packets appear sparsely in the BLE side stream and much more regularly in physical USB side-report operation.

The exact reason/scheduler for their appearance over BLE remains unresolved.

Offsets below are relative to the start of the 30-byte compact motion payload.

| Bits | Field | Encoding |
| ---: | --- | --- |
| `0..11` | sample_time12 | u12 |
| `12..15` | delta sample time | u4 |
| `16..31` | constant | `0x0c00` |
| `32..33` | quaternion selector | u2 |
| `34` | zero | structural |
| `35..40` | zero | structural |
| `41..65` | quaternion component 0 | u25 |
| `66..71` | zero | structural |
| `72..96` | quaternion component 1 | u25 |
| `97..102` | zero | structural |
| `103..127` | quaternion component 2 | u25 |
| `128..143` | `mid16` | opaque/high entropy |
| `144..159` | acceleration X | s16 |
| `160..161` | slot/status low bits #0 | opaque |
| `162..175` | U0 | raw14, opaque |
| `176..191` | acceleration Y | s16 |
| `192..193` | slot/status low bits #1 | opaque |
| `194..207` | U1 | raw14, opaque |
| `208..223` | acceleration Z | s16 |
| `224..233` | `slow_u10` | u10, opaque |
| `234..239` | zero | structural |

Although compact acceleration occupies signed-16 slots, observed values remain within the physical signed-14 accelerometer domain.

The following fields are deliberately **not** interpreted as conventional gyro axes:

```text
mid16
U0
U1
slow_u10
```

`slow_u10` varies slowly and resembles a controller-local state or temperature-like quantity, but it resets/jumps between sessions and has not been assigned units.

It is not battery level.

---

# 27. Compact quaternion codec

BLE compact motion uses the same affine smallest-three family as ordinary motion, but with 25-bit stored components.

For each component:

```text
c =
(2 × raw / (2^25 - 1) - 1) / sqrt(2)
```

Same selector semantics as the ordinary 21-bit form.

## Physical USB compact difference

A major cross-transport finding is that the physical USB compact quaternion uses a **different numeric codec** despite occupying equivalent fields.

For USB compact:

```text
z = 2 × raw / (2^25 - 1) - 1
```

and:

```text
z_i = q_i / q_omitted
```

Decode by setting:

```text
q_omitted = +1
```

inserting the three ratios and then normalizing the resulting quaternion.

Therefore:

**BLE compact and USB compact must not use the same quaternion decoder.**

Applying the USB projective decoder to BLE compact data measurably worsened orientation continuity.

---

# 28. Physical USB side-report compact framing

Observed physical USB Left:

```text
report byte 0  = 0x07
report byte 15 = 0x1e
active compact payload bytes 16..45
```

Observed physical USB Right:

```text
report byte 0  = 0x08
report byte 15 = 0x00
report byte 16 = 0x1e
active compact payload bytes 17..46
```

Again, the Right `0x00` immediately preceding motion length is consistent with NFC-idle state.

Physical USB compact side reports run at approximately:

```text
250 Hz
```

with the outer report counter advancing by one per compact report.

---

# 29. Common input report `0x05`

The common report exists on all Switch 2 controller types.

Over USB:

```text
64-byte HID report
byte 0 = 0x05
```

Over BLE:

```text
63-byte GATT value
report ID omitted
```

The table below uses both coordinate systems.

| GATT offset | HID offset | Size | Field |
| ---: | ---: | ---: | --- |
| `0x00` | `1` | 4 | 32-bit report counter |
| `0x04` | `5` | 4 | buttons |
| `0x08` | `9` | 2 | unknown |
| `0x0a` | `11` | 3 | left stick |
| `0x0d` | `14` | 3 | right stick |
| `0x10` | `17` | 8 | optical/mouse data |
| `0x18` | `25` | 1 | unknown, observed zero |
| `0x19` | `26` | 6 | magnetometer XYZ |
| `0x1f` | `32` | 2 | battery voltage, mV |
| `0x21` | `34` | 1 | charging state/rate |
| `0x22` | `35` | 2 | battery current? |
| `0x24` | `37` | 5 | unknown, observed zero |
| `0x29` | `42` | 1 | unknown, commonly `0x01` |
| `0x2a` | `43` | 18 | motion |
| `0x3c` | `61` | 1 | GameCube left analog trigger |
| `0x3d` | `62` | 1 | GameCube right analog trigger |
| `0x3e` | `63` | 1 | reserved |

One important correction to older notes:

```text
GATT 0x00..0x03
```

is a single **32-bit report counter**.

It is not a one-byte counter followed by three independent unknown bytes.

---

# 30. Common `0x05` buttons

The four button bytes begin at:

```text
GATT offset 0x04
HID report byte 5
```

## Button byte 0

| Bit | Meaning |
| ---: | --- |
| 0 | Y |
| 1 | X |
| 2 | B |
| 3 | A |
| 4 | SR Right |
| 5 | SL Right |
| 6 | R |
| 7 | ZR |

## Button byte 1

| Bit | Meaning |
| ---: | --- |
| 0 | Minus |
| 1 | Plus |
| 2 | Right stick click |
| 3 | Left stick click |
| 4 | Home |
| 5 | Capture |
| 6 | C |
| 7 | reserved / unknown |

## Button byte 2

| Bit | Meaning |
| ---: | --- |
| 0 | Down |
| 1 | Up |
| 2 | Right |
| 3 | Left |
| 4 | SR Left |
| 5 | SL Left |
| 6 | L |
| 7 | ZL |

## Button byte 3

| Bit | Meaning |
| ---: | --- |
| 0 | GR |
| 1 | GL |
| 2 | reserved / unknown |
| 3 | reserved / unknown |
| 4 | headset-related on applicable controller |
| 5 | opaque/reserved in Joy-Con observations |
| 6 | reserved |
| 7 | reserved |

Historical USB templates sometimes contained a fixed value in otherwise opaque bits of this byte. Those constants must not be promoted into button semantics.

---

# 31. Common sticks

Left:

```text
GATT 0x0a..0x0c
HID  11..13
```

Right:

```text
GATT 0x0d..0x0f
HID  14..16
```

Both use the same packed 12-bit format:

```text
X = b0 | ((b1 & 0x0f) << 8)
Y = (b1 >> 4) | (b2 << 4)
```

Values are raw/uncalibrated and require the flash calibration described earlier.

---

# 32. Common optical block

Common report optical data:

```text
GATT 0x10..0x17
HID  17..24
```

Layout:

| Relative offset | Size | Meaning |
| ---: | ---: | --- |
| `0` | 2 | cumulative/absolute X, u16 |
| `2` | 2 | cumulative/absolute Y, u16 |
| `4` | 2 | unknown optical status word #1 |
| `6` | 2 | unknown optical status word #2 |

Unlike the side report, X/Y are cumulative 16-bit coordinates rather than direct signed deltas.

Relative motion can be reconstructed using modulo arithmetic:

```text
dx = (s16)(current_x - previous_x)
dy = (s16)(current_y - previous_y)
```

Unsigned subtraction naturally wraps modulo 65536 before narrowing to signed 16-bit.

## Status word #1

The low byte of the first unknown word tracks the one-byte side-report coveredness/lift-off field.

This correspondence was confirmed in our captures.

The high byte remains opaque.

## Status word #2

The final 16-bit optical word strongly separates normal desk/fabric operation from direct skin coverage in our measurements.

Its exact physical interpretation is unresolved.

Possible labels such as:

```text
surface quality
lift-off distance
```

should be treated as hypotheses, not established semantics.

---

# 33. Current optical host behavior

This section describes `linux-switch2`, not the wire protocol.

When the optical surface detector is active:

- shoulder/trigger buttons are repurposed as mouse buttons;
- stick click is used as middle mouse;
- the stick is repurposed for scrolling;
- those controls are suppressed from the simultaneous gamepad frontend;
- the gamepad stick is presented neutral while used for scrolling.

Mouse mapping:

### Right Joy-Con

```text
R           -> left mouse button
ZR          -> right mouse button
Right Stick -> middle mouse button
```

### Left Joy-Con

```text
L          -> left mouse button
ZL         -> right mouse button
Left Stick -> middle mouse button
```

Current scroll policy:

```text
nominal stick center : calibration value, fallback 2048
deadzone             : 512
maximum scroll rate  : 20 Hz
maximum elapsed-time contribution per update: 50 ms
```

These are Linux frontend choices, not Nintendo protocol constants.

---

# 34. Common magnetometer

Common report:

```text
GATT 0x19..0x1e
HID  26..31
```

contains three 16-bit magnetometer axes:

```text
X
Y
Z
```

It is enabled through feature bit:

```text
0x80
```

The exact physical units, scaling, bias calibration and orientation have **not** been established during this project.

The current driver does not expose/use this field.

---

# 35. Common battery fields

## Battery voltage

```text
GATT 0x1f..0x20
HID  32..33
```

Little-endian 16-bit battery voltage:

```text
millivolts
```

This same quantity can be requested with battery command:

```text
0x0b / 0x03
```

## Charging state/rate

```text
GATT 0x21
HID  34
```

Observed to change when external USB power is connected.

Values reported by public hardware investigation tend to rise during charging and settle around:

```text
0x34
```

with:

```text
0x20
```

observed at full charge.

The exact unit or encoding is not established.

## Battery current

```text
GATT 0x22..0x23
HID  35..36
```

This appears to be battery-current information and becomes available with feature bit `0x20`.

Exact signedness, unit and scaling remain unknown.

---

# 36. Common motion block

Common report motion:

```text
GATT 0x2a..0x3b
HID  43..60
```

Length:

```text
18 bytes
```

Layout:

| GATT | HID | Size | Meaning |
| ---: | ---: | ---: | --- |
| `0x2a` | `43` | 4 | timestamp |
| `0x2e` | `47` | 2 | temperature / motion metadata |
| `0x30` | `49` | 2 | accel X |
| `0x32` | `51` | 2 | accel Y |
| `0x34` | `53` | 2 | accel Z |
| `0x36` | `55` | 2 | gyro X |
| `0x38` | `57` | 2 | gyro Y |
| `0x3a` | `59` | 2 | gyro Z |

Accel and gyro are ordinary signed 16-bit little-endian values.

`linux-switch2` consumes:

```text
report[49..54] accel XYZ
report[55..60] gyro XYZ
```

directly.

The two bytes at HID `47..48` were initially treated as opaque motion metadata during our USB compatibility work. Independent report mapping identifies them as a temperature field.

Its exact temperature conversion has not been established by this project, so the raw value should be preserved if exposed.

---

# 37. Common physical IMU scales

Common-report accelerometer:

```text
~4096 counts/g
```

Common-report gyro:

```text
40 / 32767 rad/s per count
```

therefore:

```text
rad/s per count ≈ 0.00122074038
```

and:

```text
counts per rad/s ≈ 819.175
```

Physical common-report raw axes for the tested Left and Right Joy-Con 2 controllers are:

```text
X Y Z
+ + +
```

before application-specific coordinate conversions.

The BLE native 15-bit gyro and common 16-bit gyro therefore have different raw scales.

Their same-angular-rate scale ratio is approximately:

```text
819.175 / 1877.468103
≈ 0.436318997
```

---

# 38. Physical USB common mode

Both physical Joy-Con 2 sides can be switched from their native side report to common `0x05`.

Observed USB selector command:

```text
03 91 00 0a 00 04 00 00
05 00 00 00
```

Observed acknowledgement:

```text
03 01 00 0a 00 f8 00 00
```

After settling:

```text
report ID = 0x05
report size = 64 bytes
rate ≈ 250 Hz
```

In physical USB captures, the 32-bit report counter advanced by approximately:

```text
+4 per report
```

rather than the +1 per notification associated with common BLE observations.

This is a transport-specific cadence observation, not evidence for separate fields inside the counter.

---

# 39. Physical USB motion clocks

Measured physical USB timing differs slightly between the two tested Joy-Cons, indicating independent controller oscillators.

Approximate measured native sensor clocks:

```text
Left  PID 2067: ~959.75 Hz
Right PID 2066: ~958.48 Hz
```

Left common timestamp relationship:

```text
~48.75 timestamp units per native clock tick
nominal ≈ 46,800 units/s at 960 Hz
```

Right:

```text
~46.25 timestamp units per native clock tick
nominal ≈ 44,400 units/s
```

These values are physical measurements, not host-generated timestamps.

The controllers therefore appear to use independent oscillators within the same approximate 960-Hz timing family.

---

# 40. USB report-mode transition behavior

Transitioning into common `0x05` is not perfectly instantaneous.

Observed phenomena include:

- an old-ID compact packet immediately after the selector acknowledgement;
- transitional packets whose motion contents are plausible but whose prefix timing/counter state is rephased;
- on one Right transition, a zero-timestamp / zero-IMU common `0x05` placeholder.

The zero placeholder was observed but is not known to be universal.

No universal additive epoch was discovered that directly maps compact `sample_time12` into the first common `0x05` timestamp.

Implementations should therefore treat the report-mode transition as a synchronization boundary.

---

# 41. Firmware information

Command:

```text
0x10 / 0x01
```

Known response structure:

```text
byte 0  controller firmware major
byte 1  controller firmware minor
byte 2  controller firmware patch
byte 3  controller type
bytes 4..7 secondary firmware/BT-related field
byte 8  DSP major
byte 9  DSP minor
byte 10 DSP patch
byte 11 DSP type
```

Public reverse engineering identifies bytes `4..6` as Bluetooth patch-version components with byte `7` padding on some firmware.

The current Linux driver deliberately treats bytes `4..7` as one opaque 32-bit field because that interpretation is not required for operation.

The controller-type byte is checked against the expected product identity.

Observed firmware examples from the development hardware included:

```text
Left  Joy-Con 2: 1.0.14
Right Joy-Con 2: 1.0.18
```

Firmware versions should not be assumed universal.

---

# 42. Player LEDs

Player LED command:

```text
command    0x09
subcommand 0x07
```

The first payload byte is an LED bit pattern.

Current eight-player pattern table:

```text
player 0 : 0x01
player 1 : 0x03
player 2 : 0x07
player 3 : 0x0f
player 4 : 0x09
player 5 : 0x05
player 6 : 0x0d
player 7 : 0x06
```

Remaining bytes in the current eight-byte payload are zero.

---

# 43. Grip / rail buttons

Joy-Con rail/grip button support is enabled using:

```text
command    0x08
subcommand 0x02
payload:
01 00 00 00
```

The native side button positions for SL/SR and GL/GR are documented in the side-button tables above.

---

# 44. Rumble / haptics

Joy-Con 2 uses an HD-rumble/LRA format.

The actual five-byte packed vibration frame contains two frequency/amplitude pairs:

```text
high frequency
high amplitude
low frequency
low amplitude
```

The current Linux packing is:

```text
byte0 = hi_freq low 8 bits

byte1 =
    hi_freq high bits
    | (hi_amp << 2)

byte2 =
    (hi_amp >> 6)
    | (lo_freq << 4)

byte3 =
    (lo_freq >> 4)
    | (lo_amp << 6)

byte4 =
    lo_amp >> 2
```

The encoded fields are effectively 10-bit-class values; exact physical frequency/amplitude units remain incompletely documented.

Current ERM-like emulation frequencies used by the driver:

```text
high = 0x187
low  = 0x112
```

The driver intentionally caps Linux FF amplitude to:

```text
450 / 1024
```

of the available encoded domain because extreme values were found excessively strong and potentially unsafe for the hardware/user.

This limit is host safety policy, not a Nintendo protocol limit.

## 44.1 USB rumble packet

Joy-Con:

```text
byte 0 = 0x01
byte 1 = 0x50 | sequence
bytes 2..6 = encoded 5-byte rumble frame
```

Sequence:

```text
4-bit
increments modulo 16
```

The physical USB driver resends active vibration approximately every:

```text
4 ms
```

## 44.2 BLE rumble packet

BLE Joy-Con vibration output is 17 bytes:

```text
byte 0 = 0x00
byte 1 = 0x50 | sequence
bytes 2..6   = rumble frame
bytes 7..11  = same rumble frame
bytes 12..16 = same rumble frame
```

Thus the same five-byte frame is repeated three times.

The current Linux BLE implementation refreshes persistent rumble at approximately:

```text
20 ms
```

to avoid starving Bluetooth input traffic.

Again, this 20-ms resend policy is a host scheduling choice, while the 17-byte frame layout is protocol behavior.

---

# 45. Common-report switch in `bluez-switch2`

Current Linux BLE operation begins using the native side report because it is required during startup and preserves native protocol access.

Once the kernel's controller-command sequence becomes quiet, `bluez-switch2` schedules the transition to common input after:

```text
2500 ms
```

since the most recent kernel command.

The plugin then:

1. removes the native side notification;
2. waits approximately `250 ms`;
3. subscribes to common handle `0x000a`.

These delays are deliberately transport/host policy and should not be confused with controller sensor timing.

The common report then supplies the stable JC1/native Linux compatibility frontend while the side report remains the documented native protocol.

---

# 46. Coordinate-frame boundary

There are three distinct coordinate concepts and they must not be conflated.

## 46.1 Physical/native Joy-Con 2 frame

This is the sensor frame decoded from native Joy-Con 2 packets.

Established native motion is:

```text
XYZ
+++ signs
```

for the physical axes used by this project.

## 46.2 Legacy Joy-Con 1 wire frame

When `linux-switch2` creates a synthetic classic Joy-Con interface, it deliberately maps the Joy-Con 2 motion into the wire frame expected from a classic Joy-Con.

That mapping is controlled by:

```text
jc1_axis_map
jc1_axis_sign
jc1_right_sdl_frame
```

Those are compatibility-boundary choices, not native JC2 protocol fields.

## 46.3 SDL application frame

SDL deliberately rearranges and flips classic Joy-Con motion axes.

The synthetic JC1 frontend therefore presents the raw frame SDL expects so that SDL's normal Joy-Con transform produces the intended application orientation.

These compatibility transforms do not alter native USB Joy-Con 2 data.

---

# 47. Known unresolved native ordinary bits

The following ordinary-motion fields are deliberately retained as unresolved:

```text
bits 148..152
bit 153
bit 262
bit 278
bit 294
bit 349
bit 365
bit 381
```

Known behavior:

- `148..152` are highly stable as part of the pre-quaternion prefix.
- `153` is normally set but rarely clears.
- `262`, `278`, `294`, `349`, `365`, `381` behave as independent one-bit side channels.
- none are required to decode accel, gyro, quaternion, or cadence.
- they must not be merged into neighboring signed sensor quantities.

Native bytes:

```text
4
8
```

also remain semantically unresolved.

Byte 4 was essentially constant `0x07` in the battery-analysis corpus.

Byte 8 changed between values including:

```text
0x30
0x38
```

inside individual sessions and was proven not to be battery level.

---

# 48. Known unresolved compact fields

Compact motion retains:

```text
mid16
slot/status low2 #0
U0 raw14
slot/status low2 #1
U1 raw14
slow_u10
```

as opaque fields.

None has been demonstrated to be:

- an additional ordinary gyro axis;
- a battery value;
- a simple extension bit for acceleration;
- a direct replacement for the ordinary quaternion.

`slow_u10` is a plausible slow controller-state or temperature-like quantity, but this remains speculative.

---

# 49. Known unresolved common-report fields

The following remain partly or completely opaque:

```text
GATT 0x08..0x09
GATT 0x18
GATT 0x22..0x23 exact battery-current encoding
GATT 0x24..0x28
GATT 0x29 exact semantic
optical status word #1 high-level meaning
optical status word #2
magnetometer scale/orientation/calibration
motion temperature conversion
```

Observed fixed values should not be mistaken for semantic constants:

```text
0x18 commonly 00
0x24..0x28 commonly all 00
0x29 commonly 01
```

They may gain meaning in other modes, firmware versions, or controller types.

---

# 50. GATT/report-map discrepancy requiring future verification

The project's actual working BLE captures establish:

```text
Left:
native[14] = 0x28 / 0x1e

Right:
native[14] = 0x00
native[15] = 0x28 / 0x1e
```

Current public report documentation instead describes:

```text
Left:
offset 0x0e = NFC state, always 0
offset 0x0f = motion length

Right:
offset 0x0e = NFC state
offset 0x0f = motion length
```

The two cannot simultaneously describe the exact Left payload observed by this project.

Therefore this difference is intentionally recorded as an open protocol issue rather than silently normalized in the documentation.

Possible explanations include:

- controller firmware variation;
- mode-dependent prefix layout;
- USB-vs-BLE layout variation;
- undocumented feature-dependent omission;
- an error in one existing report map.

The Right-side extra byte itself is established; only the universal semantic/layout rule remains to be proven.

---

# 51. Important established timing summary

```text
BLE connection interval:
7.5 ms

BLE initialization command guard:
10 ms

ordinary native motion packet:
~48.75 ms

ordinary scheduler delta:
typically 46/47 ticks

native clock family:
~960 Hz

accelerometer internal spacing:
~22.75 ms, ~13 ms, ~13 ms

G2 -> G3:
~24.375 ms

physical USB common report:
~250 Hz

physical USB native compact:
~250 Hz
```

Do not infer sensor sample cadence directly from Bluetooth notification arrival time alone.

---

# 52. Quick-reference formulas

## Side stick

```text
X = b0 | ((b1 & 0x0f) << 8)
Y = (b1 >> 4) | (b2 << 4)
```

## Ordinary scheduler continuity

```text
delta =
(current_sample_time12 - previous_sample_time12)
mod 4096
```

## BLE ordinary quaternion

```text
c =
(2 × raw / (2^21 - 1) - 1) / sqrt(2)
```

## BLE compact quaternion

```text
c =
(2 × raw / (2^25 - 1) - 1) / sqrt(2)
```

## USB compact quaternion ratio

```text
z =
2 × raw / (2^25 - 1) - 1

z_i = q_i / q_missing
```

then set:

```text
q_missing = 1
```

and normalize.

## Native gyro

```text
rad/s =
raw × radians(500) / 16384
```

## Common gyro

```text
rad/s =
raw × 40 / 32767
```

## Native battery level

```text
level =
(power_info >> 2) & 0x0f
```

## Common optical relative delta

```text
dx = (s16)(new_x - old_x)
dy = (s16)(new_y - old_y)
```

---

# 53. Protocol boundaries

The following are deliberate conclusions of the reverse-engineering work:

1. Native Joy-Con 2 BLE side reports `0x07` and `0x08` are real controller-native formats and should remain first-class.

2. Common `0x05` is also genuinely produced by the Joy-Con 2 over BLE; it is not inherently a USB-grip fabrication.

3. The values `0x28` and `0x1e` are motion payload lengths — 40 and 30 bytes — rather than arbitrary report IDs.

4. BLE ordinary motion contains genuine explicit accelerometer, gyro and quaternion state with non-uniform internal timing.

5. The isolated separator/side-channel bits adjacent to gyro fields must remain separate until their semantics are proven.

6. The native side battery byte is structurally independent of the common report's richer battery voltage/current fields.

7. The optical sensor exists in both native-relative and common-cumulative representations.

8. Native BLE and physical USB compact quaternion values occupy analogous structures but use different numerical codecs.

9. Motion should not be synthesized through prediction or interpolation merely to imitate another controller's sample cadence.

10. Compatibility coordinate transforms belong at the legacy frontend boundary, not in the native controller protocol.

---

# 54. Evidence and provenance

This reference combines:

- direct Joy-Con 2 BLE notification captures;
- physical Joy-Con 2 USB captures;
- large packet-corpus statistical analysis;
- deliberate motion/timing experiments;
- flash/calibration captures;
- battery-discharge corpus analysis;
- the final `linux-switch2` implementation;
- the `bluez-switch2` GATT transport implementation;
- cross-checking against the public `ndeadly/switch2_controller_research` project;
- startup information independently observed in Switch2Connect.

Where these sources disagree, the disagreement is documented rather than silently resolved.

In particular, the exact Left-side NFC/motion-length prefix position remains explicitly marked for future verification.