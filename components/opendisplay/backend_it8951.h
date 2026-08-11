#pragma once

// IT8951 adapter -- STUB. BLOCKED ON M0, and blocked harder than the others.
//
// Driver: esphome::it8951::IT8951Display, mainline since ESPHome 2026.7.0.
// https://esphome.io/components/display/it8951/
//
// TWO PLAN ASSUMPTIONS ARE FALSE UPSTREAM (docs/ESPHome_Display_Drivers_Reference.md):
//
//  1. The driver ALWAYS allocates a full MCU-side framebuffer (1.31 MB for
//     1872x1404 @ 4bpp, so PSRAM is mandatory) and streams from it. The plan's
//     "stream contiguous chunks into controller RAM without an MCU framebuffer"
//     is not achievable against the upstream driver.
//
//  2. UPDATE_REFRESH is explicitly fire-and-forget (it8951.cpp:277-288). The
//     driver never confirms PHYSICAL refresh completion -- the LUTAFSR poll
//     guards the NEXT refresh, and HW_RDY is not a proxy.
//
// DECISION (2026-08-10): ship a FIXED 5 s settle time instead of observable
// completion. poll_refresh() returns PENDING until the deadline, then COMPLETE.
// Work on true completion reporting is DEFERRED.
//
// What this costs, stated plainly so nobody rediscovers it in the field: 0x0073
// then means "5 s have elapsed since we issued the refresh", NOT "the panel has
// finished". A slower mode or a larger panel can still be updating when the
// client is told it is done, and a faster one wastes the remainder. This is a
// deliberate, documented deviation from the plan's "0x0073 only after real
// physical completion" rule -- not an oversight, and not to be silently
// "fixed" by shortening the delay.
//
// The eventual real fix is known and cheap (see docs/TODO.md open question 1):
// IT8951 register LUTAFSR (0x1224) reads 0 when no LUT engine is active, which
// FastEPD uses as a terminal wait in its 1-bit path
// (FastEPD/src/FastEPD.inl:2077-2087, :2125-2126). It is a plain SPI register
// read, so it converts to a per-loop() poll directly once ESPHome exposes it.

#include "backend.h"

namespace esphome {

namespace it8951 {
class IT8951Display;
}  // namespace it8951

namespace opendisplay {

// Fixed settle time standing in for observable refresh completion. Enforced as a
// deadline checked from loop() -- never a blocking delay(). See the header
// comment above for what this does and does not mean.
static constexpr uint32_t IT8951_REFRESH_SETTLE_MS = 5000;

class IT8951Backend : public OpenDisplayBackend {
 public:
  explicit IT8951Backend(it8951::IT8951Display *display) : display_(display) {}

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
  it8951::IT8951Display *display_;
  DisplayCapabilities caps_{};

  // 0 = no refresh in flight. Set to now + IT8951_REFRESH_SETTLE_MS by
  // begin_refresh(); poll_refresh() reports PENDING until it passes.
  uint32_t refresh_settle_deadline_ms_{0};
};

}  // namespace opendisplay
}  // namespace esphome
