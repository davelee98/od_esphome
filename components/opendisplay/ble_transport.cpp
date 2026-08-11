#include "ble_transport.h"

#include <cstring>

namespace esphome {
namespace opendisplay {

static inline size_t next_index(size_t i, size_t cap) { return (i + 1) % cap; }

bool RxQueue::push(const uint8_t *data, size_t len, uint32_t generation, uint32_t now_ms) {
  if (data == nullptr || len == 0 || len > RX_SLOT_BYTES)
    return false;  // oversize writes are rejected, never truncated

  const size_t head = this->head_;
  const size_t next = next_index(head, RX_QUEUE_SLOTS);
  if (next == this->tail_) {
    this->overflows_++;
    return false;  // full: reject immediately, do not block the callback
  }

  RxPacket &slot = this->slots_[head];
  slot.connection_generation = generation;
  slot.payload_length = static_cast<uint16_t>(len);
  slot.arrival_ms = now_ms;
  std::memcpy(slot.payload, data, len);

  this->head_ = next;

  const size_t depth = (next + RX_QUEUE_SLOTS - this->tail_) % RX_QUEUE_SLOTS;
  if (depth > this->high_water_)
    this->high_water_ = depth;
  return true;
}

bool RxQueue::pop(RxPacket *out) {
  const size_t tail = this->tail_;
  if (tail == this->head_)
    return false;
  *out = this->slots_[tail];
  this->tail_ = next_index(tail, RX_QUEUE_SLOTS);
  return true;
}

bool TxQueue::push(const Response &r, bool terminal) {
  if (r.empty())
    return true;  // nothing to send is not a failure

  const size_t next = next_index(this->head_, TX_QUEUE_SLOTS);
  if (next == this->tail_)
    return false;  // genuinely full

  if (!terminal) {
    // Keep headroom so an END ACK / 0x0073 / 0x0074 always fits. Dropping a
    // terminal frame leaves the client waiting forever after the panel has
    // already changed.
    const size_t used = (this->head_ + TX_QUEUE_SLOTS - this->tail_) % TX_QUEUE_SLOTS;
    if (used + TX_RESERVED_FOR_TERMINAL >= TX_QUEUE_SLOTS)
      return false;
  }

  this->slots_[this->head_] = r;
  this->head_ = next;
  return true;
}

bool TxQueue::pop(Response *out) {
  if (this->tail_ == this->head_)
    return false;
  *out = this->slots_[this->tail_];
  this->tail_ = next_index(this->tail_, TX_QUEUE_SLOTS);
  return true;
}

}  // namespace opendisplay
}  // namespace esphome
