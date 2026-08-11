#pragma once

// Typed protocol vocabulary: commands, responses, errors, capabilities.
//
// This header is deliberately free of ESPHome and ESP-IDF includes so the
// protocol and transfer engines can be unit-tested on the host against a fake
// backend and fake transport. Keep it that way.
//
// Wire values come from the vendored canonical headers -- never redefine an
// opcode or status byte literal here.

#include <cstddef>
#include <cstdint>

#include "opendisplay_protocol.h"
// OD_COLOR_SCHEME_*, OD_PKT_*, and every config struct live here, NOT in
// opendisplay_protocol.h.
#include "opendisplay_structs.h"

namespace esphome {
namespace opendisplay {

// --- Transaction state machine ----------------------------------------------
// idle -> receiving -> validating -> refreshing -> idle
// Any non-idle state can drop to idle via error/disconnect/timeout.
enum class State : uint8_t {
  IDLE = 0,
  RECEIVING,
  VALIDATING,
  WRITING,
  REFRESHING,
  ERROR,
};

const char *state_to_string(State state);

enum class TransferMode : uint8_t {
  NONE = 0,
  DIRECT,  // 0x0070 / 0x0071 / 0x0072
  PIPE,    // 0x0080 / 0x0081 / 0x0082
};

// --- Internal error categories ----------------------------------------------
// These are LOCAL diagnostic categories. They are mapped to canonical NACK codes
// at the point of response, and that mapping is opcode-scoped -- the same
// canonical byte means different things under different opcodes (e.g. 0x03 is
// OD_ERR_PARTIAL_RECT_OOB under 0x76 but OD_ERR_PIPE_START_SIZE_MISMATCH under
// 0x80). Never translate one of these to a wire byte without the opcode.
enum class ErrorCode : uint8_t {
  NONE = 0,
  BUSY,
  UNSUPPORTED_COMMAND,
  MALFORMED_PACKET,
  INVALID_STATE,
  UNSUPPORTED_FORMAT,
  INVALID_SIZE,
  SEQUENCE_GAP,
  WINDOW_VIOLATION,
  QUEUE_FULL,
  BACKEND_BEGIN_FAILED,
  BACKEND_WRITE_FAILED,
  BACKEND_FINISH_FAILED,
  TRANSFER_TIMEOUT,
  REFRESH_TIMEOUT,
  DISCONNECTED,
};

const char *error_to_string(ErrorCode error);

// --- PIPE negotiation --------------------------------------------------------
// THESE ARE OUR MAXIMA, NOT FIXED VALUES. 0x0080 carries the client's
// [req_window:1][req_ack_every:1][client_max_frame:2 LE], and the response must
// echo negotiated maxima with the MIN RULE applied
// (opendisplay_protocol.h:592-616). Legal range is 1..32.
//
// Treating these as fixed DEADLOCKS: a client requesting window 1 / ack_every 1
// sends one packet and waits, while a receiver hard-coded to ACK every 4 waits
// for three more. Always compute:
//     window     = min(req_window,     PIPE_MAX_WINDOW)
//     ack_every  = min(req_ack_every,  PIPE_MAX_ACK_EVERY)
//     frame      = min(client_max_frame, PIPE_MAX_FRAME)
static constexpr uint8_t PIPE_MAX_WINDOW = 16;
static constexpr uint8_t PIPE_MAX_ACK_EVERY = 4;
static constexpr uint8_t PIPE_WINDOW_LIMIT_LO = 1;
static constexpr uint8_t PIPE_WINDOW_LIMIT_HI = 32;

// resp_flags bit0 = selective-repeat supported. We have no reorder queue, so we
// MUST clear it -- that bit is the protocol's own mechanism for saying
// "in-order only", which this profile previously reinvented as a unilateral
// policy. bit1 = partial accepted; also always clear (no partial support).
static constexpr uint8_t PIPE_RESP_FLAG_SELECTIVE_REPEAT = 0x01u;
static constexpr uint8_t PIPE_RESP_FLAG_PARTIAL_ACCEPTED = 0x02u;

static constexpr uint8_t MAX_ACTIVE_TRANSFERS = 1;

// --- Colour scheme -----------------------------------------------------------
// Carry the CANONICAL ColorScheme byte (opendisplay_structs.h:542) rather than a
// local enum. An earlier local `PixelFormat` could not express the distinctions
// the wire depends on -- GRAY4 (2bpp) vs GRAY16 (4bpp) have DIFFERENT frame byte
// counts, and BWGBRY vs BWGBRY_SPLIT differ in plane layout. Collapsing either
// pair produces a config that parses cleanly and then corrupts every image.
//
// This value goes straight into the 0x0040 DisplayConfig response and is the
// input to the frame-size formula, so there is exactly one representation.
using ColorSchemeByte = uint8_t;  // values: OD_COLOR_SCHEME_* from the canonical header

// Three-valued on the wire; do not model as a bool.
enum class PartialSupport : uint8_t {
  NONE = 0,
  SUPPORTED = 1,
  UNKNOWN = 0xFF,
};

// --- Refresh ----------------------------------------------------------------
//
// v1 POLICY: ALWAYS FULL. Neither driver actually offers mode selection --
// epaper_spi::update() takes no mode argument, and it8951's
// prepare_update_region_ forces GC16 on the full-update cycle -- so the enum
// exists to parse the wire, not to steer the backend.
//
// The refresh:1 byte on 0x0072 / 0x0082 is parsed and IGNORED. We advertise full
// refresh only, so a conforming client never asks for anything else; a client
// that does is honoured as FULL rather than NACKed, and logged at debug.
//
// The two opcodes encode this byte DIFFERENTLY (0x0072: 0=FULL, 1=FAST/PARTIAL;
// 0x0082: 0=FULL, 1=FAST, 2/absent=PARTIAL). Since the value is unused, only its
// PRESENCE matters -- it shifts where an optional new_etag begins.
enum class RefreshMode : uint8_t {
  FULL = 0,
  FAST = 1,
  PARTIAL = 2,
};

struct RefreshRequest {
  RefreshMode mode{RefreshMode::FULL};  // backends may assume FULL in v1
};

// --- Capabilities ------------------------------------------------------------
// ONE capability model drives both the backend and the 0x0040 config response,
// so the two can never disagree. A backend must not claim partial refresh,
// compression, or a color format merely because another OpenDisplay device
// supports it.
struct DisplayCapabilities {
  uint16_t width{0};
  uint16_t height{0};
  uint8_t rotation{0};

