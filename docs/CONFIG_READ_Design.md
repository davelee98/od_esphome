# `0x0040` CONFIG_READ — response design

**Status:** design, pre-implementation (M1). Normative for the `0x0040` handler.
**Scope:** how `opendisplay:` synthesizes and serves a configuration blob it does not store.

Every wire claim below cites a file and line. Paths are relative to the workspace root
`/home/davelee/opendisplay/`. `od_esphome/components/opendisplay/opendisplay_protocol.h` and
`opendisplay_structs.h` are byte-identical to `opendisplay-protocol/src/*.h` (verified by
`sha256sum`, 2026-08-10), so a citation into the vendored copy is a citation into the canonical
header. Anything not verifiable from those two headers, from `py-opendisplay`, or from the
reference `Firmware` is marked **UNVERIFIED**.

---

## 0. The problem in one paragraph

`CMD_CONFIG_READ` is defined as "return the stored configuration blob"
(`opendisplay_protocol.h:294-307`); the reference device literally reads flash and chunks the bytes
out (`Firmware/src/communication.cpp:504-553`). We have no such blob and deliberately do not
implement `0x0041`/`0x0042`/`0x0045` (plan, "Commands not to implement"), so our configuration
authority is the compiled ESPHome YAML plus what the selected display driver already knows. We
therefore **synthesize the blob at `setup()`** and serve the same immutable bytes to every reader.
The acceptance criterion is not "the header allows it" but "a real client accepts it and then
computes the same frame size we will enforce" — concretely
`py-opendisplay`'s `parse_config_response` → `_capabilities_from_config` → `prepare_image`.

---

## 1. Exact wire format

### 1.1 Request

```
[0x00][0x40]                       # opcode 16-bit BIG-endian, no payload
```
`opendisplay_protocol.h:296` (request), `:180-182` (opcode is 2 bytes big-endian; payload
multi-byte fields are little-endian unless a block says BE), `:307` (`CMD_CONFIG_READ 0x0040`).

Auth gating: `@state: session required when security enabled` (`:303`). v1 has no auth, so the
command is always answered; when `0x0050` lands in M6 this handler must gain the
`[0xFE][0x40]` (`RESP_AUTH_REQUIRED`, `:190-191`) path.

### 1.2 Success response — chunked notifications

