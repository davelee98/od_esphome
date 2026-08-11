// Compiled only when this backend is selected. ESPHome copies EVERY file in
// a used component into the build (writer.py copy_src_tree), so without this
// guard the driver header below is included even when that display component
// is absent from the config -- which is a hard "No such file" build failure.
// The define is emitted by to_code() in __init__.py and reaches us via
// defines.h, which MUST be included BEFORE the guard -- an #ifdef ahead of
// every #include sees nothing and silently compiles the file to nothing,
// which surfaces only as an undefined vtable at link time.
#include "esphome/core/defines.h"
#ifdef USE_OPENDISPLAY_IT8951
#include "backend_it8951.h"

#include "esphome/components/it8951/it8951.h"
#include "esphome/core/color.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace opendisplay {

static const char *const TAG = "opendisplay.it8951";

bool IT8951Backend::init() {
  if (this->display_ == nullptr)
    return false;
  if (this->display_->is_failed()) {
    ESP_LOGE(TAG, "display driver has failed; refusing to advertise capabilities");
    return false;
  }
  if (this->display_->get_rotation() != display::DISPLAY_ROTATION_0_DEGREES) {
    ESP_LOGE(TAG, "display rotation must be 0 for opendisplay");
    return false;
  }

  this->caps_.width = this->display_->get_native_width();
  this->caps_.height = this->display_->get_native_height();
  if (this->caps_.width == 0 || this->caps_.height == 0)
    return false;

  // Only 1bpp mono is implemented. The 4bpp path needs the grayscale packing
  // (2 px/byte, even x in the high nibble) plus a validated panel id, and must
  // not be advertised before that is tested.
  if (this->caps_.color_scheme != OD_COLOR_SCHEME_MONO) {
    ESP_LOGE(TAG, "only OD_COLOR_SCHEME_MONO is implemented (got %u)", this->caps_.color_scheme);
    return false;
  }

  this->caps_.row_stride_bytes = (this->caps_.width + 7) / 8;
  this->caps_.full_frame_bytes =
      static_cast<uint32_t>(this->caps_.row_stride_bytes) * this->caps_.height;
  this->caps_.byte_alignment = 1;
  this->caps_.max_contiguous_write = this->caps_.full_frame_bytes;
  this->caps_.staging_bytes_required = 0;

  this->caps_.supports_full_refresh = true;
  this->caps_.supports_fast_refresh = false;
  this->caps_.supports_partial = PartialSupport::NONE;
  this->caps_.supports_compression = false;
  this->caps_.supports_direct_write = true;
  this->caps_.supports_pipe_write = true;

  ESP_LOGCONFIG(TAG, "it8951 backend: %ux%u, %u bytes/frame", static_cast<unsigned>(this->caps_.width),
                static_cast<unsigned>(this->caps_.height),
                static_cast<unsigned>(this->caps_.full_frame_bytes));
  ESP_LOGW(TAG, "0x0073 reports a fixed %u ms settle, NOT physical refresh completion",
           static_cast<unsigned>(IT8951_REFRESH_SETTLE_MS));
  return true;
}

BackendResult IT8951Backend::begin_frame(const FrameDescriptor &frame) {
  if (this->display_ == nullptr)
    return BackendResult::DEVICE_ERROR;
  if (frame.compressed || frame.partial)
    return BackendResult::UNSUPPORTED;
  if (frame.color_scheme != this->caps_.color_scheme)
    return BackendResult::UNSUPPORTED;
  if (frame.total_bytes != this->caps_.full_frame_bytes)
    return BackendResult::INVALID_ARGUMENT;
  this->refresh_settle_deadline_ms_ = 0;
  return BackendResult::OK;
}

BackendResult IT8951Backend::write_contiguous(uint32_t offset, const uint8_t *data, size_t length) {
  if (this->display_ == nullptr || data == nullptr)
    return BackendResult::DEVICE_ERROR;
  // Checked before any access, phrased so offset + length cannot wrap.
  if (offset > this->caps_.full_frame_bytes || length > this->caps_.full_frame_bytes - offset)
    return BackendResult::INVALID_ARGUMENT;

  static const Color WHITE{255, 255, 255};
  static const Color BLACK{0, 0, 0};

  const uint16_t stride = this->caps_.row_stride_bytes;
  const int width = static_cast<int>(this->caps_.width);

  // Same per-pixel path as epaper_spi: the driver always owns a full MCU
  // framebuffer, so there is no "stream into controller RAM" shortcut and no
  // packed-1bpp bulk ingress anywhere in the ESPHome display API.
  for (size_t i = 0; i < length; i++) {
    const uint32_t byte_index = offset + i;
    const int y = static_cast<int>(byte_index / stride);
    const int x0 = static_cast<int>((byte_index % stride) * 8);
    const uint8_t bits = data[i];
    for (uint8_t b = 0; b < 8; b++) {
      const int x = x0 + b;
      if (x >= width)
        break;
      this->display_->draw_pixel_at(x, y, (bits & (0x80u >> b)) != 0 ? WHITE : BLACK);
    }
  }
  return BackendResult::OK;
}

BackendResult IT8951Backend::finish_frame() { return BackendResult::OK; }

BackendResult IT8951Backend::begin_refresh(RefreshRequest request) {
  if (this->display_ == nullptr)
    return BackendResult::DEVICE_ERROR;
  (void) request;  // v1 is always FULL

  // Verify the driver actually takes the request. update() returns void and
  // upstream rejects re-entry when the component is not idle -- ACKing an
  // ignored call would later attribute someone else's refresh to our frame.
  if (!this->display_->is_idle())
    return BackendResult::DEVICE_ERROR;

  this->display_->update();
  this->refresh_settle_deadline_ms_ = millis() + IT8951_REFRESH_SETTLE_MS;
  return BackendResult::OK;
}

PollResult IT8951Backend::poll_refresh() {
  if (this->display_ == nullptr || this->display_->is_failed())
    return PollResult::FAILED;
  if (this->refresh_settle_deadline_ms_ == 0)
    return PollResult::PENDING;

  // FIXED SETTLE TIME, not observable completion. UPDATE_REFRESH is
  // fire-and-forget upstream and the only real fix needs an upstream addition,
  // which the project forbids. So COMPLETE here means "the settle time elapsed".
  // Do not shorten this to make transfers feel faster.
  if (static_cast<int32_t>(millis() - this->refresh_settle_deadline_ms_) >= 0) {
    this->refresh_settle_deadline_ms_ = 0;
    return PollResult::COMPLETE;
  }
  return PollResult::PENDING;
}

void IT8951Backend::abort_transfer() { this->refresh_settle_deadline_ms_ = 0; }

}  // namespace opendisplay
}  // namespace esphome

#endif  // USE_OPENDISPLAY_IT8951
