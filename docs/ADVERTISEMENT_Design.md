# BLE advertisement (MSD) — what this component must broadcast

Closes the "advertisement unspecified" gap in [TODO.md](TODO.md). Derived from the canonical struct
definition and from the reference firmware's actual behavior, not from inference.

Sources:
- `components/opendisplay/opendisplay_structs.h:1209-1240` — SECTION 6, `struct MsdAdvertisement`
  and the `OD_MSD_STATUS_*` bits.
- `Firmware/src/display_service.cpp:1821-1853` — `updatemsdata()`, the reference implementation.
- `py-opendisplay/src/opendisplay/models/advertisement.py:28-60` — how the client parses it.

## Wire format

A **16-byte** manufacturer-specific data record, little-endian, broadcast in the BLE advertisement
and also returned verbatim by `CMD_READ_MSD` (`0x0044`). `OD_STATIC_ASSERT` pins the size at 16.

| Offset | Field | Width | Meaning |
|---|---|---|---|
| 0–1 | `company_id` | u16 LE | `0x2446`. Bleak strips this, so clients parse 14 bytes. |
| 2–12 | `dynamic[11]` | 11 B | Config-driven sensor/button/touch slots. **Always zero for us** — see below. |
| 13 | `chip_temperature` | u8 | `clamp((temp_c + 40) * 2, 0, 255)` — 0.5 °C resolution, +40 °C bias. |
| 14 | `battery_voltage_low` | u8 | Low 8 bits of a 10-bit value in 10 mV units. |
| 15 | `status` | u8 | Flags + battery MSB + liveness counter. |

`status` bit layout (`opendisplay_structs.h:1221-1226`):

| Bit(s) | Constant | Our value |
|---|---|---|
| 0 | `OD_MSD_STATUS_BATTERY_VOLTAGE_BIT8` | 9th bit of the 10-bit battery voltage. `0` for us. |
| 1 | `OD_MSD_STATUS_REBOOT_FLAG` | Set after boot until the host reads it. |
| 2 | `OD_MSD_STATUS_CONNECTION_REQUESTED` | `0` — we never request a connection (see below). |
| 3 | `OD_MSD_STATUS_RESERVED_3` | Must be `0`. |
| 4–7 | `..._MAIN_LOOP_COUNTER_{SHIFT,MASK}` | Free-running 4-bit liveness nibble. |

## Field derivation for an ESPHome device

### `dynamic[11]` — all zeros, and that is correct

The dynamic area carries button, touch, SHT40, and BQ27220 slots at indices assigned by config
packets (`SensorData.msd_data_start_byte`, `BinaryInputs.button_data_byte_index`,
`TouchController.touch_data_start_byte`). This component implements none of those config packets and
has no buttons, touch controller, or sensors — ESPHome owns any such peripheral through its own
components, and the plan excludes them from scope.

So all 11 bytes are zero. This is not a stub or a gap: an unwritten slot is genuinely zero in the
reference firmware too (`memset(&m, 0, sizeof m)` before the selective field writes). No client
misreads it, because no config packet ever told the client to look at a slot.

**Consequence:** our advertisement is effectively static apart from `chip_temperature` and the
counter nibble. The firmware's "skip the rebuild when nothing changed" optimization
(`display_service.cpp:1858-1866`) therefore matters much less for us.

### `chip_temperature` — populate honestly

The ESP32 exposes an internal temperature reading, so this is real data, not a placeholder. Encode
exactly as the firmware does:

```
tempEncoded = (int16_t)((chip_temp_c + 40.0f) * 2.0f)   // clamp to [0, 255]
```

Note this is the **MCU die temperature**, not ambient — the client documents it as such. Do not wire
an ESPHome ambient temperature sensor into this byte.

### `battery_voltage_low` + status bit 0 — report zero

A mains-powered ESP32 has no battery, and the reference firmware already defines what absence looks
like: `readBatteryVoltage()` returns a negative value when unavailable, and `updatemsdata()` leaves
`batteryVoltage10mv = 0` (`display_service.cpp:1826-1834`). Zero is the sanctioned "no battery"
encoding, not a value we are inventing.

So: `battery_voltage_low = 0` and status bit 0 clear. If a battery-powered ESP32 build ever appears,
follow the firmware: value in 10 mV units, clamped to 511, low 8 bits here and the 9th in bit 0.

### `status` bit 1 — reboot flag

Set on boot, cleared once the host has observed it. Cheap and genuinely informative for an ESPHome
device that may reboot on OTA.

### `status` bit 2 — connection requested: always 0

Battery tags set this to ask a host to connect during their brief awake window. A mains-powered
ESPHome device is continuously connectable, so it has nothing to request. Leave it clear.

### `status` bits 4–7 — main-loop counter

A free-running nibble that advances each advertisement update so successive advertisements stay
distinguishable and stale data is detectable. Increment and mask to `0x0F`; the semantics are pure
liveness, so any monotonic wrap is conformant.

## Consequence for `0x0044` (CMD_READ_MSD)

The plan defers `0x0044` with the reason: *"Manufacturer-specific data is optional and needs a
separate ESPHome definition before it has useful semantics."*

**That reason no longer holds.** `0x0044` returns exactly the same 16-byte record we must already
assemble to advertise at all, and every field's semantics are now pinned above. Implementing it is
close to free — return the current `MsdAdvertisement`. Recommend promoting it to v1; at minimum the
plan's stated rationale should be corrected.

## Single central

This component accepts **one** BLE central at a time. Consequences to enforce:

- Advertise as connectable; stop advertising (or reject) once connected, and resume on disconnect.
- `connection_generation` still matters: it distinguishes the *current* session from a stale one
  after a reconnect, so a late queued packet from the previous connection is dropped rather than
  applied to the new one.
- A second central attempting to connect is refused at the link layer, so no protocol-level
  "busy" response is needed for connection setup — only for a second transfer within one session,
  which the one-active-transaction rule already covers.

## Open items

- [ ] Confirm ESPHome's BLE server allows setting raw manufacturer data with company id `0x2446`,
      and how to update it periodically without a full advertising restart.
- [ ] Decide the advertisement update interval. The firmware ties it to its main loop; we should
      pick something explicit and record it.
- [ ] Confirm the reboot flag's clear condition against firmware (`rebootFlag` lifetime) — the exact
      "until the host reads it" trigger is UNVERIFIED here.