Universal response framing is `[status][cmd_echo][data...]` with `status = 0x00` (`RESP_ACK`,
`:186`, `:702`) and `cmd_echo` = the command's **low byte** (`:192-194`), i.e. `RESP_CONFIG_READ =
0x40` (`:707`).

`0x0040` answers with **one or more** notifications (`:297-301`):

| Frame | Offset | Width | Endian | Field | Value |
|---|---|---|---|---|---|
| every | 0 | 1 | – | `status` | `0x00` |
| every | 1 | 1 | – | `cmd_echo` | `0x40` |
| every | 2 | 2 | **LE** | `chunk_number` | 0-based, increments per frame |
| chunk 0 only | 4 | 2 | **LE** | `total_len` | full config-blob byte count |
| chunk 0 | 6 | n | – | `data` | `blob[0 .. n-1]` |
| chunk k>0 | 4 | n | – | `data` | next `n` blob bytes |

Per-frame budget: `data <= MAX_RESPONSE_DATA_SIZE (100) minus its header`
(`opendisplay_protocol.h:304`; the macro is `:890`). So the reference maximum is **94** data bytes
in chunk 0 and **96** in later chunks — exactly what `Firmware/src/communication.cpp:518-533`
implements (see its comment at `:514-517`).

Empty-config degenerate form: a single `[0x00][0x40][0x00 0x00][0x00 0x00]`
(`opendisplay_protocol.h:300-301`). **We must never emit this** — a `total_len` of 0 means "no
config" to a human but parses as a 0-byte blob, and `py-opendisplay` treats short data as a hard
error (`py-opendisplay/src/opendisplay/protocol/config_parser.py:68-69`).

### 1.3 Error response

```
[0xFF][0x40][0x00][0x00]
```
`opendisplay_protocol.h:302` — "on storage-init failure". The client special-cases exactly this
4-byte shape and raises `ProtocolError("Device has no stored configuration")`
(`py-opendisplay/src/opendisplay/device.py:1108-1112`). This is the **only** correct answer when
our synthesized capabilities failed validation at `setup()`. Do not answer a half-truth blob.

Note the NACK error-code scoping rule (`:196-204`): the two `0x00` bytes here are the `0x40`
namespace, not a global error enum. Do not route them through a shared error-mapping helper.

### 1.4 The config blob (the reassembled `data` stream)

Framing is defined in `opendisplay_structs.h:269-280`:

```
[OuterPacketHeader][single_packet]...[single_packet][crc:2 LE]
single_packet = [SinglePacketHeader][fixed-size payload]
```

- `OuterPacketHeader` = `{uint16 length; uint8 version;}`, 3 bytes (`:308-312`).
  - `length` = "total outer-packet length INCLUDING the trailing CRC", but is **inconsistently
    populated** — the website toolbox patches it, some encoders leave a zero pad, and the CRC is
    always computed as if it were `0x0000` (`:301-311`, `:289-294`). The reference device ignores it
    entirely on parse (`Firmware/src/config_parser.cpp:364-365` skips 2 bytes), and the client only
    debug-logs it (`config_parser.py:72-79`).
  - `version` = `OD_CONFIG_VERSION` = **1** (`:286`). The minor (`OD_CONFIG_MINOR_VERSION` = 4,
    `:287`) is **not** carried on the wire. The client stores `minor_version=1` unconditionally
    (`config_parser.py:242`) — an acknowledged client-side inaccuracy, not something we can fix from
    the device.
- `SinglePacketHeader` = `{uint8 number; uint8 id;}`, 2 bytes (`:318-322`). **There is no per-packet
  length field** — `sizeof` the struct selected by `id` *is* the payload length (`:314-317`), so a
  packet id the reader does not know is unrecoverable: `py-opendisplay` stops parsing at the first
  unknown id (`config_parser.py:129-131`) and `Firmware`'s default case skips to the CRC
  (`config_serializer.py:661-664` documents the consequence). **Emit only ids every consumer
  knows.**
- CRC: CRC-16/CCITT-FALSE, poly `0x1021`, init `0xFFFF`, MSB-first, no reflection, no final XOR,
  over `length+version+packets` **with the two length bytes forced to zero**, excluding the CRC
  field itself; stored little-endian (`opendisplay_structs.h:289-294`;
  `Firmware/src/config_parser.cpp:308-326`; `py-opendisplay/.../config_serializer.py:45-82`).

Blob-wide conventions that apply to every struct below (`opendisplay_structs.h:167-176`): default
**little-endian**; all structs `__attribute__((packed))`; **reserved fields must be written as 0**;
bitfields number from bit 0 = LSB and unused bits must be 0.

### 1.5 Protocol version we target

`OD_PROTOCOL_VERSION 2.2` (`opendisplay_protocol.h:265-267`) — a spec marker, **not sent on the
wire**. The only version bytes we actually emit are `OuterPacketHeader.version = 1`
(`OD_CONFIG_VERSION`) and, separately, `PIPE_VERSION 0x01` in the `0x0080` handshake
(`opendisplay_protocol.h:875`) — which is explicitly *not* the protocol version.

### 1.6 Unknown / reserved field handling by a client

- Reserved bytes: written as 0 by us, ignored by older parsers — this is what makes later
  promotions out of `reserved[]` backward-compatible (`opendisplay_structs.h:174-175`).
- Trailing bytes beyond a packet's known size: the client sizes each packet from a fixed table
  (`config_parser.py:247-273`) and would mis-frame everything after a wrong-sized packet. Sizes are
  therefore contract: system 22, manufacturer 22, power 30, display 46 — matching the
  `OD_STATIC_ASSERT`s at `opendisplay_structs.h:430`, `:449`, `:507`, `:720`.
- Unknown enum *values* (as opposed to ids) are tolerated: the client stores raw ints and only
  converts opportunistically (`models/config.py:382-388`, `:390-398`).

### 1.7 Our concrete blob: 133 bytes, 2 chunks

We emit exactly four packets — the four the client requires (`config_parser.py:208-219`: `system`,
`manufacturer`, `power`, and at least one `display`) and nothing else.

| Blob offset | Size | Content |
|---:|---:|---|
| 0 | 2 | `OuterPacketHeader.length` |
| 2 | 1 | `OuterPacketHeader.version` = 1 |
| 3 | 2 | `SinglePacketHeader{number, id=0x01}` |
| 5 | 22 | `SystemConfig` |
| 27 | 2 | `SinglePacketHeader{number, id=0x02}` |
| 29 | 22 | `ManufacturerData` |
| 51 | 2 | `SinglePacketHeader{number, id=0x04}` |
| 53 | 30 | `PowerOption` |
| 83 | 2 | `SinglePacketHeader{number, id=0x20}` |
| 85 | 46 | `DisplayConfig` |
| 131 | 2 | CRC-16 LE |
| **133** | | total |

At the reference 100-byte frame budget that is two notifications:

```
frame 0 (100 B): 00 40 | 00 00 | 85 00 | blob[0..93]
frame 1 ( 43 B): 00 40 | 01 00 |        | blob[94..132]
```

`SinglePacketHeader.number`: `opendisplay_structs.h:319` says "0-based sequential packet index
within the outer packet" (global), while `py-opendisplay`'s serializer writes a **per-type** index
(`config_serializer.py:592-606`) and its parser comment claims firmware numbers globally
(`config_parser.py:158-159`). The parser keys packets by `(type, number)`
(`config_parser.py:152-154`), so both schemes parse. **Decision: follow the header — global
sequential 0,1,2,3.** With one instance of each type the two schemes differ only for the display
packet (global 3 vs per-type 0); `DisplayConfig.instance_number` carries the real display index
either way.

### 1.8 Chunk sizing vs. BLE MTU (a real hazard)

`MAX_RESPONSE_DATA_SIZE` is 100 (`opendisplay_protocol.h:890`), but a 100-byte notification needs a
negotiated ATT MTU ≥ 103; the BLE default is 23 (20-byte notification payload). The reference
firmware ships fixed 94/96-byte chunks and relies on MTU negotiation.

The client imposes **no** chunk-size expectation — it appends `chunk[2:]` from every frame until it
has `total_len` bytes (`device.py:1127-1142`) — so smaller chunks are legal. Therefore:

> **Rule:** `chunk_data_max = min(MAX_RESPONSE_DATA_SIZE, negotiated_att_mtu - 3) - frame_header_len`,
> where `frame_header_len` is 6 for chunk 0 and 4 afterwards. Never exceed 100 total.
> If `chunk_data_max < 1`, answer the `0xFF` error frame rather than emitting an empty chunk — the
> client raises `TruncatedConfigError` on a zero-length chunk (`device.py:1138-1141`).

Obtaining the negotiated MTU from ESPHome's BLE server is **TBD** (driver/stack-interface work).
Falling back to the ESP32 default negotiated MTU is acceptable only if the value is read, not
assumed.

---

## 2. Field-by-field derivation

`Source` is one of **DRIVER** (already known by `epaper_spi`/`it8951`), **YAML** (our config
schema), **CONSTANT** (fixed by this profile), **DERIVED** (computed from the others). Accessors on
the two drivers are marked **TBD** — a separate agent is documenting those interfaces; here we name
only the *value* we need.

### 2.1 `SystemConfig` — packet `0x01`, 22 B (`opendisplay_structs.h:418-430`)

| Field | Type / offset | Source | Derivation | Fallback | Risk if wrong |
|---|---|---|---|---|---|
| `ic_type` | u16 LE @5 | YAML/build | ESP32 variant → `ICType` (`:383-393`): S3→2, C3→3, C6→4 | `0` for any other variant — see §3.1 | Selects the firmware repo HA offers release notes from (`Home_Assistant_Integration/custom_components/opendisplay/update.py:117-119`, `py-opendisplay/.../models/firmware.py:9-16`). OTA *install* is gated to EFR32BG22 only (`update.py:63`), so a wrong value is misleading, not destructive |
| `communication_modes` | u8 @7 | CONSTANT | `OD_COMM_MODE_BLE` = `0x01` (`:407`) | – | Under-claiming is safe; claiming WiFi/OEPL invites a transport we do not serve |
| `device_flags` | u8 @8 | CONSTANT | `0x00` — no external power management, no XIAO/PhotoPainter init, no power latch (`:411-416`) | – | `PWR_LATCH_DFF` would advertise `0x0052` power-off support we do not implement |
| `pwr_pin` | u8 @9 | CONSTANT | `OD_PIN_UNUSED` `0xFF` (`:296-299`, field default `:425`) | – | none |
| `reserved[15]` | @10–24 | CONSTANT | zeros (`:174`, `:426`) | – | Non-zero blocks future field promotion |
| `pwr_pin_2` / `pwr_pin_3` | u8 @25, @26 | CONSTANT | `0xFF` (`:427-428`) | – | none |

### 2.2 `ManufacturerData` — packet `0x02`, 22 B (`:436-449`)

| Field | Type / offset | Source | Derivation | Fallback | Risk if wrong |
|---|---|---|---|---|---|
| `manufacturer_id` | u16 LE @29 | CONSTANT | `OD_MANUFACTURER_DIY` = 0 (`:399`) | – | Claiming Seeed/Waveshare mislabels the HA device card (`HA .../__init__.py:313`) |
| `board_type` | u8 @31 | CONSTANT | `0` — board tables live in `config.yaml` per manufacturer and have no ESPHome analogue (`:441`) | – | HA falls back to the raw number for `hw_version` (`__init__.py:316`) |
| `board_revision` | u8 @32 | CONSTANT | `0` | – | none |
| `simple_config_driver_index` | u16 LE @33 | CONSTANT | `0` = not set (`:443`) | – | none |
| `simple_config_display_index` | u16 LE @35 | CONSTANT | `0` (`:444`) | – | none |
| `simple_config_power_index` | u16 LE @37 | CONSTANT | `0` (`:445`) | – | none |
| `simple_config_configured_at[6]` | @39–44 | CONSTANT | zeros — never configured by the website tool (`:446`) | – | A fabricated timestamp would imply a website-applied preset |
| `reserved[6]` | @45–50 | CONSTANT | zeros (`:447`) | – | – |

### 2.3 `PowerOption` — packet `0x04`, 30 B (`:484-507`)

Required by the client (`config_parser.py:208-219`) but read by nothing on the image path. HA reads
`power_mode`, `sleep_timeout_ms`, `deep_sleep_time_seconds` (`sleep.py:146-160`) and
`capacity_estimator` (`sensor.py:102-103`).

| Field | Type / offset | Source | Derivation | Fallback | Risk if wrong |
|---|---|---|---|---|---|
| `power_mode` | u8 @53 | CONSTANT | `OD_POWER_MODE_USB` = 2 (`:458`) | – | With `BATTERY` + non-zero `deep_sleep_time_seconds`, HA marks the device "sleepy" and defers writes to a wake window (`sleep.py:122-124`). USB keeps HA writing immediately, which is what an always-on ESPHome node does. See §3.4 |
| `battery_capacity_mah[3]` | u24 LE @54 | CONSTANT | `0` = unknown (`:489`) | – | none |
| `sleep_timeout_ms` | u16 LE @57 | CONSTANT | `0` — we never sleep (`:490`) | – | Only consulted when `is_sleepy` (`sleep.py:130-139`), which USB power prevents |
| `tx_power` | u8 @59 | CONSTANT | `0` (`:491`) | – | No "unknown" sentinel exists (§3.3). No client consumer found |
| `sleep_flags` | u8 @60 | CONSTANT | `0` (`:492`) | – | – |
| `battery_sense_pin` / `_enable_pin` | u8 @61, @62 | CONSTANT | `0xFF` (`:493-494`) | – | – |
| `battery_sense_flags` | u8 @63 | CONSTANT | `0` (`:495`) | – | – |
| `capacity_estimator` | u8 @64 | CONSTANT | `0` — outside the 1..5 enum (`:464-470`), meaning "unset". HA explicitly tolerates it: `capacity_estimator or CapacityEstimator.LI_ION` (`sensor.py:103`) and only uses it for battery power modes (`sensor.py:102`) | – | Picking a real chemistry would produce a fabricated battery curve |
| `voltage_scaling_factor` | u16 LE @65 | CONSTANT | `0` (`:497`) | – | – |
| `deep_sleep_current_ua` | u32 LE @67 | CONSTANT | `0` = unknown (`:498`) | – | – |
| `deep_sleep_time_seconds` | u16 LE @71 | CONSTANT | `0` = disabled (`:499`) | – | Non-zero + battery ⇒ HA queues writes instead of sending them |
| `charge_enable_pin` / `charge_state_pin` | u8 @73, @74 | CONSTANT | `0xFF` (`:500-501`) | – | – |
| `charger_flags` | u8 @75 | CONSTANT | `0` (`:502`) | – | – |
| `min_wake_time_seconds` | u16 LE @76 | CONSTANT | `0` = firmware default 120 s (`:503`) | – | Unused while not sleepy |
| `screen_timeout_seconds` | u8 @78 | CONSTANT | `0` (`:504`) — panel rail management belongs to the ESPHome driver, not us | – | Describes device-side behavior nothing else acts on |
| `reserved[4]` | @79–82 | CONSTANT | zeros (`:505`) | – | – |

### 2.4 `DisplayConfig` — packet `0x20`, 46 B (`:688-720`) — the heart

| Field | Type / offset | Source | Derivation | Fallback | Risk if wrong |
|---|---|---|---|---|---|
| `instance_number` | u8 @85 | CONSTANT | `0` — the codegen enforces exactly one display (`__init__.py:66`, plan "require exactly one display ID") | – | – |
| `display_technology` | u8 @86 | CONSTANT | `OD_DISPLAY_TECH_E_PAPER` = 1 (`:516`) | – | Both supported backends are e-paper by construction |
| `panel_ic_type` | u16 LE @87 | DRIVER + table | `it8951` → `OD_PANEL_IC_ED103TC2_1872X1404` (3000) for 1 bpp or `..._4GRAY` (3001) for 4 bpp (`:673-674`); `epaper_spi` → a hand-maintained `model` → `PanelIC` table (`:565-675`) | `0` = `EP_PANEL_UNDEFINED` (`:566`) | Drives the client's **measured palette** (`display_palettes.py:74-116`), **4-gray code table** (`:37-47`) and **BWRY nibble order** (`:56-68`). `0` degrades safely to idealized palettes and base code tables — but a 4-gray panel that needs `_GRAY4_CODES_V2` (panels `0x28`, `0x48`) renders inverted mid-greys. See §3.5 |
| `pixel_width` | u16 LE @89 | DRIVER | Native panel width in pixels, **unrotated**, matching the wire buffer geometry. Value needed from the driver: *panel width in pixels* (accessor **TBD**) | **none** — fail setup | Frame-size mismatch: `0x0080` NACKs with `OD_ERR_PIPE_START_SIZE_MISMATCH` (`opendisplay_protocol.h:609`, `:806`), and direct write silently truncates or overflows |
| `pixel_height` | u16 LE @91 | DRIVER | Native panel height in pixels, unrotated (accessor **TBD**) | **none** — fail setup | As above |
| `active_width_mm` | u16 LE @93 | – | **Cannot populate** (§3.2) → `0` | `0` | Client returns `None` from `screen_diagonal_inches` when ≤ 0 (`models/config.py:374-380`) — a defined "unknown" |
| `active_height_mm` | u16 LE @95 | – | `0` (same) | `0` | Same |
| `legacy_tag_type` | u16 LE @97 | CONSTANT | `0` = unused (`:699`) | – | OEPL-only field |
| `rotation` | u8 @99 | YAML (new key) | `Rotation` **index** 0/1/2/3 = 0°/90°/180°/270° (`:521-527`) — **not degrees** | `0` | The client rotates the *source image* by this and still targets `pixel_width × pixel_height` (`device.py:334-337`). A wrong value produces a correctly-sized but rotated image. See §3.6 |
| `reset_pin` | u8 @100 | CONSTANT | `0xFF` (`:701`) | – | Informational only; no client consumer found |
| `busy_pin` | u8 @101 | CONSTANT | `0xFF` (`:702`) | – | Same |
| `dc_pin` | u8 @102 | CONSTANT | `0xFF` — note this field has **no documented `0xFF` sentinel** (`:703`); **UNVERIFIED** whether any consumer objects. No consumer found in `py-opendisplay` or the HA integration | – | Low |
| `cs_pin` | u8 @103 | CONSTANT | `0xFF` (`:704`) | – | Low |
| `data_pin` | u8 @104 | CONSTANT | `0xFF` — no documented sentinel (`:705`), same caveat as `dc_pin` | – | Low |
| `partial_update_support` | u8 @105 | CONSTANT (v1) | `OD_PARTIAL_UPDATE_NONE` = 0 (`:531`) | – | Any non-zero value routes the client into the `0x0076` partial path we do not implement (`py-opendisplay/src/opendisplay/partial.py:83-85`). **This field is how we honestly refuse partial.** M6 may raise it per validated model |
| `color_scheme` | u8 @106 | DRIVER + YAML | `ColorScheme` (`:542-555`); see the mapping in §2.5 | **none** — fail setup | Determines the client's encoder branch (`encoding/images.py:88-120`) and therefore the byte count, packing, and palette. The single highest-consequence field in the blob |
| `transmission_modes` | u8 @107 | DERIVED | `OD_TRANSMISSION_MODE_DIRECT_WRITE` (bit 3, `:682`) always; `\| OD_TRANSMISSION_MODE_PIPE_WRITE` (bit 4, `:683`) iff the backend streams. Bits 0/1/2 (compression) **0**; bits 5/6 reserved **0** (`:684-685`); bit 7 `CLEAR_ON_BOOT` **0** (`:686`) | – | Bit 4 is the client's hard pre-flight gate for `0x0080` (`models/config.py:355-367`); bits 0/1 gate compression (`device.py:1801-1804`, `:2111`). Setting a compression bit we cannot inflate breaks every upload |
| `clk_pin` | u8 @108 | CONSTANT | `0xFF` — no documented sentinel (`:709`), same caveat | – | Low |
| `cs_pin_2` | u8 @109 | CONSTANT | `0xFF` = none (`:710`) | – | Only meaningful for dual-controller Spectra panels |
| `reserved_pin_3..8` | u8 @110–115 | CONSTANT | zeros — these are `@reserved`, must be 0 (`:711-716`), **not** `0xFF` | – | Non-zero blocks future promotion |
| `full_update_mC` | u16 LE @116 | – | `0` = unknown (`:717`) | `0` | Energy accounting only |
| `reserved[13]` | @118–130 | CONSTANT | zeros (`:718`) | – | – |

### 2.5 `color_scheme` and the frame-byte derivation

`full_frame_bytes` **is not a wire field.** Both ends derive it from
`(pixel_width, pixel_height, color_scheme)`. The device side of that derivation in the reference
firmware is `directWriteComputeGeometry` (`Firmware/src/display_service.cpp:2111-2136`) with
`getBitsPerPixel` (`:1748-1761`); the client side is the encoder in
`py-opendisplay/src/opendisplay/encoding/`. They must agree exactly, because `0x0080` carries
`total_size` and the device NACKs a mismatch (`Firmware/src/display_service.cpp:2838`;
`opendisplay_protocol.h:592-609`).

| `ColorScheme` | value | bpp | Client encoder | Bytes on the wire | Row stride |
|---|---:|---:|---|---|---|
| `MONO` | 0 (`:543`) | 1 | `encode_1bpp` — `np.packbits(axis=1)`, MSB first, **each row zero-padded to a byte** (`encoding/images.py:123-142`) | `ceil(w/8) * h` | `ceil(w/8)` |
| `BWR` | 1 (`:544`) | 1×2 planes | `encode_bitplanes` → `plane1 ++ plane2` (`encoding/bitplanes.py:49-57`; concatenated at `device.py:374-376`) | `2 * ceil(w/8) * h` | `ceil(w/8)` per plane |
| `BWY` | 2 (`:545`) | 1×2 planes | as BWR | `2 * ceil(w/8) * h` | as BWR |
| `BWRY` | 3 (`:546`) | 2 | `encode_2bpp` — 4 px/byte MSB-first, **row padded to a multiple of 4 px** (`images.py:145-177`) | `ceil(w/4) * h` | `ceil(w/4)` |
| `BWGBRY` | 4 (`:547`) | 4 | `encode_4bpp(bwgbry_mapping=True)` — 2 px/byte, high nibble first, row padded to even width (`images.py:180-235`) | `ceil(w/2) * h` | `ceil(w/2)` |
| `GRAY4` | 5 (`:548`) | 1×2 planes | `encode_gray4_bitplanes` → `plane0 ++ plane1` (`bitplanes.py:60-93`; joined at `device.py:377-379`) | `2 * ceil(w/8) * h` | `ceil(w/8)` per plane |
| `GRAY16` | 6 (`:549`) | 4 | `encode_4bpp` identity map (`images.py:106-108`) | `ceil(w/2) * h` | `ceil(w/2)` |
| `SEVEN_COLOR` | 7 (`:550`) | 4 | `encode_4bpp` identity map (`images.py:109-113`) | `ceil(w/2) * h` | `ceil(w/2)` |
| `BWGBRY_SPLIT` | 8 (`:551`) | 4 | `encode_4bpp(half_planes=True)` — left half-plane (all rows) then right half-plane (`images.py:114-119`, `:232-234`) | `ceil(mid/2)*h + ceil((w-mid)/2)*h`, `mid = w/2` | see §3.7 |

Cross-check against the reference device: `2*ceil(w/8)*h` for bitplane and gray4 schemes
(`display_service.cpp:2118`, `:2135`), `ceil(w/2)*h` at 4 bpp (`:2127`), `ceil(w/4)*h` at 2 bpp
(`:2128`), `ceil(w/8)*h` otherwise (`:2129`). Identical to the table.

**Byte alignment / stride:** stride is always per-row and always the ceiling shown above; there is
no wire field carrying it and no extra padding beyond the row boundary. `byte_alignment` in
`DisplayCapabilities` is a local concept only.

**v1 restriction:** advertise only schemes whose packing the selected backend has been tested
against (CLAUDE.md: "Add an `epaper_spi` panel model only after byte packing, color mapping, buffer
size, and BUSY behavior are tested for it"). The initial supported set should be `MONO` for
`epaper_spi` and `MONO` / `GRAY16` for `it8951` (matching `PanelIC` 3000/3001, `:673-674`); every
other scheme is a future, separately validated addition.

The exact driver value we need in order to *choose* the scheme — "does this panel/driver present
1 bpp mono, 4-bpp greyscale, or a multi-ink palette, and in what packing" — is **TBD** pending the
driver-interface work. If the driver cannot state it unambiguously, the scheme must come from an
explicit YAML key rather than being inferred.

---

## 3. Fields we cannot honestly populate

Each of these is a finding, not a detail. None is filled with a plausible-looking invention.

### 3.1 `SystemConfig.ic_type` for ESP32 variants outside {S3, C3, C6} — **NOT a blocker**
`ICType` (`opendisplay_structs.h:383-393`) enumerates ESP32-S3 (2), ESP32-C3 (3), ESP32-C6 (4) and
various nRF/EFR parts. There is no value for the original ESP32, ESP32-S2, or ESP32-H2, and no
"unknown" value. Our `DEPENDENCIES = ["esp32", "esp32_ble_server"]` (`__init__.py:14`) admits all of
them.

**Corrected 2026-08-10** — an earlier revision of this section called it a narrow blocker and
proposed failing the build. That was wrong. No client consumer uses `ic_type` behaviourally, and
every one of them already degrades gracefully:

| Consumer | Behaviour on an unmapped value |
|---|---|
| `cli.py:538` `_ic_label` | Renders `"Unknown"` — display only. |
| `models/config.py:85-90` `ic_type_enum` | Catches `ValueError`, returns the raw int. |
| `models/config_json.py:91` | `str(sys.ic_type)` — passthrough. |
| `models/firmware.py:10-15` | Maps to an OTA release repo; irrelevant here because DFU (`0x0051`) is explicitly not implemented, and an unmapped value yields `None` rather than a phantom entity. |

*Resolution:* emit `0` for any variant without an `ICType` value and move on. Do **not** fail the
build over a cosmetic field. `panel_ic_type` (§3.5) is the identity field that actually changes
rendering.

### 3.2 `DisplayConfig.active_width_mm` / `active_height_mm`
No ESPHome display driver carries physical millimetres. `0` is an accepted "unknown" — the client's
`screen_diagonal_inches` returns `None` for non-positive values (`models/config.py:374-380`).
*Resolution:* safe neutral value; optionally a future YAML key (`panel_size_mm: [x, y]`) for users
who want HA to show a diagonal. Not a blocker.

### 3.3 `PowerOption.tx_power`
"BLE transmit-power setting (platform-specific units)" (`:491`) with no unknown sentinel and no
unit we can map to ESP-IDF's dBm-index scale. We emit `0`. No consumer found in `py-opendisplay` or
the HA integration. *Resolution:* neutral value; document as meaningless for this profile.

### 3.4 `PowerOption.power_mode`
The enum is `BATTERY | USB | SOLAR` (`:456-460`) — there is no "mains" or "unknown". An ESPHome
node is typically mains/USB powered, so `USB` (2) is the least-wrong and produces correct HA
behavior (`deep_sleep_enabled = power_mode == BATTERY and deep_sleep_time_seconds > 0`,
`sleep.py:122-124`). *Resolution:* documented approximation, not an invention. If ESPHome deep sleep
is ever combined with this component, this and `deep_sleep_time_seconds` need a real YAML key —
until then, claiming `BATTERY` would make HA queue writes for a device that is actually awake.

### 3.5 `DisplayConfig.panel_ic_type` for `epaper_spi` models without a `PanelIC` counterpart
`PanelIC` (`:557-675`) is an OpenDisplay-owned list keyed to specific panels; `epaper_spi`'s model
list is its own. Where they overlap we can map honestly; where they do not, only `0`
(`EP_PANEL_UNDEFINED`, `:566`) is truthful. Consequences of `0` are bounded but real: no measured
palette (falls back to idealized, `display_palettes.py:110-116`), base 4-gray code table
(`:43-47` — wrong mid-greys for panels that need `_GRAY4_CODES_V2`), default BWRY nibble order
(`:64-68`). *Resolution:* a hand-maintained mapping table, `0` where unknown, **and** a rule that a
model with no `PanelIC` value may not advertise `GRAY4` or `BWRY` (the two schemes whose correctness
depends on the panel id).

### 3.6 `DisplayConfig.rotation` — **needs a new YAML key**
Two different meanings collide. On the wire, `rotation` tells the client to pre-rotate the *source
image* while still targeting `pixel_width × pixel_height` (`device.py:334-337`: `target_size` is
always the config's pixel dimensions; rotation only feeds `_rotate_source_image`). In ESPHome,
a display driver's own `rotation:` option changes what the driver's width/height accessors report
and how it draws. If we forwarded ESPHome's rotation we would report swapped dimensions and produce
a frame the client sizes wrongly.
*Resolution:* add an `opendisplay:` YAML key (e.g. `rotation: 0|90|180|270`, encoded as the
`Rotation` **index**, `:521-527`), default 0; and **validate at codegen that the display driver's own
rotation is 0/unset**, failing the config otherwise. `pixel_width`/`pixel_height` must always be the
native, unrotated buffer geometry. Also note `DisplayCapabilities.rotation` in `protocol_types.h:105`
is an undocumented `uint8_t` — it must be defined as the wire *index*, not degrees.

### 3.7 `BWGBRY_SPLIT` frame size when `w % 4 != 0`
The client packs `mid = w // 2` then each half independently
(`encoding/images.py:232-234`), while the reference device sizes it as plain 4 bpp `ceil(w/2)*h`
(`display_service.cpp:2127`, with scheme 8 mapped to 4 bpp at `:1755-1756`). These agree only when
`mid` is even, i.e. `w % 4 == 0`. *Resolution:* if we ever advertise scheme 8, reject panel widths
not divisible by 4 at setup. Not applicable to v1 (scheme 8 is not in the initial supported set).

