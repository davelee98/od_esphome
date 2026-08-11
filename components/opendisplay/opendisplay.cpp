#include "opendisplay.h"

#include "esphome/core/log.h"

#ifdef USE_ESP32
#include <esp_gap_ble_api.h>
#include <esp_mac.h>
// esp_ble_gatt_set_local_mtu() lives here, NOT in esp_gatts_api.h.
#include <esp_gatt_common_api.h>
#include <esp_gatts_api.h>
#include <soc/soc_caps.h>

#include "esphome/components/esp32_ble/ble.h"
#include "esphome/components/esp32_ble_server/ble_2902.h"

// Chip-temperature access differs by variant, exactly as ESPHome's own
// internal_temperature component does it (internal_temperature_esp32.cpp).
#if defined(USE_ESP32_VARIANT_ESP32)
// No official API on the original ESP32; the symbol name really is misspelled.
extern "C" {
uint8_t temprature_sens_read();
}
#elif SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif
#endif

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
      build_config_blob(this->backend_->capabilities(), this->ic_type_, this->config_blob_,
                        sizeof(this->config_blob_), &len);
  if (err == ConfigBuildError::NONE) {
    this->config_blob_len_ = len;
    this->config_valid_ = true;
  } else {
    ESP_LOGE(TAG, "config blob build failed (%u); 0x40 will answer with an error frame",
             static_cast<unsigned>(err));
  }

  this->setup_chip_temperature_();
  this->update_msd_(0);
#ifdef USE_ESP32
  this->setup_gatt_();
#endif
  this->publish_state_();
}

