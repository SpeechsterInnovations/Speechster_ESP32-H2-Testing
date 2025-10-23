## 🧩 Speechster B1 — BLE PCM Stream Baseline (STABLE)

**Commit:** `e9e5e66-dirty`  
**Firmware Name:** `BT-Test`  
**Build Target:** `esp32-h2`  
**ESP-IDF:** `v5.5.1-dirty`  
**Status:** ✅ Stable (as of Oct 2025)

### Overview
Speechster B1 represents the **first stable baseline** of the Speechster platform.  
It features **uncompressed PCM streaming over BLE** with real-time ringbuffer telemetry,  
command-based microphone control, and robust recovery from link or buffer faults.

This build is the reference implementation for future ADPCM and dual-mic releases.

### Features
- **Real-time PCM streaming** via BLE GATT (MTU 256)
- **Dynamic mic control** through JSON command (`{"mic":true/false}`)
- **Ringbuffer monitoring** (`rb_fill`, `free`, `waiting`) every 0.5 s
- **Full recovery on BLE disconnects**
- **Low-latency I²S pipeline**
  - INMP441 (left channel, SD=GPIO 10, WS=14, BCLK=3)
- **FSR402 ADC monitoring** on ADC1-CH1
- **UART logging** @ 115200
- **Advertising name:** `Speechster-B1`
- **Bluetooth MAC:** auto-generated from efuse

### Known Behaviors
- **BLE connection handle 0** is valid and now fully supported (previously ignored).  
- A single harmless `"mbuf exhausted"` warning may appear when stopping the mic — safe to ignore.  
- PCM stream slices at 8 KB aggregate (`Frame sliced (→8192 bytes)`) for optimal BLE throughput.  
- Compression is **not enabled** in this baseline.

### Stability Metrics
| Test | Result |
|------|--------|
| Continuous stream duration | ✅ > 1 hour stable |
| Ringbuffer utilization | 0–92 % dynamic, instant drain on `mic:false` |
| Health telemetry | 100 % consistent output |
| BLE throughput | ~180 KB/s (raw PCM) |
| Latency | < 150 ms (avg) |

### Internal Notes
- `bt_connected` is now the **sole gate** for sender loop execution.  
  Connection handle 0 is valid and must not be filtered out.
- To reduce bursts, you may lower the aggregate size (`AGGREGATE_TARGET 4096`) — optional.
- No ADPCM or dual-mic features are active in this branch.

**Speechster B1 — Built by Speechster Innovations, 2025**  
_Internal Release: PCM Stable Baseline (Reference Build)_
