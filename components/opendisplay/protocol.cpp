#include "protocol.h"

#include <cstring>

#include "opendisplay_structs.h"

namespace esphome {
namespace opendisplay {

// --- helpers -----------------------------------------------------------------

static inline void put_u16_le(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xFF);
  p[1] = static_cast<uint8_t>(v >> 8);
}

static inline uint16_t get_u16_le(const uint8_t *p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}

static inline uint32_t get_u32_le(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool parse_frame(const uint8_t *data, size_t len, Frame *out) {
  if (data == nullptr || out == nullptr || len < 2)
    return false;
  // Opcode is BIG-endian on the wire; everything after it is little-endian.
  out->opcode = static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
  out->payload = len > 2 ? data + 2 : nullptr;
  out->payload_len = len - 2;
  return true;
}

// --- generic responses -------------------------------------------------------

Response encode_ack(uint16_t opcode) {
  Response r;
  r.data[0] = RESP_ACK;
  r.data[1] = static_cast<uint8_t>(opcode & 0xFF);
  r.len = 2;
  return r;
}

Response encode_nack(uint16_t opcode) {
  Response r;
  r.data[0] = RESP_NACK;
  r.data[1] = static_cast<uint8_t>(opcode & 0xFF);
  r.len = 2;
  return r;
}

Response encode_nack_err(uint16_t opcode, uint8_t scoped_error) {
  Response r = encode_nack(opcode);
  r.data[2] = scoped_error;
  r.data[3] = 0x00;
  r.len = 4;
  return r;
}

// --- 0x0080 ------------------------------------------------------------------

bool parse_pipe_start(const uint8_t *payload, size_t len, PipeStartRequest *out,
                      uint8_t *scoped_error) {
  // Payload excludes the 2 opcode bytes, so the 10-byte header is 8 here.
  static constexpr size_t HDR = 8;
  static constexpr size_t HDR_PARTIAL = HDR + 12;

  if (payload == nullptr || out == nullptr || len < HDR) {
    *scoped_error = OD_ERR_PIPE_START_BAD_HEADER;
    return false;
  }

  out->version = payload[0];
  if (out->version != PIPE_VERSION) {
    *scoped_error = OD_ERR_PIPE_START_BAD_HEADER;
    return false;
  }

  out->flags = payload[1];
  if ((out->flags & ~(PIPE_FLAG_COMPRESSED | PIPE_FLAG_PARTIAL)) != 0) {
    *scoped_error = OD_ERR_PIPE_START_UNKNOWN_FLAG;
    return false;
  }

  out->req_window = payload[2];
  out->req_ack_every = payload[3];
  out->client_max_frame = get_u16_le(payload + 4);
  // total_size is at header offset 6..9, i.e. payload 6..9 -- but the header is
  // 10 bytes counting the opcode, so within the payload it starts at 4+2 = 6.
  if (len < HDR) {
    *scoped_error = OD_ERR_PIPE_START_BAD_HEADER;
    return false;
  }
  out->total_size = get_u32_le(payload + 6);

  out->partial = (out->flags & PIPE_FLAG_PARTIAL) != 0;
  if (out->partial) {
    if (len < HDR_PARTIAL) {
      *scoped_error = OD_ERR_PIPE_START_BAD_HEADER;
      return false;
    }
    // We never accept partial; report it in the 0x80-scoped namespace.
    *scoped_error = OD_ERR_PIPE_START_PARTIAL_UNSUPPORTED;
    return false;
  }

  // Window and cadence are negotiated, but a request outside the legal range is
  // a malformed header rather than something to clamp silently.
  if (out->req_window < PIPE_WINDOW_LIMIT_LO || out->req_window > PIPE_WINDOW_LIMIT_HI ||
      out->req_ack_every < PIPE_WINDOW_LIMIT_LO || out->req_ack_every > PIPE_WINDOW_LIMIT_HI) {
    *scoped_error = OD_ERR_PIPE_START_BAD_HEADER;
    return false;
  }

  *scoped_error = 0;
  return true;
}

Response encode_pipe_start_response(uint8_t max_window, uint8_t max_ack_every, uint16_t max_frame,
                                    uint8_t resp_flags) {
  Response r;
  r.data[0] = RESP_ACK;
  r.data[1] = static_cast<uint8_t>(CMD_PIPE_WRITE_START & 0xFF);
  r.data[2] = PIPE_VERSION;
  r.data[3] = max_window;
  r.data[4] = max_ack_every;
  put_u16_le(r.data + 5, max_frame);
  r.data[7] = resp_flags;
  r.len = 8;
  return r;
}

// --- 0x0081 ------------------------------------------------------------------

Response encode_pipe_sack(uint8_t highest_seen, uint32_t ack_mask) {
  Response r;
  r.data[0] = RESP_ACK;
  r.data[1] = static_cast<uint8_t>(CMD_PIPE_WRITE_DATA & 0xFF);
  r.data[2] = highest_seen;
  r.data[3] = static_cast<uint8_t>(ack_mask & 0xFF);
  r.data[4] = static_cast<uint8_t>((ack_mask >> 8) & 0xFF);
  r.data[5] = static_cast<uint8_t>((ack_mask >> 16) & 0xFF);
  r.data[6] = static_cast<uint8_t>((ack_mask >> 24) & 0xFF);
  r.len = 7;
  return r;
}

Response encode_pipe_nack(uint8_t err, uint8_t highest_seen, uint32_t ack_mask) {
  Response r;
  r.data[0] = RESP_NACK;
  r.data[1] = static_cast<uint8_t>(CMD_PIPE_WRITE_DATA & 0xFF);
  r.data[2] = err;
  r.data[3] = highest_seen;
  r.data[4] = static_cast<uint8_t>(ack_mask & 0xFF);
  r.data[5] = static_cast<uint8_t>((ack_mask >> 8) & 0xFF);
  r.data[6] = static_cast<uint8_t>((ack_mask >> 16) & 0xFF);
  r.data[7] = static_cast<uint8_t>((ack_mask >> 24) & 0xFF);
  r.len = 8;
  return r;
}

uint32_t build_inorder_ack_mask(uint32_t accepted_packets) {
  // highest_seen is implicitly acked, so the mask describes its predecessors.
  // Strictly in-order receipt means every predecessor that exists was received.
  if (accepted_packets <= 1)
    return 0;
  const uint32_t predecessors = accepted_packets - 1;
  if (predecessors >= PIPE_ACK_MASK_BITS)
    return 0xFFFFFFFFu;
  return (1u << predecessors) - 1u;
}

// --- 0x0043 ------------------------------------------------------------------

Response encode_version(uint8_t major, uint8_t minor, uint8_t patch, const char *sha) {
  Response r;
  r.data[0] = RESP_ACK;
  r.data[1] = static_cast<uint8_t>(CMD_FIRMWARE_VERSION & 0xFF);
  r.data[2] = major;
  r.data[3] = minor;

  size_t sha_len = 0;
  if (sha != nullptr) {
    while (sha[sha_len] != '\0' && sha_len < 40)
      sha_len++;
  }
  r.data[4] = static_cast<uint8_t>(sha_len);
  for (size_t i = 0; i < sha_len; i++)
    r.data[5 + i] = static_cast<uint8_t>(sha[i]);
  // Newer firmware always appends patch after the SHA so major/minor/shaLen stay
  // at fixed offsets for old hosts.
  r.data[5 + sha_len] = patch;
  r.len = static_cast<uint8_t>(6 + sha_len);
  return r;
}

// --- MSD ---------------------------------------------------------------------

void build_msd(const MsdInputs &in, uint8_t out[MSD_BYTES]) {
  struct MsdAdvertisement m;
  std::memset(&m, 0, sizeof m);

  m.company_id = OD_BLE_MANUFACTURER_ID;

  // dynamic[11] stays zero: an ESPHome build has no buttons, touch controller,
  // or OpenDisplay-managed sensors, so no config packet ever points a client at
  // a slot. This matches the reference firmware, which memsets then writes only
  // configured slots.

  int32_t temp_encoded = static_cast<int32_t>((in.chip_temperature_c + 40.0f) * 2.0f);
  if (temp_encoded < 0)
    temp_encoded = 0;
  else if (temp_encoded > 255)
    temp_encoded = 255;
  m.chip_temperature = static_cast<uint8_t>(temp_encoded);

  uint16_t batt = in.battery_10mv;
  if (batt > 511)
    batt = 511;
  m.battery_voltage_low = static_cast<uint8_t>(batt & 0xFF);

  uint8_t status = 0;
  if ((batt >> 8) & 0x01)
    status |= OD_MSD_STATUS_BATTERY_VOLTAGE_BIT8;
  if (in.reboot_flag)
    status |= OD_MSD_STATUS_REBOOT_FLAG;
  if (in.connection_requested)
    status |= OD_MSD_STATUS_CONNECTION_REQUESTED;
  status |= static_cast<uint8_t>((in.loop_counter << OD_MSD_STATUS_MAIN_LOOP_COUNTER_SHIFT) &
                                 OD_MSD_STATUS_MAIN_LOOP_COUNTER_MASK);
  m.status = status;

  std::memcpy(out, &m, MSD_BYTES);
}

Response encode_msd_response(const uint8_t msd[MSD_BYTES]) {
  Response r;
  r.data[0] = RESP_ACK;
  r.data[1] = static_cast<uint8_t>(CMD_READ_MSD & 0xFF);
  std::memcpy(r.data + 2, msd, MSD_BYTES);
  r.len = 2 + MSD_BYTES;
  return r;
}

// --- config blob -------------------------------------------------------------

uint16_t crc16_ccitt_false(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
  }
  return crc;
}

// Appends [number][id][payload] and advances *pos.
template<typename T>
static bool append_packet(uint8_t *out, size_t cap, size_t *pos, uint8_t id, const T &payload) {
  if (*pos + 2 + sizeof(T) > cap)
    return false;
  out[(*pos)++] = 0;  // instance number: singletons are 0
  out[(*pos)++] = id;
  std::memcpy(out + *pos, &payload, sizeof(T));
  *pos += sizeof(T);
  return true;
}

ConfigBuildError build_config_blob(const DisplayCapabilities &caps, uint16_t ic_type, uint8_t *out,
                                   size_t out_cap, size_t *out_len) {
  // Validate BEFORE serialising: an invalid capability set must yield the error
  // frame, never a plausible-looking blob.
  if (caps.width == 0 || caps.height == 0)
    return ConfigBuildError::ZERO_GEOMETRY;
  if (caps.full_frame_bytes == 0)
    return ConfigBuildError::FRAME_BYTES_ZERO;
  if (!caps.supports_direct_write && !caps.supports_pipe_write)
    return ConfigBuildError::NO_TRANSMISSION_MODE;

  size_t pos = 0;
  if (out_cap < sizeof(struct OuterPacketHeader))
    return ConfigBuildError::ZERO_GEOMETRY;

  struct OuterPacketHeader outer;
  std::memset(&outer, 0, sizeof outer);
  // length is inconsistently populated across encoders and the CRC is always
  // computed as if it were zero; the reference device ignores it on parse. Emit
  // zero and let the CRC rule stand.
  outer.length = 0;
  outer.version = OD_CONFIG_VERSION;
  std::memcpy(out, &outer, sizeof outer);
  pos += sizeof outer;

  struct SystemConfig sys;
  std::memset(&sys, 0, sizeof sys);
  sys.ic_type = ic_type;  // cosmetic; 0 is acceptable for unmapped ESP32 variants
  if (!append_packet(out, out_cap, &pos, OD_PKT_SYSTEM, sys))
    return ConfigBuildError::ZERO_GEOMETRY;

  struct ManufacturerData mfr;
  std::memset(&mfr, 0, sizeof mfr);
  mfr.manufacturer_id = OD_MANUFACTURER_DIY;
  if (!append_packet(out, out_cap, &pos, OD_PKT_MANUFACTURER, mfr))
    return ConfigBuildError::ZERO_GEOMETRY;

  struct PowerOption pwr;
  std::memset(&pwr, 0, sizeof pwr);
  if (!append_packet(out, out_cap, &pos, OD_PKT_POWER, pwr))
    return ConfigBuildError::ZERO_GEOMETRY;

  struct DisplayConfig dsp;
  std::memset(&dsp, 0, sizeof dsp);
  dsp.pixel_width = caps.width;
  dsp.pixel_height = caps.height;
  dsp.color_scheme = caps.color_scheme;
  dsp.panel_ic_type = caps.panel_ic_type;
  // Partial is three-valued on the wire; v1 never advertises it.
  dsp.partial_update_support = 0;

  // THE field the client uses to choose a transfer path. Leaving it 0 advertises
  // a device that supports no transmission mode at all, and the client then has
  // nothing to send with. Compression bits stay CLEAR -- that is what makes the
  // client fall back to uncompressed, which v1 depends on.
  dsp.transmission_modes = 0;
  if (caps.supports_direct_write)
    dsp.transmission_modes |= OD_TRANSMISSION_MODE_DIRECT_WRITE;
  if (caps.supports_pipe_write)
    dsp.transmission_modes |= OD_TRANSMISSION_MODE_PIPE_WRITE;

  // 0xFF means "none" for pin fields; a memset would claim GPIO 0. We do not
  // expose the driver's pins, so report them all as absent.
  dsp.reset_pin = 0xFF;
  dsp.busy_pin = 0xFF;
  dsp.cs_pin = 0xFF;
  dsp.cs_pin_2 = 0xFF;
  if (!append_packet(out, out_cap, &pos, OD_PKT_DISPLAY, dsp))
    return ConfigBuildError::UNSUPPORTED_SCHEME;

  if (pos + 2 > out_cap)
    return ConfigBuildError::ZERO_GEOMETRY;

  // CRC covers length+version+packets with the two length bytes forced to zero
  // -- they already are, above -- and excludes the CRC field itself.
  const uint16_t crc = crc16_ccitt_false(out, pos);
  put_u16_le(out + pos, crc);
  pos += 2;

  *out_len = pos;
  return ConfigBuildError::NONE;
}

Response encode_config_chunk(const uint8_t *blob, size_t blob_len, uint16_t index) {
  Response r;
  // Chunk 0 header is 6 bytes (status, echo, chunk:2, total:2); later chunks 4.
  const uint8_t header = (index == 0) ? 6 : 4;
  const uint8_t capacity = static_cast<uint8_t>(MAX_RESPONSE_DATA_SIZE - header);

  // Offsets follow the reference implementation exactly: chunk 0 carries 94
  // bytes, every later chunk 96.
  size_t offset;
  if (index == 0) {
    offset = 0;
  } else {
    offset = static_cast<size_t>(MAX_RESPONSE_DATA_SIZE - 6) +
             static_cast<size_t>(index - 1) * (MAX_RESPONSE_DATA_SIZE - 4);
  }
  if (offset >= blob_len)
    return r;  // exhausted

  size_t remaining = blob_len - offset;
  const uint8_t n = remaining < capacity ? static_cast<uint8_t>(remaining) : capacity;

  r.data[0] = RESP_ACK;
  r.data[1] = static_cast<uint8_t>(CMD_CONFIG_READ & 0xFF);
  put_u16_le(r.data + 2, index);
  if (index == 0)
    put_u16_le(r.data + 4, static_cast<uint16_t>(blob_len));
  std::memcpy(r.data + header, blob + offset, n);
  r.len = static_cast<uint8_t>(header + n);
  return r;
}

}  // namespace opendisplay
}  // namespace esphome
