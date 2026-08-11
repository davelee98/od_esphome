#pragma once

// Component owner: holds the backend and transfer engine, runs the state
// machine from loop(), and publishes coordination state.
//
// Threading model -- the rule everything else depends on:
//   BLE callback context : bounds-check, copy into a fixed-capacity queue, return.
//   loop() context       : everything else. Only loop() touches the backend or
//                          mutates terminal transaction state.

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <cmath>
#include <cstring>
#include <vector>

#ifdef USE_ESP32
#include <soc/soc_caps.h>
#if !defined(USE_ESP32_VARIANT_ESP32) && SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif
#include "esphome/components/esp32_ble/ble_uuid.h"
#include "esphome/components/esp32_ble_server/ble_characteristic.h"
#include "esphome/components/esp32_ble_server/ble_server.h"
#include "esphome/components/esp32_ble_server/ble_service.h"
#endif

#include "backend.h"
#include "ble_transport.h"
#include "protocol.h"
#include "protocol_types.h"
#include "transfer_engine.h"

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

namespace esphome {
namespace opendisplay {

class OpenDisplayComponent : public Component {
 public:
  explicit OpenDisplayComponent(OpenDisplayBackend *backend) : backend_(backend), engine_(backend) {}

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void set_transfer_timeout(uint32_t ms) { this->transfer_timeout_ms_ = ms; }
  void set_refresh_timeout(uint32_t ms) { this->refresh_timeout_ms_ = ms; }
  // SystemConfig.ic_type, set from the build variant at codegen. Cosmetic (no
  // client consumes it behaviourally) but free to get right.
  void set_ic_type(uint16_t ic_type) { this->ic_type_ = ic_type; }
  void set_version(uint8_t major, uint8_t minor, uint8_t patch) {
    this->ver_major_ = major;
    this->ver_minor_ = minor;
    this->ver_patch_ = patch;
  }

  bool is_busy() const { return this->engine_.is_busy(); }
  void abort();

  // Called from the BLE write callback. Does no work beyond bounds-checking and
  // enqueuing -- see ble_transport.h.
  void on_ble_write(const uint8_t *data, size_t len);
  void on_ble_connect();
  void on_ble_disconnect();

  // 16-byte MSD record, rebuilt periodically. Also the 0x0044 payload.
  const uint8_t *msd() const { return this->msd_; }

#ifdef USE_BINARY_SENSOR
  void set_busy_binary_sensor(binary_sensor::BinarySensor *s) { busy_sensor_ = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_activity_text_sensor(text_sensor::TextSensor *s) { activity_sensor_ = s; }
#endif

  void add_transfer_started_callback(std::function<void()> &&cb) {
    transfer_started_callback_.add(std::move(cb));
  }
  void add_refresh_complete_callback(std::function<void()> &&cb) {
    refresh_complete_callback_.add(std::move(cb));
  }
  void add_error_callback(std::function<void(std::string)> &&cb) {
    error_callback_.add(std::move(cb));
  }

 protected:
  void handle_packet_(const RxPacket &pkt);
  void apply_result_(const EngineResult &res);
  void send_(const Response &r, bool terminal = false);
  void drain_tx_();
  void publish_state_();
  void update_msd_(uint32_t now_ms);
  float read_chip_temperature_();
  void setup_chip_temperature_();
  void send_config_(uint32_t now_ms);

  OpenDisplayBackend *backend_;
  TransferEngine engine_;
  RxQueue rx_;
  TxQueue tx_;

  uint32_t transfer_timeout_ms_{30000};
  uint32_t refresh_timeout_ms_{180000};
  uint8_t ver_major_{0}, ver_minor_{1}, ver_patch_{0};
  uint16_t ic_type_{0};  // 0 = unmapped variant; degrades to "Unknown" client-side

  uint32_t connection_generation_{0};
  bool connected_{false};

#ifdef USE_ESP32
  void setup_gatt_();
  esp32_ble_server::BLEService *service_{nullptr};
  esp32_ble_server::BLECharacteristic *characteristic_{nullptr};
  bool gatt_started_{false};
  char ble_name_[16]{};
  int ble_name_err_{0};
  void apply_ble_name_();
  std::vector<uint8_t> notify_scratch_{};
#endif

  // Config blob, synthesised once at setup(). If the build failed we must answer
  // [0xFF][0x40][0x00][0x00] rather than a half-true blob.
  uint8_t config_blob_[CONFIG_BLOB_MAX]{};
  size_t config_blob_len_{0};
  bool config_valid_{false};

  uint8_t msd_[MSD_BYTES]{};
  uint8_t msd_published_[MSD_BYTES]{0xFF};  // forces a first publish
  uint8_t msd_loop_counter_{0};
  // Last good die-temperature reading; retained across transient read failures
  // so the advertisement does not jump.
  float last_chip_temp_c_{25.0f};
#if defined(USE_ESP32) && !defined(USE_ESP32_VARIANT_ESP32) && SOC_TEMP_SENSOR_SUPPORTED
  temperature_sensor_handle_t tsens_{nullptr};
#endif
  uint32_t msd_next_update_ms_{0};
  bool reboot_flag_{true};  // set at boot, cleared once the host has read it

  // Suppressed bulk-frame counters; reported once per transfer.
  uint32_t bulk_rx_count_{0};
  uint32_t bulk_tx_count_{0};

  State published_state_{State::IDLE};
  bool published_busy_{false};
  bool error_pending_{false};

#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *busy_sensor_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *activity_sensor_{nullptr};
#endif

  CallbackManager<void()> transfer_started_callback_{};
  CallbackManager<void()> refresh_complete_callback_{};
  CallbackManager<void(std::string)> error_callback_{};
};

// --- Automations -------------------------------------------------------------

template<typename... Ts> class IsBusyCondition : public Condition<Ts...> {
 public:
  explicit IsBusyCondition(OpenDisplayComponent *parent) : parent_(parent) {}
  bool check(Ts... x) override { return this->parent_->is_busy(); }

 protected:
  OpenDisplayComponent *parent_;
};

// Cancels a RECEIVING transaction. Makes no promise about an issued refresh --
// the panel cannot be un-refreshed.
template<typename... Ts> class AbortAction : public Action<Ts...> {
 public:
  explicit AbortAction(OpenDisplayComponent *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->abort(); }

 protected:
  OpenDisplayComponent *parent_;
};

class TransferStartedTrigger : public Trigger<> {
 public:
  explicit TransferStartedTrigger(OpenDisplayComponent *parent) {
    parent->add_transfer_started_callback([this]() { this->trigger(); });
  }
};

class RefreshCompleteTrigger : public Trigger<> {
 public:
  explicit RefreshCompleteTrigger(OpenDisplayComponent *parent) {
    parent->add_refresh_complete_callback([this]() { this->trigger(); });
  }
};

class ErrorTrigger : public Trigger<std::string> {
 public:
  explicit ErrorTrigger(OpenDisplayComponent *parent) {
    parent->add_error_callback([this](std::string err) { this->trigger(std::move(err)); });
  }
};

}  // namespace opendisplay
}  // namespace esphome
