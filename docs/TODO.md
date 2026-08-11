# TODO

Live work list for the `opendisplay:` ESPHome external component.

Spec: [OpenDisplay_ESPHome_Component_Plan.md](OpenDisplay_ESPHome_Component_Plan.md). Wire truth:
`components/opendisplay/opendisplay_protocol.h` / `opendisplay_structs.h`. Conventions and gotchas:
[../CLAUDE.md](../CLAUDE.md).

Keep this file current as work happens — tick items when they land, add what you discover, and note
blockers and decisions inline. `[ ]` open · `[x]` done · `[~]` in progress · `[!]` blocked.

Status as of 2026-08-11: protocol, engine, config/MSD builders and both backends are written.
**The component still cannot communicate** — the BLE layer is not wired, so nothing below is
exercisable yet. See "Not implemented" immediately below.

## BLE GATT — IMPLEMENTED 2026-08-11

Written against the `../esphome` checkout (tag 2026.7.4), not inferred.

- [x] GATT service + characteristic `0x2446` created via `global_ble_server->create_service()` /
      `create_service(...)->create_characteristic(uint16_t, esp_gatt_char_prop_t)`. Properties are
      READ | WRITE | **WRITE_NR** | NOTIFY — write-without-response is what makes PIPE throughput
      possible.
- [x] `esp_ble_gatt_set_local_mtu(256)` declares the preferred MTU at `OD_BLE_MAX_FRAME`.
- [x] Write callback routed to `on_ble_write()` — bounds-check, copy, return, nothing else.
- [x] `drain_tx_()` notifies via `set_value()` + `notify()`, bounded to 4 frames per `loop()`.
- [x] MSD broadcast via `esp32_ble::global_ble->advertising_set_manufacturer_data()`, republished
      only when the 16 bytes change. The company id is in the payload because ESPHome passes the
      buffer verbatim to ESP-IDF.
- [x] Connect/disconnect wired to `on_ble_connect()` / `on_ble_disconnect()`.
- [x] Stop advertising while connected, resume on disconnect. **Enforced by config, not code:**
      ESPHome resumes advertising on connect only while `client_count_ < max_clients_`
      (`esp32_ble_server/ble_server.cpp:172-174`), so `max_clients: 1` — the default — already gives
      exactly this. `FINAL_VALIDATE_SCHEMA` rejects anything higher. Calling
      `esp_ble_gap_stop_advertising()` ourselves would have raced the server's own
      `advertising_start()`.

## Not implemented — functional gaps

- [x] MSD `chip_temperature` now reads the real die sensor, following ESPHome's own
      `internal_temperature_esp32.cpp`: `temprature_sens_read()` on the original ESP32 (with its 128
      failure sentinel), `temperature_sensor_get_celsius()` elsewhere. A failed read keeps the last
      good value so the advertisement does not jump.
- [ ] `set_version()` is never called from codegen — `0x0043` reports 0.1.0 with an empty SHA.
- [ ] `_panel_ic_for()` always returns 0 and `PANEL_IC_BY_MODEL` is dead code. 0
      (`EP_PANEL_UNDEFINED`) degrades safely for mono, so this is a limitation rather than a defect —
      but it must be wired before any GRAY4/BWRY model is allowed.
- [x] `transmission_modes` was never written to the config blob — the device advertised **zero**
      transfer capability while `build_config_blob` validated that it had some. Fixed 2026-08-11.
- [x] Pin fields were memset to 0, claiming GPIO 0 rather than 0xFF "none". Fixed.

## Not verified

- [ ] **Never compiled.** No ESPHome on this machine, no CI. Expect type errors, signature
      mismatches and wrong include paths.
- [ ] **Zero host tests exist** — `components/opendisplay/tests/` holds only the case list.

---

## M0 — Driver hooks (gates everything)

**VERDICT: M0 does not pass on public APIs alone.** Full analysis in
[ESPHome_Display_Drivers_Reference.md](ESPHome_Display_Drivers_Reference.md). Both drivers are
mainline ESPHome, documented against tag 2026.7.4:

| | `it8951` | `epaper_spi` |
|---|---|---|
| Docs | <https://esphome.io/components/display/it8951/> | <https://esphome.io/components/display/epaper_spi/> |
| Source | `esphome/components/it8951/` | `esphome/components/epaper_spi/` |
| C++ class | `esphome::it8951::IT8951Display` | `esphome::epaper_spi::EPaperBase` |
| YAML | `display: - platform: it8951` | `display: - platform: epaper_spi` |
| Since | 2026.7.0 | 2025.10.0 |

- [x] Document the `epaper_spi` and `it8951` interfaces.
- [x] Correct the scaffold's class names (both were wrong) and pin
      `cv.require_esphome_version(2026, 7, 0)` — the later of the two floors.
- [x] **IT8951 refresh completion — RESOLVED by decision, not by code.** Fixed 5 s settle time;
      observable completion deferred. See open question 1 for the decision and its cost. The
      milestone reorder this previously forced is **withdrawn** — the plan's original "M2 IT8951
      first" ordering stands.
- [ ] Implement the settle deadline in `poll_refresh()` and make sure the component's
      `refresh_timeout` is comfortably longer than `IT8951_REFRESH_SETTLE_MS` (5 s), or the timeout
      will fire first and mask it.
- [ ] **Require `busy_pin` in codegen for `epaper_spi`.** Without it `is_idle_()` is hard-true and
      completion reporting becomes a lie — the exact failure `0x0073` exists to prevent.
- [ ] **Buffer load is NOT satisfied on either driver.** Both framebuffers are protected with no
      accessor; `epaper_spi` uses `SplitBuffer`, deliberately non-contiguous with no `data()`, so
      even a public accessor gives no memcpy target. Only public path is per-pixel
      `draw_pixel_at`/`draw_pixels_at`. Decide: upstream `write_frame_bytes(offset, data, len)` +
      `SplitBuffer::write()`, or accept per-pixel conversion cost for v1.
- [x] **Refresh mode is NOT selectable on `epaper_spi`** — `update()` takes no mode argument, and
      `it8951`'s `prepare_update_region_` forces GC16 on the full-update cycle. RESOLVED by the
      "always FULL" decision (open question 3): advertise full only, parse and ignore the wire's
      `refresh` byte. The plan's "request an advertised refresh mode" is superseded.
- [x] Upstream hooks: **decided — none.** The standing "no new upstream functions" constraint
      (CLAUDE.md) rules out all three of the reference doc's §6.2 proposals. We ship narrower and
      document the gaps. This closes M0 as far as it can be closed.
- [ ] Correct the plan: **the IT8951 data path it specifies does not exist.** The driver always
      allocates a full MCU framebuffer (1.31 MB at 1872×1404 4bpp → PSRAM mandatory) and streams
      from it; "no MCU framebuffer" is not achievable upstream.
