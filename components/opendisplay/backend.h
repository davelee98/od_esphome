#pragma once

// Hardware-neutral adapter between the transfer engine and an ESPHome display
// driver.
//
// Deliberately narrow: it exposes no raw SPI and requires no access to private
// driver state. If an implementation cannot be written against a driver's public
// API, the fix is a small upstreamable ESPHome hook -- not a friend declaration,
// not a reinterpret_cast, not a copied driver.
//
// The adapter never sends BLE responses. It returns typed results; the component
// decides what goes on the wire.

#include "protocol_types.h"

namespace esphome {
namespace opendisplay {

class OpenDisplayBackend {
 public:
  virtual ~OpenDisplayBackend() = default;

  // Called once from Component::setup(). Allocate staging storage here and fail
  // loudly (return false) if it cannot be satisfied -- a half-initialised
  // backend must never advertise capabilities.
  virtual bool init() = 0;

  // Set by codegen before init(). The colour scheme cannot be read back from
  // either driver -- get_display_type() only distinguishes BINARY/COLOR -- so it
  // must come from the YAML `model:`. panel_ic_type drives the client's measured
  // palette tables.
  virtual void set_color_scheme(uint8_t scheme) = 0;
  virtual void set_panel_ic_type(uint16_t id) = 0;

  // Single source of truth for what this panel can do. Drives both the transfer
  // engine's validation and the 0x0040 config response.
  virtual const DisplayCapabilities &capabilities() const = 0;

  // Open a frame. May reserve/preclear staging storage or configure an IT8951
  // memory load. Called from loop(), never from a BLE callback.
  virtual BackendResult begin_frame(const FrameDescriptor &frame) = 0;

  // Write a contiguous run. In v1 `offset` is monotonically increasing; an
  // implementation may assume that but must still bounds-check.
  virtual BackendResult write_contiguous(uint32_t offset, const uint8_t *data, size_t length) = 0;

  // Final validation or bounded conversion before refresh.
  virtual BackendResult finish_frame() = 0;

  // Must return quickly. Physical completion is observed via poll_refresh().
  virtual BackendResult begin_refresh(RefreshRequest request) = 0;

  // Called from loop(). Never blocks, never waits on panel BUSY.
  virtual PollResult poll_refresh() = 0;

  // Release transfer resources and leave the backend in a defined state. Must be
  // safe to call from any non-idle state, including mid-refresh.
  virtual void abort_transfer() = 0;
};

}  // namespace opendisplay
}  // namespace esphome
