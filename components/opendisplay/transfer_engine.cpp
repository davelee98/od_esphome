#include "transfer_engine.h"

namespace esphome {
namespace opendisplay {

void Transaction::reset() { *this = Transaction{}; }

const char *state_to_string(State state) {
  switch (state) {
    case State::IDLE:
      return "idle";
    case State::RECEIVING:
      return "receiving";
    case State::VALIDATING:
      return "validating";
    case State::WRITING:
      return "writing";
    case State::REFRESHING:
      return "refreshing";
    case State::ERROR:
      return "error";
  }
  return "unknown";
}

const char *error_to_string(ErrorCode error) {
  switch (error) {
    case ErrorCode::NONE:            return "none";
    case ErrorCode::BUSY:            return "busy";
    case ErrorCode::UNSUPPORTED_COMMAND: return "unsupported_command";
    case ErrorCode::MALFORMED_PACKET: return "malformed_packet";
    case ErrorCode::INVALID_STATE:   return "invalid_state";
    case ErrorCode::UNSUPPORTED_FORMAT: return "unsupported_format";
    case ErrorCode::INVALID_SIZE:    return "invalid_size";
    case ErrorCode::SEQUENCE_GAP:    return "sequence_gap";
    case ErrorCode::WINDOW_VIOLATION: return "window_violation";
    case ErrorCode::QUEUE_FULL:      return "queue_full";
    case ErrorCode::BACKEND_BEGIN_FAILED: return "backend_begin_failed";
    case ErrorCode::BACKEND_WRITE_FAILED: return "backend_write_failed";
    case ErrorCode::BACKEND_FINISH_FAILED: return "backend_finish_failed";
    case ErrorCode::TRANSFER_TIMEOUT: return "transfer_timeout";
    case ErrorCode::REFRESH_TIMEOUT: return "refresh_timeout";
    case ErrorCode::DISCONNECTED:    return "disconnected";
  }
  return "unknown";
}

// Wrap-safe deadline test. millis() wraps every ~49.7 days; a plain
// `now >= deadline` fires immediately for a transfer started near the wrap.
static inline bool deadline_passed(uint32_t now_ms, uint32_t deadline_ms) {
  return deadline_ms != 0 && static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

// --- terminal paths ----------------------------------------------------------

void TransferEngine::abort(ErrorCode error) {
  if (this->txn_.state != State::IDLE) {
    if (this->backend_ != nullptr)
      this->backend_->abort_transfer();
    this->stats_.transfers_aborted++;
  }
  this->txn_.reset();
  this->last_error_ = error;
  // NOTE: hw_owned_ is deliberately NOT cleared here. If a refresh was already
  // issued the driver cannot be cancelled, and accepting a new frame would
  // mutate the framebuffer while the driver reads it. release_hardware_() is
  // called only when the backend reports a terminal poll result.
}

void TransferEngine::release_hardware_() { this->hw_owned_ = false; }

void TransferEngine::on_disconnect(uint32_t connection_generation) {
  if (this->txn_.state == State::IDLE)
    return;
  if (this->txn_.connection_generation != connection_generation)
    return;  // stale generation: not our session
  // A disconnect during REFRESHING must not abort the physical refresh -- keep
  // polling and release ownership on the terminal result instead.
  if (this->txn_.state == State::REFRESHING)
    return;
  this->abort(ErrorCode::DISCONNECTED);
}

// --- shared START ------------------------------------------------------------

bool TransferEngine::begin_transaction_(TransferMode mode, uint32_t total_bytes, uint32_t now_ms,
                                        ErrorCode *why) {
  if (this->backend_ == nullptr) {
    *why = ErrorCode::BACKEND_BEGIN_FAILED;
    return false;
  }

  // The panel may still be physically refreshing from a previous transfer. The
  // protocol says a new START aborts the in-flight TRANSFER; it does not let us
  // scribble on a framebuffer the driver is currently reading.
  if (this->hw_owned_) {
    *why = ErrorCode::BUSY;
    return false;
  }

  // A new START ABORTS any in-flight transfer (opendisplay_protocol.h:501,
  // :615). It is never rejected -- that is how a client recovers from loss.
  if (this->txn_.state != State::IDLE)
    this->abort(ErrorCode::INVALID_STATE);

  const auto &caps = this->backend_->capabilities();
  if (total_bytes != caps.full_frame_bytes) {
    *why = ErrorCode::INVALID_SIZE;
    return false;
  }

  this->txn_.reset();
  this->txn_.mode = mode;
  this->txn_.connection_generation = this->connection_generation_;
  this->txn_.expected_frame_bytes = total_bytes;
  this->txn_.transfer_deadline_ms = now_ms + this->transfer_timeout_ms_;
  this->txn_.frame.total_bytes = total_bytes;
  this->txn_.frame.color_scheme = caps.color_scheme;

  if (this->backend_->begin_frame(this->txn_.frame) != BackendResult::OK) {
    this->txn_.reset();
    *why = ErrorCode::BACKEND_BEGIN_FAILED;
    return false;
  }

  this->txn_.state = State::RECEIVING;
  return true;
}

// --- direct write ------------------------------------------------------------

EngineResult TransferEngine::on_direct_start(const uint8_t *payload, size_t len, uint32_t now_ms) {
  EngineResult res;
  // v1 accepts only the uncompressed empty-payload form. The compressed form
  // carries [uncompressed_size:4 LE][zlib...], which we never advertise.
  if (len != 0) {
    this->last_error_ = ErrorCode::UNSUPPORTED_FORMAT;
    res.response = encode_nack(CMD_DIRECT_WRITE_START);
    res.terminal_error = true;
    return res;
  }
  (void) payload;

  const uint32_t total = this->backend_ != nullptr
                             ? this->backend_->capabilities().full_frame_bytes
                             : 0;
  ErrorCode why = ErrorCode::NONE;
  if (!this->begin_transaction_(TransferMode::DIRECT, total, now_ms, &why)) {
    this->last_error_ = why;
    res.response = encode_nack(CMD_DIRECT_WRITE_START);
    res.terminal_error = true;
    return res;
  }
  res.response = encode_ack(CMD_DIRECT_WRITE_START);
  res.transfer_started = true;
  return res;
}

EngineResult TransferEngine::on_direct_data(const uint8_t *payload, size_t len, uint32_t now_ms) {
  EngineResult res;
  if (this->txn_.state != State::RECEIVING || this->txn_.mode != TransferMode::DIRECT) {
    this->last_error_ = ErrorCode::INVALID_STATE;
    res.response = encode_nack(CMD_DIRECT_WRITE_DATA);
    return res;
  }
  if (payload == nullptr || len == 0) {
    this->abort(ErrorCode::MALFORMED_PACKET);
    res.response = encode_nack(CMD_DIRECT_WRITE_DATA);
    res.terminal_error = true;
    return res;
  }
  // Overflow check before any buffer access, phrased so the sum cannot wrap.
  if (len > this->txn_.expected_frame_bytes - this->txn_.accepted_bytes) {
    this->abort(ErrorCode::INVALID_SIZE);
    res.response = encode_nack(CMD_DIRECT_WRITE_DATA);
    res.terminal_error = true;
    return res;
  }

  if (this->backend_->write_contiguous(this->txn_.next_direct_offset, payload, len) !=
      BackendResult::OK) {
    this->stats_.backend_failures++;
    this->abort(ErrorCode::BACKEND_WRITE_FAILED);
    res.response = encode_nack(CMD_DIRECT_WRITE_DATA);
    res.terminal_error = true;
    return res;
  }

  this->txn_.next_direct_offset += len;
  this->txn_.accepted_bytes += len;
  this->txn_.transfer_deadline_ms = now_ms + this->transfer_timeout_ms_;
  this->stats_.direct_packets++;

  res.response = encode_ack(CMD_DIRECT_WRITE_DATA);
  return res;
}

EngineResult TransferEngine::on_direct_end(const uint8_t *payload, size_t len, uint32_t now_ms) {
  EngineResult res;
  // The refresh byte and an optional trailing new_etag:4 may be present. We act
  // on neither -- v1 is always FULL and keeps no displayed_etag -- but a frame
  // carrying them must be ACCEPTED, not treated as malformed.
  (void) payload;
  (void) len;

  if (this->txn_.state != State::RECEIVING || this->txn_.mode != TransferMode::DIRECT) {
    this->last_error_ = ErrorCode::INVALID_STATE;
    res.response = encode_nack(CMD_DIRECT_WRITE_END);
    return res;
  }
  return this->finish_and_refresh_(CMD_DIRECT_WRITE_END, now_ms);
}

// --- PIPE write --------------------------------------------------------------

EngineResult TransferEngine::on_pipe_start(const uint8_t *payload, size_t len, uint32_t now_ms) {
  EngineResult res;
  PipeStartRequest req;
  uint8_t scoped = 0;
  if (!parse_pipe_start(payload, len, &req, &scoped)) {
    this->last_error_ = ErrorCode::MALFORMED_PACKET;
    res.response = encode_nack_err(CMD_PIPE_WRITE_START, scoped);
    res.terminal_error = true;
    return res;
  }
  if ((req.flags & PIPE_FLAG_COMPRESSED) != 0) {
    // We advertise no compression in 0x0040; a client honouring that never gets
    // here. Reject in the 0x80-scoped namespace.
    this->last_error_ = ErrorCode::UNSUPPORTED_FORMAT;
    res.response = encode_nack_err(CMD_PIPE_WRITE_START, OD_ERR_PIPE_START_UNKNOWN_FLAG);
    res.terminal_error = true;
    return res;
  }

  ErrorCode why = ErrorCode::NONE;
  if (!this->begin_transaction_(TransferMode::PIPE, req.total_size, now_ms, &why)) {
    this->last_error_ = why;
    const uint8_t code = (why == ErrorCode::INVALID_SIZE) ? OD_ERR_PIPE_START_SIZE_MISMATCH
                                                          : OD_ERR_PIPE_START_BAD_HEADER;
    res.response = encode_nack_err(CMD_PIPE_WRITE_START, code);
    res.terminal_error = true;
    return res;
  }

  // MIN RULE. Treating these as fixed deadlocks a client that asked for
  // window 1 / ack_every 1.
  this->txn_.negotiated_window =
      req.req_window < PIPE_MAX_WINDOW ? req.req_window : PIPE_MAX_WINDOW;
  this->txn_.negotiated_ack_every =
      req.req_ack_every < PIPE_MAX_ACK_EVERY ? req.req_ack_every : PIPE_MAX_ACK_EVERY;
  this->txn_.negotiated_frame =
      req.client_max_frame < PIPE_MAX_FRAME ? req.client_max_frame : PIPE_MAX_FRAME;

  // resp_flags: selective-repeat CLEARED (no reorder queue -- this bit is the
  // protocol's own way of saying in-order only), partial CLEARED.
  res.response = encode_pipe_start_response(this->txn_.negotiated_window,
                                            this->txn_.negotiated_ack_every,
                                            this->txn_.negotiated_frame, 0x00);
  res.transfer_started = true;
  return res;
}

EngineResult TransferEngine::on_pipe_data(const uint8_t *payload, size_t len, uint32_t now_ms) {
  EngineResult res;
  // After a fatal NACK, further 0x81 frames are discarded until the next START
  // or disconnect (opendisplay_protocol.h:630-633). Silence is correct here.
  if (this->txn_.pipe_fatal)
    return res;

  if (this->txn_.state != State::RECEIVING || this->txn_.mode != TransferMode::PIPE) {
    this->last_error_ = ErrorCode::INVALID_STATE;
    res.response = encode_pipe_nack(OD_PIPE_NACK_PROTOCOL, this->txn_.highest_seen,
                                    build_inorder_ack_mask(this->txn_.accepted_packets));
    this->txn_.pipe_fatal = true;
    return res;
  }
  if (payload == nullptr || len < 1) {
    this->txn_.pipe_fatal = true;
    this->abort(ErrorCode::MALFORMED_PACKET);
    res.response = encode_pipe_nack(OD_PIPE_NACK_PROTOCOL, 0, 0);
    res.terminal_error = true;
    return res;
  }

  const uint8_t seq = payload[0];
  const uint8_t *data = payload + 1;
  const size_t data_len = len - 1;

  // Strictly in-order. A duplicate or out-of-order frame is NOT retained; answer
  // with the most recent SACK so the client resumes from the known position.
  if (seq != this->txn_.next_pipe_sequence) {
    this->stats_.retransmit_requests++;
    res.response = encode_pipe_sack(this->txn_.highest_seen,
                                    build_inorder_ack_mask(this->txn_.accepted_packets));
    return res;
  }

  if (data_len > this->txn_.expected_frame_bytes - this->txn_.accepted_bytes) {
    this->txn_.pipe_fatal = true;
    this->abort(ErrorCode::INVALID_SIZE);
    res.response = encode_pipe_nack(OD_PIPE_NACK_WRITE_SIZE, this->txn_.highest_seen, 0);
    res.terminal_error = true;
    return res;
  }

  if (this->backend_->write_contiguous(this->txn_.accepted_bytes, data, data_len) !=
      BackendResult::OK) {
    this->stats_.backend_failures++;
    this->txn_.pipe_fatal = true;
    this->abort(ErrorCode::BACKEND_WRITE_FAILED);
    res.response = encode_pipe_nack(OD_PIPE_NACK_WRITE_SIZE, this->txn_.highest_seen, 0);
    res.terminal_error = true;
    return res;
  }

  this->txn_.accepted_bytes += data_len;
  this->txn_.highest_seen = seq;
  this->txn_.next_pipe_sequence = static_cast<uint8_t>(seq + 1);  // mod 256 by type
  this->txn_.accepted_packets++;
  this->txn_.accepted_since_ack++;
  this->txn_.transfer_deadline_ms = now_ms + this->transfer_timeout_ms_;
  this->stats_.pipe_packets++;

  if (this->txn_.accepted_since_ack >= this->txn_.negotiated_ack_every) {
    this->txn_.accepted_since_ack = 0;
    res.response = encode_pipe_sack(this->txn_.highest_seen,
                                    build_inorder_ack_mask(this->txn_.accepted_packets));
  }
  return res;
}

EngineResult TransferEngine::on_pipe_end(const uint8_t *payload, size_t len, uint32_t now_ms) {
  EngineResult res;
  (void) payload;
  (void) len;  // refresh byte + optional new_etag: tolerated, never acted on

  if (this->txn_.state != State::RECEIVING || this->txn_.mode != TransferMode::PIPE) {
    this->last_error_ = ErrorCode::INVALID_STATE;
    res.response = encode_nack(CMD_PIPE_WRITE_END);
    return res;
  }
  // The canonical order is a tail-flush SACK, then the end-ACK, then 0x73/0x74.
  // The component sends `pre_response` first when present.
  if (this->txn_.accepted_since_ack != 0) {
    this->txn_.accepted_since_ack = 0;
    // Emitted by the caller before the end-ACK; see OpenDisplayComponent.
    this->pending_tail_sack_ = encode_pipe_sack(
        this->txn_.highest_seen, build_inorder_ack_mask(this->txn_.accepted_packets));
    this->has_tail_sack_ = true;
  }
  return this->finish_and_refresh_(CMD_PIPE_WRITE_END, now_ms);
}

// --- finish + refresh --------------------------------------------------------

EngineResult TransferEngine::finish_and_refresh_(uint16_t end_opcode, uint32_t now_ms) {
  EngineResult res;
  this->txn_.state = State::VALIDATING;

  if (this->txn_.accepted_bytes != this->txn_.expected_frame_bytes) {
    this->abort(ErrorCode::INVALID_SIZE);
    res.response = encode_nack(end_opcode);
    res.terminal_error = true;
    return res;
  }
  if (this->backend_->finish_frame() != BackendResult::OK) {
    this->stats_.backend_failures++;
    this->abort(ErrorCode::BACKEND_FINISH_FAILED);
    res.response = encode_nack(end_opcode);
    res.terminal_error = true;
    return res;
  }

  // Start the refresh BEFORE ACKing the END. begin_refresh() verifies the driver
  // actually accepted the request; if we ACKed first and the call were ignored,
  // we would later emit 0x0073 for someone else's refresh.
  this->txn_.state = State::WRITING;
  RefreshRequest rq;  // always FULL in v1
  if (this->backend_->begin_refresh(rq) != BackendResult::OK) {
    this->stats_.backend_failures++;
    this->abort(ErrorCode::BACKEND_BEGIN_FAILED);
    res.response = encode_nack(end_opcode);
    res.terminal_error = true;
    return res;
  }

  this->hw_owned_ = true;
  this->txn_.state = State::REFRESHING;
  this->txn_.refresh_deadline_ms = now_ms + this->refresh_timeout_ms_;
  res.response = encode_ack(end_opcode);
  return res;
}

// --- loop --------------------------------------------------------------------

EngineResult TransferEngine::tick(uint32_t now_ms) {
  EngineResult res;

  // Hardware may still be owned after the transaction ended (refresh timeout or
  // a disconnect mid-refresh). Keep polling until the driver is genuinely done
  // before letting anything else touch the panel.
  if (this->hw_owned_ && this->backend_ != nullptr) {
    const PollResult p = this->backend_->poll_refresh();
    if (p == PollResult::COMPLETE || p == PollResult::FAILED) {
      this->release_hardware_();
      if (this->txn_.state == State::REFRESHING) {
        this->txn_.reset();
        if (p == PollResult::COMPLETE) {
          this->stats_.transfers_ok++;
          res.refresh_complete = true;
        } else {
          this->stats_.backend_failures++;
          this->last_error_ = ErrorCode::BACKEND_FINISH_FAILED;
          res.terminal_error = true;
        }
      }
      return res;
    }
  }

  switch (this->txn_.state) {
    case State::RECEIVING:
    case State::VALIDATING:
    case State::WRITING:
      if (deadline_passed(now_ms, this->txn_.transfer_deadline_ms)) {
        this->abort(ErrorCode::TRANSFER_TIMEOUT);
        res.terminal_error = true;
      }
      break;
    case State::REFRESHING:
      if (deadline_passed(now_ms, this->txn_.refresh_deadline_ms)) {
        this->stats_.refresh_timeouts++;
        // Ends the transaction and emits 0x0074, but hw_owned_ stays set: the
        // driver cannot be cancelled, so the panel is not free yet.
        this->abort(ErrorCode::REFRESH_TIMEOUT);
        res.refresh_timeout = true;
      }
      break;
    default:
      break;
  }
  return res;
}

}  // namespace opendisplay
}  // namespace esphome
