#pragma once

// BLE GATT binding and bounded RX handling.
//
// THE RULE: the BLE write callback does exactly four things --
//   1. check connection/state,
//   2. bounds-check the payload length,
//   3. copy into a fixed-capacity slot and enqueue,
//   4. reject immediately if the queue is full.
// It must NEVER allocate a frame, convert pixels, touch SPI, or wait on panel
// BUSY. Everything else happens in loop().
//
// BLE callbacks and loop() are distinct execution contexts, so the queue handoff
// is guarded. Only loop() touches the backend or terminal transaction state.

#include <cstddef>
#include <cstdint>

#include "protocol.h"
#include "protocol_types.h"

namespace esphome {
namespace opendisplay {

// 0x2446 is overloaded across the project: BLE service UUID (16-bit short form),
// BLE manufacturer ID (9286 decimal), and the default LAN TCP port.
static constexpr uint16_t OD_BLE_SERVICE_UUID = 0x2446;

// Always request OD_BLE_MAX_FRAME (256) and declare the GATT value length to
// match: the canonical guidance is to declare preferred MTU here rather than the
// 512 ATT maximum, so an oversize write draws ATT error 0x0D instead of being
// silently dropped (opendisplay_protocol.h:63-73, :886).
static constexpr uint16_t OD_BLE_PREFERRED_MTU = OD_BLE_MAX_FRAME;      // 256
static constexpr size_t OD_BLE_MAX_ATT_VALUE = OD_BLE_MAX_FRAME - 3;    // 253

// Response frames use the standardized MAX_RESPONSE_DATA_SIZE (100), NOT a
// negotiated-MTU derivation -- the reference Firmware chunks 0x0040 against
// exactly that constant.
static constexpr size_t RX_SLOT_BYTES = PIPE_MAX_FRAME;  // 244, the GATT write ceiling
static constexpr size_t RX_QUEUE_SLOTS = 24;             // >= max PIPE window + headroom

// Fixed-capacity descriptor. No std::vector, no per-packet allocation.
struct RxPacket {
  uint32_t connection_generation{0};
  uint16_t payload_length{0};
  uint32_t arrival_ms{0};
  uint8_t payload[RX_SLOT_BYTES]{};
};

// Single-producer (BLE callback) / single-consumer (loop) ring.
class RxQueue {
 public:
  // Returns false when full -- the caller must reject immediately rather than
  // block or allocate.
  bool push(const uint8_t *data, size_t len, uint32_t generation, uint32_t now_ms);
  bool pop(RxPacket *out);

  size_t high_water() const { return this->high_water_; }
  uint32_t overflows() const { return this->overflows_; }

 protected:
  RxPacket slots_[RX_QUEUE_SLOTS];
  volatile size_t head_{0};
  volatile size_t tail_{0};
  size_t high_water_{0};
  uint32_t overflows_{0};
};

// Outbound frames are queued too, so a stalled notification path cannot block
// display progression. Terminal frames (END ACK, 0x0073, 0x0074) reserve
// capacity: if they were dropped the panel would change while the client waited
// in "refreshing" forever.
static constexpr size_t TX_QUEUE_SLOTS = 16;
static constexpr size_t TX_RESERVED_FOR_TERMINAL = 4;

class TxQueue {
 public:
  bool push(const Response &r, bool terminal);
  bool pop(Response *out);
  bool empty() const { return this->head_ == this->tail_; }

 protected:
  Response slots_[TX_QUEUE_SLOTS];
  size_t head_{0};
  size_t tail_{0};
};

}  // namespace opendisplay
}  // namespace esphome
