// Compiled only when this backend is selected. ESPHome copies EVERY file in
// a used component into the build (writer.py copy_src_tree), so without this
// guard the driver header below is included even when that display component
// is absent from the config -- which is a hard "No such file" build failure.
// The define is emitted by to_code() in __init__.py and reaches us via
// defines.h, which MUST be included BEFORE the guard -- an #ifdef ahead of
// every #include sees nothing and silently compiles the file to nothing,
// which surfaces only as an undefined vtable at link time.
#include "esphome/core/defines.h"
#ifdef USE_OPENDISPLAY_EPAPER_SPI
#include "backend_epaper_spi.h"

#include "esphome/components/epaper_spi/epaper_spi.h"
#include "esphome/core/color.h"
#include "esphome/core/log.h"

namespace esphome {
namespace opendisplay {

static const char *const TAG = "opendisplay.epaper_spi";

bool EpaperSPIBackend::init() {
  if (this->display_ == nullptr)
    return false;

  // Pre-rotation panel geometry. get_width()/get_height() are rotation-applied
  // and would be wrong here: the wire format is always unrotated, and codegen
  // requires the driver's own rotation to be 0.
  // The driver marks itself failed when its framebuffer allocation fails. Never
  // advertise transfer support over a display with no valid buffer.
  if (this->display_->is_failed()) {
    ESP_LOGE(TAG, "display driver has failed; refusing to advertise capabilities");
    return false;
  }

  // Rotation MUST be 0: the wire format is unrotated, and draw_pixel_at applies
  // the driver's own transform on top of our x/y. A non-zero rotation silently
  // rotates or transposes every frame while the byte count stays valid.
  if (this->display_->get_rotation() != display::DISPLAY_ROTATION_0_DEGREES) {
    ESP_LOGE(TAG, "display rotation must be 0 for opendisplay");
    return false;
  }

  this->caps_.width = this->display_->get_native_width();
  this->caps_.height = this->display_->get_native_height();
  if (this->caps_.width == 0 || this->caps_.height == 0)
    return false;

  // color_scheme is set at codegen time from the YAML `model:` -- it cannot be
  // read back from the driver, because get_display_type() only distinguishes
  // BINARY/COLOR and would conflate mono with 3-colour.
  if (this->caps_.color_scheme != OD_COLOR_SCHEME_MONO) {
    ESP_LOGE(TAG, "only OD_COLOR_SCHEME_MONO is implemented (got %u)", this->caps_.color_scheme);
    return false;
  }

  // EPaperMono layout: 1 bpp, MSB first, row-major, stride (w + 7) / 8.
  this->caps_.row_stride_bytes = (this->caps_.width + 7) / 8;
  this->caps_.full_frame_bytes =
      static_cast<uint32_t>(this->caps_.row_stride_bytes) * this->caps_.height;
  this->caps_.byte_alignment = 1;
  this->caps_.max_contiguous_write = this->caps_.full_frame_bytes;

  // No staging buffer and no PSRAM requirement: writes land in the driver's own
  // framebuffer via draw_pixel_at.
  this->caps_.staging_bytes_required = 0;

  // v1 capability set. Full refresh only (open question 3); no compression, no
  // partial. Do not widen any of these without implementing them.
  this->caps_.supports_full_refresh = true;
  this->caps_.supports_fast_refresh = false;
  this->caps_.supports_partial = PartialSupport::NONE;
  this->caps_.supports_compression = false;
  this->caps_.supports_direct_write = true;
  this->caps_.supports_pipe_write = true;

  ESP_LOGCONFIG(TAG, "epaper_spi backend: %ux%u, %u bytes/frame, stride %u",
                static_cast<unsigned>(this->caps_.width), static_cast<unsigned>(this->caps_.height),
                static_cast<unsigned>(this->caps_.full_frame_bytes),
                static_cast<unsigned>(this->caps_.row_stride_bytes));
  return true;
}

BackendResult EpaperSPIBackend::begin_frame(const FrameDescriptor &frame) {
  if (this->display_ == nullptr)
    return BackendResult::DEVICE_ERROR;
  if (frame.compressed || frame.partial)
    return BackendResult::UNSUPPORTED;
  if (frame.color_scheme != this->caps_.color_scheme)
    return BackendResult::UNSUPPORTED;
  if (frame.total_bytes != this->caps_.full_frame_bytes)
    return BackendResult::INVALID_ARGUMENT;

  // Deliberately NOT clearing the buffer here. A full frame overwrites every
  // pixel, and pre-clearing would flash the previous content away if the
  // transfer then fails partway.
  this->refresh_started_ = false;
  return BackendResult::OK;
}

BackendResult EpaperSPIBackend::write_contiguous(uint32_t offset, const uint8_t *data,
                                                 size_t length) {
  if (this->display_ == nullptr || data == nullptr)
    return BackendResult::DEVICE_ERROR;

  // Checked BEFORE any access, and phrased so offset + length cannot wrap.
  if (offset > this->caps_.full_frame_bytes || length > this->caps_.full_frame_bytes - offset)
    return BackendResult::INVALID_ARGUMENT;

  // Hoisted out of the loop: Color has a non-trivial constructor and this runs
  // eight times per byte.
  static const Color WHITE{255, 255, 255};  // (r+g+b) = 765 >= 382 -> bit 1
  static const Color BLACK{0, 0, 0};        // 0 < 382                -> bit 0

  const uint16_t stride = this->caps_.row_stride_bytes;
  const int width = static_cast<int>(this->caps_.width);

  for (size_t i = 0; i < length; i++) {
    const uint32_t byte_index = offset + i;
    const int y = static_cast<int>(byte_index / stride);
    const int x0 = static_cast<int>((byte_index % stride) * 8);
    const uint8_t bits = data[i];

    // OpenDisplay MONO and EPaperMono agree byte-for-byte: 8 px/byte, MSB
    // first, 1 = white, 0 = black, rows padded to a byte boundary
    // (py-opendisplay encoding/images.py encode_1bpp -> np.packbits, and
    // epaper_spi.cpp's 0x80 >> (x & 7)). So bit 7 is the leftmost pixel of this
    // byte and no inversion or bit reversal is needed. If either side ever
    // changes, this loop is where it must be fixed -- not in the caller.
    for (uint8_t b = 0; b < 8; b++) {
      const int x = x0 + b;
      if (x >= width)
        break;  // padding bits: present on the wire, absent on the panel
      this->display_->draw_pixel_at(x, y, (bits & (0x80u >> b)) != 0 ? WHITE : BLACK);
    }
  }
  return BackendResult::OK;
}

BackendResult EpaperSPIBackend::finish_frame() {
  // Nothing to flush: draw_pixel_at wrote straight into the driver's buffer.
  // Exact-byte-count validation is the transfer engine's job and has already
  // happened by the time we get here.
  return BackendResult::OK;
}

BackendResult EpaperSPIBackend::begin_refresh(RefreshRequest request) {
  if (this->display_ == nullptr)
    return BackendResult::DEVICE_ERROR;
  // v1 is always FULL; the wire's refresh byte is parsed and ignored, and
  // epaper_spi::update() takes no mode argument anyway.
  (void) request;
  this->refresh_started_ = false;

  // update() returns void and upstream REJECTS re-entry when the driver is not
  // idle -- it logs and returns. If we ACKed the END and the call were ignored,
  // poll_refresh() would observe an unrelated in-progress refresh and we would
  // emit 0x0073 for someone else's update while our frame was never refreshed.
  // So require idle first, then confirm the call took effect.
  if (!this->display_->is_idle()) {
    ESP_LOGW(TAG, "display busy; refusing to start refresh");
    return BackendResult::DEVICE_ERROR;
  }

  this->display_->update();

  // update() drives the component out of LOOP_DONE synchronously, so this is
  // checkable right now. Confirming here rather than in poll_refresh() also
  // removes the race where a refresh completing between two polls is never
  // observed as busy and therefore never reported complete.
  if (this->display_->is_idle()) {
    ESP_LOGW(TAG, "update() did not start a refresh");
    return BackendResult::DEVICE_ERROR;
  }
  this->refresh_started_ = true;
  return BackendResult::OK;
}

PollResult EpaperSPIBackend::poll_refresh() {
  if (this->display_ == nullptr || this->display_->is_failed())
    return PollResult::FAILED;
  if (!this->refresh_started_)
    return PollResult::PENDING;

  // is_idle() is true iff the component state is LOOP_DONE, and the driver
  // disable_loop()s once its state machine reaches IDLE -- having BUSY-waited
  // before POWER_OFF. So for this backend, unlike IT8951, completion is genuine.
  //
  // This depends on busy_pin being configured. Without it the driver's
  // is_idle_() is hard-true and completion is meaningless -- codegen rejects
  // that configuration rather than letting it through.
  if (!this->display_->is_idle())
    return PollResult::PENDING;

  this->refresh_started_ = false;
  return PollResult::COMPLETE;
}

void EpaperSPIBackend::abort_transfer() {
  // Nothing of ours to release -- but note what this does NOT do.
  //
  // There is no staging buffer, so a partial frame is already sitting live in
  // the driver's framebuffer and we cannot roll it back. We leave it: clearing
  // would flash the panel white on every aborted transfer, and stale bytes are
  // harmless AS LONG AS nothing refreshes before a complete frame replaces them.
  //
  // That condition is NOT enforced. A local automation calling panel.update()
  // after busy clears will display half the new image over half the old. See
  // docs/TODO.md finding 14 -- an accepted documented exposure, not a fix.
  this->refresh_started_ = false;
}

}  // namespace opendisplay
}  // namespace esphome

#endif  // USE_OPENDISPLAY_EPAPER_SPI
