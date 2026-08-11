# od_esphome

An ESPHome external component that implements the **OpenDisplay BLE protocol**, so an ESP32 running
ESPHome can act as an OpenDisplay device and accept e-paper frames from any OpenDisplay client.

## Purpose

The component's job is the protocol: BLE transport, command parsing, transfer accounting, and
transaction state. It does not drive panel hardware itself.

**Direct control of the panel is delegated to the existing ESPHome display components** —
[`epaper_spi`](https://esphome.io/components/display/epaper_spi/) (ESPHome 2025.10.0+) or
[`it8951`](https://esphome.io/components/display/it8951/) (ESPHome 2026.7.0+). `opendisplay:`
converts received OpenDisplay pixel data into the form its configured backend expects and asks that
driver to refresh.

**Other peripherals are out of scope.** LEDs, buzzers, NFC, deep sleep, DFU/OTA, and reboot are not
implemented here. Where those overlap with the OpenDisplay protocol, the corresponding opcodes are
deliberately unimplemented — ESPHome already provides its own components and lifecycle management for
them, and duplicating that inside this component would compete with it.

The component accesses its one configured display directly; it is not a display arbiter. It exposes
busy/activity state, and cooperating ESPHome automations are expected to wait on that state before
writing to the same panel.

## Security

**There is no access control.** The BLE characteristic is open and unauthenticated: no pairing, no
bonding, and no OpenDisplay session authentication. Anyone within radio range can connect and write
an image to the display.

This is a deliberate choice for v1 — it matches the reference firmware's default and keeps discovery
frictionless — but it should be a conscious one on your side too. Do not deploy this where an
untrusted party in BLE range writing to the screen would matter.

## Status

Pre-implementation. The repository currently contains the design document and the vendored protocol
headers; no component source exists yet.

- Design spec: [docs/OpenDisplay_ESPHome_Component_Plan.md](docs/OpenDisplay_ESPHome_Component_Plan.md)
- Work list: [docs/TODO.md](docs/TODO.md)
- Driver reference: [docs/ESPHome_Display_Drivers_Reference.md](docs/ESPHome_Display_Drivers_Reference.md)
- Config response: [docs/CONFIG_READ_Design.md](docs/CONFIG_READ_Design.md)
- Advertisement: [docs/ADVERTISEMENT_Design.md](docs/ADVERTISEMENT_Design.md)

## License

GPL-3.0. See [LICENSE](LICENSE).