### 3.8 Fields that are structurally required but semantically empty
`ManufacturerData.board_type` / `board_revision` / `simple_config_*`, all of `PowerOption`'s
battery/charger/sleep pins and flags, `DisplayConfig.legacy_tag_type`, `full_update_mC`, and every
GPIO field. Each has a documented "absent" encoding (`0` or `OD_PIN_UNUSED 0xFF`,
`opendisplay_structs.h:296-299`) except `dc_pin`, `data_pin`, `clk_pin`, which document no sentinel
(`:703`, `:705`, `:709`) — **UNVERIFIED** whether any consumer rejects `0xFF` there; none was found.
*Resolution:* neutral values, no new YAML keys, no blockers.

### 3.9 Not a field, but not populatable either: the config minor version
`OD_CONFIG_MINOR_VERSION` (4, `:287`) is not carried on the wire and the client hardcodes 1
(`config_parser.py:242`). Nothing we can do from the device side; noted so nobody looks for a place
to put it.

---

## 4. Capability advertisement rules

### 4.1 What v1 may claim

| Wire capability | v1 value | Justification |
|---|---|---|
| `transmission_modes` bit 3 `DIRECT_WRITE` | **set** | Plan implements `0x0070`/`0x0071`/`0x0072` |
| `transmission_modes` bit 4 `PIPE_WRITE` | set **iff** the backend streams contiguously | Plan implements `0x0080`–`0x0082`; the bit is the client's hard gate (`models/config.py:355-367`). PIPE is `@targets: Firmware` only (`opendisplay_protocol.h:617`), so we are a *new* PIPE implementation and must be tested against that target's behavior |
| `transmission_modes` bit 0 `STREAMING_DECOMPRESSION` | **clear** | No inflate in v1. Setting it makes the client compress (`device.py:1801-1804`, `:2111`) and every transfer fails |
| `transmission_modes` bit 1 `ZIP` | **clear** | Same |
| `transmission_modes` bit 2 `G5` | **clear** | Not implemented anywhere |
| `transmission_modes` bits 5/6 | **clear** | `@reserved`, must be 0 (`opendisplay_structs.h:684-685`) |
| `transmission_modes` bit 7 `CLEAR_ON_BOOT` | **clear** | Behavioral request, not a capability; could become a YAML key later |
| `partial_update_support` | **0** (`NONE`) | `0x0076` is M6/capability-gated. `0` keeps the client on the full-frame path (`partial.py:83-85`) |
| `SystemConfig.communication_modes` | **BLE only** (`0x01`) | Transport is BLE in v1 (`__init__.py:67`) |
| Auth (`0x0050`) | not advertised | No `SecurityConfig` (`0x27`) packet is emitted; the client treats its absence as "no security" (`device.py:803-805`) |

