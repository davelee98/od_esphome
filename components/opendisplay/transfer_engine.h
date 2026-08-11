#pragma once

// Byte and sequence accounting for both transfer paths.
//
// Host-testable: no ESPHome or ESP-IDF includes. Drive it with a fake backend
// and a fake clock.
//
// Invariants this class exists to enforce:
//   * exactly one active transaction;
//   * every offset/size computation is checked BEFORE any buffer access;
//   * a transfer is valid only when accepted_bytes == expected_frame_bytes;
//   * PIPE_WRITE is strictly in-order with NO reorder queue -- advertised by
//     CLEARING resp_flags bit0 (selective-repeat) in the 0x0080 response;
//   * window and ACK cadence are NEGOTIATED per transaction via the min rule,
//     never fixed (see PIPE_MAX_* in protocol_types.h);
//   * hardware ownership is tracked SEPARATELY from protocol state: a refresh
//     timeout ends the transaction but must not release the panel while the
//     driver is still reading its framebuffer.

#include "backend.h"
#include "protocol.h"
#include "protocol_types.h"

namespace esphome {
namespace opendisplay {

struct Transaction {
  TransferMode mode{TransferMode::NONE};
  uint32_t connection_generation{0};
  State state{State::IDLE};

  uint32_t expected_frame_bytes{0};
  uint32_t accepted_bytes{0};
  uint32_t next_direct_offset{0};

  // seq is mod 256; every comparison must be modular.
  uint8_t next_pipe_sequence{0};
  uint8_t highest_seen{0};
  uint32_t accepted_packets{0};
  uint8_t accepted_since_ack{0};
  bool pipe_fatal{false};  // after a 0x81 NACK, discard until the next START

  // Negotiated at 0x0080 via the min rule.
  uint8_t negotiated_window{0};
  uint8_t negotiated_ack_every{0};
  uint16_t negotiated_frame{0};

  uint32_t transfer_deadline_ms{0};
  uint32_t refresh_deadline_ms{0};

  FrameDescriptor frame{};
  ErrorCode error{ErrorCode::NONE};

  void reset();
};

struct TransferStats {
  uint32_t transfers_ok{0};
  uint32_t transfers_aborted{0};
  uint32_t direct_packets{0};
  uint32_t pipe_packets{0};
  uint32_t retransmit_requests{0};
  uint32_t queue_overflows{0};
  uint32_t backend_failures{0};
  uint32_t refresh_timeouts{0};
  uint32_t queue_high_water{0};
};

// What the component should do after handing a frame to the engine.
struct EngineResult {
  Response response{};      // may be empty
  bool refresh_complete{false};  // emit 0x0073
  bool refresh_timeout{false};   // emit 0x0074
  bool transfer_started{false};
  bool terminal_error{false};
};

class TransferEngine {
 public:
  explicit TransferEngine(OpenDisplayBackend *backend) : backend_(backend) {}

  void set_timeouts(uint32_t transfer_ms, uint32_t refresh_ms) {
    this->transfer_timeout_ms_ = transfer_ms;
    this->refresh_timeout_ms_ = refresh_ms;
  }
  void set_connection_generation(uint32_t gen) { this->connection_generation_ = gen; }

  EngineResult on_direct_start(const uint8_t *payload, size_t len, uint32_t now_ms);
  EngineResult on_direct_data(const uint8_t *payload, size_t len, uint32_t now_ms);
  EngineResult on_direct_end(const uint8_t *payload, size_t len, uint32_t now_ms);

  EngineResult on_pipe_start(const uint8_t *payload, size_t len, uint32_t now_ms);
  EngineResult on_pipe_data(const uint8_t *payload, size_t len, uint32_t now_ms);
  EngineResult on_pipe_end(const uint8_t *payload, size_t len, uint32_t now_ms);

  // Advance refresh polling and deadline checks. Called every loop().
  EngineResult tick(uint32_t now_ms);

  void abort(ErrorCode error);
  void on_disconnect(uint32_t connection_generation);

  // Canonical 0x0082 ordering is: tail-flush SACK, then the end-ACK, then
  // 0x0073/0x0074. on_pipe_end() stages the SACK here; the component must send
  // it before EngineResult::response.
  bool take_tail_sack(Response *out) {
    if (!this->has_tail_sack_)
      return false;
    *out = this->pending_tail_sack_;
    this->has_tail_sack_ = false;
    return true;
  }

  bool is_busy() const { return this->txn_.state != State::IDLE || this->hw_owned_; }
  State state() const { return this->txn_.state; }
  ErrorCode last_error() const { return this->last_error_; }
  const Transaction &transaction() const { return this->txn_; }
  const TransferStats &stats() const { return this->stats_; }

 protected:
  // Common START handling: a new START ABORTS any in-flight transfer, per
  // opendisplay_protocol.h:501 and :615. It is never rejected.
  bool begin_transaction_(TransferMode mode, uint32_t total_bytes, uint32_t now_ms,
                          ErrorCode *why);
  EngineResult finish_and_refresh_(uint16_t end_opcode, uint32_t now_ms);
  void release_hardware_();

  OpenDisplayBackend *backend_;
  Transaction txn_{};
  TransferStats stats_{};
  ErrorCode last_error_{ErrorCode::NONE};

  uint32_t transfer_timeout_ms_{30000};
  uint32_t refresh_timeout_ms_{180000};
  uint32_t connection_generation_{0};

  // True from a successful begin_refresh() until the backend reports a terminal
  // poll result. Survives a refresh TIMEOUT: the protocol transaction ends, but
  // the panel is still ours until the driver is genuinely idle.
  bool hw_owned_{false};
  uint32_t refresh_started_ms_{0};

  Response pending_tail_sack_{};
  bool has_tail_sack_{false};
};

}  // namespace opendisplay
}  // namespace esphome
