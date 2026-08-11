"""ESPHome codegen for the ``opendisplay:`` component.

The backend is chosen HERE, at code-generation time, from the declared display
type -- never at runtime from a platform name. Only the two explicitly supported
driver families are accepted; anything else is a configuration error.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import automation
from esphome.const import CONF_ID, CONF_TRIGGER_ID
from esphome.core import CORE

CODEOWNERS = ["@OpenDisplay"]
DEPENDENCIES = ["esp32", "esp32_ble_server"]
AUTO_LOAD = ["binary_sensor", "text_sensor"]
MULTI_CONF = False

CONF_DISPLAY = "display"
CONF_TRANSPORT = "transport"
CONF_TRANSFER_TIMEOUT = "transfer_timeout"
CONF_REFRESH_TIMEOUT = "refresh_timeout"
CONF_ON_TRANSFER_STARTED = "on_transfer_started"
CONF_ON_REFRESH_COMPLETE = "on_refresh_complete"
CONF_ON_ERROR = "on_error"

opendisplay_ns = cg.esphome_ns.namespace("opendisplay")
OpenDisplayComponent = opendisplay_ns.class_("OpenDisplayComponent", cg.Component)
OpenDisplayBackend = opendisplay_ns.class_("OpenDisplayBackend")
IT8951Backend = opendisplay_ns.class_("IT8951Backend", OpenDisplayBackend)
EpaperSPIBackend = opendisplay_ns.class_("EpaperSPIBackend", OpenDisplayBackend)

IsBusyCondition = opendisplay_ns.class_("IsBusyCondition", automation.Condition)
AbortAction = opendisplay_ns.class_("AbortAction", automation.Action)

TransferStartedTrigger = opendisplay_ns.class_("TransferStartedTrigger", automation.Trigger.template())
RefreshCompleteTrigger = opendisplay_ns.class_("RefreshCompleteTrigger", automation.Trigger.template())
ErrorTrigger = opendisplay_ns.class_("ErrorTrigger", automation.Trigger.template(cg.std_string))

# --- Supported display drivers ------------------------------------------------
# Both are mainline ESPHome; class names verified against tag 2026.7.4. See
# docs/ESPHome_Display_Drivers_Reference.md.
#   it8951     -> esphome::it8951::IT8951Display    (since ESPHome 2026.7.0)
#   epaper_spi -> esphome::epaper_spi::EPaperBase   (since ESPHome 2025.10.0)
#
# Hold EPaperBase, not a concrete model subclass: epaper_spi is ten subclasses
# with seven buffer layouts, and several re-declare draw_pixel_at as protected --
# binding to a subclass would fail to compile for half the models.
from esphome.components.epaper_spi.display import EPaperBase  # noqa: E402
from esphome.components.it8951.display import IT8951Display  # noqa: E402

# Backend selection table: supported display class -> backend class. Adding a row
# means committing to a validated adapter, not merely to a driver that compiles.
#
# A LIST OF PAIRS, not a dict: MockObjClass overloads __eq__ to build a C++
# expression (cpp_generator.py:986) and therefore defines no __hash__, so it
# cannot be a dict key.
BACKENDS = [
    (IT8951Display, IT8951Backend),
    (EPaperBase, EpaperSPIBackend),
]

# Fixed v1 protocol limits. These are wire/policy contract (see docs/CLAUDE.md),
# not user-tunable knobs -- they are asserted, not configured.
PIPE_WINDOW_PACKETS = 16
PIPE_ACK_CADENCE = 4
MAX_ACTIVE_TRANSFERS = 1

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(OpenDisplayComponent),
            cv.Required(CONF_DISPLAY): cv.use_id(cg.Component),
            cv.Optional(CONF_TRANSPORT, default="ble"): cv.one_of("ble", lower=True),
            cv.Optional(CONF_TRANSFER_TIMEOUT, default="30s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(seconds=1), max=cv.TimePeriod(seconds=300)),
            ),
            # Minimum 10s, NOT 1s: an e-paper full refresh takes seconds, and the
            # IT8951 backend reports a fixed 5s settle. A 1s timeout would fire
            # before any real refresh finished, permanently masking success.
            cv.Optional(CONF_REFRESH_TIMEOUT, default="180s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(seconds=10), max=cv.TimePeriod(seconds=600)),
            ),
            cv.Optional(CONF_ON_TRANSFER_STARTED): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(TransferStartedTrigger)}
            ),
            cv.Optional(CONF_ON_REFRESH_COMPLETE): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(RefreshCompleteTrigger)}
            ),
            cv.Optional(CONF_ON_ERROR): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ErrorTrigger)}
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    # it8951 landed in 2026.7.0, which is the later of the two floors. This does
    # NOT yet include the upstream hooks M0 found missing -- raise it when they
    # land. See docs/ESPHome_Display_Drivers_Reference.md.
    cv.require_esphome_version(2026, 7, 0),
)


# OpenDisplay panel identity. Drives the client's measured-palette and code
# tables, so a wrong value degrades colour silently rather than erroring.
# 39 = OD_PANEL_IC_EP426_800X480, "Waveshare 4.26in B/W 800x480" -- the same
# panel definition seeed-ee04-mono-4.26 extends.
OD_PANEL_IC_EP426_800X480 = 39
PANEL_IC_BY_MODEL = {
    "seeed-ee04-mono-4.26": OD_PANEL_IC_EP426_800X480,
}


def _panel_ic_for(config):
    """Best-effort panel id; 0 (EP_PANEL_UNDEFINED) degrades safely for mono."""
    return 0


# SystemConfig.ic_type (opendisplay_structs.h:384). Verified against the canonical
# EE04 board preset, which declares ic_type "2" for its ESP32-S3.
# Cosmetic -- every client consumer degrades gracefully -- so an unmapped variant
# emits 0 rather than failing the build.
IC_TYPE_BY_VARIANT = {
    "esp32s3": 2,
    "esp32c3": 3,
    "esp32c6": 4,
}


def _ic_type_for_variant():
    from esphome.components.esp32 import get_esp32_variant

    try:
        variant = str(get_esp32_variant()).lower().replace("-", "").replace("_", "")
    except Exception:  # noqa: BLE001 - variant unavailable at this point
        return 0
    return IC_TYPE_BY_VARIANT.get(variant, 0)


def _backend_class_for(config):
    """Resolve the backend from the referenced display's PLATFORM name.

    DO NOT try to read the C++ type off the variable. ``MockObj.__getattr__``
    fabricates a MockObj for *any* attribute (``cpp_generator.py:895``) and
    ``MockObj.__slots__`` has no ``type``, so ``display_var.type`` is a C++
    expression object rather than a ``MockObjClass``. Calling
    ``.inherits_from()`` on it returns another expression -- always truthy -- so
    every display silently resolved to the first backend in the table. That is
    the same trap as ``MockObj.__eq__`` returning an expression instead of a
    bool, and it is invisible until the generated C++ fails to compile with a
    mismatched constructor argument.

    The platform name in the validated config is a plain string and cannot lie.
    Selecting on it HERE, at codegen time, is not the "choose a backend by
    runtime platform name" the plan forbids -- nothing is decided on-device.
    """
    target = config[CONF_DISPLAY]
    for platform in CORE.config.get("display", []):
        pid = platform.get(CONF_ID)
        if pid is None or getattr(pid, "id", None) != getattr(target, "id", None):
            continue
        plat = platform.get("platform")
        if plat == "it8951":
            return IT8951Backend
        if plat == "epaper_spi":
            return EpaperSPIBackend
        raise cv.Invalid(
            f"opendisplay: unsupported display platform {plat!r}. "
            f"Supported: it8951, epaper_spi."
        )
    raise cv.Invalid(
        f"opendisplay: could not resolve display id {target} to a display platform."
    )


async def to_code(config):
    display_var = await cg.get_variable(config[CONF_DISPLAY])
    backend_class = _backend_class_for(config)

    # ESPHome compiles EVERY file in a used component (writer.py copy_src_tree),
    # so the unselected backend's .cpp would still be built and would #include a
    # driver header that is not in this configuration. Gate each one.
    cg.add_define(
        "USE_OPENDISPLAY_IT8951"
        if backend_class is IT8951Backend
        else "USE_OPENDISPLAY_EPAPER_SPI"
    )

    # The backend instance is created BEFORE component registration so the
    # component owns a fully-constructed adapter for the whole of setup().
    backend = cg.new_Pvariable(
        cv.declare_id(backend_class)(f"{config[CONF_ID].id}_backend"),
        display_var,
    )

    var = cg.new_Pvariable(config[CONF_ID], backend)
    await cg.register_component(var, config)

    cg.add(var.set_transfer_timeout(config[CONF_TRANSFER_TIMEOUT]))
    cg.add(var.set_refresh_timeout(config[CONF_REFRESH_TIMEOUT]))

    # The backend cannot discover its colour scheme at runtime -- see the
    # allow-list above -- so codegen must set it. Without this the capability
    # struct defaults to 0, which is mono only by accident.
    cg.add(backend.set_color_scheme(OD_COLOR_SCHEME_MONO))
    cg.add(backend.set_panel_ic_type(_panel_ic_for(config)))
    cg.add(var.set_ic_type(_ic_type_for_variant()))

    for conf in config.get(CONF_ON_TRANSFER_STARTED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
    for conf in config.get(CONF_ON_REFRESH_COMPLETE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
    for conf in config.get(CONF_ON_ERROR, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "x")], conf)


# --- Model allow-list ---------------------------------------------------------
# A model may appear here ONLY after its byte packing, colour mapping, buffer
# size and BUSY behaviour have been validated. `epaper_spi` is ten subclasses
# with seven distinct buffer layouts, so "supports epaper_spi" is meaningless --
# support is per model.
#
# Colour scheme CANNOT be read back from the driver: get_display_type() returns
# only BINARY/COLOR, which conflates mono with 3-colour. It must be derived here
# from the YAML `model:`.
OD_COLOR_SCHEME_MONO = 0

# NOTE: keys are compared UPPER-CASED. epaper_spi validates `model:` with
# cv.one_of(..., upper=True, space="-"), so the value reaching final validation
# is e.g. "SSD1683", never "ssd1683".
EPAPER_SPI_MODELS = {
    # model value -> canonical OD_COLOR_SCHEME_*
    # All EPaperMono / EPaperSSD1683: 1 bpp MSB-first, 1 = white, which is
    # byte-identical to the OpenDisplay MONO wire format.
    "seeed-ee04-mono-4.26": OD_COLOR_SCHEME_MONO,  # 800x480, SSD1677
    # Generic SSD16xx mono base; dimensions must be given in YAML. Covers
    # SSD1680 panels too -- ESPHome treats 1680/1683 as one family.
    "ssd1683": OD_COLOR_SCHEME_MONO,
}

IT8951_COLOR_SCHEMES = {
    # `grayscale: false` selects the packed 1bpp buffer. The 4bpp path needs the
    # grayscale packing plus a validated panel id and is not implemented.
    False: OD_COLOR_SCHEME_MONO,
}


def _final_validate(config):
    """Cross-component validation against the referenced display's own config.

    `cv.use_id` yields only an ID, so the display's `model:`/`busy_pin:` are
    reachable only from the full config at final-validation time.
    """
    fconf = fv.full_config.get()
    display_id = config[CONF_DISPLAY]

    # Single central. This is enforced by CONFIG, not by code: ESPHome's BLE
    # server resumes advertising on connect only while
    # `client_count_ < max_clients_` (ble_server.cpp:172-174), so max_clients: 1
    # -- the default -- makes the device stop advertising as soon as a client
    # connects and resume on disconnect. That is exactly the behaviour we want,
    # and fighting it with raw esp_ble_gap_stop_advertising() would race the
    # server's own advertising_start().
    ble_server = fconf.get("esp32_ble_server")
    if isinstance(ble_server, dict) and ble_server.get("max_clients", 1) != 1:
        raise cv.Invalid(
            "opendisplay: esp32_ble_server must use max_clients: 1. The component "
            "supports a single central; with more, ESPHome keeps advertising while "
            "connected and a second client could start a competing transfer."
        )

    for platform in fconf.get("display", []):
        if platform.get(CONF_ID) != display_id:
            continue
        plat = platform.get("platform")

        if plat == "epaper_spi":
            model = str(platform.get("model", "")).upper()
            if model not in {k.upper() for k in EPAPER_SPI_MODELS}:
                raise cv.Invalid(
                    f"opendisplay: epaper_spi model {model!r} is not validated. "
                    f"Validated models: {sorted(EPAPER_SPI_MODELS)}. Adding one "
                    f"requires testing its byte packing, colour mapping, buffer "
                    f"size and BUSY behaviour first."
                )
            # busy_pin is REQUIRED: without it the driver's is_idle_() is
            # hard-true, so refresh completion is meaningless and 0x0073 would
            # be emitted before the panel has updated.
            if not platform.get("busy_pin"):
                raise cv.Invalid(
                    "opendisplay: the display must configure busy_pin, otherwise "
                    "refresh completion cannot be detected and 0x0073 would be a lie."
                )
        elif plat == "it8951":
            if platform.get("grayscale", True):
                raise cv.Invalid(
                    "opendisplay: it8951 requires grayscale: false (only 1bpp mono "
                    "is implemented)."
                )

        # Rotation must be 0: draw_pixel_at applies the driver's transform on top
        # of our unrotated wire coordinates.
        if platform.get("rotation", 0) not in (0, "0°", "0"):
            raise cv.Invalid("opendisplay: the display must use rotation: 0.")
        break

    return config


FINAL_VALIDATE_SCHEMA = _final_validate

CONF_OPENDISPLAY_ID = "opendisplay_id"

OPENDISPLAY_CLIENT_SCHEMA = cv.Schema(
    {cv.GenerateID(CONF_OPENDISPLAY_ID): cv.use_id(OpenDisplayComponent)}
)


# maybe_simple_value so both forms work:
#     opendisplay.is_busy: od
#     opendisplay.is_busy: {opendisplay_id: od}
# Without it the shorthand fails with "expected a dictionary", which is the form
# every example and the plan document use.
@automation.register_condition(
    "opendisplay.is_busy",
    IsBusyCondition,
    cv.maybe_simple_value(OPENDISPLAY_CLIENT_SCHEMA, key=CONF_OPENDISPLAY_ID),
)
async def is_busy_to_code(config, condition_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_OPENDISPLAY_ID])
    return cg.new_Pvariable(condition_id, template_arg, parent)


# synchronous=True: AbortAction::play() calls parent_->abort() and returns, so
# play_next_() always runs before the initial play() returns. Declaring it lets
# ESPHome keep the StringRef optimisation.
@automation.register_action(
    "opendisplay.abort",
    AbortAction,
    cv.maybe_simple_value(OPENDISPLAY_CLIENT_SCHEMA, key=CONF_OPENDISPLAY_ID),
    synchronous=True,
)
async def abort_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_OPENDISPLAY_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)
