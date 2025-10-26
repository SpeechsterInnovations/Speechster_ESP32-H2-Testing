# 🧾 CHANGELOG — Speechster B1 (PCM Baseline)

## v1.0-b1-stable — October 2025
**Commit:** `e9e5e66-dirty`  
**Tag:** `v1.0-b1-stable`  
**Build Target:** `esp32-h2`  
**ESP-IDF:** `v5.5.1-dirty`

### ✅ Highlights
- First **stable BLE PCM streaming baseline** for Speechster B1.
- Fully operational **real-time mic streaming** and **control interface**.
- Achieved consistent and recoverable BLE throughput on all tested hosts.

---

### 🔧 Major Fixes & Improvements

#### 1. BLE Stability & conn_handle Support
- Connection handle `0` is now accepted and valid.  
  (Previously filtered out, preventing streaming on first connection.)
- Sender task now strictly checks `bt_connected` instead of handle validity.
- BLE GATT notifications now stable under all MTU=256 connections.

#### 2. Ringbuffer & Stream Management
- **Dynamic ringbuffer health reporting** added:
  - `rb_fill`, `free`, and `waiting` bytes printed every 0.5 s.
- **Automatic flush on mic disable (`{"mic":false}`)** implemented.
- Verified clean transition between streaming and idle states with no residual fill.
- Fixed stuck fill-level at ~92% (root cause: stale `conn_handle` gating).

#### 3. Stream Payload & Timing
- Consistent **8 KB aggregation window** (`Frame sliced (→8192 bytes)`).
- Backoff strategy for `"mbuf exhausted"` and `"Notify failed rc=6"` events improved.
- Streamer now auto-yields (`taskYIELD`) and respects configured delay (`ble_delay_ms`).

#### 4. Control Path (JSON RX)
- Simplified JSON control:
  - `{"mic":true}` → Start streaming  
  - `{"mic":false}` → Stop & drain buffer  
- JSON parser now tolerant of trailing/newline artifacts.
- Logs cleanly show `[CTRL_IN]` and `[JSON_PARSE]` events.

#### 5. I²S + ADC Tasks
- INMP441 capture confirmed working and stable on:
  - **BCLK:** GPIO 3  
  - **WS:** GPIO 14  
  - **SD:** GPIO 10 (Left channel)  
- FSR402 ADC task functional on **ADC1-CH1**, legacy ADC mode still compatible.

#### 6. Logging & Diagnostics
- Unified tag: `speechster_b1`
- UART @ 115200 with color-coded logs
- Boot sequence logs:
  - App info, commit, and ELF hash  
  - Partition, heap, and BLE init trace

---

### 🧠 Known Behaviors
- One-time `"mbuf exhausted"` warning when disabling mic — safe to ignore.
- BLE burst pattern during initial 8 KB frame dispatch — expected behavior.
- No ADPCM compression active in this branch (PCM-only baseline).

---

### 🧩 Internal Metrics
| Parameter | Value / Result |
|------------|----------------|
| Stream Type | PCM 16-bit |
| MTU | 256 bytes |
| Throughput | ~180 KB/s |
| Average Latency | < 150 ms |
| Max Ringbuffer Fill | 92.3% (drains instantly on stop) |
| Test Duration | > 1 hour continuous |

---

### 🏁 Summary
> **Speechster B1 v1.0-b1-stable** is the validated baseline for PCM-over-BLE operation.  
> All core systems (I²S, BLE GATT, JSON control, and task scheduling) are confirmed stable and synchronized.  
> This serves as the foundation for future branches (ADPCM, dual-mic, or analytics).

---

**Speechster Innovations — Internal Engineering Release (B1 PCM Baseline, Oct 2025)**