Refresh modes have **no** advertisement field. `0x0072` carries `refresh: 0=FULL, 1=FAST/PARTIAL`
(`opendisplay_protocol.h:521-522`) and the client may send either (`models/enums.py:9-19`). Since
nothing in the config declares fast-refresh support, the handler must accept `refresh=1` and either
honor it or transparently downgrade to a full refresh — never NACK on it.

### 4.2 `DisplayCapabilities` ↔ wire mapping

`protocol_types.h:101-124` is the single capability model that must drive both the backend and this
response.

| `DisplayCapabilities` field | Wire counterpart | Notes |
|---|---|---|
| `width` (`:102`) | `DisplayConfig.pixel_width` | Native, unrotated |
| `height` (`:103`) | `DisplayConfig.pixel_height` | Native, unrotated |
| `rotation` (`:104`) | `DisplayConfig.rotation` | **Must be the enum index 0..3**, not degrees (§3.6) |
| `pixel_format` (`:106`) | `DisplayConfig.color_scheme` | **Lossy — see below** |
| `full_frame_bytes` (`:107`) | *none* | Derived on both sides from (w, h, scheme); the only place it appears on the wire is `0x0080`'s `total_size`, which we validate against |
| `row_stride_bytes` (`:108`) | *none* | Local only |
| `byte_alignment` (`:109`) | *none* | Local only |
| `supports_full_refresh` (`:111`) | *none* | Implicit; always true |
| `supports_fast_refresh` (`:112`) | *none* | No config field exists; see §4.1 |
| `supports_partial` (`:113`) | `DisplayConfig.partial_update_support` | Three-valued on the wire (`NONE`/`SUPPORTED`/`FULL_FRAME`, `:529-534`) vs. `bool` locally — the `FULL_FRAME` distinction is *not* representable and materially changes client behavior (`partial.py:112-118`). **Widen the local field to the enum before M6** |
| `partial_x_alignment` / `partial_y_alignment` (`:114-115`) | *none* | The client hardcodes 8-px alignment and MONO-only for partials (`partial.py:86-92`, `:107-110`) |
| `supports_direct_write` (`:117`) | `transmission_modes` bit 3 | |
| `supports_pipe_write` (`:118`) | `transmission_modes` bit 4 | |
| `supports_compression` (`:119`) | `transmission_modes` bits 0/1 | One bool, two distinct wire bits with different firmware semantics (`models/config.py:312-343`). Fine while both are 0; needs splitting if compression ever lands |
| `max_contiguous_write` (`:121`) | *none* | Local; PIPE's per-frame ceiling is `PIPE_MAX_FRAME` 244 (`opendisplay_protocol.h:878`) |
| `staging_bytes_required` (`:122`) | *none* | Local |
| `staging_in_psram` (`:123`) | *none* | Local |

