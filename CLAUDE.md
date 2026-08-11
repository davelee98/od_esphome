# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Status: implemented, NEVER COMPILED

All layers now exist — codegen, protocol parse/encode, config-blob synthesis, MSD advertisement,
transfer engine (direct + PIPE), bounded RX/TX queues, and both backends.

**Nothing has ever been compiled.** ESPHome is not installed here and there is no CI, so every C++
and codegen file is written against documented interfaces and unverified. Assume type errors,
signature mismatches, and wrong header paths until a build runs. Treat "it looks right" as unproven.

Two deliberate gaps, both marked in-file:

- `OpenDisplayComponent::drain_tx_()` logs instead of notifying — wiring the GATT characteristic is
  the one piece needing the `esp32_ble_server` API, and there was no way to check it.
- The MSD chip temperature is hard-coded; it needs the ESP32 internal sensor read.

No build/lint/test commands exist yet. Add them here as they land.

**Deviation from the plan worth knowing:** the plan's "Repository layout" lists a `manifest.json`
for dependencies and minimum ESPHome version. That is a Home Assistant convention — ESPHome external
components have no such file. The scaffold instead uses `DEPENDENCIES` / `AUTO_LOAD` in
`__init__.py` and `cv.require_esphome_version(...)` for the version floor.

[docs/OpenDisplay_ESPHome_Component_Plan.md](docs/OpenDisplay_ESPHome_Component_Plan.md) is the
**normative spec**, not a background note (unlike `FINDINGS.md`-style docs elsewhere in the
workspace). Read it before writing code. This file summarizes the decisions that are easy to violate
by accident; the plan is authoritative wherever the two differ.

## What this repo is

An ESPHome external component, `opendisplay:`, that lets an ESP32 running ESPHome act as an
OpenDisplay *device* — accepting frames over BLE GATT from an OpenDisplay client and driving an
e-paper panel through either the `epaper_spi` or `it8951` ESPHome display driver.

It is **not** a display arbiter. It writes its one configured display directly and exposes
busy/activity state; cooperating ESPHome automations must wait on that state before touching the
same panel. `update_interval: never` on the panel is a convention, not a lock.

This is one repo in the `/home/davelee/opendisplay` multi-repo workspace (see the workspace
`CLAUDE.md` one level up). Run git inside this folder — the workspace root is not a repository.

## Architecture

Five layers, deliberately separated so the middle three are host-testable:

| Layer | Owns | Hard constraint |
|---|---|---|
| BLE transport | GATT service/characteristic `0x2446`, RX/TX | Callbacks only bounds-check and enqueue into a fixed-capacity queue. Never allocate a frame, convert pixels, touch SPI, or wait on panel BUSY. |
| Protocol engine | Opcode parse/encode, ACK/error responses, capabilities | Transport-neutral (a future LAN/TCP transport reuses it). Capabilities are *generated from YAML/backend config*, never writable over the wire. |
| Transfer engine | Byte/sequence accounting, timeouts, one-transaction rule | No unbounded allocation, no per-packet `std::vector`, no reorder queue. |
| Backend adapter | OpenDisplay pixels → `epaper_spi` buffer or IT8951 image RAM, refresh request | Narrow interface only — no raw SPI, no private driver members, no `reinterpret_cast` shortcuts. Advertise only capabilities the selected panel actually has. |
| ESPHome integration | Busy/activity entities, triggers | Cooperative coordination, not arbitration. |

Keep `protocol*`, `protocol_types.h`, and `transfer_engine*` free of ESPHome/ESP-IDF headers so they
unit-test against a fake backend and fake transport. Planned file layout is in the plan's
"Repository layout" section (`components/opendisplay/`, `examples/`).

All slow work — backend writes, conversion, refresh polling — happens in `loop()`. Only `loop()`
mutates backend state and terminal transaction state.

## Vendored protocol headers — the wire source of truth

`components/opendisplay/opendisplay_protocol.h` and `opendisplay_structs.h` are **byte-identical
copies** of `opendisplay-protocol/src/*.h`. They are heavily annotated (`@opcode`, `@request`,
`@response`, `@errors`, `@state`, `@limits`, `@targets` per command) and answer nearly every wire
question — read them before inferring a byte layout from the plan or from prose.

Never hand-edit these copies. Change the canonical file in `opendisplay-protocol` and propagate with
`tools/sync_protocol_header.py --push` / `--check`. **Caveat:** `od_esphome` is not yet registered in
that tool's `ARTIFACTS` copy map (`opendisplay-protocol/tools/sync_protocol_header.py:85`), so
`--check` will *not* catch drift here until it is added. Re-verify with `sha256sum` against the
canonical files after any protocol bump.

## Protocol invariants that are easy to get wrong

Two full-frame transfer paths:

```
Direct write: 0x0070 start → 0x0071 data (ACK every packet) → 0x0072 end → 0x0073 complete
PIPE_WRITE:   0x0080 start → 0x0081 data (windowed)         → 0x0082 end → 0x0073 complete
```

**The plan restricts PIPE_WRITE; it does not redefine it.** The canonical protocol's `0x0081`
acknowledgement is a *selective* ACK carrying a 32-bit `ack_mask` (`PIPE_ACK_MASK_BITS 32`), and
`0x0082` emits a tail-flush SACK before the end-ACK. The plan's "cumulative ACK" is a **policy**
choice layered on that frame — emit the canonical SACK frame with a contiguous-only mask, never an
ESPHome-specific reply. Keep the distinction explicit in code and comments.

Plan-imposed v1 policy limits:

- **16** packets maximum in flight; exceeding the window is a protocol violation → error + last
  cumulative ACK + abort. Do not buffer the excess.
