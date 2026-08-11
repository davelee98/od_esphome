# ESPHome Display Drivers Reference — `it8951` and `epaper_spi`

**Purpose:** answer the M0 gate for the `opendisplay:` external component — *can we drive these panels
through safe PUBLIC hooks, without touching private members or raw SPI?*

**Both components exist in ESPHome mainline.** Canonical homes, up front:

| Component | Docs | Source | Class | YAML |
|---|---|---|---|---|
| `it8951` | [esphome.io](https://esphome.io/components/display/it8951/) | [`esphome/components/it8951`](https://github.com/esphome/esphome/tree/2026.7.4/esphome/components/it8951) | `esphome::it8951::IT8951Display` | `display: - platform: it8951` |
| `epaper_spi` | [esphome.io](https://esphome.io/components/display/epaper_spi/) | [`esphome/components/epaper_spi`](https://github.com/esphome/esphome/tree/2026.7.4/esphome/components/epaper_spi) | `esphome::epaper_spi::EPaperBase` | `display: - platform: epaper_spi` |

Full name/import/codegen details in [§0](#0-canonical-locations-exact-names-and-yaml-form).

**Verification basis.** ESPHome is not installed on this machine. Every claim below was read from
upstream source at tag **`2026.7.4`** (latest release, published 2026-08-05) via
`raw.githubusercontent.com`. For all files cited here, `dev` was byte-identical to `2026.7.4` at the
time of writing, **except** the four `epaper_spi` files noted in
[Divergence between `2026.7.4` and `dev`](#divergence-between-20267 4-and-dev). Line numbers are for
`2026.7.4`. Source root:
`https://github.com/esphome/esphome/blob/2026.7.4/esphome/`.

---

## 0. Canonical locations, exact names, and YAML form

**Both components exist in ESPHome mainline (`esphome/esphome`).** Neither is an external component,
neither is PR-only, and neither is hypothetical. Canonical homes:

| | `it8951` | `epaper_spi` |
|---|---|---|
| **Docs URL** | <https://esphome.io/components/display/it8951/> | <https://esphome.io/components/display/epaper_spi/> |
| **Source URL** | <https://github.com/esphome/esphome/tree/2026.7.4/esphome/components/it8951> | <https://github.com/esphome/esphome/tree/2026.7.4/esphome/components/epaper_spi> |
| **Repo path** | `esphome/components/it8951/` | `esphome/components/epaper_spi/` |
| **Platform module** | `it8951/display.py` | `epaper_spi/display.py` |
| **First release** | `2026.7.0` | `2025.10.0` |

Directory listings were confirmed live against the GitHub contents API for both `dev` and tag
`2026.7.4`.

### 0.1 Exact C++ names — our scaffold is wrong on both

| Where | Scaffold guess (wrong) | **Correct** | Evidence |
|---|---|---|---|
| IT8951 C++ class | `esphome::it8951::IT8951` | **`esphome::it8951::IT8951Display`** | `it8951/it8951.h:140` |
| epaper_spi C++ class | `esphome::epaper_spi::EpaperSPI` | **`esphome::epaper_spi::EPaperBase`** | `epaper_spi/epaper_spi.h:35` |

Note the capitalisation: `EPaperBase` — capital **P**, and `EPaper*` (not `Epaper*`) for every
subclass *except* `EpaperWaveshare` (`epaper_waveshare.h:9`) and `EpaperWaveshareB`
(`epaper_waveshare_b.h:10`), which upstream spells with a lowercase `p`. Both namespaces are correct
as guessed (`it8951`, `epaper_spi`).

Fixes required in our scaffold:

- `components/opendisplay/backend_it8951.h:24` — forward declaration `class IT8951;` →
  `class IT8951Display;`; and `:31`, `:43` — `it8951::IT8951 *` → `it8951::IT8951Display *`.
- `components/opendisplay/backend_epaper_spi.h:26` — `class EpaperSPI;` → `class EPaperBase;`;
  and `:33`, `:45` — `epaper_spi::EpaperSPI *` → `epaper_spi::EPaperBase *`.
- `components/opendisplay/__init__.py:45` — `IT8951 = it8951_ns.class_("IT8951")` →
  `class_("IT8951Display")`; `:47` — `EpaperSPI = epaper_spi_ns.class_("EpaperSPI")` →
  `class_("EPaperBase")`; and the `:52-53` backend map keys accordingly.

**Hold the `epaper_spi` pointer as `EPaperBase *`, never as a concrete subclass.** The generated
variable is typed as the model's subclass (`epaper_spi/display.py:75`, `:104`), and several
subclasses re-declare `draw_pixel_at`/`fill`/`clear` under `protected:` — calling through
`EPaperBase *` is legal and dispatches virtually; calling through the subclass type does not
compile. See [§4.3](#43-public-c-interface--epaperbase).

The IT8951 has no subclasses; `IT8951Display` is both the declared and the generated type.

### 0.2 Python codegen — import path and `cv.use_id` target

| | `it8951` | `epaper_spi` |
|---|---|---|
| Python package | `esphome.components.it8951` | `esphome.components.epaper_spi` |
| Module holding the class object | `esphome.components.it8951.display` | `esphome.components.epaper_spi.display` |
| Class object name | `IT8951Display` (`it8951/display.py:58`) | `EPaperBase` (`epaper_spi/display.py:52-54`) |
| Namespace object | `it8951_ns = cg.esphome_ns.namespace("it8951")` (`display.py:57`) | `epaper_spi_ns = cg.esphome_ns.namespace("epaper_spi")` (`display.py:51`) |
| Declared bases | `display.Display, spi.SPIDevice` | `cg.PollingComponent, spi.SPIDevice, display.Display` |
| `cv.use_id(...)` target | `cv.use_id(IT8951Display)` | `cv.use_id(EPaperBase)` |

Option A — import the upstream objects (canonical, keeps the real base list):

```python
from esphome.components.it8951.display import IT8951Display
from esphome.components.epaper_spi.display import EPaperBase
```

Option B — re-declare locally (no cross-component import; **this also works**):

```python
it8951_ns = cg.esphome_ns.namespace("it8951")
IT8951Display = it8951_ns.class_("IT8951Display")
epaper_spi_ns = cg.esphome_ns.namespace("epaper_spi")
EPaperBase = epaper_spi_ns.class_("EPaperBase")
```

Option B is safe because `MockObjClass.inherits_from()` compares **stringified names**, not object
identity (`cpp_generator.py:1169-1172`) — a locally declared
`esphome::epaper_spi::EPaperBase` matches upstream's. It matters for `epaper_spi` specifically:
the generated ID's type is the model subclass, declared as
`epaper_spi_ns.class_(model.class_name, EPaperBase)` (`epaper_spi/display.py:75`), so
`cv.use_id(EPaperBase)` resolves through `inherits_from` — which is exactly the string comparison
above. Prefer Option B for `epaper_spi`: importing its `display.py` also imports
`esphome.components.mipi` and dynamically imports every module under `models/`
(`epaper_spi/display.py:8-12`, `:57-59`), which is heavier than we need.

### 0.3 YAML inclusion form — both are `display:` platforms

Neither component is a top-level block. Each component directory has a `display.py` platform module
and an `__init__.py` containing only `CODEOWNERS` (`it8951/__init__.py` is 52 bytes,
`epaper_spi/__init__.py` is 31 bytes), which is the ESPHome signature of a platform-only component.
Both schemas extend `display.FULL_DISPLAY_SCHEMA` and call `display.register_display(var, config)`
(`it8951/display.py:209`, `:365`; `epaper_spi/display.py:80`, `:201`).

```yaml
display:
  - platform: it8951      # or: epaper_spi
    id: od_panel
    model: ...
```

**Our example YAMLs' `display: - platform:` assumption is correct.** There is no
`it8951:` or `epaper_spi:` top-level block, and `model:` is required for both.

---

## 1. Findings / risks (read this first)

### 1.1 M0 verdict at a glance

| Requirement | `it8951` | `epaper_spi` |
|---|---|---|
| **R1** Prepare/load full frame, contiguous byte range at offset | **NOT SATISFIED** | **NOT SATISFIED** |
| **R2** Start refresh, mode selectable, returns quickly | **PARTIAL** — `update_mode()` is public and returns fast, but the driver can silently override the mode | **NOT SATISFIED** for mode; the "returns quickly" half is satisfied by `update()` |
| **R3** Non-blocking completion poll from `loop()` | **PARTIAL** — `Component::is_idle()` works but signals *commands issued*, not *physical refresh done* | **SATISFIED** — `Component::is_idle()` genuinely covers physical completion, **iff `busy_pin` is configured** |

**Net: M0 does not pass on public APIs alone.** Three small upstream hooks would close it — see
[§6.2](#62-minimal-upstream-hooks-that-would-close-m0). None of the gaps require a driver fork or
private-member access; all three are additive, low-risk, and independently upstreamable.

### 1.2 The five things that will surprise us

1. **The plan's IT8951 data path does not exist.** The plan says the IT8951 backend should
   "stream chunks through a narrow adapter into IT8951 image RAM"
   (`docs/OpenDisplay_ESPHome_Component_Plan.md:137`). The upstream driver **always** allocates a
   full MCU-side framebuffer in `setup()` and streams *from that buffer* into controller image RAM
   during its own update phase (`it8951.cpp:329-336`, `it8951.cpp:593-626`). There is no public
   (or even private) entry point that pushes caller bytes straight to `TCON_LD_IMG_AREA`. The
   framebuffer is unconditional. **Budget for it:** 960×540 4bpp = 259 200 B; 1872×1404 4bpp =
   1 314 144 B → PSRAM mandatory on the big Seeed panels. (The allocator does prefer PSRAM
   automatically; see [§3.5](#35-buffer-model).)

2. **`it8951` never confirms physical refresh completion.** `Phase::UPDATE_REFRESH` is documented
   in-code as *"Fire-and-forget: don't block here waiting for the refresh to complete"*
   (`it8951.cpp:277-288`). The LUT-idle poll (`LUTAFSR`) runs at the **start of the next** refresh
   (`it8951.cpp:430-434`), not at the end of this one. So the component returns to `IDLE` — and
   `Component::is_idle()` goes true — while the panel is still physically refreshing. Emitting
   OpenDisplay `0x0073` there would violate `CLAUDE.md`'s rule that `0x0073` follows *physical*
   completion. The HW_RDY/BUSY pin does **not** substitute: it is the SPI host-ready line, and the
   driver deliberately proceeds while the LUT engine runs.

3. **`epaper_spi` has no per-refresh mode selection at all.** `update()` takes no arguments
   (`epaper_spi.h:74`); full-vs-partial is decided internally from `update_count_ != 0` against
   `full_update_every_` (`epaper_spi.cpp:206`, `epaper_spi.cpp:226-227`). `refresh_screen(bool
   partial)` is a protected pure virtual. Our `RefreshRequest` therefore cannot be honoured on this
   backend in v1 — advertise **full refresh only**, exactly one mode.

4. **Two YAML landmines will silently destroy our frame.** Both are in shared `display` plumbing,
   not in either driver:
   - `Display::do_update_()` calls `this->clear()` first when `auto_clear_enabled_` is true, and
     that flag defaults to **true whenever a `lambda:` or `pages:` is present**
     (`display/display.cpp:691-694`, `display/__init__.py:132-137`, `display/display.h:794`).
     `update()`/`update_mode()` both call `do_update_()` (`it8951.cpp:258`, `epaper_spi.cpp:198`),
     so a stray auto-clear wipes everything we just wrote.
   - If **no** `lambda:` and **no** `pages:` are configured and LVGL is absent, both components'
     `FINAL_VALIDATE_SCHEMA` **force-inject `show_test_card: True`**
     (`it8951/display.py:348-353`, `epaper_spi/display.py:164-170`) — an unconditional assignment
     that overwrites an explicit `show_test_card: false`. `do_update_()` then paints a test card
     over our buffer (`display/display.cpp:695-697`). The schema-level guard that normally rejects
     `show_test_card` + `update_interval: never` (`display/__init__.py:86-94`) runs during
     `CONFIG_SCHEMA`, *before* final validation injects the flag, so it does not fire.

   Safe recipe in [§7](#7-recommended-yaml-for-the-opendisplay-backend).

5. **`Component::is_idle()` is the only public completion signal, and it is a proxy.** It is public
   on `Component` (`core/component.h:215`) and returns true exactly when the component's loop has
   been disabled (`core/component.cpp:255-261`). Both drivers `disable_loop()` on reaching their
   IDLE state (`it8951.cpp:48-50` and `181-184`; `epaper_spi.cpp:186-188` and `253-255`), so it is a
   usable, allocation-free, non-blocking poll. It is *not* part of either driver's contract, though
   — it is an emergent property of their loop management, and nothing upstream promises to keep it.
   Treat it as a v1 expedient, and push hook (c) in §6.2 to make it explicit.

### 1.3 Name check against our plan doc

| Our name | Upstream reality | Verdict |
|---|---|---|
| Component `it8951` | `esphome/components/it8951/`, mainline, `display:` platform | **correct** |
| Component `epaper_spi` | `esphome/components/epaper_spi/`, mainline, `display:` platform | **correct** |
| Namespace `it8951` / `epaper_spi` | `esphome::it8951` / `esphome::epaper_spi` | **correct** |
| Scaffold class `it8951::IT8951` | **`it8951::IT8951Display`** (`it8951.h:140`) | **WRONG — fix** |
| Scaffold class `epaper_spi::EpaperSPI` | **`epaper_spi::EPaperBase`** (`epaper_spi.h:35`) | **WRONG — fix** |

Component and namespace names are right; **both C++ class names in our scaffold are wrong.**
Exact fixes, file by file, in [§0.1](#01-exact-c-names--our-scaffold-is-wrong-on-both).
Two adjacent facts worth recording:

- **`waveshare_epaper`** still exists in `2026.7.4` and is a *different* component with a different
  C++ class tree — not a rename in place. Do not accept its IDs in our codegen **as currently
  scoped** (see the caveat below).

  **Correction (2026-08-10):** an earlier revision of this line called it "legacy" and `epaper_spi`
  its "successor". That is **not an upstream designation** — the `epaper_spi` docs page does not
  mention `waveshare_epaper` at all, does not deprecate it, and offers no migration guidance. What
  the page *does* state is the architectural difference: `epaper_spi` uses "a queue-based state
  machine that eliminates blocking waits for the busy pin and provides better integration with
  ESPHome's async architecture". Treat the distinction as **architectural, not lifecycle**.

  That difference matters to us more than any deprecation would: the non-blocking state machine is
  precisely why `epaper_spi` can report completion via `is_idle()` (R3 below). Whether
  `waveshare_epaper` blocks on the busy pin — and is therefore unusable under our
  "never block in a BLE callback or `loop()`" rule — is **UNVERIFIED**; it is implied by the wording
  above but has not been checked in source.
- `epaper_spi` is **not** one C++ class — it is a base class plus ten concrete subclasses with
  *different buffer layouts*. "Support `epaper_spi`" is not a single integration; it is
  per-subclass. See [§4.6](#46-buffer-model-per-subclass).

### 1.4 Minimum ESPHome version to pin

| Component | First release containing it | Evidence |
|---|---|---|
| `epaper_spi` | **2025.10.0** | present at tag `2025.10.0`, HTTP 404 at `2025.9.3` |
| `it8951` | **2026.7.0** | present at `2026.7.0b1`, HTTP 404 at `2026.6.2` |
| `Component::is_idle()` | **2025.11.0** | absent at `2025.10.0`, present at `2025.11.0` |

**Pin `>= 2026.7.0` in `manifest.json`** — it is the floor for `it8951`, and it covers `epaper_spi`
and `is_idle()` too. Verified/documented against `2026.7.4`.

---

## 2. Shared ESPHome plumbing both drivers inherit

Neither driver uses `DisplayBuffer`. Both derive from `display::Display`, which is itself a
`PollingComponent` (`display/display.h:317`).

```
esphome::Component
  └── esphome::PollingComponent                       core/component.h:512
        └── esphome::display::Display                 display/display.h:317
              ├── esphome::it8951::IT8951Display      + spi::SPIDevice<…>    it8951.h:140-142
              └── esphome::epaper_spi::EPaperBase     + spi::SPIDevice<…>    epaper_spi.h:35-37
                    └── (10 concrete subclasses)
```

### 2.1 `Display` members we can legitimately use

| Member | Access | Virtual | Notes |
|---|---|---|---|
| `void fill(Color)` | public | virtual | `display.h:320`; overridden by both drivers with a fast whole-buffer `memset` path |
| `void clear()` | public | virtual | `display.h:322`; both drivers alias it to fill-white |
| `int get_width()` / `get_height()` | public | virtual | `display.h:325-327`; **rotation-applied** |
| `int get_native_width()` / `get_native_height()` | public | non-virtual | `display.h:330-332`; pre-rotation panel geometry |
| `void draw_pixel_at(int x, int y, Color)` | public | pure virtual | `display.h:338` |
| `void draw_pixels_at(…11 args…)` | public | virtual | `display.h:359-361`; naive base impl loops `draw_pixel_at` (`display.cpp:54-85`) |
| `void set_rotation(DisplayRotation)` | public | virtual | `display.h:706`; both drivers override to recompute their transform |
| `DisplayRotation get_rotation() const` | public | non-virtual | `display.h:711` |
| `DisplayType get_display_type()` | public | pure virtual | `display.h:716`; `BINARY` / `GRAYSCALE` / `COLOR` (`display.h:127-131`) |
| `void set_auto_clear(bool)` | public | non-virtual | `display.h:709` |
| `void do_update_()` | **protected** | non-virtual | `display.h:774`; called by each driver's own update path |
| `bool auto_clear_enabled_` | **protected** | — | `display.h:794`, **defaults `true`** |

`ColorBitness` has only three values: `COLOR_BITNESS_888 = 0`, `COLOR_BITNESS_565 = 1`,
`COLOR_BITNESS_332 = 2` (`display/display_color_utils.h:6`). **There is no 8-bit-grayscale
bitness.** A one-byte-per-pixel source is interpreted as RGB332 by
`ColorUtil::to_color` (`display_color_utils.h:11-46`) — three bits red, three green, two blue. This
is the single biggest constraint on using `draw_pixels_at` as a bulk-load path; see
[§3.4](#34-what-a-public-bulk-load-through-draw_pixels_at-would-actually-cost).

### 2.2 `Component` loop-state helpers (the R3 mechanism)

| Member | Access | Source |
|---|---|---|
| `bool is_idle() const` | **public** | `core/component.h:215` — true iff state == `COMPONENT_STATE_LOOP_DONE` |
| `bool is_ready() const` | public | `core/component.h:274` — SETUP/LOOP/LOOP_DONE; *not* a refresh signal |
| `bool is_failed() const` | public | `core/component.h:272` |
| `void enable_loop()` | public | `core/component.h:248` |
| `void disable_loop()` | public | `core/component.h:238`; impl `core/component.cpp:255-261` sets `LOOP_DONE` |

### 2.3 `FULL_DISPLAY_SCHEMA` keys both components inherit

From `display/__init__.py:79-124`:

| Key | Type | Default | Notes for us |
|---|---|---|---|
| `id` | ID | generated | required for us |
| `lambda` | lambda | — | mutually exclusive with `pages` |
| `pages` | list | — | mutually exclusive with `lambda` |
| `rotation` | 0/90/180/270 | 0 | applied per-pixel by both drivers |
| `auto_clear_enabled` | bool | *unspecified* → `true` if `lambda`/`pages` present, else `false` | **must be explicitly `false` for us** |
| `show_test_card` | bool | — | see landmine #4 above |
| `on_page_change` | automation | — | unused by us |
| `update_interval` | time | see per-component | `never` → `SCHEDULER_DONT_RUN`; scheduler then never registers the poll (`core/scheduler.cpp:118-125`) |

`update_interval: never` works and is honoured: `cpp_helpers.py:173-174` emits
`set_update_interval()` only when the key is present, and the scheduler drops
`SCHEDULER_DONT_RUN` timers outright.

---

## 3. Component `it8951`

### 3.1 Identity

| Field | Value |
|---|---|
| **Docs URL** | <https://esphome.io/components/display/it8951/> |
| **Source URL** | <https://github.com/esphome/esphome/tree/2026.7.4/esphome/components/it8951> |
| Where it lives | **ESPHome mainline** (`esphome/esphome`) — not an external component, not a PR |
| Component name | `it8951` (platform of `display:`, i.e. `display: - platform: it8951`) |
| Upstream path | `esphome/components/it8951/` — `__init__.py`, `display.py`, `it8951.h`, `it8951.cpp`, `it8951_defs.h` |
| C++ namespace | `esphome::it8951` (`it8951.h:15`) |
| Main class | **`IT8951Display`** (`it8951.h:140`) — *not* `IT8951` |
| Python import | `from esphome.components.it8951.display import IT8951Display` |
| `cv.use_id` target | `cv.use_id(IT8951Display)` |
| Bases | `display::Display`, `spi::SPIDevice<BIT_ORDER_MSB_FIRST, CLOCK_POLARITY_LOW, CLOCK_PHASE_LEADING, DATA_RATE_2MHZ>` (`it8951.h:140-142`) |
| Python class binding | `it8951_ns.class_("IT8951Display", display.Display, spi.SPIDevice)` (`display.py:58`) |
| Action class | `IT8951UpdateAction` (`it8951.h:354`), action `it8951.update` (`display.py:416-433`, `synchronous=True`) |
| Setup priority | `setup_priority::PROCESSOR` (`it8951.h:154`) |
| First release | **2026.7.0** |
| Docs | <https://esphome.io/components/display/it8951/> |
| Other symbols | `it8951_defs.h`: `DevInfo`, `UpdateMode`, register/command constants, `TRANSFORM_*` |
| Quirk | `AUTO_LOAD = ["split_buffer"]` (`display.py:39`) but no `split_buffer` symbol appears in `it8951.h`/`.cpp` — an apparently vestigial auto-load. |

### 3.2 YAML configuration

Schema is model-driven (`display.py:191-293`), extending `display.FULL_DISPLAY_SCHEMA` +
`spi.spi_device_schema(cs_pin_required=False, default_mode="MODE0")`.

| Key | Type | Default | Req. | Source | Matters to us |
|---|---|---|---|---|---|
| `model` | enum | — | **yes** | `display.py:218` | picks pin/dimension/VCOM presets |
| `dimensions` | `{width,height}` | model preset | req. for generic `it8951` | `display.py:197-207` | frame geometry |
| `cs_pin` / `busy_pin` / `reset_pin` | pin | model preset | req. if no preset | `display.py:284-292` | `busy_pin` = HW_RDY, HIGH = ready |
| `enable_pin` | list of pins | model preset or `[]` | no | `display.py:272-274` | board power rails |
| `rotation` | 0/90/180/270 | `0` | no | `display.py:219` | ✔ affects `get_width()`/`get_height()` |
| `transform` | `{mirror_x, mirror_y, swap_xy}` | model preset | no | `display.py:222-228` | ✔ combined with rotation |
| `update_interval` | time | **`cv.UNDEFINED`** | no | `display.py:220` | ✔ set `never` explicitly (see note below) |
| `full_update_every` | 1–255 | **`30`** | no | `display.py:221` | ✔ **overrides our requested mode** every Nth update |
| `update_mode` | enum | unset → `GC16` | no | `display.py:269` | default waveform if we call `update()` |
| `grayscale` | bool | `true` | no | `display.py:239-241` | ✔ selects 4bpp vs packed 1bpp buffer |
| `dithering` | bool | `true` | no | `display.py:245-247` | mono only; Bayer 4×4 |
| `invert_colors` | bool | `false` | no | `display.py:229-231` | |
| `sleep_when_done` | bool | model preset | no | `display.py:232-234` | `TCON_SLEEP` after each update |
| `vcom` | 0–5000 (mV) | `2300` or model | no | `display.py:248-250` | |
| `vcom_register` | `0x0001`/`0x0002` | `0x0001` or model | no | `display.py:251-254` | |
| `force_temperature` | −40…85 °C | model preset | only offered if model sets one | `display.py:255-264` | |
| `use_legacy_dpy_area` | bool | `false` or model | no | `display.py:265-268` | `DPY_AREA 0x34` vs `DPY_BUF_AREA 0x37` |
| `reset_duration` | ≤ 500 ms | C++ `10` ms | no | `display.py:275-278`, `it8951.h:318` | |
| `data_rate`, `spi_id`, … | from `spi_device_schema` | model preset | no | `display.py:210-214` | |

**`update_interval` note.** The schema default is `cv.UNDEFINED` (`display.py:220`), which means the
key is *omitted* when not given, so `set_update_interval()` is never emitted
(`cpp_helpers.py:173-174`) and the C++ `PollingComponent()` default `SCHEDULER_DONT_RUN` applies
(`core/component.h:512`). The docs page's stated "1 min default" does **not** match the code path.
Set `update_interval: never` explicitly anyway — it is self-documenting and immune to future
schema changes.

**Supported `model:` values** (`display.py:122-172`):

| `model` | W×H | Preset pins | VCOM | Other presets |
|---|---|---|---|---|
| `it8951` (generic) | — (`dimensions` required) | none | 2300 | `sleep_when_done: true`, 12 MHz |
| `m5stack-m5paper` | 960×540 | busy 27, reset 23, cs 15 | 2300 | `sleep_when_done: true`, 20 MHz |
| `seeed-reterminal-e1003` | 1872×1404 | busy 13, reset 12, cs 10, enable [21, 11] | 1400 | `vcom_register: 0x0002`, `force_temperature: 25`, `mirror_x`, 20 MHz |
| `seeed-ee03` | 1872×1404 | busy 4, reset 38, cs 44 | 1400 | `sleep_when_done: false`, 4 MHz |

### 3.3 Public / protected C++ interface

**Public — lifecycle** (`it8951.h:150-154`)

```cpp
void setup() override;
void loop() override;
void dump_config() override;
void on_safe_shutdown() override;
float get_setup_priority() const override;   // setup_priority::PROCESSOR
```

**Public — config setters, codegen-facing** (`it8951.h:157-191`) — all non-virtual except
`set_rotation`:

```cpp
void set_reset_pin(GPIOPin *);            void set_busy_pin(GPIOPin *);
void set_enable_pins(std::vector<GPIOPin *>);
void set_reset_duration(uint32_t ms);     void set_full_update_every(uint8_t n);
void set_invert_colors(bool);             void set_sleep_when_done(bool);
void set_vcom(uint16_t mv);               void set_vcom_register(uint16_t selector);
void set_force_temperature(int16_t c);    void set_use_legacy_dpy_area(bool);
void set_grayscale(bool);                 void set_dithering(bool);
void set_update_mode(uint16_t m);         void set_transform(uint8_t t);
void set_rotation(DisplayRotation) override;   // virtual
```

`set_full_update_every(n)` also seeds `partial_update_count_ = n` (`it8951.h:161-167`) so the first
update after boot is always a full GC16.

**Public — display / refresh API** (`it8951.h:194-206`) — the surface that matters to us:

```cpp
void update() override;                        // virtual; PollingComponent hook. Uses default_update_mode_ or GC16
void update_mode(UpdateMode mode);             // NON-virtual. Explicit waveform selection  <-- R2 hook
DisplayType get_display_type() override;       // GRAYSCALE if grayscale_, else BINARY
void fill(Color) override;                     // virtual
void clear() override;                         // virtual; fill(WHITE)
void draw_pixel_at(int x, int y, Color) override;                                    // virtual
void draw_pixels_at(int x_start, int y_start, int w, int h, const uint8_t *ptr,
                    ColorOrder order, ColorBitness bitness, bool big_endian,
                    int x_offset, int y_offset, int x_pad) override;                 // virtual  <-- best R1 hook
int get_width() override;   int get_height() override;                               // virtual, rotation-applied
```

**Protected — everything else.** Notably, *all of these are unreachable to us*
(`it8951.h:208-351`): `get_width_internal()`, `get_height_internal()`, `apply_transform_`,
`rotate_coordinates_`, `reset_dirty_region_`, `compute_row_width_`, `set_mono_pixel_`,
`set_gray_pixel_`, `write_pixel_native_`, the whole op-queue machinery
(`enqueue_`/`prepend_`/`is_busy_`/`process_op_`/`advance_phase_`/`set_phase_`/`start_update_`), all
`spi_*` primitives, all `op_*` compound ops, all `enqueue_*_` phase builders,
`prepare_update_region_`, `recover_`, and every state member — including
**`uint8_t *buffer_`** (`it8951.h:314`), `buffer_length_` (`:313`), `row_width_` (`:312`),
`phase_` (`:280`), `active_mode_` (`:290`), `initialised_` (`:293`), and the dirty-region bounds
(`:338`).

**Automation surface.** `it8951.update` action with optional templatable `mode:`
(`display.py:416-433`); `IT8951UpdateAction::play` calls `display_->update_mode(mode)` or
`display_->update()` (`it8951.h:360-368`). **No triggers, no conditions, no `on_refresh_*`.**

### 3.4 What a public bulk-load through `draw_pixels_at` would actually cost

`IT8951Display::draw_pixels_at` (`it8951.cpp:958-1023`) is a genuine optimisation over the base
class — it skips the per-pixel clipping test and dirty-box clamp, writes natively, and expands the
dirty box once from the transformed block corners. But it is still **per-pixel conversion**:
every source pixel goes through `ColorUtil::to_color()` then `write_pixel_native_()`
(`it8951.cpp:1001-1002`), which for grayscale runs `color_to_nibble()` — a Rec.601 luma computation
per pixel (`it8951.cpp:877-894`). There is no `memcpy` path from caller bytes to `buffer_`.

Practical consequences for an OpenDisplay 4bpp frame:

- **1 byte/pixel is not grayscale.** The `default:` branch takes `ptr[source_idx]` verbatim
  (`it8951.cpp:993`) and hands it to `ColorUtil::to_color(value, order, bitness)`. With the only
  1-byte bitness being `COLOR_BITNESS_332`, that gives ≤ 8 distinct luma steps, not 16. Loading a
  16-level frame through 1 byte/pixel **loses half the greyscale**.
- **To preserve all 16 levels you must feed `COLOR_BITNESS_888`** — a 3 bytes/pixel scratch row.
  Additional trap: `color_to_nibble()`'s fast grayscale path requires `color.w == 0xFF`
  (`it8951.cpp:880`), but `ColorUtil::to_color()` leaves `w = 0`, so every pixel falls through to
  the luma branch. Encode nibble *n* as `r = g = b = n * 16` (not `n * 17`) — `quantize_8bit_to_nibble`
  computes `(v + 8) >> 4` (`it8951.cpp:872-875`), which round-trips `n*16` exactly for all
  *n* ∈ [0, 15] but is off by one for `n*17` when *n* ≥ 8.
- **Cost:** ~2.63 M pixel conversions for a 1872×1404 frame, plus a 3× expansion buffer per row
  (5 616 B/row at 888). `App.feed_wdt()` is called once per row (`it8951.cpp:973`), so it will not
  trip the watchdog, but it is nowhere near the "write contiguous byte range" the plan assumes, and
  it must be chunked across `loop()` iterations by us.

### 3.5 Buffer model

| Property | Value | Source |
|---|---|---|
| Storage | `uint8_t *buffer_`, **protected**, no accessor | `it8951.h:314` |
| Allocated in | `setup()`, once, before controller init completes | `it8951.cpp:329-336` |
| Allocator | `RAMAllocator<uint8_t>{}` — default flags `ALLOC_INTERNAL \| ALLOC_EXTERNAL`, **external (PSRAM) tried first**, internal fallback | `it8951.cpp:331`, `core/helpers.h:2071-2091`, `:2172-2184` |
| Zeroing | allocator does not zero; `setup()` calls `fill(Color::WHITE)` | `it8951.cpp:337-339` |
| Alloc failure | `mark_failed("Failed to allocate IT8951 framebuffer")`, then returns | `it8951.cpp:333-336` |
| `buffer_length_` | `row_width_ * height` | `it8951.cpp:330`, `it8951.h:146` |

**Row stride and packing** (`compute_row_width_`, `it8951.h:225-228`):

| Mode | `row_width_` (bytes) | Packing | Source |
|---|---|---|---|
| `grayscale: true` (4bpp) | `(width + 1) / 2` | 2 px/byte. **Even x in the high nibble, odd x in the low nibble.** | `set_gray_pixel_`, `it8951.cpp:1044-1053` |
| `grayscale: false` (1bpp) | `((width + 15) / 16) * 2` | 8 px/byte, grouped in 16s. **Within a 16-px group the high byte (px 8..15) precedes the low byte (px 0..7)**; bit index = `x & 7`, **LSB = lowest x**. Set bit = black. | `set_mono_pixel_`, `it8951.cpp:1025-1042` |

The 1bpp layout is deliberately the *exact wire order* the controller expects for the 8bpp-load /
1bpp-display trick, so rows stream verbatim (`it8951.cpp:614-621`). Note it is **not** the
conventional MSB-first packing — an OpenDisplay mono frame will need a byte-swap within each
16-pixel group *and* a bit reversal within each byte.

Colour mapping (both modes) is by Rec.601 luma, `luma = (77·r + 150·g + 29·b + 128) >> 8`, then
`nibble = (luma + 8) >> 4` clamped to `0x0F` (`it8951.cpp:872-894`). Mono adds a 4×4 Bayer threshold
when `dithering: true` (`it8951.cpp:901-904`, `:947-954`).

Sizes: 960×540 grayscale = **259 200 B**; 1872×1404 grayscale = **1 314 144 B**; 1872×1404 mono
(`row_width_` = 234) = **328 536 B**.

### 3.6 Refresh model

**Modes** — `enum UpdateMode : uint16_t` (`it8951_defs.h:139-149`):

| Enum | Value | YAML alias |
|---|---|---|
| `UPDATE_MODE_INIT` | 0 | `INIT` |
| `UPDATE_MODE_DU` | 1 | `DU`, `FAST` |
| `UPDATE_MODE_GC16` | 2 | `GC16`, `FULL` |
| `UPDATE_MODE_GL16` | 3 | `GL16` |
| `UPDATE_MODE_GLR16` | 4 | `GLR16` |
| `UPDATE_MODE_GLD16` | 5 | `GLD16` |
| `UPDATE_MODE_DU4` | 6 | `DU4` |
| `UPDATE_MODE_A2` | 7 | `A2` |
| `UPDATE_MODE_NONE` | 8 | (sentinel, rejected by `update_mode()`) |

YAML mapping at `display.py:67-78`. Waveform semantics documented at `it8951_defs.h:72-138`.

**Trigger and blocking.** `update_mode(mode)` → `start_update_(mode)` (`it8951.cpp:779-787`).
`start_update_` either sets `Phase::UPDATE_PREPARE` + `enable_loop()` + one synchronous
`advance_phase_()`, or coalesces into `update_pending_` (`it8951.cpp:754-767`). The synchronous
`advance_phase_()` call from `UPDATE_PREPARE` runs `do_update_()` and `prepare_update_region_()`
inline — bounded work, no SPI, no `delay()` — then enqueues ops and returns. **All SPI, all waiting,
and all row streaming happen in `loop()`.** So yes, it returns quickly.

**Mode is not honoured verbatim.** `prepare_update_region_(UpdateMode &mode)` mutates the caller's
mode (`it8951.cpp:677-745`):

- if `partial_update_count_ >= full_update_every_` → **`mode = UPDATE_MODE_GC16`** and the region is
  forced to the whole panel (`it8951.cpp:679-686`). With the default `full_update_every: 30`, one
  update in 30 ignores our request — and the counter is seeded so the **first** update after boot
  always does (`it8951.h:161-167`).
- on a non-full update, `GC16` on a *monochrome* panel is silently downgraded to `DU`
  (`it8951.cpp:737-738`).
- partial X extent is snapped to a 32-pixel boundary (`it8951.cpp:695-700`), and the driver reports
  `draw_rounding=32` to LVGL (`display.py:330`).

**Region is the dirty box, not the whole frame.** Unless the full-update cycle trips, only
`x_low_..x_high_ × y_low_..y_high_` is transferred and refreshed. With `auto_clear_enabled: false`
and a full-frame write from us, the dirty box will be the full panel anyway (our writes touch every
pixel), so this is benign — but it means the *transfer* is not deterministic in size.

**BUSY handling — non-blocking, and this is the driver's headline feature.** `loop()` gates every
SPI op on HW_RDY (`it8951.cpp:60-75`), with a 5 s `BUSY_TIMEOUT_MS` (`it8951.h:277`) that triggers
`recover_()` (up to 3 hardware resets, then `mark_failed`, `it8951.cpp:791-827`). The only
in-transaction spin is `wait_for_hardware_ready()`, capped at **50 µs** (`it8951.cpp:471-483`).
Row streaming is time-sliced to 20 ms per op (`it8951.cpp:17`, `:620-622`) and a
`HighFrequencyLoopRequester` runs while non-IDLE (`it8951.h:285`, `it8951.cpp:164-168`).
`delay()` appears **nowhere** in `it8951.cpp`.

**But completion is never confirmed.** `Phase::UPDATE_REFRESH` advances to `UPDATE_SLEEP` the moment
`DPY_BUF_ARGS` is enqueued (`it8951.cpp:277-288`):

> *"Fire-and-forget: don't block here waiting for the refresh to complete. The next update's
> pre-display LUT-idle poll (and the HW_RDY-gated `TCON_SLEEP`) wait as needed…"*

The `LUTAFSR` poll (`CMD(TCON_REG_RD) → WRITE_W(LUTAFSR) → READ_WORD → CHECK_LUT_IDLE`, re-enqueued
every 5 ms until zero) is built by `enqueue_update_refresh_()` (`it8951.cpp:428-444`) and consumed
by `op_check_lut_idle_()` (`it8951.cpp:651-662`) — i.e. it runs **before** issuing the display
command, guarding against the *previous* refresh, not confirming the current one.

**Durations.** Not documented anywhere in-tree. The docs page states only that on the supported
Seeed panels "only `GC16` and `DU` have been verified to render correctly". Typical GC16 ≈ hundreds
of ms — **UNVERIFIED**, must be measured on our hardware.

---

## 4. Component `epaper_spi`

### 4.1 Identity

| Field | Value |
|---|---|
| **Docs URL** | <https://esphome.io/components/display/epaper_spi/> |
| **Source URL** | <https://github.com/esphome/esphome/tree/2026.7.4/esphome/components/epaper_spi> |
| Where it lives | **ESPHome mainline** (`esphome/esphome`) — not an external component, not a PR |
| Component name | `epaper_spi` (platform of `display:`, i.e. `display: - platform: epaper_spi`) |
| Upstream path | `esphome/components/epaper_spi/` (25 files + `models/` package) |
| C++ namespace | `esphome::epaper_spi` (`epaper_spi.h:8`) |
| Base class | **`EPaperBase`** (`epaper_spi.h:35`) — *not* `EpaperSPI`; note capital **P** |
| Python import | `from esphome.components.epaper_spi.display import EPaperBase` (or re-declare locally — see §0.2) |
| `cv.use_id` target | `cv.use_id(EPaperBase)` — resolves the model subclass via `inherits_from` |
| Bases | `display::Display`, `spi::SPIDevice<BIT_ORDER_MSB_FIRST, CLOCK_POLARITY_LOW, CLOCK_PHASE_LEADING, DATA_RATE_2MHZ>` (`epaper_spi.h:35-37`) |
| Python class binding | `epaper_spi_ns.class_("EPaperBase", cg.PollingComponent, spi.SPIDevice, display.Display)` (`display.py:52-54`) — the *generated* variable is typed as the concrete subclass (`display.py:75`, `:104`) |
| Setup priority | `setup_priority::PROCESSOR` (`epaper_spi.cpp:58`) |
| Depends / auto-loads | `DEPENDENCIES = ["spi"]`, `AUTO_LOAD = ["split_buffer"]` (`display.py:45-46`) |
| First release | **2025.10.0** |
| Docs | <https://esphome.io/components/display/epaper_spi/> |
| **No** automation actions/triggers | verified: no `automation.register_action` anywhere in the component |

**Concrete subclasses at 2026.7.4:**

| Class | Header | Direct base | `DisplayType` |
|---|---|---|---|
| `EPaperMono` | `epaper_spi_mono.h:9` | `EPaperBase` | `BINARY` |
| `EPaperSSD1683` | `epaper_spi_ssd1683.h:9` | `EPaperMono` | `BINARY` |
| `EpaperWaveshare` | `epaper_waveshare.h:9` | `EPaperMono` | `BINARY` |
| `EPaperWeAct3C` | `epaper_weact_3c.h:17` | `EPaperBase` | `BINARY` |
| `EpaperWaveshareB` | `epaper_waveshare_b.h:10` | `EPaperWeAct3C` | `BINARY` |
| `EPaperWaveshareBWR` | `epaper_waveshare_bwr.h:21` | `EPaperBase` | `BINARY` |
| `EPaperInkplate2` | `epaper_spi_inkplate2.h:8` | `EPaperBase` | `COLOR` |
| `EPaperJD79660` | `epaper_spi_jd79660.h:26` | `EPaperBase` (`final`) | `COLOR` |
| `EPaperSpectraE6` | `epaper_spi_spectra_e6.h:7` | `EPaperBase` (`final`) | `COLOR` |
| `EPaperT133A01` | `epaper_spi_t133a01.h:21` | `EPaperBase` | `COLOR` |

### 4.2 YAML configuration

Schema at `display.py:73-117`, extending `display.FULL_DISPLAY_SCHEMA` +
`spi.spi_device_schema(cs_pin_required=False, default_mode="MODE0")` + per-model options.

| Key | Type | Default | Req. | Source | Matters to us |
|---|---|---|---|---|---|
| `model` | enum | — | **yes** | `display.py:89` | selects the C++ subclass and its buffer layout |
| `dimensions` | `{width,height}` | model preset | req. if model has no preset | `display.py:79`, `:106` | |
| `cs_pin` | pin | model preset | model-dependent | `display.py:101` | |
| `cs1_pin` | pin | model preset | T133A01 only | `models/t133a01.py:34-39` | dual-CS panels |
| `dc_pin` | pin | model preset | **yes** (`fallback=None`) | `display.py:102` | |
| `busy_pin` | pin | model preset | model-dependent | `display.py:100` | ✔ **required for us** — without it `is_idle_()` is hard-true (`epaper_spi.cpp:86-91`) |
| `reset_pin` | pin | model preset | model-dependent | `display.py:103` | |
| `enable_pin` | list of pins | model preset | no | `display.py:107` | |
| `rotation` | 0/90/180/270 | `0` | no | `display.py:88` | ✔ |
| `transform` | `{mirror_x, mirror_y}` | model preset | no | `display.py:93-98` | `swap_xy` is forced `False` (`display.py:225-226`) |
| `update_interval` | time, ≥ model min | `cv.UNDEFINED`; final-validate injects `1min` (lambda/pages) or `never` (LVGL, no lambda) | no | `display.py:90-92`, `:164-172` | ✔ set `never` explicitly |
| `full_update_every` | 1–255 | **`1`** | no | `display.py:99` | ✔ `1` ⇒ every update is full — what we want |
| `init_sequence` | list | model preset | no | `display.py:108-110` | raw override; do not use |
| `reset_duration` | ≤ 500 ms | not set ⇒ C++ `10` ms | no | `display.py:111-114`, `epaper_spi.h:202` | docs page says 200 ms; **the code default is 10 ms** |
| `data_rate`, `spi_id`, … | `spi_device_schema` | model preset (typically 10–20 MHz) | no | `display.py:81-85` | |

`minimum_update_interval` (`display.py:49`, `:76-78`) is a **model property, not a YAML key** — it
sets the `cv.Range(min=…)` floor on `update_interval`, e.g. `30s` for Spectra-E6/JD79660/T133A01,
`1s` default.

**Supported `model:` values** (from `models/*.py`; all matched case-insensitively with `-`/space
folding via `cv.one_of(..., upper=True, space="-")`):

| `model` | W×H | C++ class | Min. update interval |
|---|---|---|---|
| `jd79660` | (dimensions required) | `EPaperJD79660` | 30 s |
| `waveshare-1.54in-g` | 200×200 | `EPaperJD79660` | 30 s |
| `waveshare-7.5in-h` | 800×480 | `EPaperJD79660` | 30 s |
| `spectra-e6` | (dimensions required) | `EPaperSpectraE6` | 30 s |
| `7.3in-spectra-e6` | 800×480 | `EPaperSpectraE6` | 30 s |
| `seeed-reterminal-e1002` | 800×480 | `EPaperSpectraE6` | 30 s |
| `ssd1677` | (dimensions required) | `EPaperMono` | 1 s |
| `waveshare-4.26in` | 800×480 | `EPaperMono` | 1 s |
| `waveshare-3.97in` | 800×480 | `EPaperMono` | 1 s |
| `seeed-ee04-mono-4.26` | 800×480 | `EPaperMono` | 1 s |
| `seeed-reterminal-sticky` | 800×480 | `EPaperMono` | 1 s |
| `ssd1683` | (dimensions required) | `EPaperSSD1683` | 1 s |
| `goodisplay-gdey042t81-4.2` | 400×300 | `EPaperSSD1683` | 1 s |
| `waveshare-2.13in-v3` | 122×250 | `EpaperWaveshare` | 1 s |
| `waveshare-2.13in-bv4` | 122×250 | `EpaperWaveshareB` | 1 s |
| `waveshare-7.5in-bv2-bwr` | 800×480 | `EPaperWaveshareBWR` | 30 s |
| `weact-2.13in-3c` | 122×250 | `EPaperWeAct3C` | 1 s |
| `weact-2.9in-3c` | 128×296 | `EPaperWeAct3C` | 1 s |
| `weact-4.2in-3c` | 400×300 | `EPaperWeAct3C` | 10 s |
| `inkplate2` | 104×212 | `EPaperInkplate2` | 30 s |
| `t133a01` | (dimensions required) | `EPaperT133A01` | 30 s |
| `seeed-reterminal-e1004` | 1200×1600 | `EPaperT133A01` | 30 s |

### 4.3 Public C++ interface — `EPaperBase`

**Public — lifecycle** (`epaper_spi.h:50`, `:64`, `:74-79`)

```cpp
float get_setup_priority() const override;
void dump_config() override;
void setup() override;
void loop() override;
void on_safe_shutdown() override;
```

**Public — config setters** (`epaper_spi.h:49-63`), all non-virtual except `set_rotation`:

```cpp
void set_dc_pin(GPIOPin *);      void set_reset_pin(GPIOPin *);   void set_busy_pin(GPIOPin *);
void set_enable_pins(std::vector<GPIOPin *>);
void set_reset_duration(uint32_t);
void set_transform(uint8_t);
void set_rotation(DisplayRotation) override;      // virtual
void set_full_update_every(uint8_t);              // <-- the only public lever on refresh "mode"
```

**Public — SPI passthrough (do not use).** These are public but are a raw-SPI escape hatch and
violate the plan's "no raw SPI" rule; listed only so we can be explicit about *not* calling them:

```cpp
void command(uint8_t value);                                          // epaper_spi.h:66
void cmd_data(uint8_t command, const uint8_t *ptr, size_t length);    // epaper_spi.h:67
void cmd_data(uint8_t command, std::initializer_list<uint8_t> data);  // epaper_spi.h:70
```

**Public — display / refresh API** (`epaper_spi.h:74`, `:81`, `:92-116`):

```cpp
void update() override;                       // virtual; the ONLY refresh entry point. No mode argument.
DisplayType get_display_type() override;      // returns display_type_, fixed by the subclass ctor
static uint8_t color_to_bit(Color);           // mono default: (r+g+b) >= 382 -> 1
void fill(Color) override;                    // virtual; whole-buffer memset fast path
void clear() override;                        // virtual; fill(COLOR_ON)
int get_width() override;  int get_height() override;   // virtual, rotation-applied
void draw_pixel_at(int x, int y, Color) override;       // virtual  <-- only R1 hook
```

`EPaperBase` does **not** override `draw_pixels_at`, so bulk blits fall back to
`Display::draw_pixels_at` (`display.cpp:54-85`), which loops `draw_pixel_at` per pixel with no
watchdog feed and no fast path.

**Access-level trap (important, and benign).** Several subclasses re-declare `draw_pixel_at`,
`fill`, or `clear` under `protected:` — `EPaper4bpp`/`EPaperSpectraE6`
(`epaper_spi_spectra_e6.h:22`), `EPaperJD79660` (`epaper_spi_jd79660.h:41`), `EPaperWeAct3C`
(`epaper_weact_3c.h:34`), `EPaperWaveshareBWR` (`epaper_waveshare_bwr.h:32`). C++ checks access on
the **static type at the call site**, so calling through an `EPaperBase *` (or `display::Display *`)
still compiles and still dispatches virtually to the protected override. **Store the pointer as
`EPaperBase *`, never as the concrete subclass**, or half the models will fail to compile.

**Protected — unreachable to us** (`epaper_spi.h:118-203`): `get_width_internal`,
`get_height_internal`, `is_using_partial_update_`, `process_state_`, `epaper_state_to_string_`,
`is_idle_`, `setup_pins_`, `reset`, `initialise`, `send_init_sequence_`, `wait_for_idle_`,
`init_buffer_`, `update_effective_transform_`, `rotate_coordinates_`, `set_state_`, `start_data_`,
the four pure virtuals `transfer_data()` / `refresh_screen(bool)` / `power_on()` / `power_off()` /
`deep_sleep()` (`epaper_spi.h:142-160`), and every state member — including
**`split_buffer::SplitBuffer buffer_`** (`epaper_spi.h:177`), `buffer_length_` (`:175`),
`current_data_index_` (`:176`), `state_` (`:201`), `full_update_every_` (`:203`), `update_count_`
(`:187`), and the dirty bounds (`:190`).

### 4.4 Buffer model — storage

Unlike `it8951`'s flat `uint8_t *`, `epaper_spi` uses `split_buffer::SplitBuffer`
(`split_buffer/split_buffer.h:14-44`), which is **deliberately non-contiguous**:

> *"since the buffer may not be contiguous in memory, there is no easy way to access the buffer as a
> single array, i.e. no `.data()` access like a vector."* — `split_buffer.h:11-12`

- `init(total_length)` tries one allocation, then **halves the chunk size and retries** until it
  succeeds or reaches zero (`split_buffer.cpp:11-85`). Chunks are `RAMAllocator<uint8_t>` with
  default flags, i.e. **PSRAM preferred, internal fallback** (`split_buffer.cpp:22`,
  `core/helpers.h:2071-2091`, `:2184`). Chunks are zeroed on allocation (`split_buffer.cpp:58`).
- Public API: `operator[](size_t)`, `fill(uint8_t)`, `size()`, `get_buffer_count()`, `is_valid()`.
  **No `data()`, no range write, no `memcpy` entry point.**
- `operator[]` does a bounds check plus a division and a modulo per byte
  (`split_buffer.cpp:104-122`) — so even *with* a public accessor, a byte-range write would be
  ~3 ops/byte, not a `memcpy`.
- `EPaperBase::setup()` calls `init_buffer_(buffer_length_)` and `mark_failed("Failed to initialise
  buffer")` on failure (`epaper_spi.cpp:23-30`, `:32-38`). `init_buffer_` then calls `clear()`.

### 4.5 Buffer model — geometry

Base `row_width_ = (width + 7) / 8` (`epaper_spi.h:47`); subclasses override it where the packing
differs. `buffer_length_` is set by each subclass constructor.

### 4.6 Buffer model per subclass

| Class | `buffer_length_` | Layout | Colour mapping | Source |
|---|---|---|---|---|
| `EPaperMono`, `EPaperSSD1683`, `EpaperWaveshare` | `((w+7)/8) * h` | 1 bpp, **MSB first** (`0x80 >> (x & 7)`), row-major, stride `row_width_`. **1 = white, 0 = black** | `color_to_bit`: `(r+g+b) >= 382 → 1` | `epaper_spi_mono.h:14`, `epaper_spi.cpp:325-337`, `epaper_spi.h:84-91` |
| `EPaperWeAct3C`, `EpaperWaveshareB` | `row_width_ * h * 2` | **two 1bpp planes**: `[0, len/2)` = B/W (**1 = white**), `[len/2, len)` = red (1 = red). MSB first | `color_to_bwr` | `epaper_weact_3c.h:22`, `epaper_weact_3c.cpp:30-52` |
| `EPaperWaveshareBWR` | `row_width_ * h * 2` | two 1bpp planes; B/W plane inverted vs WeAct — **1 = black** | `color_to_bwr` | `epaper_waveshare_bwr.h:26`, `.cpp:29-50` |
| `EPaperInkplate2` | `row_width_ * h * 2` | two 1bpp planes; black = `(0,1)`, red = `(1,0)`, white = `(1,1)` | `to_inkplate2_color` | `epaper_spi_inkplate2.h:14`, `.cpp:82-104` |
| `EPaperJD79660` | `((w+3)/4) * h`, `row_width_ = (w+3)/4` | **2 bpp, 4 px/byte**, bit offsets **6, 4, 2, 0** (leftmost pixel in the high pair) | `color_to_hex` → BWYR 4-colour | `epaper_spi_jd79660.h:33-34`, `.cpp:35-47` |
| `EPaperSpectraE6` | `w * h / 2` | **4 bpp, 2 px/byte**, **even x in the high nibble**, index `x + y*width` | `color_to_hex` → `{BLACK 0, WHITE 1, YELLOW 2, RED 3, BLUE 5, GREEN 6, CYAN 7}` | `epaper_spi_spectra_e6.h:12`, `.cpp:11-21`, `:114-126` |
| `EPaperT133A01` | `w * h / 2` | 4 bpp, 2 px/byte, even x high nibble | `color_to_index` + `remap_color`, 6 colours | `epaper_spi_t133a01.h:26`, `.cpp:237-249` |

Sizes: 800×480 Spectra-E6 = **192 000 B**; 1200×1600 T133A01 = **960 000 B** (docs: E1004 "requires
PSRAM"); 800×480 mono = **48 000 B**; 800×480 BWR = **96 000 B**.

Colour quantisation for the multi-colour panels is a crude corner-of-the-RGB-cube classifier with a
`GRAY_THRESHOLD` of 50 (`epaper_spi_spectra_e6.cpp:9`, `:23-74`; generic template
`colorconv.h:18-…`). **Do not feed it dithered OpenDisplay palette bytes and expect identity
round-trip** — our backend must emit RGB values that land unambiguously on the intended corner
(pure `#000000`/`#FFFFFF`/`#FF0000`/`#FFFF00`/`#0000FF`/`#00FF00`), and that must be verified
per model before the model is enabled.

### 4.7 Refresh model

**Modes.** There is **no mode enum and no public mode selector.** The only distinction is
full vs partial, passed as `bool partial` to the protected `refresh_screen(bool)`
(`epaper_spi.h:146`), and `partial` is computed as `update_count_ != 0` (`epaper_spi.cpp:226`),
where `update_count_ = (update_count_ + 1) % full_update_every_` (`epaper_spi.cpp:227`). With the
default `full_update_every: 1`, `update_count_` is always 0 and **every refresh is full**.
`is_using_partial_update_()` (`full_update_every_ > 1`, `epaper_spi.h:121`) additionally changes
`deep_sleep()` and `set_window()` behaviour (`epaper_spi_mono.cpp:16-22`, `:32-40`), so
`full_update_every` is not a clean per-call knob.

**Trigger and blocking.** `update()` (`epaper_spi.cpp:121-131`):

```cpp
void EPaperBase::update() {
  if (this->state_ != EPaperState::IDLE) { ESP_LOGE(TAG, "Display already in state %s", …); return; }
  this->set_state_(EPaperState::UPDATE);
  this->enable_loop();
}
```

Two bytes of state and a loop enable — **returns immediately**, and it self-rejects re-entry while
busy. Everything else is `loop()`-driven.

**State machine** (`epaper_spi.h:11-24`, `epaper_spi.cpp:179-240`):

```
IDLE → UPDATE → RESET → RESET_END → INITIALISE → TRANSFER_DATA
     → POWER_ON → REFRESH_SCREEN → POWER_OFF → DEEP_SLEEP → IDLE
```

- `UPDATE` calls `do_update_()` (the auto-clear/lambda/test-card hazard) and bails back to `IDLE`
  if the dirty box is empty (`epaper_spi.cpp:197-204`).
- `TRANSFER_DATA` calls the subclass `transfer_data()`, which returns `false` to be resumed next
  loop; every implementation yields after `MAX_TRANSFER_TIME = 10 ms` (`epaper_spi.h:31`,
  `epaper_spi_mono.cpp:75-79`, `epaper_spi_spectra_e6.cpp:147-150`) in `MAX_TRANSFER_SIZE = 128`
  byte SPI bursts (`epaper_spi.h:32`).
- `set_state_` sets `waiting_for_idle_ = (state > EPaperState::SHOULD_WAIT)`
  (`epaper_spi.cpp:245`), and `loop()` refuses to advance while `waiting_for_idle_ && !is_idle_()`
  (`epaper_spi.cpp:152-167`). Since `POWER_OFF` and `DEEP_SLEEP` both sort above `SHOULD_WAIT`,
  **the panel's post-refresh BUSY is waited on before `POWER_OFF` runs** — which is exactly why
  reaching `IDLE` on this driver *does* mean the refresh physically finished.

**BUSY handling.** `is_idle_()` reads `busy_pin_` (`epaper_spi.cpp:86-91`) and **returns `true`
unconditionally when `busy_pin_ == nullptr`.** Several models mark the pin `inverted: true` in their
presets (`models/inkplate2.py`, `models/spectra_e6.py`, `models/t133a01.py`). There is **no busy
timeout and no recovery path** — a stuck BUSY line parks the component in `waiting_for_idle_`
forever, and `is_idle()` stays false. Our transfer engine's own refresh timeout (`0x0074`) is
therefore load-bearing here, and must not assume the driver will ever unstick itself.

One residual blocking call: `send_init_sequence_` honours `DELAY_FLAG` entries with a synchronous
`delay(cmd)` (`epaper_spi.cpp:274-276`), bounded by one byte (≤ 255 ms) and only during
`INITIALISE`. `EPaperJD79660` also does a deliberate 2 ms synchronous wait during reset
(`epaper_spi_jd79660.h:120-129`). Both are inside `loop()`, not our callback, so they are
acceptable — but they do mean `loop()` is not strictly bounded to 10 ms on those models.

**Durations.** Documented on the docs page: 4-colour full refresh "typically around 20 seconds";
6-colour E1004 "approximately 30 seconds". These match the models' 30 s `minimum_update_interval`.
Mono panel timings: **UNVERIFIED**.

### 4.8 Divergence between 2026.7.4 and dev

`dev` (as of this writing) has refactored the 4bpp family and added a model. This does **not** affect
any public API we depend on, but it will change class names in a future release:

| Change on `dev` | Impact |
|---|---|
| New `epaper_spi_4bpp.{h,cpp}` — intermediate base `EPaper4bpp : EPaperBase` owning `fill`/`clear`/`draw_pixel_at`/`transfer_data`; subclasses supply only `virtual uint8_t color_to_native(Color)` | `EPaperSpectraE6`'s base changes from `EPaperBase` to `EPaper4bpp`. Harmless if we hold an `EPaperBase *`. |
| New `epaper_spi_inkplate6color.{h,cpp}` + `models/inkplate6color.py` | new model value, not yet in a release |
| `colorconv.h` gained the generic `color_to_bwyr<>` template | internal |

---

## 5. Detailed verdict per requirement

### R1 — prepare/load a full-frame buffer, ideally contiguous byte ranges at a known offset

**`it8951` — NOT SATISFIED.**

- No public accessor exists for the framebuffer. `uint8_t *buffer_` is protected
  (`it8951.h:314`), as are `buffer_length_` (`:313`) and `row_width_` (`:312`).
- No public method accepts a byte offset + length. The closest public entry points are
  `draw_pixel_at` (`it8951.h:199`) and `draw_pixels_at` (`it8951.h:203-204`), both of which convert
  **per pixel** through `ColorUtil::to_color` + `write_pixel_native_` (`it8951.cpp:1001-1002`).
- The plan's "stream into controller image RAM, no MCU-side framebuffer" is **impossible**: the
  framebuffer is allocated unconditionally in `setup()` (`it8951.cpp:329-336`) and
  `op_xfer_rows_` reads exclusively from it (`it8951.cpp:614-621`).
- *Workaround that is public-API-legal:* expand each OpenDisplay row into a scratch
  `COLOR_BITNESS_888` line and call `draw_pixels_at(0, row, width, 1, …)`. Cost and pitfalls in
  [§3.4](#34-what-a-public-bulk-load-through-draw_pixels_at-would-actually-cost). Viable for M2, but
  it is a conversion path, not a byte-range write — plan for the CPU budget and the `loop()`
  chunking.

**`epaper_spi` — NOT SATISFIED.**

- `buffer_` is protected (`epaper_spi.h:177`) and is a `SplitBuffer` with **no contiguous view by
  design** (`split_buffer.h:11-12`, no `data()`), so even a hypothetical public accessor would not
  give us a `memcpy` target.
- Only `draw_pixel_at` (`epaper_spi.h:116`) is public. `draw_pixels_at` is *not* overridden, so bulk
  blits degrade to the base per-pixel loop (`display.cpp:54-85`) with no watchdog feed.
- **Missing hook:** a public `bool write_buffer_range(size_t offset, const uint8_t *data, size_t len)`
  (or `SplitBuffer::write(offset, ptr, len)` plus a `EPaperBase` forwarder) that validates against
  `buffer_length_` and copies chunk-wise. This is the single most valuable upstream addition for us.

### R2 — start a refresh, with the refresh MODE selectable, that returns quickly

**`it8951` — PARTIAL.**

- ✔ *Public and fast:* `void IT8951Display::update_mode(UpdateMode mode)` (`it8951.h:195`,
  `it8951.cpp:779-787`). Full mode enum at `it8951_defs.h:139-149`. Returns after enqueuing;
  no SPI, no `delay()` on the caller's stack (`it8951.cpp:754-767`).
- ✘ *Mode is not authoritative:* `prepare_update_region_` forces `GC16` on the full-update cycle
  (`it8951.cpp:679-686`) and downgrades `GC16 → DU` on mono partials (`it8951.cpp:737-738`).
  With `full_update_every: 30` this bites 1 update in 30, and always on the first update after boot
  (`it8951.h:161-167`).
- **Mitigation without an upstream change:** set `full_update_every: 1`, which makes *every* update
  a forced full `GC16` — deterministic, at the cost of losing every other waveform. Our capabilities
  response would then advertise exactly one refresh mode (`GC16`/full).
- **Missing hook (to advertise more than one mode honestly):** an "honour the requested mode
  verbatim" path — e.g. `void update_mode(UpdateMode mode, bool force_full)` or a public
  `bool set_next_update_forced_full(bool)` — so the caller learns which waveform actually ran.

**`epaper_spi` — NOT SATISFIED (mode); the "returns quickly" half is satisfied.**

- ✔ `void EPaperBase::update()` (`epaper_spi.h:74`, `epaper_spi.cpp:121-131`) returns immediately and
  rejects re-entry while non-`IDLE`.
- ✘ No mode argument anywhere. `refresh_screen(bool partial)` is protected pure-virtual
  (`epaper_spi.h:146`) and `partial` is derived internally (`epaper_spi.cpp:206`, `:226-227`).
- Toggling `set_full_update_every(1|N)` between calls is public but is a **hack** with side effects
  on `deep_sleep()` and `set_window()` via `is_using_partial_update_()`
  (`epaper_spi_mono.cpp:16-22`, `:32-40`). Do not ship it.
- **Missing hook:** `virtual void update(bool full)` or `void request_full_refresh()` on
  `EPaperBase`.
- **v1 decision:** advertise **full refresh only, one mode**, with `full_update_every: 1`.

### R3 — non-blocking poll of refresh/BUSY completion from `loop()`

**`epaper_spi` — SATISFIED (conditionally).**

- `Component::is_idle()` is public (`core/component.h:215`) and true iff the loop is disabled
  (`core/component.cpp:255-261`). `EPaperBase` disables its loop only on entering
  `EPaperState::IDLE` (`epaper_spi.cpp:186-188` and `:253-255`), and enables it in `update()`
  (`epaper_spi.cpp:127`).
- Because `set_state_` forces a BUSY wait for every state above `SHOULD_WAIT`
  (`epaper_spi.cpp:245`, `:152-167`) and `POWER_OFF`/`DEEP_SLEEP` follow `REFRESH_SCREEN`, reaching
  `IDLE` **does** imply the panel finished its physical refresh. This is a real completion signal.
- **Conditions we must enforce in config validation:**
  1. `busy_pin` **required** — without it `is_idle_()` is hard-true (`epaper_spi.cpp:86-91`) and
     the state machine races through with no waiting at all.
  2. Our own refresh timeout must fire independently — the driver has **no** busy timeout and no
     recovery, so a stuck panel never returns to `IDLE`.
  3. Poll only *after* our `update()` call returned (it enables the loop synchronously), so there is
     no false-idle race.

**`it8951` — PARTIAL / NOT SATISFIED for *physical* completion.**

- `is_idle()` works mechanically: the driver `disable_loop()`s only at `Phase::IDLE`
  (`it8951.cpp:48-50`, `:181-184`) and `enable_loop()`s in `start_update_` (`it8951.cpp:759`,
  `:765`). So it is a valid non-blocking poll for "the driver finished issuing this update".
- ✘ But `Phase::UPDATE_REFRESH` is explicitly fire-and-forget (`it8951.cpp:277-288`) — the display
  command is sent and the phase advances without waiting for the LUT engine. The `LUTAFSR` poll
  belongs to the *next* update (`it8951.cpp:430-434`, `:651-662`). So `is_idle()` goes true while
  the panel is still refreshing.
- HW_RDY is **not** a proxy: `is_busy_()` (`it8951.cpp:35-38`) reads the SPI host-ready line, and
  the driver deliberately dispatches ops while the LUT runs. Polling it ourselves would report
  "done" early.
- Reading `LUTAFSR` ourselves is forbidden (raw SPI + it would corrupt the driver's op sequencing).
- **Missing hook (this is the M0 blocker):** a public non-blocking completion signal on
  `IT8951Display`. Minimal upstream change in [§6.2](#62-minimal-upstream-hooks-that-would-close-m0).
- **Interim options, in preference order:**
  1. Land hook (a) upstream — smallest possible diff, reuses existing ops.
  2. Emit `0x0073` on `is_idle()` **plus** a mode-dependent settling delay, and document the
     approximation loudly. Delay values are **UNVERIFIED** and must be measured per panel.
  3. Do not use `sleep_when_done: true` with option 2 — `TCON_SLEEP` is enqueued right after the
     display command and is gated only on HW_RDY (`it8951.cpp:446-453`), so it may land mid-LUT.
     **UNVERIFIED** whether the controller defers it; do not rely on it either way.

---

## 6. What this means for the build

### 6.1 Capability sets we can honestly advertise today

| | `it8951` | `epaper_spi` |
|---|---|---|
| Direct write `0x0070`–`0x0072` | yes (via per-pixel conversion) | yes (via per-pixel conversion) |
| `PIPE_WRITE` `0x0080`–`0x0082` | yes — same backend path | yes — same backend path |
| Full refresh modes | **1** (`GC16`) with `full_update_every: 1`; more only after hook (b) | **1** (full) |
| Partial `0x0076` | not in v1 (driver owns the dirty box) | not in v1 (no per-region control) |
| Max contiguous write size | n/a — no byte-range API; adapter converts row-at-a-time | same |
| Staging bytes required | driver framebuffer (unavoidable) **+** our row scratch (`3 × width` B at 888) | driver framebuffer **+** our row scratch |
| `0x0073` after physical completion | **not achievable on public API** — see R3 | achievable via `is_idle()` + required `busy_pin` |

The plan's "IT8951 first, `epaper_spi` later" ordering (M2 before M4) is **backwards relative to the
M0 evidence**: `epaper_spi` is the backend whose completion semantics we can actually honour today.
Recommend either (i) reordering so the first end-to-end hardware bring-up is `epaper_spi`, or
(ii) landing upstream hook (a) before M2 starts. Either is a plan change and should be recorded in
`docs/TODO.md`.

### 6.2 Minimal upstream hooks that would close M0

Ordered by value. All three are additive, do not change existing behaviour, and touch < 30 lines.

**(a) `it8951`: public physical-completion signal — the M0 blocker.**

```cpp
// it8951.h, public:
bool is_refreshing() const { return this->phase_ != Phase::IDLE || this->update_pending_; }
```
…plus an opt-in terminal LUT-idle wait so the flag means something. The machinery already exists:
add a `Phase::UPDATE_WAIT_LUT` that re-enqueues the existing
`CMD(TCON_REG_RD) → WRITE_W(LUTAFSR) → READ_WORD → CHECK_LUT_IDLE` sequence
(`it8951.cpp:430-434`, `:651-662`) after `DPY_BUF_ARGS`, gated by a new
`set_confirm_refresh(bool)` so the default fire-and-forget latency is preserved for existing users.
An `on_refresh_complete` trigger would be the more idiomatic ESPHome shape and is equally cheap.

**(b) `epaper_spi`: per-call refresh selection.**

```cpp
// epaper_spi.h, public:
virtual void update(bool full);   // update() == update(this->update_count_ == 0)
```
or a `void request_full_refresh()` latch consumed by `process_state_` at `REFRESH_SCREEN`
(`epaper_spi.cpp:225-229`).

**(c) Both: a public bounded buffer-range write.**

```cpp
// SplitBuffer (split_buffer.h): write into a possibly-split buffer without exposing pointers
bool write(size_t offset, const uint8_t *data, size_t length);
// EPaperBase / IT8951Display, public:
bool write_frame_bytes(size_t offset, const uint8_t *data, size_t length);  // validates vs buffer_length_
size_t get_frame_buffer_size() const;
size_t get_frame_row_stride() const;
```
This is the hook that turns R1 from "convert 2.6 M pixels" into "`memcpy` the wire bytes", and it
matches our `write_contiguous(offset, data, length)` adapter signature exactly
(`docs/OpenDisplay_ESPHome_Component_Plan.md:282-284`). Its correctness burden falls entirely on us:
we would then be responsible for producing driver-native bytes, which is only defensible per
validated panel model — consistent with `CLAUDE.md`'s "add an `epaper_spi` panel model only after
byte packing, colour mapping, buffer size, and BUSY behaviour are tested for it".

Until (c) lands, the backend adapter should keep `write_contiguous` in its interface but implement it
by buffering into our own staging row and flushing through `draw_pixels_at`/`draw_pixel_at` — the
adapter contract survives, only the implementation is slow.

### 6.3 Config-validation rules our codegen must enforce

1. Exactly one display ID; its platform must be `it8951` or `epaper_spi` (per `CLAUDE.md`'s codegen
   contract). Reject `waveshare_epaper` and everything else with a clear message.
2. `update_interval: never` — required.
3. `auto_clear_enabled: false` — required (otherwise `do_update_()` clears our frame,
   `display.cpp:691-694`).
4. A `lambda:` must be present (even a no-op) — otherwise final validation force-injects
   `show_test_card: True` (`it8951/display.py:348-353`, `epaper_spi/display.py:164-170`) and the
   test card overwrites our frame.
5. `epaper_spi`: `busy_pin` required (otherwise no completion signal, `epaper_spi.cpp:86-91`).
6. `epaper_spi`: `full_update_every: 1` required in v1 (deterministic full refresh).
7. `it8951`: `full_update_every: 1` recommended in v1 if we advertise a single refresh mode.
8. Model allow-list: only models whose byte packing and colour mapping we have tested
   ([§4.6](#46-buffer-model-per-subclass)).
9. Reject `pages:` (a page writer would fight us for the buffer).
10. Fail setup loudly if the driver's own buffer allocation failed — `is_failed()`
    (`core/component.h:272`) is public and both drivers `mark_failed()` on allocation failure
    (`it8951.cpp:333-336`, `epaper_spi.cpp:24-27`).

---

## 7. Recommended YAML for the OpenDisplay backend

```yaml
# epaper_spi backend
display:
  - platform: epaper_spi
    id: od_panel
    model: 7.3in-Spectra-E6
    cs_pin: GPIO10
    dc_pin: GPIO11
    reset_pin: GPIO12
    busy_pin:                 # REQUIRED for us: without it is_idle_() is hard-true
      number: GPIO13
      inverted: true
    rotation: 0
    full_update_every: 1      # every refresh is full — deterministic
    update_interval: never    # we drive refreshes; the poller must never fire
    auto_clear_enabled: false # do_update_() must NOT clear our frame
    lambda: |-
      // intentionally empty: OpenDisplay owns this buffer.
      // Required so FINAL_VALIDATE_SCHEMA does not force show_test_card: True.

opendisplay:
  display_id: od_panel
```

```yaml
# it8951 backend
display:
  - platform: it8951
    id: od_panel
    model: m5stack-m5paper
    grayscale: true           # 4bpp; false = packed 1bpp with the 16-px group byte order
    full_update_every: 1      # forces GC16 every time -> requested mode is never overridden
    update_interval: never
    auto_clear_enabled: false
    lambda: |-
      // intentionally empty: OpenDisplay owns this buffer.
```

**UNVERIFIED:** whether a *completely empty* lambda body passes `cv.lambda_` and `process_lambda`.
Keep a comment line in the body as shown.

---

## 8. Open questions and UNVERIFIED items

| # | Item | Why it matters |
|---|---|---|
| 1 | Real refresh durations per panel/waveform (IT8951 `GC16`/`DU`; mono `epaper_spi`) — nothing in-tree documents them | needed for the `0x0074` refresh timeout and for the IT8951 settling-delay fallback |
| 2 | Whether `TCON_SLEEP` under `sleep_when_done: true` is deferred by the controller until the LUT finishes (`it8951.cpp:446-453`) | decides whether option 2 in R3 is safe with sleep enabled |
| 3 | Whether an empty `lambda:` body validates | affects the recommended YAML |
| 4 | Why `it8951` declares `AUTO_LOAD = ["split_buffer"]` when it uses a flat `RAMAllocator` buffer (`display.py:39`) | may hint at a planned `SplitBuffer` migration that would change the R1 answer |
| 5 | Behaviour of `it8951` `draw_pixels_at` under `rotation: 90/270` for a full-frame blit — the transform is applied per pixel (`it8951.cpp:996-1002`) and the dirty box from the two corners (`:1009-1022`) | our adapter must map OpenDisplay's frame orientation onto pre-rotation coordinates |
| 6 | Exact BWR/6-colour RGB inputs that land on each panel colour without ambiguity (`GRAY_THRESHOLD 50` classifier, `epaper_spi_spectra_e6.cpp:23-74`) | required before any `epaper_spi` colour model is enabled |
| 7 | Upstream appetite for hooks (a)–(c) — no PR or issue has been filed | determines whether M0 closes by upstreaming or by scope reduction |
| 8 | `dev`'s `EPaper4bpp` refactor landing in a release (§4.8) | changes `EPaperSpectraE6`'s base class; harmless if we hold `EPaperBase *` |

---

## 9. Source index

All paths relative to `https://github.com/esphome/esphome/blob/2026.7.4/esphome/`.

| File | Lines cited |
|---|---|
| `components/it8951/it8951.h` | 15, 140-142, 144-147, 150-154, 157-191, 194-206, 208-351, 277, 354-371 |
| `components/it8951/it8951.cpp` | 17, 35-38, 40-79, 48-50, 164-168, 173-297, 277-288, 301-347, 329-336, 428-453, 471-483, 593-626, 651-673, 677-745, 754-787, 791-827, 872-904, 929-1023, 1025-1053 |
| `components/it8951/it8951_defs.h` | 72-138, 139-149, 163-166 |
| `components/it8951/display.py` | 39, 57-59, 67-81, 122-172, 191-293, 296-333, 339-357, 360-433 |
| `components/epaper_spi/epaper_spi.h` | 8, 11-24, 26-33, 35-48, 49-63, 66-72, 74-79, 81-116, 118-203 |
| `components/epaper_spi/epaper_spi.cpp` | 23-38, 40-58, 60-84, 86-91, 121-131, 133-169, 179-240, 242-256, 258-263, 265-293, 301-337, 339-356 |
| `components/epaper_spi/display.py` | 45-54, 73-117, 120-150, 156-176, 179-237 |
| `components/epaper_spi/models/*.py` | model registry — see §4.2 table |
| `components/epaper_spi/epaper_spi_mono.{h,cpp}` | `h`:9-26; `cpp`:10-30, 32-51, 53-89 |
| `components/epaper_spi/epaper_spi_spectra_e6.{h,cpp}` | `h`:7-18; `cpp`:9-74, 96-126, 128-161 |
| `components/epaper_spi/epaper_spi_jd79660.{h,cpp}` | `h`:26-35, 41, 114-135; `cpp`:35-47 |
| `components/epaper_spi/epaper_spi_t133a01.{h,cpp}` | `h`:21-27, 29-38; `cpp`:237-249 |
| `components/epaper_spi/epaper_weact_3c.{h,cpp}` | `h`:17-40; `cpp`:30-52 |
| `components/epaper_spi/epaper_waveshare_bwr.{h,cpp}` | `h`:7-38; `cpp`:29-50 |
| `components/epaper_spi/epaper_spi_inkplate2.{h,cpp}` | `h`:8-31; `cpp`:82-104 |
| `components/epaper_spi/epaper_waveshare_b.h` | 10-17 |
| `components/epaper_spi/epaper_spi_ssd1683.h` | 9-20 |
| `components/epaper_spi/colorconv.h` | 16-60 |
| `components/display/display.h` | 127-138, 317-361, 693-716, 774, 794 |
| `components/display/display.cpp` | 54-85, 691-703 |
| `components/display/display_color_utils.h` | 5-6, 11-46 |
| `components/display/__init__.py` | 79-124, 127-161, 223-231, 264-266 |
| `components/split_buffer/split_buffer.{h,cpp}` | `h`:7-44; `cpp`:11-85, 87-101, 104-122, 125-141 |
| `core/component.h` | 81-86, 215, 238-249, 272-274, 510-544 |
| `core/component.cpp` | 255-261, 262-266, 265-269, 335-344, 381-391 |
| `core/scheduler.cpp` | 115-125 |
| `core/helpers.h` | 2067-2107, 2130-2184 |
| `cpp_helpers.py` | 169-175 |

Docs pages: <https://esphome.io/components/display/it8951/>,
<https://esphome.io/components/display/epaper_spi/>.
