#include "opendisplay.h"

#include "esphome/core/log.h"

namespace esphome {
namespace opendisplay {

static const char *const TAG = "opendisplay";

static constexpr uint32_t MSD_UPDATE_INTERVAL_MS = 1000;

void OpenDisplayComponent::setup() {
  if (this->backend_ == nullptr || !this->backend_->init()) {
    // A backend that cannot allocate or whose driver has failed must never
    // advertise capabilities, so fail outright rather than degrade.
    ESP_LOGE(TAG, "backend init failed");
    this->mark_failed();
    return;
  }

  this->engine_.set_timeouts(this->transfer_timeout_ms_, this->refresh_timeout_ms_);

  // Synthesised once: our configuration authority is the compiled YAML plus what
  // the driver knows, and none of it changes at runtime.
  size_t len = 0;
  const ConfigBuildError err =
      build_config_blob(this->backend_->capabilities(), /*ic_type=*/0, this->config_blob_,
                        sizeof(this->config_blob_), &len);
  if (err == ConfigBuildError::NONE) {
    this->config_blob_len_ = len;
    this->config_valid_ = true;
  } else {
    ESP_LOGE(TAG, "config blob build failed (%u); 0x40 will answer with an error frame",
             static_cast<unsigned>(err));
  }

  this->update_msd_(0);
  this->publish_state_();
}

// --- BLE callback context ----------------------------------------------------
// Everything in this section runs off the loop task. Bounds-check, copy, return.

void OpenDisplayComponent::on_ble_write(const uint8_t *data, size_t len) {
  if (!this->rx_.push(data, len, this->connection_generation_, millis())) {
    // Full or oversize: reject immediately. Never block, never allocate.
    return;
  }
}

void OpenDisplayComponent::on_ble_connect() {
  this->connected_ = true;
  this->connection_generation_++;
  this->engine_.set_connection_generation(this->connection_generation_);
}

void OpenDisplayComponent::on_ble_disconnect() {
  this->connected_ = false;
  // Handled fully in loop(): on_disconnect() may abort a transaction, which
  // touches the backend.
}

// --- loop context ------------------------------------------------------------

void OpenDisplayComponent::loop() {
  const uint32_t now = millis();

  if (!this->connected_)
    this->engine_.on_disconnect(this->connection_generation_);

  RxPacket pkt;
  // Bounded per-iteration drain so a burst cannot monopolise the loop.
  for (uint8_t i = 0; i < 8 && this->rx_.pop(&pkt); i++) {
    if (pkt.connection_generation != this->connection_generation_)
      continue;  // stale packet from a previous connection
    this->handle_packet_(pkt);
  }

  this->apply_result_(this->engine_.tick(now));
  this->update_msd_(now);
  this->drain_tx_();

  if (this->engine_.state() != this->published_state_ ||
      this->engine_.is_busy() != this->published_busy_)
    this->publish_state_();
}

void OpenDisplayComponent::handle_packet_(const RxPacket &pkt) {
  Frame f;
  if (!parse_frame(pkt.payload, pkt.payload_length, &f))
    return;  // too short to carry an opcode

  const uint32_t now = millis();
  EngineResult res;

  switch (f.opcode) {
    case CMD_CONFIG_READ:
      this->send_config_(now);
      return;

    case CMD_FIRMWARE_VERSION:
      // ALWAYS PLAINTEXT so version stays readable pre-auth.
      this->send_(encode_version(this->ver_major_, this->ver_minor_, this->ver_patch_, ""));
      return;

    case CMD_READ_MSD:
      this->send_(encode_msd_response(this->msd_));
      // The host has now observed the reboot flag.
      this->reboot_flag_ = false;
      return;

    case CMD_DIRECT_WRITE_START:
      res = this->engine_.on_direct_start(f.payload, f.payload_len, now);
      break;
    case CMD_DIRECT_WRITE_DATA:
      res = this->engine_.on_direct_data(f.payload, f.payload_len, now);
      break;
    case CMD_DIRECT_WRITE_END:
      res = this->engine_.on_direct_end(f.payload, f.payload_len, now);
      break;

    case CMD_PIPE_WRITE_START:
      res = this->engine_.on_pipe_start(f.payload, f.payload_len, now);
      break;
    case CMD_PIPE_WRITE_DATA:
      res = this->engine_.on_pipe_data(f.payload, f.payload_len, now);
      break;
    case CMD_PIPE_WRITE_END: {
      // Canonical order: tail-flush SACK, then end-ACK, then 0x73/0x74.
      res = this->engine_.on_pipe_end(f.payload, f.payload_len, now);
      Response tail;
      if (this->engine_.take_tail_sack(&tail))
        this->send_(tail, /*terminal=*/true);
      break;
    }

    default:
      // An unrelated unsupported command must NOT destroy an in-flight transfer.
      // Only transaction-scoped faults abort. Note 0x0073 inbound is
      // CMD_LED_ACTIVATE, not our refresh-success response -- direction
      // disambiguates, and we reject it here like any other unsupported opcode.
      ESP_LOGD(TAG, "unsupported opcode 0x%04X", f.opcode);
      this->send_(encode_nack(f.opcode));
      return;
  }

  this->apply_result_(res);
}

void OpenDisplayComponent::apply_result_(const EngineResult &res) {
  const bool terminal = res.refresh_complete || res.refresh_timeout || res.terminal_error;
  if (!res.response.empty())
    this->send_(res.response, terminal);

  if (res.transfer_started)
    this->transfer_started_callback_.call();

  if (res.refresh_complete) {
    // Emitted ONLY after the backend confirmed completion. On IT8951 that
    // confirmation is a fixed settle time, not a physical signal -- see
    // backend_it8951.h.
    this->send_(encode_ack(RESP_DIRECT_WRITE_REFRESH_SUCCESS), /*terminal=*/true);
    this->refresh_complete_callback_.call();
  }
  if (res.refresh_timeout) {
    this->send_(encode_ack(RESP_DIRECT_WRITE_REFRESH_TIMEOUT), /*terminal=*/true);
    this->error_pending_ = true;
    this->error_callback_.call(error_to_string(this->engine_.last_error()));
  }
  if (res.terminal_error) {
    this->error_pending_ = true;
    this->error_callback_.call(error_to_string(this->engine_.last_error()));
  }
}

void OpenDisplayComponent::send_config_(uint32_t now_ms) {
  (void) now_ms;
  if (!this->config_valid_) {
    // The client special-cases exactly this 4-byte shape. Never answer a
    // half-true blob.
    this->send_(encode_nack_err(CMD_CONFIG_READ, 0x00));
    return;
  }
  // Chunked against MAX_RESPONSE_DATA_SIZE (100), not the negotiated MTU. Queued
  // rather than sent inline so a full TX ring cannot truncate the config, which
  // is the failure the reference firmware works around with a per-chunk flush.
  for (uint16_t i = 0;; i++) {
    const Response r = encode_config_chunk(this->config_blob_, this->config_blob_len_, i);
    if (r.empty())
      break;
    if (!this->tx_.push(r, /*terminal=*/false)) {
      ESP_LOGW(TAG, "tx full during config read; client will see a truncated config");
      break;
    }
  }
}

void OpenDisplayComponent::send_(const Response &r, bool terminal) {
  if (!this->tx_.push(r, terminal))
    ESP_LOGW(TAG, "tx queue full, dropped a %s frame", terminal ? "terminal" : "normal");
}

void OpenDisplayComponent::drain_tx_() {
  Response r;
  // Bounded per iteration: notifications are paced by the stack, and draining
  // without limit would block display progression.
  for (uint8_t i = 0; i < 4 && this->tx_.pop(&r); i++) {
    // TODO(M1): hand `r` to the ESP32 BLE server characteristic and notify.
    // Deliberately not implemented here -- it is the only part of the component
    // that needs the esp32_ble_server API, and it has never been compiled.
    ESP_LOGV(TAG, "notify %u bytes (0x%02X 0x%02X)", r.len, r.data[0], r.data[1]);
  }
}

void OpenDisplayComponent::update_msd_(uint32_t now_ms) {
  if (now_ms != 0 && static_cast<int32_t>(now_ms - this->msd_next_update_ms_) < 0)
    return;
  this->msd_next_update_ms_ = now_ms + MSD_UPDATE_INTERVAL_MS;

  MsdInputs in;
  // MCU die temperature, not ambient. A mains-powered ESP32 reports battery 0,
  // which is the reference firmware's own "no battery" encoding.
  in.chip_temperature_c = 25.0f;  // TODO(M1): read the ESP32 internal sensor
  in.battery_10mv = 0;
  in.reboot_flag = this->reboot_flag_;
  in.connection_requested = false;  // continuously connectable; nothing to request
  in.loop_counter = this->msd_loop_counter_;
  build_msd(in, this->msd_);

  this->msd_loop_counter_ = static_cast<uint8_t>((this->msd_loop_counter_ + 1) & 0x0F);
}

void OpenDisplayComponent::abort() { this->engine_.abort(ErrorCode::INVALID_STATE); }

void OpenDisplayComponent::publish_state_() {
  this->published_state_ = this->engine_.state();
  this->published_busy_ = this->engine_.is_busy();

#ifdef USE_BINARY_SENSOR
  if (this->busy_sensor_ != nullptr)
    this->busy_sensor_->publish_state(this->published_busy_);
#endif
#ifdef USE_TEXT_SENSOR
  if (this->activity_sensor_ != nullptr) {
    // `error` is an orthogonal last-result property, not a state: abort() takes
    // the machine straight back to IDLE, so without this the activity sensor
    // could never show `error` at all.
    if (this->published_state_ == State::IDLE && this->error_pending_) {
      this->activity_sensor_->publish_state("error");
      this->error_pending_ = false;
    } else {
      this->activity_sensor_->publish_state(state_to_string(this->published_state_));
    }
  }
#endif
}

void OpenDisplayComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "OpenDisplay:");
  ESP_LOGCONFIG(TAG, "  Transfer timeout: %u ms", this->transfer_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Refresh timeout: %u ms", this->refresh_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Config blob: %s (%u bytes)", this->config_valid_ ? "valid" : "INVALID",
                static_cast<unsigned>(this->config_blob_len_));
  ESP_LOGCONFIG(TAG, "  Security: NONE (open, unauthenticated characteristic)");
  if (this->backend_ != nullptr) {
    const auto &caps = this->backend_->capabilities();
    ESP_LOGCONFIG(TAG, "  Panel: %ux%u, frame %u bytes, stride %u", caps.width, caps.height,
                  caps.full_frame_bytes, caps.row_stride_bytes);
    ESP_LOGCONFIG(TAG, "  Direct write: %s, PIPE_WRITE: %s", YESNO(caps.supports_direct_write),
                  YESNO(caps.supports_pipe_write));
  }
}

}  // namespace opendisplay
}  // namespace esphome