- [ ] Defend against two YAML landmines in codegen/examples: `auto_clear_enabled` defaults true when
      a `lambda:` exists (wipes our frame), and with no lambda/pages final validation force-injects
      `show_test_card: True`, overriding an explicit `false`.
- [ ] Install ESPHome locally so codegen and builds can actually be exercised.
- [ ] Confirm both backends compile with **no** private-member access and no SPI `reinterpret_cast`.
- [ ] Record the pinned minimum ESPHome version in `README.md`.

**Gate:** backends compile without private-member access, and each backend can honestly report
physical refresh completion.

## M1 — Skeleton, BLE, config/version, entities

Scaffolded but **never compiled**. Everything below still needs a real build to count.

- [x] `components/opendisplay/` layout per the plan's "Repository layout".
- [x] `__init__.py` `CONFIG_SCHEMA` + codegen. *(No `manifest.json` — not an ESPHome convention;
      using `DEPENDENCIES`/`AUTO_LOAD` + `cv.require_esphome_version`.)*
- [x] Backend selected at **codegen** time from display type (`it8951` → `IT8951Backend`,
      `epaper_spi` → `EpaperSPIBackend`); exactly one display ID; clear error on unsupported type.
- [x] Validate fixed v1 limits in the schema: one active transfer, PIPE window 16, ACK cadence 4,
      bounded `transfer_timeout` / `refresh_timeout`.
- [x] Busy binary sensor (false at boot, publishes every transition), activity text sensor,
      `opendisplay.is_busy` condition, `opendisplay.abort` action, `on_transfer_started` /
      `on_refresh_complete` / `on_error`.
- [x] `DisplayCapabilities` model (`protocol_types.h`) — shape only; not yet populated.
- [ ] **Compile it.** Install ESPHome, build an example, fix what the scaffold got wrong. Nothing in
      M1 is trustworthy until this passes.
- [ ] BLE GATT service + characteristic `0x2446`; manufacturer ID 9286 in the advertisement.
      (`ble_transport.h` has constants and the `RxPacket` shape only — no GATT registration yet.)
- [ ] Bounded RX queue + critical-section handoff between BLE callback and `loop()`.
- [ ] `0x0040` config read — synthesize the 133-byte blob (4 packets + CRC) per
      [CONFIG_READ_Design.md](CONFIG_READ_Design.md); chunked response, built once at `setup()`.
- [ ] **Advertise no compression in `0x0040`.** Load-bearing: the client defaults to zlib and only
      falls back when this says otherwise.
- [ ] Build and broadcast the 16-byte MSD per [ADVERTISEMENT_Design.md](ADVERTISEMENT_Design.md).
- [ ] `0x0044` READ_MSD — **v1**. `[0x00][0x44]` in, `[0x00][0x44][msd:16]` out, no error path.
      Returns the same record built for the advertisement, so it is near-free once that exists.