  ColorSchemeByte color_scheme{0};  // OD_COLOR_SCHEME_MONO
  // uint16_t, NOT uint8_t: the wire field is u16 (opendisplay_structs.h:694) and
  // IT8951 panel ids are 3000/3001 -- a uint8_t silently truncates them.
  uint16_t panel_ic_type{0};

  // NOT a wire field: both peers derive it from (width, height, color_scheme).
  // A mismatch surfaces at 0x0080 as OD_ERR_PIPE_START_SIZE_MISMATCH.
  uint32_t full_frame_bytes{0};
  uint16_t row_stride_bytes{0};
  uint8_t byte_alignment{1};

  // v1: full only. Do not set the other two without also implementing real mode
  // selection -- advertising a mode we silently downgrade is the failure this
  // component is meant to avoid.
  bool supports_full_refresh{true};
  bool supports_fast_refresh{false};
  PartialSupport supports_partial{PartialSupport::NONE};
  uint16_t partial_x_alignment{1};
  uint16_t partial_y_alignment{1};

  bool supports_direct_write{false};
  bool supports_pipe_write{false};
  bool supports_compression{false};

  uint32_t max_contiguous_write{0};
  // Staging is 0 for epaper_spi: writes land in the driver's own framebuffer,
  // so this component allocates nothing and needs no PSRAM. Non-zero only if a
  // backend genuinely requires its own scratch (e.g. IT8951 conversion).
  uint32_t staging_bytes_required{0};
};

// --- Frame descriptor --------------------------------------------------------
struct FrameDescriptor {
  uint32_t total_bytes{0};
  ColorSchemeByte color_scheme{0};
  bool compressed{false};
  bool partial{false};
  uint16_t x{0}, y{0}, w{0}, h{0};  // partial only
};

// --- Backend results ---------------------------------------------------------
enum class BackendResult : uint8_t {
  OK = 0,
  UNSUPPORTED,
  INVALID_ARGUMENT,
  OUT_OF_MEMORY,
  DEVICE_ERROR,
};

enum class PollResult : uint8_t {
  PENDING = 0,
  COMPLETE,
  FAILED,
};

}  // namespace opendisplay
}  // namespace esphome