**Gaps in `DisplayCapabilities` that the wire needs** (fields with no local counterpart):
`panel_ic_type`, `display_technology`, `active_*_mm`, `legacy_tag_type`, `full_update_mC`, all pin
fields, and everything in `SystemConfig` / `ManufacturerData` / `PowerOption`. `panel_ic_type` is
the important one — it changes the client's palette and code tables (§3.5), so it is a genuine
capability and belongs in the model. Recommendation:

- add `uint16_t panel_ic_type` to `DisplayCapabilities`;
- replace `PixelFormat` with the canonical `ColorScheme` value. `PixelFormat` as written
  (`protocol_types.h:78-83`: `MONO_1BPP`, `GRAY_4BPP`, `SPECTRA_E6`) cannot express the distinctions
  that matter: `GRAY_4BPP` is ambiguous between `GRAY4` (scheme 5, two 1-bit planes) and `GRAY16`
  (scheme 6, packed 4 bpp) — **different byte counts** — and `SPECTRA_E6` cannot distinguish
  `BWGBRY` (4) from `BWGBRY_SPLIT` (8), which have different plane ordering. Carrying the wire enum
  removes a translation layer that can only lose information;
- keep the identity/power constants (§2.1–2.3) in a separate small `DeviceIdentity` struct rather
  than bloating `DisplayCapabilities`; they are fixed at compile time and never consulted by the
  transfer engine.