- [ ] `SystemConfig.ic_type` — **not a blocker; pick any value and move on.** Verified against the
      client: no consumer uses it behaviourally. `cli.py:538` renders it for display and already
      falls back to `"Unknown"`; `models/config.py:85-90` catches `ValueError` and returns the raw
      int; `config_json.py:91` stringifies it. `models/firmware.py:10-15` maps it to an OTA repo,
      which is irrelevant here because DFU (`0x0051`) is explicitly not implemented. An unmapped
      ESP32 variant degrades to a cosmetic "Unknown". *(An earlier note called this a near-blocker —
      it isn't. `panel_ic_type` below is the field that actually has teeth.)*
- [ ] `DisplayConfig.panel_ic_type` — **this one has real effect.** `display_palettes.py:43-111`
      keys *measured* GRAY4/BWRY code tables and palettes off it, falling back to idealized palettes
      when it is `None` or unmapped. Wrong or absent means silently degraded colour, not an error.
      Hence the rule already recorded: do not advertise GRAY4/BWRY without a known panel id.
- [x] **MTU policy DECIDED: always request `OD_BLE_MAX_FRAME` (256).** This matches the canonical
      guidance — peers SHOULD declare GATT value length and preferred MTU at `OD_BLE_MAX_FRAME`
      rather than the 512 ATT maximum, so an oversize write draws ATT error `0x0D` instead of being
      silently dropped (`opendisplay_protocol.h:63-73`, `:886`). Usable single-write value is 253.
- [ ] Request 256 at BLE server setup, and **declare the GATT characteristic value length to
      match** — the header is explicit that declaring it is what converts a silent drop into a
      visible ATT error.
- [x] **Config chunk size needs no MTU derivation — it is already standardized.**
      `MAX_RESPONSE_DATA_SIZE` (100) is the max bytes in a single notification frame
      (`opendisplay_protocol.h:885`, cited as the `0x0040` limit at `:299`), and the reference
      Firmware chunks the config read against exactly that, not against the negotiated MTU
      (`Firmware/src/communication.cpp:504-546`): chunk 0 carries 94 data bytes after its 6-byte
      header, later chunks 96 after 4. Match the standard; do not build an adaptive scheme.
      *(Note `CONFIG_CHUNK_SIZE` = 200 is a different constant — it bounds the host→device
      CONFIG_WRITE path we do not implement.)*
- [ ] Mirror the reference implementation's **drain-per-chunk** discipline: it flushes each chunk to
      BLE before enqueuing the next because the handler runs synchronously on the loop task and
      would otherwise overflow the notification ring, silently truncating configs
      (`communication.cpp:538-546`). Our BLE-callback/loop split makes this a real hazard too.
- [ ] **Colour scheme source.** If it cannot be read from the driver it must be an explicit YAML
      key — guessing yields a config that parses cleanly and then corrupts every image.
- [ ] **Rotation needs a new YAML key.** ESPHome rotates while drawing; the wire expects the client
      to pre-rotate with buffer geometry unchanged. Codegen must also validate the driver's own
      rotation is 0.
- [ ] `0x0043` firmware/component version.
- [ ] `protocol.h/.cpp` — transport-neutral parse/encode (not yet written).

**Gate:** a client discovers the device, reads configuration, and observes activity state.

## M2 — IT8951 direct write

- [ ] `backend.h` narrow adapter interface (plan's `OpenDisplayBackend`).
- [ ] `backend_it8951.*`: stream contiguous chunks into IT8951 image RAM — no full MCU framebuffer.
- [ ] Explicitly define input format, grayscale conversion, bit/byte packing, row stride, target
      memory address, alignment. Document them next to the code.
- [ ] `0x0070` / `0x0071` / `0x0072` flow; ACK every accepted data packet; exact final byte count.
- [ ] Refresh polled in `loop()`; `0x0073` only after real completion; `0x0074` on timeout.
- [ ] Advertise **full refresh only** (open question 3). Do not add FAST/partial without also
      implementing real mode selection.

**Gate:** exact-size frame reaches controller RAM; `0x0073` follows real physical completion.

## M3 — PIPE_WRITE engine

- [ ] `transfer_engine.*` with the plan's single `Transaction` struct, fully reset between sessions.
- [ ] `0x0080` / `0x0081` / `0x0082`. Emit the **canonical SACK frame** (32-bit `ack_mask`) with a
      contiguous-only mask — do not invent an ESPHome-specific reply. See CLAUDE.md.
- [ ] In-order only; no reorder queue; zero storage for out-of-order or duplicate packets.
- [ ] 16-packet in-flight cap; ACK every 4 accepted; tail-flush ACK at `0x0082` for a partial group.
- [ ] Window violation → error + last ACK + abort, with zero reorder allocation.
- [ ] All offset/size arithmetic checked before buffer access.
- [ ] Note: `PIPE_FRAME_OVERHEAD` 3 = cmd(2) + **seq(1)** — 1-byte sequence field.

**Gate:** full frame transfers at the window limit; loss/reorder tests resend from the last ACK
without excess buffering.

## M4 — ePaper SPI adapter

- [x] `backend_epaper_spi.*`: **no component-owned staging buffer and no PSRAM requirement** —
      received bytes go straight into the driver's own framebuffer via `draw_pixel_at`.
      (Supersedes the plan's "component-owned native-layout buffer, PSRAM-preferred".)
- [x] Convert/validate each packet into its exact full-frame offset — `write_contiguous()`.
- [x] Commit via the standard per-pixel API (open question 2). No M0 hook needed.
- [ ] **Compile and test it.** Written against the documented interfaces, never built.
- [ ] Verify the `poll_refresh()` idle guard against real driver behaviour: it assumes `update()`
      drives the component out of `LOOP_DONE` before our next poll. If it does not, completion is
      reported one full cycle late (safe) — but if the driver returns to idle *between* polls,
      completion is missed entirely (not safe). Confirm on hardware.
- [ ] Per-model validation before adding any panel: byte packing, color mapping, buffer size, BUSY
      behavior. One checklist entry per model added.

**Gate:** a supported mono panel renders an exact frame with no concurrent driver writes.

## M5 — Errors, cleanup, CI

- [ ] Error categories from the plan mapped to canonical NACKs. **Decode `data[0]` only in the scope
      of the echoed opcode** — no shared opcode-agnostic error decoder.
- [ ] Transfer timeout, refresh timeout, disconnect from every non-idle state, backend error — each
      aborts, releases resources, and clears `busy`. No stuck busy, ever.
- [ ] Disconnect *during refresh*: keep polling as the backend requires; clear ownership on result.
- [ ] Counters: successful/aborted transfers, direct + PIPE packets, retransmit requests, queue
      overflow, backend failures, refresh timeouts, queue high-water mark.
- [ ] Logging: opcode, state, expected vs. received sequence/offset/byte count, backend error.
      Never log image payloads.
- [ ] `examples/`: `it8951_full_frame.yaml`, `epaper_spi_mono.yaml`, `epaper_spi_spectra_e6.yaml`.
- [ ] CI: compile against the pinned minimum ESPHome version + run host tests.

**Gate:** no stuck busy state or leaked resource; unsupported features never advertised.

## M6 — Capability-gated extras

- [ ] `0x0076` partial region — backend/model gated, start with documented 1-bpp only.
- [ ] `0x0050` AES-128 session auth. **Must not reuse ESPHome native-API encryption keys.**
- [ ] Compression (`PIPE_FLAG_COMPRESSED`) — note the workspace default zlib window is 9 bits
      (512 B) and targets reject larger declared windows.
- [ ] LAN/TCP transport reusing the transport-neutral parser (explicitly out of scope for v1).

**Gate:** each independently tested and capability-gated before advertisement.

---

## Tests

### Host tests (fake backend + fake transport)

- [ ] Config/version replies and capability generation.
- [ ] Start rejected while a transaction is active, without mutating it.
- [ ] Direct write: success, underflow, overflow, malformed data, ACK-per-packet, timeout.
- [ ] PIPE ACKs at packets 4, 8, 12, 16; final ACK for an incomplete group at end.
- [ ] Out-of-order / duplicate PIPE data → retransmit request with **no** storage.
- [ ] Window violation aborts with zero reorder allocation.
- [ ] Disconnect from every non-idle state.
- [ ] `0x0073` only after simulated physical completion; `0x0074` on timeout.

### Hardware tests (per supported profile)

- [ ] Config read and exact wire frame-size discovery.
- [ ] Known test image catching row order, stride, bit/nibble order, color mapping.
- [ ] Direct write at normal and negotiated-small BLE MTUs.
- [ ] PIPE_WRITE at 16 in flight, ACK cadence 4.
- [ ] Deliberate dropped / duplicated / reordered PIPE packets.
- [ ] Disconnect during receive and during refresh.
- [ ] Transfer timeout and refresh timeout.
- [ ] Local ESPHome automation waits on busy, then updates the same display.
- [ ] Repeated transfers: no leaked buffers, queue buildup, or stuck busy.

---

## Housekeeping

- [ ] Register `od_esphome` in `opendisplay-protocol/tools/sync_protocol_header.py` `ARTIFACTS` so
      `--check` catches header drift here. Until then, verify with `sha256sum` after a protocol bump.
- [x] Expand `README.md` beyond its one-line title.
- [ ] Choose and record the codegen lint/test toolchain (workspace siblings use uv + `mypy --strict`
      + `ruff` + `pylint` + `prek`, line length 120), then replace the "no commands yet" note in
      CLAUDE.md with the real commands.

## Plan gaps (found 2026-08-10 reviewing the plan against the vendored header + py-opendisplay)

Each needs a decision folded back into `OpenDisplay_ESPHome_Component_Plan.md` — the plan is the
spec, so a gap here is a spec bug, not a TODO detail.

- [x] **BLE advertisement was entirely unspecified.** RESOLVED — see
      [ADVERTISEMENT_Design.md](ADVERTISEMENT_Design.md), derived from
      `opendisplay_structs.h:1209-1240` and the reference firmware's `updatemsdata()`
      (`Firmware/src/display_service.cpp:1821-1853`). Decisions recorded there: `dynamic[11]` all
      zero (no buttons/touch/sensors in an ESPHome build — those are ESPHome's own components and
      out of scope); `chip_temperature` populated honestly from the ESP32 die sensor as
      `(temp_c+40)*2`; battery = 0, which is the firmware's own "no battery" encoding;
      connection-requested always clear (we are continuously connectable); reboot flag and the 4-bit
      liveness nibble implemented. Still to fold into the plan doc.
- [ ] **Compression is a negotiation we must win, not a feature we can skip.** `py-opendisplay`
      defaults `compress=True` (`device.py:283`) and only falls back to uncompressed when the config
      response says so — `(display_cfg.supports_zip or display_cfg.supports_streaming_decompression)
      if display_cfg else True` (`device.py:1803`, `:1923`). So v1's uncompressed-only stance holds
      **only if** `0x0040` explicitly advertises no compression. If `0x0040` is wrong or unanswered,
      the real client streams zlib and every transfer fails. The plan treats this as "do not
      implement"; it is actually a hard dependency on the config response.
- [x] **ETag is referenced but never defined.** RESOLVED by open question 8: not implemented at all.
      No `displayed_etag`, no partial refresh, so the plan's "invalidate partial-update state" at
      `0x0074` refers to state that will not exist. Remove that clause from the plan. The optional
      trailing `new_etag:4 BE` must still be *tolerated* by the length parser.
- [ ] **`0x0073` is a direction-dependent collision.** `CMD_LED_ACTIVATE` is `0x0073` host→device
      (`:544`), while `0x73` device→host is refresh-success. The plan lists `0x0073` only as
      "Device → client" and omits it from the do-not-implement list. An inbound `0x0073` must NACK as
      unsupported — a parser keyed on opcode alone will misroute it.
- [ ] **Per-opcode size limits are unstated.** Header gives `0x0070` START ≤ 200 bytes plaintext
      (`:502`) and `0x0071` ≤ 230 plaintext / 154 encrypted (`:514`). The plan only says "bounded".
      Validation must be per-opcode, not one global cap.
- [x] **Refresh-byte encoding differs between the two END opcodes.** `0x0072`: 0=FULL, 1=FAST/PARTIAL
      (`:521`). `0x0082`: 0=FULL, 1=FAST, 2/absent=PARTIAL (`:642`). Neutralised by the "always FULL"
      decision (open question 3) — the value is parsed and ignored. **Its presence still matters**
      for frame-length parsing, since it shifts where an optional `new_etag` begins.
- [ ] **Protocol version vs. firmware version.** `0x0043` returns
      `[major][minor][shaLen][sha][patch?]` and is ALWAYS plaintext (`:0x0043` block). Separately,
      `OD_PROTOCOL_VERSION` is 2.2 (`:265`). Decide what we report for each and whether clients gate
      on the protocol version.
- [x] **MTU policy.** DECIDED: always request `OD_BLE_MAX_FRAME` (256) and declare the GATT value
      length to match. Response frames use the standardized `MAX_RESPONSE_DATA_SIZE` (100), not a
      negotiated-MTU derivation. Still to fold into the plan doc.
- [ ] **Pairing/bonding is undecided.** Auth (`0x0050`) is deferred to M6, which leaves the GATT
      characteristic open — anyone in range can write the panel. That may be fine, but it should be a
      stated decision rather than a side effect of deferral.
- [x] **Multiple concurrent centrals.** DECIDED: **one central at a time.** Advertise connectable,
      stop advertising once connected, resume on disconnect. `connection_generation` is still
      required — it distinguishes the current session from a stale one so a late queued packet from
      a previous connection is dropped rather than applied to the new one. Detail in
      [ADVERTISEMENT_Design.md](ADVERTISEMENT_Design.md#single-central).
- [ ] **Coexistence with other ESPHome BLE users** (`esp32_ble_tracker`, improv, native API) is
      unaddressed.
- [x] **`0x0044` (READ_MSD) promoted to v1.** The plan's deferral reason (manufacturer data "needs a
      separate ESPHome definition before it has useful semantics") is obsolete — it returns exactly
      the 16-byte record we must already assemble to advertise, and every field is now pinned.
      Now an M1 task. Remove it from the plan's "Commands not to implement" table.
- [ ] **No peripherals, ever — make it explicit in the plan.** An ESPHome build has no buttons,
      touch controller, or OpenDisplay-managed sensors; ESPHome owns those through its own
      components. This is why `dynamic[11]` is zero and why the button/touch/sensor config packets
      are not merely deferred but permanently out of scope.
- [ ] **Acceptance gates name "a client", never *the* client.** The real consumer is
      `Home_Assistant_Integration` pinning `py-opendisplay`. Interop against that stack should be an
      explicit gate, not implied.

## Codex review findings (2026-08-10) — NEW WORK, several invalidate earlier decisions

An independent review found issues we had missed. Verified against the canonical header where noted.

### 10. PIPE window and ACK cadence are NEGOTIATED, not fixed — our "16 / every 4" is wrong
**VERIFIED** against `opendisplay_protocol.h:592-616`. `0x0080` carries
`[req_window:1][req_ack_every:1][client_max_frame:2 LE]`, and the response is
`[ver:1][max_window:1][max_ack_every:1][max_frame:2 LE][resp_flags:1]` — *"Device echoes its
negotiated maxima (min-rule applies)"*, legal range 1..32.

**Deadlock scenario:** a client requests `window=1, ack_every=1`, sends one packet and waits for its
ACK; a receiver hard-coded to ACK every 4 waits for three more packets. Neither side moves.

- [ ] Replace the fixed constants with `min(requested, our_max)` for window, ack_every, and frame.
      `protocol_types.h` now names them `PIPE_MAX_*` to make this explicit.
- [ ] **`resp_flags` bit0 (selective-repeat) must be CLEARED.** That bit is the protocol's own way of
      saying "in-order only" — the plan reinvented it as a unilateral no-reorder-queue policy while
      leaving the negotiation field unspecified. bit1 (partial accepted) also always clear.
- [ ] Return the negotiated `max_frame` too; the design never mentioned `client_max_frame`.

### 11. "Cumulative ACK" is not a valid SACK encoding
`0x0081`'s ACK is `[0x00][0x81][highest_seen:1][ack_mask:4 LE]`, where *"mask bit i (LSB first) =
chunk (highest_seen-1-i) received; highest_seen is implicitly acked"* (`:624-629`). So an ACK with
`ack_mask = 0` acknowledges **only** `highest_seen`, not everything below it. To acknowledge 0-3
contiguously we must send `highest_seen=3` **with mask bits 0,1,2 set**.

- [ ] Define the exact mask construction; "cumulative" is not implementable as written.
- [ ] **Sequence wraps mod 256** (`:626`). 48 000 bytes stays under 256 packets, but an IT8951 frame
      wraps repeatedly. Window distance, duplicate detection, and mask generation all need explicit
      modulo arithmetic.
- [ ] A `0x0081` NACK is **FATAL**: further `0x81` frames are discarded until the next START or
      disconnect (`:630-633`). That is a different mechanism from re-sending the last ACK.

### 12. A new START must ABORT the active transfer, not be rejected
**VERIFIED**: *"a new START aborts any in-flight transfer; seq resets to 0"* (`:615`), and the same
for `0x0070` (`:501`). The plan says reject without mutating the active transaction
(`OpenDisplay_ESPHome_Component_Plan.md:162`, `:422`) — the opposite.

**Failure:** after packet loss the client sends a fresh START to recover; we reject it and stay
wedged until timeout.

### 13. Refresh timeout must not release the display while the driver is still running
The driver has no cancel. Clearing `busy` on `0x0074` and accepting a new frame mutates the
framebuffer *while the driver is reading it* → torn image; later `update()` calls are silently
rejected until the old state machine finishes. Made worse by `refresh_timeout` accepting values as
low as 1 s (`__init__.py:74-77`).

- [ ] Separate **protocol state** from **hardware ownership**. `0x0074` may end the transaction, but
      the backend stays quarantined until `is_idle()` returns true.
- [ ] Raise the `refresh_timeout` minimum well above any real refresh.

### 14. Abort leaves a corrupted frame that local automation will happily display
No staging buffer means a half-received frame sits live in the driver's buffer. On abort we clear
`busy`, and a waiting local automation calls `panel.update()` — displaying half the new image over
half the old. Silent, persistent corruption. The current "a partial buffer is better than white"
comment (`backend_epaper_spi.cpp`) is not defensible when another writer can refresh it.

- [ ] Decide: clear the whole buffer on abort, hold a dirty/quarantine state until a complete frame
      replaces it, or explicitly accept the corruption and document it.

### 15. `begin_refresh()` cannot tell whether `update()` was accepted
`update()` returns `void` and upstream **rejects re-entry** when the driver is not IDLE. We ACK
`0x0072` first, then call it. If a local/scheduled update is already running, our call is ignored,
`poll_refresh()` sees that unrelated refresh as busy, and we emit `0x0073` for someone else's
refresh — our frame was never refreshed at all.

- [ ] Require `is_idle()` before `update()`, then verify it went false; only then ACK the END.

### 16. Smaller confirmed defects
- [x] `panel_ic_type` was `uint8_t`; wire field is `uint16_t` — IT8951 ids 3000/3001 truncated. Fixed.
- [x] `millis()` wrap: `now >= deadline` fires immediately for transfers started near the ~49.7-day
      wrap. Fixed with signed-difference comparison.
- [x] Codegen used `display_var.type == display_class`, which can never match `epaper_spi`'s
      model-specific subclass — every real config would have been rejected. Fixed to `inherits_from`.
- [x] `init()` did not check `display_->is_failed()`, so we could advertise transfer support over a
      driver whose framebuffer allocation failed. Fixed.
- [x] Rotation-0 was asserted in a comment but never enforced. Now checked in `init()`.
- [ ] `poll_refresh()`'s "must observe busy" guard is in the wrong place — `update()` leaves IDLE
      synchronously, so the check belongs at `begin_refresh()`. As written, a refresh that completes
      between two polls is never reported.
- [ ] `caps_.color_scheme` is documented as codegen-set but **no setter and no generated call
      exist** — it defaults to 0, which is mono only by accident. Needs a real setter plus a model
      allow-list.
- [ ] Plan lists `0x0052` as deep sleep; canonically `0x0052` is POWER_OFF and `0x0053` is
      DEEP_SLEEP (`:409-444`, `:481-492`). Both must be rejected; the plan omits `0x0053` entirely.
- [ ] Plan's example YAML uses a top-level `epaper_spi:` block; both drivers are `display:`
      platforms. It will fail validation before any of our code runs.
- [ ] `epaper_spi` **partial refresh is impossible**, not merely deferred: no public region or
      per-call refresh control exists, and upstream changes are forbidden. Mark it so.
- [ ] "Any invalid command aborts the transaction" is too broad — an unrelated unsupported command
      would destroy a valid in-flight transfer. Only transaction-scoped faults should abort.
- [ ] TX backpressure: if the queue is full when the END ACK or `0x0073` is generated, the panel
      refreshes but the client waits forever. Terminal frames need reserved capacity.
- [ ] Timeout semantics undefined: which events extend the transfer deadline?
- [ ] `error` is never published — `abort()` resets straight to IDLE, and busy is defined as
      "any non-IDLE state", so the activity sensor can never show `error`.

## Open questions

**All nine are now settled.** Kept in place as the decision record — items are cross-referenced from
code comments, so nothing is renumbered or deleted. New questions append as #10 onward.

| # | Question | Status |
|---|---|---|
| 1 | IT8951 refresh completion | **Decided** — fixed 5 s settle, real completion deferred |
| 2 | Buffer commit path | **Decided** — standard per-pixel `draw_pixel_at`; no bypass, no upstream hook |
| 3 | Refresh mode selection | **Decided** — always FULL; advertise full only |
| 4 | First validated `epaper_spi` model | **Decided** — Seeed EE04 4.26" mono (`seeed-ee04-mono-4.26`), already upstream |
| 5 | Colour scheme source | **Answered** — derive from `model:`; needs a mapping table |
| 6 | PSRAM policy | **Resolved** — none required for `epaper_spi` |
| 7 | Pairing/bonding | **Decided** — none; open, unauthenticated characteristic |
| 8 | ETag in v1 | **Decided** — not implemented; no partial refresh, so no ETag state |
| 9 | `0x0044` READ_MSD | **Decided** — promoted to v1 (M1) |

### 1. IT8951 refresh completion — DECIDED: fixed 5 s settle, real completion deferred
`UPDATE_REFRESH` is fire-and-forget (`it8951.cpp:277-288`). **Decision (2026-08-10): ship a fixed
5 s settle time.** `begin_refresh()` stamps a deadline, `poll_refresh()` returns `PENDING` until it
passes, then `COMPLETE`. Enforced from `loop()` as a deadline — never a blocking `delay()`.
Constant: `IT8951_REFRESH_SETTLE_MS` in `backend_it8951.h`.

Consequence, recorded so it is not rediscovered in the field: on IT8951, `0x0073` means "5 s elapsed
since we issued the refresh", not "the panel finished". A slower mode or larger panel may still be
updating when the client is told it is done. This is a deliberate documented deviation from the
plan's "`0x0073` only after real physical completion" rule — the rule still holds for `epaper_spi`,
which reports honestly.

**This unblocks M0 and M2, and reverses the reorder recommendation below** — the plan's original
"M2 IT8951 first" ordering stands again.

**The 5 s settle is now PERMANENT, not deferred.** The only real fix required an upstream addition
(`is_refreshing()` plus an opt-in terminal LUT-idle wait), and the standing "no new upstream
functions" constraint rules that out. So IT8951's `0x0073` will continue to mean "5 s elapsed" for as
long as this constraint holds. Document that limitation for users of the IT8951 backend — it is a
real behavioural difference from the `epaper_spi` backend, which reports honestly.

*Kept for the record, should the constraint ever be lifted:* IT8951 register `LUTAFSR` (0x1224) reads
0 when no LUT engine is active; FastEPD uses exactly this as a terminal wait in its 1-bit path
(`FastEPD/src/FastEPD.inl:2077-2087`, `:2125-2126`). It is a plain SPI register read, so it would
convert to a per-`loop()` poll directly. Two things to check if that day comes: whether ESPHome's
driver reads LUTAFSR at all (claimed by the driver research, not independently verified), and why
FastEPD's 2-/4-bit grayscale paths do *not* do the terminal wait — there may be a multi-LUT reason it
settles differently.

### 2. Buffer commit path — DECIDED: standard per-pixel `draw_pixel_at`
**Use the supported API. No bypass, no upstream hook.** `write_contiguous()` converts wire bytes to
`draw_pixel_at` calls; see `backend_epaper_spi.cpp`.

This is well-supported by the analysis below: there is no packed-1bpp bulk ingress anywhere in the
ESPHome display API, so no upstream change would have produced a `memcpy` path anyway — and the
bypass options all forfeit honest completion reporting and take on per-model controller sequencing.

Two properties make this cheap in practice:

- **The formats are byte-identical.** OpenDisplay MONO and `EPaperMono` agree exactly — 8 px/byte,
  MSB first, 1 = white, rows padded to a byte boundary. No inversion, no bit reversal, no remap.
- **The protocol packet is the natural `loop()` chunk.** One 244-byte packet is 1 952 pixels, well
  under a millisecond, so `write_contiguous` converts synchronously without ever blocking `loop()`.
  The 384 000-call total arrives as ~200 independently-bounded pieces.

Still worth measuring once a build exists, but it is no longer a decision input — only a check that
the whole-frame time fits inside the panel's 1 s minimum update interval.

*Retained below as the rationale, and as the record of what was rejected.*

#### Original framing: upstream a hook, or accept per-pixel conversion?
Neither driver exposes its framebuffer, and `epaper_spi`'s `SplitBuffer` is deliberately
non-contiguous with no `data()`, so even a public accessor gives no memcpy target. Either upstream
`write_frame_bytes(offset, data, len)` + `SplitBuffer::write()`, or go through `draw_pixel_at` and
eat the conversion cost. The second is shippable now; the first is what the plan's "narrow adapter"
actually assumes.

**What `epaper_spi` actually exposes for writing pixels — the full set:**

| Public on `EPaperBase` | Use to us |
|---|---|
| `void draw_pixel_at(int x, int y, Color)` — virtual | **The only positional write.** This is the whole R1 surface. |
| `void fill(Color)` / `void clear()` — virtual | Whole-buffer only, no positional control. |
| `static uint8_t color_to_bit(Color)` | Mono packing rule: `(r+g+b) >= 382 → 1`. |
| `command()` / `cmd_data()` | Raw-SPI escape hatch — **forbidden** by the plan's "no raw SPI" rule. |

Everything else is protected and unreachable: `buffer_` (the `SplitBuffer`), `buffer_length_`,
`transfer_data()`, `refresh_screen(bool)`, `is_idle_`, and the dirty bounds.

Three consequences that decide this question:

1. **`EPaperBase` does not override `draw_pixels_at`**, so bulk blits fall back to
   `Display::draw_pixels_at` (`display.cpp:54-85`), which loops `draw_pixel_at` per pixel with **no
   fast path and no watchdog feed**. Do not call it — drive `draw_pixel_at` ourselves in bounded
   per-`loop()` batches, which our "never block `loop()`" rule requires anyway.
2. **Cost for the chosen target** (`seeed-ee04-mono-4.26`, 800×480 mono): 384 000 `draw_pixel_at`
   calls per frame, each doing a rotation transform and bounds check, then `SplitBuffer::operator[]`
   with a division and a modulo (`split_buffer.cpp:104-122`) — roughly 3 ops/byte even if the buffer
   *were* exposed. Measure this before assuming it is acceptable.
3. **Access trap:** several subclasses re-declare `draw_pixel_at` as `protected`. C++ checks access
   on the static type at the call site, so calling through `EPaperBase *` compiles and still
   dispatches virtually. Store `EPaperBase *`, never the concrete subclass.

*Side note confirming open question 6:* `SplitBuffer` chunks are allocated with `RAMAllocator` default
flags — **PSRAM preferred, internal fallback** (`split_buffer.cpp:22`) — and `init()` halves the
chunk size and retries until it fits. So the driver adapts to available RAM on its own; nothing here
requires PSRAM.

#### Precedent check: what other ESPHome display drivers do (2026-08-10)

Surveyed to see whether the proposed `write_frame_bytes()` hook has precedent:

| Driver | Overrides `draw_pixels_at`? | Public raw byte-range write? | Public framebuffer accessor? |
|---|---|---|---|
| `it8951::IT8951Display` | **Yes** (`it8951.cpp:958-1023`) | No | No |
| `ili9xxx::ILI9XXXDisplay` (`display::DisplayBuffer`) | **Yes** | No | No |
| `mipi_spi::MipiSpi` | **Yes** | No — `write_display_data_` is protected | No |
| `epaper_spi::EPaperBase` | **No** — falls back to the naive base loop | No | No |

Two conclusions, both decision-relevant:

1. **`draw_pixels_at` is ESPHome's sanctioned bulk-write API.** Drivers that care about throughput
   override it; *none* expose a raw byte-range write or a public framebuffer accessor. That is a
   consistent, deliberate design choice across the whole display subsystem — so the proposed
   `write_frame_bytes()` / `SplitBuffer::write()` hook (§6.2(c) of the driver reference) **has no
   precedent and cuts against the grain.** Rate its odds of being accepted upstream accordingly. The
   idiomatic ask instead would be "override `draw_pixels_at` in `EPaperBase`", which needs no new API
   surface at all.
2. **But that would not help us**, and this is the important part: `draw_pixels_at`'s source format
   is **≥ 1 byte per pixel** — `ColorBitness` offers only `332`, `565`, `888`
   (`display_color_utils.h:6`). There is **no packed-1bpp ingress anywhere in the ESPHome display
   API.** So our 48 000-byte mono frame must expand to 384 000 bytes before any bulk path will take
   it, no matter how well optimised the override is.

**Net: for a packed 1 bpp source there is no fast supported path in ESPHome, and no plausible
upstream change creates one.** The bottleneck is the API's pixel format, not dispatch overhead. That
leaves per-pixel `draw_pixel_at` (simple, supported, measure it) versus a `cmd_data` bypass
(unsupported, but the only thing that consumes wire bytes as-is).

#### Bypass options, and why raw SPI is the worst of them

Two further options exist that skip the framebuffer entirely:

- **(d) `cmd_data(cmd, ptr, len)`** — public controller passthrough (`epaper_spi.h:67`). Streams
  bytes to the panel controller's RAM-write command. The driver still owns CS, DC, and the SPI
  transaction.
- **(e) Our own raw SPI** — register a second `spi::SPIDevice` in this component and drive the panel
  ourselves.

**(e) is strictly worse than (d) and should not be pursued.** It adds two conflicts (d) does not
have, and buys nothing:

| | (d) `cmd_data` | (e) raw SPI |
|---|---|---|
| Bus arbitration | Driver's `SPIDevice` handles it | Two devices, one bus — we must re-implement enable/disable and transaction settings |
| CS + DC pins | Driver-owned, correct by construction | **Two owners of the same pins.** DC lives in a protected member; we would have to duplicate the GPIO config and hope the two agree |
| Per-model controller command set | Ours to own | Ours to own |
| Driver overwrites our write on any `update()` | Yes | Yes |
| `is_idle()` completion honesty | Forfeited | Forfeited |

So if the framebuffer is to be bypassed at all, (d) is the way. Raw SPI only adds pin and bus
ownership fights on top of the same fundamental problems.

**Before choosing any bypass, measure.** Rough order-of-magnitude for the 800×480 mono target: the
frame is 48 000 bytes, so the SPI transfer alone is ~192 ms at 2 MHz (`data_rate` is configurable, so
this varies). The 384 000 per-pixel writes are plausibly the same order of magnitude — virtual
dispatch + rotation transform + a division and modulo each. **If the per-pixel conversion is not
substantially more expensive than the SPI transfer it feeds, every bypass option is optimising the
wrong half and should be dropped.** These are estimates, not measurements — get a real number first.
Note also the panel's minimum update interval is 1 s, which is the budget both paths sit inside.

### 3. Refresh mode — DECIDED: always FULL
**v1 performs a FULL refresh unconditionally.** This matches what the drivers actually do:
`epaper_spi::update()` takes no mode argument at all, and `it8951`'s `prepare_update_region_`
silently forces GC16 on the full-update cycle — so "select a mode" was never really on offer.

Consequences to implement:

- `0x0040` advertises **full refresh only**: `supports_full_refresh = true`,
  `supports_fast_refresh = false`, `supports_partial = PartialSupport::NONE`. A conforming client
  will then never request anything else.
- The `refresh:1` byte on `0x0072` / `0x0082` is **parsed but ignored** — we always do FULL. Accept
  rather than NACK a non-FULL request: we advertised FULL, so a client asking for FAST is
  out-of-spec, but failing the whole transfer over an ignorable hint is worse than honouring it as
  FULL. Log it at debug.
- This also neutralises the "refresh-byte encoding differs between `0x0072` and `0x0082`" plan gap:
  the two opcodes encode the value differently (`0x0072` 0=FULL/1=FAST-or-PARTIAL; `0x0082`
  0=FULL/1=FAST/2-or-absent=PARTIAL), but since we never act on it, only the *frame length* parsing
  matters — the byte's presence still shifts where an optional `new_etag` starts.

*Assumption flagged:* read from "refresh mode always full". If you meant something narrower — e.g.
still accept FAST where a driver genuinely supports it — say so and this reverts to a per-backend
capability.

### 4. First validated `epaper_spi` model — DECIDED: Seeed EE04, 4.26" mono
**Target: `model: seeed-ee04-mono-4.26`** — 800×480, class `EPaperMono`, 1 s minimum update interval
(`ESPHome_Display_Drivers_Reference.md:639`). Already in the upstream `epaper_spi` model list, so
**no upstream model work and no third backend are needed**. This is the cleanest possible target:
`EPaperMono` is the plain 1 bpp class, and mono is the least palette-sensitive scheme.

Derived values for `0x0040`:

| Field | Value | Source |
|---|---|---|
| `width` × `height` | 800 × 480 | model preset |
| `color_scheme` | `OD_COLOR_SCHEME_MONO` (0) | `EPaperMono` is 1 bpp |
| `full_frame_bytes` | 48 000 (`800 × 480 / 8`) | derived; verify against the client's encoder |
| `panel_ic_type` | `OD_PANEL_IC_EP426_800X480` (39) | `opendisplay_structs.h:605`, "Waveshare 4.26\" B/W 800x480" |

**Confirmed 2026-08-10:** `seeed-ee04-mono-4.26` is defined in `epaper_spi/models/ssd1677.py` and
**extends `waveshare-4.26in`**, adding only pin presets (cs 44, dc 10, reset 38, busy 4). So the
controller is **SSD1677** and it is literally the same panel definition as the Waveshare 4.26" —
which is exactly what `OD_PANEL_IC_EP426_800X480` (39) names. The mapping is sound.

Pins come from the model preset; **do not override them in YAML**. Note
`OD_PANEL_IC_EP426_800X480_4GRAY` (40) exists for the same panel in 2-bit grey if a grayscale
variant is ever wanted.

Everything below is retained as **background only** — it documents the previously-considered Seeed
E1001 target and no longer gates M4.

---

#### Superseded: Seeed reTerminal E1001, 800×480, 7.5" V2 panel

What the workspace pins down:

| Fact | Source |
|---|---|
| E1001 is 800×480, OpenDisplay panel IC `0x003C` | `Firmware_Unified/targets/esp32-idf/README.md:26` |
| `0x003C` → `EP75_800x480_4GRAY_GEN2` | `Firmware/src/display_service.cpp:806` |
| = `OD_PANEL_IC_EP75_800X480_4GRAY_GEN2` (60), "GDEY075T7-D2 (newer) 4-gray mode" | `opendisplay_structs.h:626` |
| 1 bpp sibling is `0x003B` → `EP75_800x480_GEN2` (59), "GEDY075-D2 (Waveshare/Xiao V2 panels)" | `:625` |
| **Controller is `BBEP_CHIP_UC81xx`** for both | `bb_epaper/src/bb_ep.inl:3229`, `:3231` |

**Problem: ESPHome `epaper_spi` has no model for a UC81xx mono 800×480 panel.** Its mono classes are
SSD1677-based (`EPaperMono`) and SSD1683-based (`EPaperSSD1683`); neither matches. So the generic
`model: ssd1677` + `dimensions:` escape hatch does **not** apply here — wrong controller, wrong init
sequence. The nearest UC81xx-family driver upstream is likely `EPaperWaveshareBWR`
(`waveshare-7.5in-bv2-bwr`, the 7.5" V2 *B* panel), but that is 3-colour, not mono. *(That the BWR
driver is UC81xx is inferred from its model name — UNVERIFIED.)*

So this is not "pick a model from the list" within `epaper_spi`.

**However — the legacy `waveshare_epaper` component already supports this panel.** Its model list
includes `7.50inV2` (800×480 mono) and `7.50inV2p` (same, with partial/fast refresh on Sept-2023+
panels), plus `7.50in`, `7.50in-bV2`, `7.50in-bV3`, `7.50in-bc`, `7.50in-hd-b`
(<https://esphome.io/components/display/waveshare_epaper/>). That is the 7.5" V2 mono family the
E1001 uses.

That turns open question 4 into a **scope decision with three options**:

| Option | Cost | Risk |
|---|---|---|
| (a) Add a **third backend** for `waveshare_epaper` | A fresh M0-equivalent analysis — its C++ class tree is entirely separate from `epaper_spi`, and its buffer/refresh/completion interface is **completely unexamined by us** | It may **block on the busy pin**, which our design forbids — see below |
| (b) Add a UC81xx mono model **upstream to `epaper_spi`** | Upstream PR + release wait | Slower, but lands where the ecosystem is going |
| (c) Fork / external driver | Fastest to a working demo | Ours to maintain forever; contradicts the plan's "no copied drivers" stance |

Note the driver reference currently instructs codegen to **reject** `waveshare_epaper` IDs
(`ESPHome_Display_Drivers_Reference.md:208-210`, `:1002`). Choosing (a) reverses that instruction.

**On "legacy": that was our word, not upstream's.** The `epaper_spi` docs page does not mention
`waveshare_epaper`, does not deprecate it, and gives no migration guidance. The real distinction is
architectural — `epaper_spi` advertises "a queue-based state machine that eliminates blocking waits
for the busy pin and provides better integration with ESPHome's async architecture". So option (a)'s
risk is **not** "it might get removed"; it is that the older component may **block on the busy pin**,
which this component cannot tolerate (we must never block in a BLE callback or in `loop()`), and
which would also deny us the `is_idle()` completion signal that makes `epaper_spi`'s `0x0073`
honest. Implied by the wording, **UNVERIFIED** in source.

*Also unverified:* nobody has looked at `waveshare_epaper`'s public C++ interface at all — buffer
access, refresh trigger, completion. Settle both before committing to (a).

**Also unresolved: mono or 4-gray?** The same physical panel has both OpenDisplay identities — `0x003B`
1 bpp and `0x003C` 4-gray — and the reference firmware runs E1001 as **4-gray**. You described it as
mono. This is not cosmetic: it sets `color_scheme` (`MONO` vs `GRAY4`), `panel_ic_type`, and the
frame byte count (48 000 vs 96 000 at 800×480), and a mismatch surfaces as
`OD_ERR_PIPE_START_SIZE_MISMATCH`. Decide before building the model→scheme table (open question 5).

### 5. Colour scheme — ANSWERED: no new YAML key needed, but a mapping table is
Neither driver has a colour-scheme key, but both force something we can derive from:

- **`epaper_spi` forces `model:`** (required, `display.py:89`). The model selects the C++ subclass,
  which fixes colour capability *and* buffer layout. So the scheme is fully determined by `model:` —
  derive it with a hand-maintained `model → ColorScheme` table, same shape as the `panel_ic_type`
  table.
- **`it8951` forces nothing, but exposes `grayscale:`** (bool, default `true`, `display.py:239-241`)
  which selects the 4bpp vs packed-1bpp buffer → `GRAY16` vs `MONO`.

**Trap: do not use the C++ `get_display_type()`.** It returns only `BINARY`/`COLOR`, which is far too
coarse — `EPaperWeAct3C` reports `BINARY` despite being a 3-colour panel, and `COLOR` lumps
JD79660 (4-colour) with Spectra E6 (6-colour). Colour scheme must come from the YAML `model:` at
codegen time, not from the driver at runtime.

*Remaining work:* reading the referenced display's `model:` requires a `FINAL_VALIDATE_SCHEMA` that
inspects the full config (the same mechanism `epaper_spi` itself uses to inject `show_test_card`),
since `cv.use_id` alone yields only the ID. Then keep the model→scheme table on the allow-list rule
already recorded: a model may only be advertised once its byte packing and colour mapping are tested.

### 6. PSRAM policy — RESOLVED
**No PSRAM requirement for `epaper_spi`.** The driver owns its framebuffer and this component adds
no staging buffer of its own, so there is nothing to allocate. The plan's "component-owned
native-layout buffer, PSRAM-preferred for large panels" is superseded — it would have duplicated
storage the driver already holds. `it8951` still forces PSRAM, but that is the *driver's* 1.31 MB
framebuffer, not ours.

### 7. Pairing/bonding — DECIDED: none. Open characteristic, unauthenticated.
**v1 ships with no BLE pairing, no bonding, and no `0x0050` session auth.** This is now an
intentional choice rather than a side effect of deferring auth, which was the only thing wrong with
it.

What that means in practice: **anyone within BLE range can write to the panel** — connect, send
`0x0070`, stream a frame, trigger a refresh. There is no credential of any kind. This matches the
reference firmware's default and keeps HA discovery frictionless.

Required follow-through, so this stays an informed choice for users too:

- [ ] State the exposure plainly in `README.md` — done; keep it there.
- [ ] Do not describe the component as "secure" or imply access control anywhere in docs or logs.
- [ ] If `0x0050` is implemented later (M6), it must **not** reuse ESPHome native-API encryption
      keys — that constraint survives this decision.

*Not chosen, recorded for completeness:* requiring an encrypted/bonded link would have blocked
unpaired writers, at the cost of a pairing step during setup and likely friction with the HA
integration's connection flow, which expects an open characteristic.

### 8. ETag — DECIDED: not implemented. No partial refresh, so no ETag state.
**We keep no `displayed_etag` and never act on `new_etag`.** ETags exist to let a client assert what
is currently on the panel before sending a partial region. With no partial refresh (`0x0076` not
implemented, `PartialSupport::NONE` advertised, refresh always FULL), there is nothing for the
mechanism to protect.

This also disposes of the plan's dangling reference: `0x0074` was told to "invalidate partial-update
state" that the plan never defined. There is no such state to invalidate.

**One thing that still must be right:** the optional trailing `new_etag:4 BE` on `0x0072` / `0x0082`
is detected **by frame length**, and the `refresh:1` byte sits in front of it. We ignore both
*values*, but the parser must still accept a frame that carries them and not treat the extra four
bytes as malformed. Cover this explicitly in host tests: `0x0072` and `0x0082` each with and without
a trailing etag, both accepted identically.

*Consequence to hold the line on:* because we never track what is displayed, this component can never
honestly advertise partial support later without adding that state first. Do not add
`PartialSupport::SUPPORTED` as a one-line change.

### 9. `0x0044` (READ_MSD) — DECIDED: promoted to v1 (M1)
Returns the same 16-byte record we must already assemble to advertise, so it is near-free. Moved
into M1 above. See [ADVERTISEMENT_Design.md](ADVERTISEMENT_Design.md).
