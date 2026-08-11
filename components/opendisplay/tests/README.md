# Host tests

These tests run on the **host**, not on an ESP32. They exercise `protocol*`,
`protocol_types.h`, and `transfer_engine*` against a fake backend and a fake
transport — which is why those translation units must stay free of ESPHome and
ESP-IDF includes.

The test list is the specification for the M2/M3 engine work; implement against
these cases rather than against prose. Full checklist lives in
[../../../docs/TODO.md](../../../docs/TODO.md) under "Host tests".

Highest-value cases, in the order they should be written:

| Case | Why it matters |
|---|---|
| Start rejected while a transaction is active | Must not mutate the active transaction. |
| Direct write underflow / overflow | A transfer is valid only on an exact byte-count match. |
| PIPE ACK at packets 4, 8, 12, 16 | Fixed cadence is wire contract. |
| Final ACK for an incomplete group at `0x0082` | The 1–3 packet tail is easy to drop. |
| Out-of-order / duplicate PIPE packet | Must request retransmit with **zero** storage. |
| Window violation | Must abort with **zero** reorder allocation. |
| Disconnect from every non-idle state | Disconnect during REFRESHING must not abort the panel. |
| `0x0073` only after simulated physical completion | Never emit at end-of-transfer. |
| Timeout from every non-idle state | "No stuck busy state, ever" is the M5 gate. |
| `0x0072` / `0x0082` with **and without** a trailing `new_etag:4` | We ignore the value, but the optional field is detected by frame length — both forms must be accepted identically, not treated as malformed. |

Test runner: **TBD**. ESPHome has no host-test convention for external
components, so pick one (plain CMake + a lightweight framework is likely
simplest) and record the command in `CLAUDE.md` when it lands.