#ifdef USE_ESP32
void OpenDisplayComponent::setup_gatt_() {
  using esp32_ble::ESPBTUUID;

  if (esp32_ble_server::global_ble_server == nullptr) {
    ESP_LOGE(TAG, "no BLE server; add esp32_ble_server: to the config");
    this->mark_failed();
    return;
  }

  // Advertise as OD<chipid>, matching real OpenDisplay firmware exactly
  // (Firmware/src/ble_transport_esp32.cpp:544 -> encryption.cpp:863-873):
  //     chipId = (efuse_mac >> 24) & 0xFFFFFF, printed %06X upper-case.
  // Arduino's getEfuseMac() loads the six MAC bytes little-endian, so that
  // chipId is mac[3] | mac[4]<<8 | mac[5]<<16 and %06X emits mac[5], mac[4],
  // mac[3] -- i.e. the last three MAC bytes in REVERSE order. Reproduced here
  // byte-for-byte so an ESPHome device is indistinguishable from a tag.
  //
  // Set here rather than via `esp32_ble: name:` because that path forces a
  // hyphen before the MAC suffix (make_name_with_suffix_to in ble.cpp:304),
  // giving "OD-123456" instead of "OD123456". Safe to override: ESP32BLE sets
  // the name at setup_priority BLUETOOTH (350) and this component runs at
  // AFTER_CONNECTION (100), so ours is applied last and nothing resets it on an
  // advertising restart.
  uint8_t mac[6] = {};
  if (esp_efuse_mac_get_default(mac) == ESP_OK) {
    char od_name[16];
    snprintf(od_name, sizeof(od_name), "OD%02X%02X%02X", mac[5], mac[4], mac[3]);
    const esp_err_t nerr = esp_ble_gap_set_device_name(od_name);
    if (nerr != ESP_OK) {
      ESP_LOGW(TAG, "esp_ble_gap_set_device_name(%s) failed: %d", od_name, static_cast<int>(nerr));
    } else {
      // Kept for dump_config() as well as here: setup() output scrolls past
      // before `esphome logs` attaches, so a boot-only line is easy to miss and
      // indistinguishable from the code not running at all.
      snprintf(this->ble_name_, sizeof(this->ble_name_), "%s", od_name);
    }
  } else {
    ESP_LOGW(TAG, "esp_efuse_mac_get_default failed; BLE name left as the ESPHome default");
  }

  // Declare the preferred ATT MTU at OD_BLE_MAX_FRAME (256) rather than the 512
  // ATT maximum, so an oversize write draws ATT error 0x0D instead of being
  // silently dropped (opendisplay_protocol.h:63-73).
  const esp_err_t merr = esp_ble_gatt_set_local_mtu(OD_BLE_PREFERRED_MTU);
  if (merr != ESP_OK)
    ESP_LOGW(TAG, "esp_ble_gatt_set_local_mtu(%u) failed: %d",
             static_cast<unsigned>(OD_BLE_PREFERRED_MTU), static_cast<int>(merr));

  // 0x2446 is both the service and the characteristic UUID. advertise=true so
  // the service UUID appears in the advertisement alongside our manufacturer
  // data -- clients filter on manufacturer id 9286, but the service UUID is what
  // makes the device recognisable in a generic scanner.
  this->service_ = esp32_ble_server::global_ble_server->create_service(
      ESPBTUUID::from_uint16(OD_BLE_SERVICE_UUID), /*advertise=*/true);
  if (this->service_ == nullptr) {
    ESP_LOGE(TAG, "failed to create GATT service");
    this->mark_failed();
    return;
  }

  // WRITE_NR matters: the client streams image data without waiting for an ATT
  // write response, which is what makes PIPE throughput possible at all.
  const auto props = static_cast<esp_gatt_char_prop_t>(
      ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE |
      ESP_GATT_CHAR_PROP_BIT_WRITE_NR | ESP_GATT_CHAR_PROP_BIT_NOTIFY);
  this->characteristic_ = this->service_->create_characteristic(OD_BLE_SERVICE_UUID, props);
  if (this->characteristic_ == nullptr) {
    ESP_LOGE(TAG, "failed to create GATT characteristic");
    this->mark_failed();
    return;
  }

  // A NOTIFY characteristic is useless without a Client Characteristic
  // Configuration Descriptor (0x2902). ESPHome ships BLE2902 but does NOT add it
  // automatically -- add_descriptor() must be called explicitly
  // (esp32_ble_server/ble_characteristic.cpp:73-90). Without it every client
  // that calls start_notify() fails with "BT_GATT: format mismatch" and drops
  // the link, which looks like a connection bug rather than a missing
  // descriptor.
  this->characteristic_->add_descriptor(new esp32_ble_server::BLE2902());  // NOLINT

  // THE BLE CALLBACK. Bounds-check, copy, return -- nothing else. Any work here
  // runs off the loop task and would block the BLE stack.
  this->characteristic_->on_write([this](std::span<const uint8_t> data, uint16_t conn_id) {
    (void) conn_id;
    this->on_ble_write(data.data(), data.size());
  });

  esp32_ble_server::global_ble_server->on_connect([this](uint16_t conn_id) {
    (void) conn_id;
    this->on_ble_connect();
  });
  esp32_ble_server::global_ble_server->on_disconnect([this](uint16_t conn_id) {
    (void) conn_id;
    this->on_ble_disconnect();
  });

  this->service_->start();
  this->gatt_started_ = true;
  ESP_LOGCONFIG(TAG, "GATT service 0x%04X started", OD_BLE_SERVICE_UUID);
}
#endif

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
#ifdef USE_ESP32
  if (this->characteristic_ == nullptr || !this->connected_)
    return;

  Response r;
  // Bounded per iteration: notifications are paced by the stack, and draining
  // without limit would starve the rest of loop() and stall display progression.
  for (uint8_t i = 0; i < 4 && this->tx_.pop(&r); i++) {
    this->notify_scratch_.assign(r.data, r.data + r.len);
    this->characteristic_->set_value(std::move(this->notify_scratch_));
    this->characteristic_->notify();
    ESP_LOGV(TAG, "notify %u bytes (0x%02X 0x%02X)", static_cast<unsigned>(r.len), r.data[0],
             r.data[1]);
  }
#endif
}

