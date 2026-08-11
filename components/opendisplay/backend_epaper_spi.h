#pragma once

// ePaper SPI adapter. Implemented in backend_epaper_spi.cpp -- NOT yet compiled.
//
// Driver: esphome::epaper_spi::EPaperBase, mainline since ESPHome 2025.10.0.
// https://esphome.io/components/display/epaper_spi/
//
// COMMIT PATH (decided, TODO.md open question 2): standard per-pixel
// draw_pixel_at. No cmd_data bypass, no upstream hook. There is no packed-1bpp
// bulk ingress anywhere in the ESPHome display API -- ColorBitness offers only
// 332/565/888 -- so no upstream change would have yielded a memcpy path, and
// every bypass forfeits honest completion reporting.
//
// Hold EPaperBase, never a concrete model subclass: epaper_spi is ten subclasses
// with seven distinct buffer layouts, and several re-declare draw_pixel_at as
// protected -- binding to a subclass breaks compilation for half the models.
//
// M0 findings (docs/ESPHome_Display_Drivers_Reference.md):
//   * The framebuffer is protected with no accessor, AND it is a SplitBuffer --
//     deliberately non-contiguous with no data(). Even a public accessor would
//     not yield a memcpy target, so a byte-range commit needs an upstream
//     SplitBuffer::write() plus write_frame_bytes(offset, data, len).
//     The only public path today is per-pixel draw_pixel_at/draw_pixels_at.
//   * update() takes NO refresh-mode argument; full-vs-partial is internal.
//   * Completion IS observable: Component::is_idle() is public and the driver
//     disable_loop()s at IDLE, having BUSY-waited before POWER_OFF -- so this is
//     the ONE backend whose 0x0073 can be honest today. But ONLY when busy_pin
//     is configured; without it is_idle_() is hard-true and completion is a lie.
//     Codegen must therefore REQUIRE busy_pin.
//
// Two YAML landmines this adapter must defend against:
//   * auto_clear_enabled defaults TRUE when a lambda: exists -> do_update_()
//     wipes our frame.
//   * with no lambda/pages, final validation FORCE-INJECTS show_test_card: True,
//     overwriting an explicit false and painting a test card over our buffer.
//   Mitigation: no-op lambda: + explicit auto_clear_enabled: false +
//   update_interval: never.
//
// NO PSRAM REQUIREMENT. The driver owns its framebuffer and this adapter adds no
// staging buffer of its own -- received bytes go straight into the driver's
// buffer. The plan's "component-owned native-layout buffer, PSRAM-preferred for
// large panels" is superseded: it would duplicate storage the driver already
// holds. (Contrast backend_it8951.h, where the driver's own full framebuffer
// does force PSRAM.)

#include "backend.h"

namespace esphome {

namespace epaper_spi {
class EPaperBase;
}  // namespace epaper_spi

namespace opendisplay {

class EpaperSPIBackend : public OpenDisplayBackend {
 public:
  explicit EpaperSPIBackend(epaper_spi::EPaperBase *display) : display_(display) {}

  bool init() override;
  void set_color_scheme(uint8_t scheme) override { this->caps_.color_scheme = scheme; }
  void set_panel_ic_type(uint16_t id) override { this->caps_.panel_ic_type = id; }
  const DisplayCapabilities &capabilities() const override { return caps_; }
  BackendResult begin_frame(const FrameDescriptor &frame) override;
  BackendResult write_contiguous(uint32_t offset, const uint8_t *data, size_t length) override;
  BackendResult finish_frame() override;
  BackendResult begin_refresh(RefreshRequest request) override;
  PollResult poll_refresh() override;
  void abort_transfer() override;

 protected:
  epaper_spi::EPaperBase *display_;
  DisplayCapabilities caps_{};
  // No staging buffer: writes land in the driver's own framebuffer.

  // Set by begin_refresh() only after it has confirmed update() actually drove
  // the driver out of idle. Checking there rather than in poll_refresh() avoids
  // both failure modes: attributing an unrelated refresh to our frame, and
  // missing a refresh that starts and finishes between two polls.
  bool refresh_started_{false};
};

}  // namespace opendisplay
}  // namespace esphome