---

## 5. Where the data is captured: snapshot at `setup()`

**Recommendation: build the complete 133-byte blob once, at the end of `setup()`, and serve a
`const` buffer thereafter.** The `0x0040` handler then does nothing but chunk bytes.

Justification:

1. **Nothing in the blob can change at runtime.** Panel model, geometry, colour scheme and rotation
   are all compile-time YAML/driver facts. There is no runtime path that legitimately alters them —
   and if one existed, changing geometry between a client's `interrogate()` and its `0x0080`
   `total_size` would produce a size mismatch NACK with no explanation.
2. **Driver init order is a real hazard, and setup is where we can fail loudly.** Our component must
   be set up *after* the display (ESPHome setup priority — exact value **TBD** with the driver
   work). If the driver reports zero geometry at our `setup()`, that is a hard error: call
   `mark_failed()` and have `0x0040` answer `[0xFF][0x40][0x00][0x00]`
   (`opendisplay_protocol.h:302`). Reading live per request would instead hand a client a
   `0×0` panel and let it discover the problem mid-transfer.
3. **Allocation failures belong to setup, not to the blob.** The `epaper_spi` staging buffer
   (PSRAM-preferred, plan §"ePaper SPI adapter requirements") maps to `staging_bytes_required` /
   `staging_in_psram`, which have **no wire counterpart** (§4.2). A failed allocation therefore must
   not be papered over by editing the config — it fails setup, and the error frame is the answer.
4. **Rotation is ours, not the driver's** (§3.6), so there is no "the user rotated the display at
   runtime" case to track.