- ACK after every **4** consecutively accepted packets, naming the highest *contiguous* accepted
  packet only. Send a final ACK at `0x0082` if 1–3 packets are unacked.
- **Strictly in-order, no reorder queue, zero storage for out-of-order packets.** A duplicate or
  out-of-order packet is dropped and answered with the last ACK so the client resends from there.
  This is narrower than the wire format permits — deliberately, to bound RAM.

From the header (hard wire facts, not policy):

- `PIPE_MAX_FRAME` 244 bytes; `PIPE_FRAME_OVERHEAD` 3 = cmd(2) + **seq(1)** — the sequence field is a
  single byte. `PIPE_VERSION` 0x01 is negotiated in `0x0080` and is *not* the protocol version.
- Response frame is `[status][cmd_echo][data]`; `0x00` = ACK, `0xFF` = NACK.
- **NACK error codes are opcode-scoped.** `data[0]` only means something once you know the echoed
  opcode — `0x03` is `OD_ERR_PARTIAL_RECT_OOB` under `0x76` but `OD_ERR_PIPE_START_SIZE_MISMATCH`
  under `0x80`. Never decode `data[0]` in a shared helper that ignores the opcode.
- PIPE (`0x0080`–`0x0082`) is `@targets: Firmware` only — NRF54, Silabs, and NRF52811 do not
  implement it. This component is a *new* PIPE implementation, so test against the Firmware target's
  behavior, not the others.
- `0x2446` is overloaded three ways: BLE service UUID (`00002446-0000-1000-8000-00805F9B34FB`), BLE
  manufacturer ID (9286 decimal, used for discovery), and the default LAN TCP port. Don't conflate.

Other rules:

- `0x0073` is emitted only after the driver/controller confirms *physical* refresh completion —
  never at end-of-transfer. `0x0074` on refresh timeout.
- `busy` stays asserted through the physical refresh, not just while pixels arrive.
- A transfer is valid only when accepted bytes **exactly** equal the advertised full-frame count.

**Do not implement** (each rejected for a stated reason in the plan): `0x000F`, `0x0041`, `0x0042`,
`0x0045`, `0x0051`, `0x0052`, `0x0064`, `0x0065`, `0x0075`, `0x0077`, `0x0083`.
`0x0076` (partial region) and `0x0050` (AES-128 auth) are later, capability-gated additions —
authentication must not reuse ESPHome native-API encryption keys.

`0x0044` (READ_MSD) **was** on that list and has been **promoted to v1** — it returns the same
16-byte MSD record the component must already build to advertise at all. The plan doc still lists it
as excluded; the plan is stale there.

## Standing constraint: no new upstream functions

**Do not add new functions or APIs to ESPHome components.** Work within the public interfaces
`it8951` and `epaper_spi` already expose. This is a project rule, not a preference — several designs
that would otherwise be obvious are ruled out by it:

- No `write_frame_bytes()` / `SplitBuffer::write()` byte-range hook — the commit path is per-pixel
  `draw_pixel_at` (TODO open question 2).
- No `it8951::is_refreshing()` / terminal LUT-idle wait — IT8951 uses a fixed 5 s settle, and that is
  now the **permanent** answer rather than a deferral (TODO open question 1).
- No `epaper_spi::update(bool full)` — refresh is always FULL (TODO open question 3).

Where a driver cannot do what the plan assumes, the correct response is to narrow this component's
behaviour and **document the gap honestly**, never to reach into private members, drive SPI behind
the driver's back, or claim a capability we cannot deliver.

*Boundary not yet decided:* whether adding a new **panel model** (a new `models/*.py` plus, if the
controller family is new, a subclass implementing existing virtuals) counts as "a new upstream
function". It adds no API surface, so it is arguably different in kind. Moot for the current target —
`seeed-ee04-mono-4.26` is already upstream — but it decides whether panels like the E1001's UC81xx
can ever be supported. Ask before assuming.

## Codegen contract

The Python `__init__.py` picks the backend at **code-generation time** from the declared display
type: `it8951` → `IT8951Backend`, `epaper_spi` → `EpaperSPIBackend`. Exactly one display ID;
unsupported IDs must fail config validation with a clear error. Never select a backend by runtime
platform name, and never advertise blanket support for arbitrary ESPHome display components.

## Working order

M0 gates everything: identify the safe public/upstreamable hooks in the *target* ESPHome version's
`epaper_spi` and `it8951` drivers (prepare buffer, start refresh, non-blocking completion state),
pin that minimum version in `manifest.json`, and confirm both backends compile without private-member
access. Prefer landing a small upstream ESPHome hook over copying a driver.

Then: M1 skeleton + BLE + config/version + entities → M2 IT8951 direct write → M3 PIPE_WRITE engine
→ M4 `epaper_spi` adapter + buffer policy (PSRAM for large panels; fail setup loudly if storage is
insufficient) → M5 cleanup/timeouts/counters/CI → M6 capability-gated extras.

Add an `epaper_spi` panel model only after byte packing, color mapping, buffer size, and BUSY
behavior are tested for it. A backend must never claim partial refresh, compression, or a color
format just because some other OpenDisplay device supports it.

## Task tracking

[docs/TODO.md](docs/TODO.md) is the live work list, derived from the plan's milestones. **Keep it
current as part of doing the work, not as a separate chore:** check items off when they land, add
items you discover, and record blockers and decisions inline. When the plan and the TODO disagree,
the plan is the spec and the TODO is the state — fix whichever is stale.

## Conventions

Sibling Python repos in the workspace use **uv**, `mypy --strict`, `ruff`, `pylint`, and `prek`, line
length 120. Match that for the codegen module unless there's a reason not to.
