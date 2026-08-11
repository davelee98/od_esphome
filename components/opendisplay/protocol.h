#pragma once

// Transport-neutral frame parsing and response encoding.
//
// No ESPHome or ESP-IDF includes: this is host-testable against a fake
// transport, and a future LAN/TCP transport reuses it unchanged.
//
// Wire rules that this file exists to centralise:
//   * the opcode is 2 bytes BIG-endian; payload multi-byte fields are
//     LITTLE-endian unless a block says otherwise (opendisplay_protocol.h:180);
//   * a response is [status][cmd_echo][data...] where cmd_echo is the command's
//     LOW byte (:192);
//   * NACK error codes are OPCODE-SCOPED -- 0x03 means different things under
//     0x76 and 0x80 (:196-204). There is deliberately no shared error->byte
//     mapping helper here.

#include <cstddef>
#include <cstdint>

#include "protocol_types.h"

namespace esphome {
namespace opendisplay {

// A response never exceeds one notification frame.
struct Response {
  uint8_t data[MAX_RESPONSE_DATA_SIZE]{};
  uint8_t len{0};

  bool empty() const { return this->len == 0; }
};

struct Frame {
  uint16_t opcode{0};
  const uint8_t *payload{nullptr};
  size_t payload_len{0};
};

// Returns false if the buffer is too short to carry an opcode.
bool parse_frame(const uint8_t *data, size_t len, Frame *out);

// --- Generic responses -------------------------------------------------------
Response encode_ack(uint16_t opcode);
Response encode_nack(uint16_t opcode);
// NACK carrying one opcode-scoped error byte.
Response encode_nack_err(uint16_t opcode, uint8_t scoped_error);

// --- 0x0080 PIPE START -------------------------------------------------------
struct PipeStartRequest {
  uint8_t version{0};
  uint8_t flags{0};
  uint8_t req_window{0};
  uint8_t req_ack_every{0};
  uint16_t client_max_frame{0};
  uint32_t total_size{0};
  bool partial{false};
};

// Parses the 10-byte header (22 when partial). Returns false on a short or
// malformed header; `*scoped_error` is then an OD_ERR_PIPE_START_* value.
bool parse_pipe_start(const uint8_t *payload, size_t len, PipeStartRequest *out,
                      uint8_t *scoped_error);

// [0x00][0x80][ver][max_window][max_ack_every][max_frame:2 LE][resp_flags]
Response encode_pipe_start_response(uint8_t max_window, uint8_t max_ack_every, uint16_t max_frame,
                                    uint8_t resp_flags);

// --- 0x0081 PIPE DATA --------------------------------------------------------
// SACK: [0x00][0x81][highest_seen][ack_mask:4 LE]
// mask bit i (LSB first) = chunk (highest_seen - 1 - i) received; highest_seen
// is implicitly acked. A zero mask acknowledges ONLY highest_seen.
Response encode_pipe_sack(uint8_t highest_seen, uint32_t ack_mask);

// FATAL NACK: [0xFF][0x81][err][highest_seen][ack_mask:4 LE]
// After this the device discards further 0x81 frames until the next START.
Response encode_pipe_nack(uint8_t err, uint8_t highest_seen, uint32_t ack_mask);

static constexpr uint8_t OD_PIPE_NACK_DECOMPRESS = 0x02u;
static constexpr uint8_t OD_PIPE_NACK_WRITE_SIZE = 0x03u;
static constexpr uint8_t OD_PIPE_NACK_PROTOCOL = 0x04u;

// Build the in-order SACK mask: with no reorder queue every chunk below
// `highest_seen` has been received, so set one bit per existing predecessor.
uint32_t build_inorder_ack_mask(uint32_t accepted_packets);

// --- 0x0043 firmware version -------------------------------------------------
// [0x00][0x43][major][minor][shaLen][sha...][patch]
Response encode_version(uint8_t major, uint8_t minor, uint8_t patch, const char *sha);

// --- 0x0044 READ_MSD / advertisement ----------------------------------------
static constexpr size_t MSD_BYTES = 16;

// BLE company identifier carried as the first field of MsdAdvertisement. Same
// numeric value as the service UUID and the LAN port -- 0x2446 is overloaded
// three ways across this project. Defined here rather than in ble_transport.h so
// the MSD builder stays transport-independent and host-testable.
static constexpr uint16_t OD_BLE_MANUFACTURER_ID = 0x2446;

struct MsdInputs {
  float chip_temperature_c{0.0f};
  uint16_t battery_10mv{0};  // 0 = no battery, the reference firmware's own encoding
  bool reboot_flag{false};
  bool connection_requested{false};
  uint8_t loop_counter{0};  // 4-bit, free-running
};

// Builds the canonical 16-byte MsdAdvertisement. dynamic[11] is always zero for
// an ESPHome build: no buttons, touch, or OpenDisplay-managed sensors exist.
void build_msd(const MsdInputs &in, uint8_t out[MSD_BYTES]);

// [0x00][0x44][msd:16]
Response encode_msd_response(const uint8_t msd[MSD_BYTES]);

// --- 0x0040 config read ------------------------------------------------------
// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, MSB-first, no reflection, no
// final XOR.
uint16_t crc16_ccitt_false(const uint8_t *data, size_t len);

// Worst case for our four packets: 3 outer + 4*(2 + payload) + 2 CRC.
static constexpr size_t CONFIG_BLOB_MAX = 160;

enum class ConfigBuildError : uint8_t {
  NONE = 0,
  ZERO_GEOMETRY,
  UNSUPPORTED_SCHEME,
  NO_TRANSMISSION_MODE,
  FRAME_BYTES_ZERO,
  // Deliberately no IC_TYPE_UNKNOWN: ic_type is cosmetic, 0 is acceptable, and
  // no client consumes it behaviourally.
};

// Synthesises the immutable config blob from our capabilities. Built once at
// setup(); a failure here must produce the [0xFF][0x40][0x00][0x00] error frame
// rather than a plausible-looking blob.
ConfigBuildError build_config_blob(const DisplayCapabilities &caps, uint16_t ic_type,
                                   uint8_t *out, size_t out_cap, size_t *out_len);

// Emits chunk `index` of `blob`. Returns an empty Response when the blob is
// exhausted. Chunk 0 carries the 2-byte total length; later chunks do not.
Response encode_config_chunk(const uint8_t *blob, size_t blob_len, uint16_t index);

}  // namespace opendisplay
}  // namespace esphome