5. **The BLE path stays trivial.** Serving a precomputed buffer keeps the callback to a bounds check
   and a copy, matching the hard constraint in CLAUDE.md ("BLE callbacks only bounds-check and
   enqueue"). Chunk emission still happens from `loop()`.

Rejected alternative — rebuild per request: adds a driver call on a request path, opens a window for
two clients to see two different configs, and buys nothing since no input can change.

One consequence to accept: the blob is ~133 bytes of RAM held for the lifetime of the component.
That is cheaper than the code needed to rebuild it safely.

---

## 6. `build_config_response()` sketch

Host-testable: includes only `opendisplay_structs.h` (which pulls `opendisplay_protocol.h`,
`stdint.h`, `stdbool.h`) and `protocol_types.h` — no ESPHome, no ESP-IDF, per CLAUDE.md.

The sketch assumes the three `DisplayCapabilities` changes recommended in §4.2/§7.7 have landed:
`color_scheme` (the canonical `ColorScheme` value) replaces `pixel_format`, `panel_ic_type` is
added, and the compile-time identity constants live in a small separate `DeviceIdentity`.

```cpp
// config_response.h  (host-testable; no ESPHome headers)
#pragma once
#include <cstdint>
#include <cstddef>
#include "opendisplay_structs.h"   // OuterPacketHeader, SinglePacketHeader, *Config, OD_*
#include "protocol_types.h"        // DisplayCapabilities, DeviceIdentity

namespace esphome {
namespace opendisplay {

// 3 (outer) + 4 packets * 2 (header) + 22 + 22 + 30 + 46 + 2 (crc) = 133.
inline constexpr size_t CONFIG_BLOB_BYTES =
    sizeof(struct OuterPacketHeader) +
    4u * sizeof(struct SinglePacketHeader) +
    sizeof(struct SystemConfig) + sizeof(struct ManufacturerData) +
    sizeof(struct PowerOption)  + sizeof(struct DisplayConfig) + 2u;
static_assert(CONFIG_BLOB_BYTES == 133, "config blob layout changed");

// Exact wire byte count for one full frame. Mirrors py-opendisplay's encoders
// (encoding/images.py, encoding/bitplanes.py) and Firmware's
// directWriteComputeGeometry (display_service.cpp:2111-2136). 0 => unsupported.
uint32_t frame_bytes_for(uint8_t color_scheme, uint16_t w, uint16_t h);

enum class ConfigBuildError : uint8_t {
  NONE = 0, ZERO_GEOMETRY, UNSUPPORTED_SCHEME, NO_TRANSMISSION_MODE,
  FRAME_BYTES_ZERO, SPLIT_WIDTH_UNALIGNED,
  // (no IC_TYPE_UNKNOWN -- ic_type is cosmetic; see §3.1)
};

// Builds the immutable blob. Called exactly once, from setup(), AFTER the
// display driver is initialised. Returns NONE on success.
ConfigBuildError build_config_blob(const DisplayCapabilities &caps,
                                   const DeviceIdentity &ident,
                                   uint8_t (&out)[CONFIG_BLOB_BYTES]);

// Emits one 0x0040 notification. `chunk` is 0-based; `mtu_payload` is the
// usable notification payload (>= 7). Returns bytes written, or 0 when the
// blob is exhausted.
size_t build_config_response(const uint8_t (&blob)[CONFIG_BLOB_BYTES],
                             uint16_t chunk, size_t mtu_payload,
                             size_t &blob_offset, uint8_t *out, size_t out_cap);

}  // namespace opendisplay
}  // namespace esphome
```

```cpp
// config_response.cpp  (structure, not final code)

// CRC-16/CCITT-FALSE with the 2-byte length field forced to zero.
// opendisplay_structs.h:289-294; Firmware/src/config_parser.cpp:308-326.
static uint16_t config_crc16(const uint8_t *d, size_t n) {
  uint16_t crc = OD_CONFIG_CRC_INIT;
  for (size_t i = 0; i < n; i++) {
    crc ^= static_cast<uint16_t>((i < 2 ? 0u : d[i]) << 8);
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ OD_CONFIG_CRC_POLY)
                           : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}

uint32_t frame_bytes_for(uint8_t scheme, uint16_t w, uint16_t h) {
  const uint32_t W = w, H = h;
  switch (scheme) {
    case OD_COLOR_SCHEME_MONO:        return ((W + 7) / 8) * H;
    case OD_COLOR_SCHEME_BWR:
    case OD_COLOR_SCHEME_BWY:
    case OD_COLOR_SCHEME_GRAY4:       return 2u * ((W + 7) / 8) * H;   // two 1-bit planes
    case OD_COLOR_SCHEME_BWRY:        return ((W + 3) / 4) * H;
    case OD_COLOR_SCHEME_BWGBRY:
    case OD_COLOR_SCHEME_GRAY16:
    case OD_COLOR_SCHEME_SEVEN_COLOR: return ((W + 1) / 2) * H;
    case OD_COLOR_SCHEME_BWGBRY_SPLIT: {
      const uint32_t mid = W / 2;                                       // see §3.7
      return (((mid + 1) / 2) + (((W - mid) + 1) / 2)) * H;
    }
    default: return 0;
  }
}

ConfigBuildError build_config_blob(const DisplayCapabilities &caps,
                                   const DeviceIdentity &ident,
                                   uint8_t (&out)[CONFIG_BLOB_BYTES]) {
  // ---- validate BEFORE serialising; an invalid capability set must produce
  // ---- the 0xFF error frame, never a plausible-looking blob.
  if (caps.width == 0 || caps.height == 0)        return ConfigBuildError::ZERO_GEOMETRY;
  // NOTE: ic_type is deliberately NOT validated -- 0 is an acceptable value for
  // an ESP32 variant outside {S3, C3, C6}. No client consumes it behaviourally
  // and all degrade gracefully. See §3.1.
  const uint32_t frame = frame_bytes_for(caps.color_scheme, caps.width, caps.height);
  if (frame == 0)                                  return ConfigBuildError::UNSUPPORTED_SCHEME;
  if (caps.color_scheme == OD_COLOR_SCHEME_BWGBRY_SPLIT && (caps.width % 4) != 0)
                                                   return ConfigBuildError::SPLIT_WIDTH_UNALIGNED;
  uint8_t modes = 0;
  if (caps.supports_direct_write) modes |= OD_TRANSMISSION_MODE_DIRECT_WRITE;
  if (caps.supports_pipe_write)   modes |= OD_TRANSMISSION_MODE_PIPE_WRITE;
  if (modes == 0)                                  return ConfigBuildError::NO_TRANSMISSION_MODE;
  // Compression bits are unconditionally clear in v1 -- never derive them from a
  // backend flag that has not been proven end-to-end.

  memset(out, 0, sizeof(out));            // reserved fields are zero by construction
  size_t o = 0;
  // --- outer header ------------------------------------------------------
  out[o++] = 0x00; out[o++] = 0x00;       // length: see the open question in §7.1
  out[o++] = OD_CONFIG_VERSION;           // 1

  uint8_t packet_no = 0;                  // global sequential (structs.h:319)
  auto put_hdr = [&](uint8_t id) { out[o++] = packet_no++; out[o++] = id; };
  auto put = [&](const void *p, size_t n) { memcpy(out + o, p, n); o += n; };

  // --- 0x01 system -------------------------------------------------------
  struct SystemConfig sys {};
  sys.ic_type = ident.ic_type;            // ESP32 variant -> ICType (structs.h:383-393)
  sys.communication_modes = OD_COMM_MODE_BLE;
  sys.device_flags = 0;
  sys.pwr_pin = OD_PIN_UNUSED;
  sys.pwr_pin_2 = OD_PIN_UNUSED;
  sys.pwr_pin_3 = OD_PIN_UNUSED;
  put_hdr(OD_PKT_SYSTEM); put(&sys, sizeof(sys));

  // --- 0x02 manufacturer -------------------------------------------------
  struct ManufacturerData man {};
  man.manufacturer_id = OD_MANUFACTURER_DIY;   // everything else stays 0
  put_hdr(OD_PKT_MANUFACTURER); put(&man, sizeof(man));

  // --- 0x04 power --------------------------------------------------------
  struct PowerOption pwr {};
  pwr.power_mode = OD_POWER_MODE_USB;          // §3.4
  pwr.battery_sense_pin = OD_PIN_UNUSED;
  pwr.battery_sense_enable_pin = OD_PIN_UNUSED;
  pwr.charge_enable_pin = OD_PIN_UNUSED;
  pwr.charge_state_pin = OD_PIN_UNUSED;
  // capacity_estimator stays 0 = unset (HA tolerates: sensor.py:103)
  put_hdr(OD_PKT_POWER); put(&pwr, sizeof(pwr));

  // --- 0x20 display ------------------------------------------------------
  struct DisplayConfig dsp {};
  dsp.instance_number = 0;
  dsp.display_technology = OD_DISPLAY_TECH_E_PAPER;
  dsp.panel_ic_type = caps.panel_ic_type;      // 0 = EP_PANEL_UNDEFINED (§3.5)
  dsp.pixel_width = caps.width;
  dsp.pixel_height = caps.height;
  dsp.rotation = caps.rotation;                // Rotation INDEX 0..3 (§3.6)
  dsp.reset_pin = dsp.busy_pin = dsp.dc_pin = OD_PIN_UNUSED;
  dsp.cs_pin = dsp.data_pin = dsp.clk_pin = dsp.cs_pin_2 = OD_PIN_UNUSED;
  dsp.partial_update_support = OD_PARTIAL_UPDATE_NONE;   // v1 (§4.1)
  dsp.color_scheme = caps.color_scheme;
  dsp.transmission_modes = modes;
  // reserved_pin_3..8, reserved[13], active_*_mm, legacy_tag_type,
  // full_update_mC all stay 0 (§3.2, §3.8).
  put_hdr(OD_PKT_DISPLAY); put(&dsp, sizeof(dsp));

  // --- CRC ---------------------------------------------------------------
  const uint16_t crc = config_crc16(out, o);
  out[o++] = crc & 0xFF; out[o++] = (crc >> 8) & 0xFF;   // little-endian
  assert(o == CONFIG_BLOB_BYTES);
  return ConfigBuildError::NONE;
}

size_t build_config_response(const uint8_t (&blob)[CONFIG_BLOB_BYTES],
                             uint16_t chunk, size_t mtu_payload,
                             size_t &blob_offset, uint8_t *out, size_t out_cap) {
  const size_t hdr = (chunk == 0) ? 6u : 4u;             // §1.2
  const size_t cap = (mtu_payload < MAX_RESPONSE_DATA_SIZE ? mtu_payload
                                                          : MAX_RESPONSE_DATA_SIZE);
  if (cap <= hdr || out_cap < cap) return 0;             // caller emits 0xFF error frame
  const size_t remaining = CONFIG_BLOB_BYTES - blob_offset;
  if (remaining == 0) return 0;
  const size_t n = (remaining < cap - hdr) ? remaining : cap - hdr;

  size_t o = 0;
  out[o++] = RESP_ACK;                                   // 0x00  (protocol.h:702)
  out[o++] = RESP_CONFIG_READ;                           // 0x40  (protocol.h:707)
  out[o++] = chunk & 0xFF; out[o++] = (chunk >> 8) & 0xFF;          // LE
  if (chunk == 0) { out[o++] = CONFIG_BLOB_BYTES & 0xFF;
                    out[o++] = (CONFIG_BLOB_BYTES >> 8) & 0xFF; }   // total_len, LE
  memcpy(out + o, blob + blob_offset, n);
  blob_offset += n;
  return o + n;
}
```

Emission policy: chunks are pushed from `loop()`, one notification per iteration if the TX path is
backpressured, never in a BLE write callback. The reference device has to drain each chunk before
queueing the next to avoid overflowing its notification ring
(`Firmware/src/communication.cpp:538-547`) — the same hazard applies to ESPHome's BLE server and
must be respected.

### 6.1 Host tests that prove well-formedness

Structural (pure C++, fake transport):

1. `sizeof` guards: the four `OD_STATIC_ASSERT`s hold and `CONFIG_BLOB_BYTES == 133`.
2. **CRC known-answer**: a golden blob byte-vector with its expected CRC; plus the invariant that
   mutating either length byte does **not** change the CRC (the "length treated as zero" quirk,
   `opendisplay_structs.h:289-294`).
3. **Chunking**: for `mtu_payload` ∈ {20, 23, 100, 244}: every frame ≤ `min(mtu, 100)`; chunk 0
   carries `total_len == 133`; chunk numbers are 0,1,2,… little-endian; the concatenation of all
   `data` fields equals the blob exactly; no frame has zero data bytes; the sequence terminates.
4. **Self-consistency**: re-parse our own blob with an independent mini-parser — outer version is 1,
   packets are exactly `{0x01, 0x02, 0x04, 0x20}` with sizes `{22, 22, 30, 46}`, offsets sum to
   `133 - 2`, and the trailing CRC verifies.
5. **Derivation table**: `frame_bytes_for` against a fixture of `(scheme, w, h) → bytes` covering
   every supported scheme, non-byte-aligned widths (122, 792, 1200, 1872), and both `it8951` modes.
6. **Validation refuses rather than invents**: `width == 0`, `height == 0`, unknown scheme,
   no transmission mode, and split-with-`w%4!=0` each return the right
   `ConfigBuildError`, and the component then answers `[0xFF][0x40][0x00][0x00]` — asserted on the
   fake transport.
7. **Immutability**: two consecutive `0x0040` reads produce byte-identical frame sequences; a read
   during an active transfer produces the same bytes and does not disturb transaction state.
8. **Capability conformance**: for every advertised bit there is a corresponding implemented
   handler, and the compression bits are zero — the "unsupported features are never advertised" gate
   from the plan's M5.

Cross-implementation (the one that actually proves interop) — a Python test importing
`py-opendisplay` from the workspace:

9. Feed the emitted blob to `parse_config_response` and assert no `ConfigParseError` and all four
   required packets present (`config_parser.py:208-219`).
10. `_capabilities_from_config` returns our `(width, height, color_scheme, rotation)` unchanged
    (`device.py:212-230`).
11. `prepare_image(<synthetic image>, config=<parsed>)` returns `len(image_data)` **exactly equal**
    to `frame_bytes_for(...)`. This is the acceptance criterion: it links our config to the number
    the transfer engine will enforce at `0x0072`/`0x0082` and that `0x0080` validates
    (`opendisplay_protocol.h:609`).
12. `display.supports_pipe_write` / `supports_direct_write` match what we advertised, and
    `supports_zip`/`supports_streaming_decompression` are both `False`
    (`models/config.py:312-367`).
13. `compute_partial_region(...)` returns `"fallback_full"` for our config, proving `0x0076` is
    never attempted (`partial.py:83-85`).

---

## 7. Open questions and risks

### 7.1 `OuterPacketHeader.length`: emit the real total or `0x0000`?
The header calls it "total length including CRC" but records that encoders disagree and that the CRC
is always computed as if it were zero (`opendisplay_structs.h:301-311`). `py-opendisplay`'s
serializer writes `0x0000` (`config_serializer.py:587-589`); its parser ignores the field
(`config_parser.py:72-85`); the reference device ignores it (`Firmware/src/config_parser.cpp:364`).
**Recommendation: emit `0x0000`** — then a client that verifies the CRC *without* zeroing the length
field still gets the right answer, so both possible verifier implementations agree. Emitting the real
133 would break any non-zeroing verifier. `total_len` in chunk 0 already gives readers the length.
*Open:* confirm the website toolbox's verifier before freezing this.

### 7.2 `ic_type` for unsupported ESP32 variants (§3.1) — RESOLVED, not a risk
Emit `0`. Verified: no behavioural consumer, all degrade gracefully. See §3.1.

### 7.3 BLE MTU and chunk size — RESOLVED, no longer a risk
Two decisions collapse this item:

1. **Always request `OD_BLE_MAX_FRAME` (256)** — the canonical preferred MTU
   (`opendisplay_protocol.h:886`). The rationale at `:63-73` is that declaring value length and
   preferred MTU at 256 rather than the 512 ATT maximum makes an oversize write draw ATT error
   `0x0D` instead of being silently dropped. Usable single-write value is `256 - 3 = 253`.
2. **Chunk size is not MTU-derived at all — it is standardized.** `MAX_RESPONSE_DATA_SIZE` (100) is
   the max bytes in a single notification frame (`:885`, cited as `0x0040`'s limit at `:299`), and
   the reference Firmware chunks the config read against exactly that constant, never against the
   negotiated MTU (`Firmware/src/communication.cpp:504-546`). Chunk 0 carries 94 data bytes after
   its 6-byte header; later chunks carry 96 after 4.

So there is no adaptive-sizing problem and no need for a negotiated-MTU accessor on this path: 100
fits inside 253 with wide margin, and matching the standard is what guarantees interop. The earlier
"clamp to 20 bytes if the accessor is unavailable" contingency is withdrawn — it would have produced
frames no reference peer emits.

*Carried forward instead:* the reference implementation **drains each chunk to BLE before enqueuing
the next**, because the handler runs synchronously on the loop task and the notification ring would
otherwise overflow and silently truncate the config (`communication.cpp:538-546`). Reproduce that
discipline.

### 7.4 Panel geometry accessor semantics
`pixel_width`/`pixel_height` must be the **native, unrotated buffer** geometry, not a
rotation-adjusted logical size. Whether `epaper_spi`/`it8951` expose the native values (rather than
rotation-adjusted ones) is **TBD** pending the driver-interface work; if only the rotated values are
public, we must forbid driver-level rotation at codegen (§3.6) so the two coincide.

### 7.5 Colour-scheme selection may not be inferable from the driver
If the driver cannot unambiguously state its packing (1 bpp vs. two-plane vs. packed 4 bpp), the
scheme must come from an explicit YAML key. Guessing it produces a config that parses cleanly and
then corrupts every image — the worst possible failure mode, because it looks like a rendering bug.

### 7.6 `panel_ic_type = 0` degrades colour fidelity silently (§3.5)
No client error, just different colours. Mitigation: refuse to advertise `GRAY4`/`BWRY` without a
known panel id, and log a loud warning at setup when the id is 0.

### 7.7 `DisplayCapabilities` needs three changes before it can drive this response
`panel_ic_type` added, `PixelFormat` replaced by the canonical `ColorScheme` value (it cannot
represent GRAY4-vs-GRAY16 or BWGBRY-vs-SPLIT, §4.2), and `supports_partial` widened to the
three-valued `PartialUpdateSupport` enum before M6. Until then the "one capability model" rule in the
plan is aspirational for `0x0040`.

### 7.8 We are a new PIPE implementation
PIPE is `@targets: Firmware` only (`opendisplay_protocol.h:617`). Advertising bit 4 commits us to
matching that target's behavior — including that its `0x0081` ACK is a *selective* ACK with a 32-bit
mask (`PIPE_ACK_MASK_BITS`, `:880`) that our contiguous-only policy narrows but does not replace
(CLAUDE.md). Do not set bit 4 until M3's tests pass against that target's semantics.

### 7.9 `dc_pin` / `data_pin` / `clk_pin` have no documented "unused" sentinel — **UNVERIFIED**
We emit `0xFF` by analogy with `OD_PIN_UNUSED` (`opendisplay_structs.h:296-299`). No consumer that
objects was found in `py-opendisplay` or the HA integration, but the website config tool was not
inspected.

### 7.10 Sync-tool gap (inherited)
`od_esphome` is not registered in `opendisplay-protocol/tools/sync_protocol_header.py`'s copy map, so
`--check` will not catch drift in our vendored headers (CLAUDE.md). Every offset in this document is
pinned to the current copies (`sha256` verified against canonical, 2026-08-10); re-verify after any
protocol bump.