void OpenDisplayComponent::update_msd_(uint32_t now_ms) {
  if (now_ms != 0 && static_cast<int32_t>(now_ms - this->msd_next_update_ms_) < 0)
    return;
  this->msd_next_update_ms_ = now_ms + MSD_UPDATE_INTERVAL_MS;

  MsdInputs in;
  // MCU DIE temperature, not ambient -- the client documents it as such, so do
  // not wire an ESPHome ambient sensor into this byte. A mains-powered ESP32
  // reports battery 0, which is the reference firmware's own "no battery"
  // encoding.
  in.chip_temperature_c = this->read_chip_temperature_();
  in.battery_10mv = 0;
  in.reboot_flag = this->reboot_flag_;
  in.connection_requested = false;  // continuously connectable; nothing to request
  in.loop_counter = this->msd_loop_counter_;
  build_msd(in, this->msd_);

#ifdef USE_ESP32
  // Publish verbatim: ESPHome hands p_manufacturer_data straight to ESP-IDF, so
  // the 2-byte company id must be IN the payload -- which it is, as the first
  // field of MsdAdvertisement. advertising_set_manufacturer_data() restarts
  // advertising itself, so there is no separate start call.
  //
  // Only republish when the bytes actually changed. Rebuilding the advertisement
  // is comparatively expensive and, with no sensors or buttons, our record is
  // static apart from temperature and the counter nibble.
  if (esp32_ble::global_ble != nullptr &&
      std::memcmp(this->msd_, this->msd_published_, MSD_BYTES) != 0) {
    std::memcpy(this->msd_published_, this->msd_, MSD_BYTES);
    esp32_ble::global_ble->advertising_set_manufacturer_data(
        std::vector<uint8_t>(this->msd_, this->msd_ + MSD_BYTES));
  }
#endif

  this->msd_loop_counter_ = static_cast<uint8_t>((this->msd_loop_counter_ + 1) & 0x0F);
}

float OpenDisplayComponent::read_chip_temperature_() {
#ifdef USE_ESP32
#if defined(USE_ESP32_VARIANT_ESP32)
  const uint8_t raw = temprature_sens_read();
  if (raw == 128)  // the original ESP32's failure sentinel
    return this->last_chip_temp_c_;
  this->last_chip_temp_c_ = (raw - 32) / 1.8f;
#elif SOC_TEMP_SENSOR_SUPPORTED
  float t = NAN;
  if (this->tsens_ != nullptr && temperature_sensor_get_celsius(this->tsens_, &t) == ESP_OK &&
      std::isfinite(t)) {
    this->last_chip_temp_c_ = t;
  }
  // On failure keep the previous reading: the MSD encoding clamps to [0,255]
  // anyway, and a transient read error should not make the advertisement jump.
#endif
#endif
  return this->last_chip_temp_c_;
}

void OpenDisplayComponent::setup_chip_temperature_() {
#if defined(USE_ESP32) && !defined(USE_ESP32_VARIANT_ESP32) && SOC_TEMP_SENSOR_SUPPORTED
  // Range chosen to match ESPHome's own internal_temperature component.
  temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
  if (temperature_sensor_install(&cfg, &this->tsens_) != ESP_OK) {
    ESP_LOGW(TAG, "chip temperature sensor install failed; MSD will report a stale value");
    this->tsens_ = nullptr;
    return;
  }
  if (temperature_sensor_enable(this->tsens_) != ESP_OK) {
    ESP_LOGW(TAG, "chip temperature sensor enable failed");
    this->tsens_ = nullptr;
  }
#endif
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
  ESP_LOGCONFIG(TAG, "  Transfer timeout: %u ms", static_cast<unsigned>(this->transfer_timeout_ms_));
  ESP_LOGCONFIG(TAG, "  Refresh timeout: %u ms", static_cast<unsigned>(this->refresh_timeout_ms_));
  ESP_LOGCONFIG(TAG, "  Config blob: %s (%u bytes)", this->config_valid_ ? "valid" : "INVALID",
                static_cast<unsigned>(this->config_blob_len_));
  ESP_LOGCONFIG(TAG, "  Security: NONE (open, unauthenticated characteristic)");
  ESP_LOGCONFIG(TAG, "  BLE name: %s", this->ble_name_[0] ? this->ble_name_ : "(not set)");
  ESP_LOGCONFIG(TAG, "  GATT service: %s", this->gatt_started_ ? "started" : "NOT STARTED");
  if (this->backend_ != nullptr) {
    const auto &caps = this->backend_->capabilities();
    ESP_LOGCONFIG(TAG, "  Panel: %ux%u, frame %u bytes, stride %u", static_cast<unsigned>(caps.width),
                  static_cast<unsigned>(caps.height), static_cast<unsigned>(caps.full_frame_bytes),
                  static_cast<unsigned>(caps.row_stride_bytes));
    ESP_LOGCONFIG(TAG, "  Direct write: %s, PIPE_WRITE: %s", YESNO(caps.supports_direct_write),
                  YESNO(caps.supports_pipe_write));
  }
}

}  // namespace opendisplay
}  // namespace esphome
