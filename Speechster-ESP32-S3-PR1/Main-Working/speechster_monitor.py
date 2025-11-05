#!/usr/bin/env python3
"""
speechster_monitor.py

- Connects to a PTY (socat proxy) and blends an on-screen cinematic ritual
  with the real ESP32 serial logs.
- Detects the firmware build marker ("Build ... IDF") and then execs
  `idf.py -p <pty> monitor` for the real monitor (unless --no-reveal).
- Auto-clears for cinematic effect; flicker -> heartbeat -> smooth title card.
- Log lines are emitted in ESP-IDF style (I/W/E (ticks) tag: message) so they merge.
"""

import sys
import os
import re
import time
import random
import argparse
import shutil

try:
    import serial
except Exception:
    print("ERROR: pyserial required. Install with: python3 -m pip install --user pyserial")
    sys.exit(2)

# ----- args -----
parser = argparse.ArgumentParser()
parser.add_argument("port", help="PTY path to open (e.g. /tmp/speechster_proxy)")
parser.add_argument("baud", type=int, help="baud rate (e.g. 115200)")
parser.add_argument("--no-reveal", action="store_true", help="simulate handoff; do not exec idf.py monitor")
args = parser.parse_args()

PORT = args.port
BAUD = args.baud
NO_REVEAL = args.no_reveal

# build marker: matches the ESP_LOGI build line
BUILD_MARKER = re.compile(r"Build .*IDF")

# convenience: ANSI
CLEAR = "\033[2J\033[H"
RESET = "\033[0m"

# Utility: produce esp-style prefix: "I (12345) tag: "
def esp_prefix(level_char="I", tag="main"):
    # use ms since start modulo a value to resemble tick count
    t = int(round(time.time() * 1000)) % 100000
    return f"{level_char} ({t}) {tag}: "

# realistic plausible embedded errors/messages pool
PRELUDE_LINES = [
    ("W", "rtc", "RTC clock drifting; re-sync scheduled"),
    ("W", "adc", "ADC calibration unstable — retrying"),
    ("E", "wifi", "DHCP timeout, switching to passive"),
    ("W", "bt", "BT controller startup delayed"),
    ("W", "heap", "Low heap on init; increasing reserve"),
    ("I", "mem", "Heap allocator initialized"),
    ("W", "ble", "BLE buffer underrun (retrying)"),
    ("I", "core", "Initializing core services"),
    ("W", "adc", "ADC calibration: variance high"),
    ("E", "sensor", "I2C read error on peripheral; retry"),
]

# heartbeat bar settings
HEARTBEAT_STEPS = 22

# flicker corruption helper
def corrupt_text(s, intensity=0.12):
    out = []
    for ch in s:
        if ch.isspace():
            out.append(ch)
        elif random.random() < intensity:
            out.append(random.choice("@#%&$*+=-?"))
        else:
            out.append(ch)
    return "".join(out)

# print with small jitter as human/ritual timing
def jitter_print(s, jitter=0.0025):
    for ch in s:
        sys.stdout.write(ch)
        sys.stdout.flush()
        # small random delay
        if random.random() < 0.065:
            time.sleep(random.uniform(0.008, 0.05))
        else:
            time.sleep(jitter)
    sys.stdout.write("\n")
    sys.stdout.flush()

# open serial (PTY) with timeout
def open_serial(port, baud, timeout=8.0):
    start = time.time()
    last_err = None
    import serial as _serial
    while time.time() - start < timeout:
        try:
            ser = _serial.Serial(port, baud, timeout=0.12)
            return ser
        except Exception as e:
            last_err = e
            time.sleep(0.2)
    print(f"ERROR: Could not open PTY {port}: {last_err}")
    sys.exit(3)

# Clear screen helper
def clear_screen():
    sys.stdout.write(CLEAR)
    sys.stdout.flush()

# Title card gradient flicker (slow ~2s)
def title_card():
    # prepare text centered-ish (terminal width try)
    cols = 120
    try:
        cols = os.get_terminal_size().columns
    except Exception:
        pass
    lines = [
        "────────────────────────────────────────────────────────────────────────────────",
        "     Speechster-B1 — Online",
        "     “Where overheating meets overengineering.”",
        "────────────────────────────────────────────────────────────────────────────────"
    ]
    # gradient sequence: cycle cyan->magenta->white
    palette = ["\033[96m", "\033[95m", "\033[97m"]
    end = "\033[0m"
    # run flicker for ~2.0s total
    start = time.time()
    while time.time() - start < 2.0:
        clear_screen()
        pal = random.choice(palette)
        for ln in lines:
            padded = ln.center(cols)
            sys.stdout.write(pal + padded + end + "\n")
        sys.stdout.flush()
        time.sleep(0.18 + random.random() * 0.07)
    # final stable white
    clear_screen()
    for ln in lines:
        sys.stdout.write("\033[97m" + ln.center(cols) + "\033[0m\n")
    sys.stdout.flush()
    time.sleep(1.0)

# core cutscene behavior: flicker -> heartbeat -> "Hello, Creator"
def run_cutscene_interleaved(serial_obj, max_duration=8.5):
    """
    This function prints ritual lines while the serial_obj is read in the main loop;
    it returns when the ritual stabilization completes. It does NOT consume serial bytes.
    The main loop should be running concurrently reading serial and printing real logs.
    """
    # Phase I: rapid flicker (realistic plausible messages, corrupted)
    clear_screen()
    for i in range(6):
        ln = random.choice(PRELUDE_LINES)
        level, tag, msg = ln
        text = esp_prefix(level, tag) + corrupt_text(msg, intensity=0.24 if i < 3 else 0.12)
        sys.stdout.write("\033[91m" + text + "\033[0m" + "\n")
        sys.stdout.flush()
        time.sleep(0.14 + random.random() * 0.18)

    # small chaotic hex/stack-like burst (plausible)
    for j in range(2):
        clear_line = esp_prefix("E", "stack") + "stack frame check failed: dumping minimal trace"
        sys.stdout.write("\033[91m" + corrupt_text(clear_line, 0.18) + "\033[0m\n")
        sys.stdout.flush()
        time.sleep(0.22)

    # Phase II: heartbeat bar filling
    clear_screen()
    jitter_print(esp_prefix("I", "core") + "System pulse detected")
    steps = HEARTBEAT_STEPS
    for s in range(steps + 1):
        bar = "▓" * s + "░" * (steps - s)
        sys.stdout.write("\033[96m[CORE] " + bar + "\033[0m\r")
        sys.stdout.flush()
        time.sleep(0.14 - (s / (steps * 18)))  # slightly accelerate
    sys.stdout.write("\n")
    sys.stdout.flush()
    time.sleep(0.45)

    # Phase III: stabilization messages (clean, realistic)
    jitter_print(esp_prefix("I", "mem") + "Heap stabilized")
    jitter_print(esp_prefix("I", "adc") + "ADC calibration: nominal")
    jitter_print(esp_prefix("W", "wifi") + "DHCP slow; using fallback IP")
    time.sleep(0.45)

    # The soft realization
    jitter_print(esp_prefix("I", "core") + "Signal stabilized.")
    # a dot-lead pause, then Hello, Creator line with ellipsis
    time.sleep(0.8)
    jitter_print(esp_prefix("I", "core") + "...Hello, Creator.")

    # small pause before title card
    time.sleep(0.6)

# main listening loop that interleaves ritual prints and serial passthrough
def main():
    clear_screen()
    jitter_print("[RITUAL] Preparing listening aperture...", jitter=0.004)
    ser = open_serial(PORT, BAUD)
    jitter_print(esp_prefix("I", "ritual") + f"Attached to {PORT} @ {BAUD}")
    time.sleep(0.18)
    # begin monitoring: we'll both read serial and run the cutscene. The cutscene prints to stdout
    # while serial reads are printed immediately: this causes a natural interleaving.
    ser_nonblocking = ser
    buff = b""
    found_build = False

    # start cutscene in main flow (not threaded) but maintain serial reads interleaved
    # We'll run a small event loop: for a certain number of iterations, do:
    #  - try to read serial bytes and flush any full lines immediately
    #  - at controlled times, print ritual lines
    start_time = time.time()
    ritual_done = False
    ritual_stage = 0
    last_ritual_time = 0

    # We'll schedule a sequence of ritual actions with delays
    ritual_actions = [
        (0.05, "flicker"),    # start flicker quickly
        (1.1, "burst"),
        (2.1, "heartbeat"),
        (4.2, "stabilize"),
        (6.0, "realize"),     # Hello Creator printed here
    ]

    next_action_idx = 0

    try:
        # run until we detect BUILD_MARKER in serial; during this time, periodically do ritual actions
        while True:
            # read any available serial data
            chunk = ser_nonblocking.read(512)
            if chunk:
                buff += chunk
                while b"\n" in buff:
                    line_bytes, buff = buff.split(b"\n", 1)
                    try:
                        sline = line_bytes.decode("utf-8", errors="ignore")
                    except:
                        sline = line_bytes.decode(errors="ignore")
                    # Immediately print real serial lines in their raw form (they already have ESP-IDF format)
                    sys.stdout.write(sline + "\n")
                    sys.stdout.flush()
                    # If this real serial line contains the build marker, we trigger handoff
                    if BUILD_MARKER.search(sline):
                        found_build = True
                        # after detection, show title card (slow cinematic), then handoff
                        title_card()
                        # print short transfer messages
                        clear_screen()
                        sys.stdout.write(esp_prefix("I", "Speechster-B1") + "Transferring consciousness to silicon...\n")
                        sys.stdout.write(esp_prefix("I", "Speechster-B1") + "Entering Real-Time Monitor Mode...\n")
                        sys.stdout.flush()
                        # close serial and exec monitor (unless no-reveal)
                        try:
                            ser_nonblocking.close()
                        except:
                            pass
                        if NO_REVEAL:
                            print("\n[NO-REVEAL] simulation mode: not exec'ing idf.py monitor. Exiting.")
                            sys.exit(0)
                        # exec official monitor (replaces this process)
                        if shutil.which("idf.py") is None:
                            print("ERROR: idf.py not found in PATH. Cannot exec monitor.")
                            sys.exit(5)
                        os.execvp("idf.py", ["idf.py", "-p", PORT, "monitor"])
                        # exec shouldn't return
            # perform ritual actions based on time schedule
            now = time.time() - start_time
            if next_action_idx < len(ritual_actions) and now >= ritual_actions[next_action_idx][0]:
                act = ritual_actions[next_action_idx][1]
                if act == "flicker":
                    # print a sequence of flickery prelude lines
                    for i in range(5):
                        lvl, tag, msg = random.choice(PRELUDE_LINES)
                        txt = esp_prefix(lvl, tag) + corrupt_text(msg, intensity=0.26 if i < 2 else 0.12)
                        sys.stdout.write("\033[91m" + txt + "\033[0m\n")
                        sys.stdout.flush()
                        time.sleep(0.12 + random.random() * 0.08)
                elif act == "burst":
                    # a short stack/hex-like burst plausible for embedded dev
                    for _ in range(2):
                        sys.stdout.write("\033[91m" + esp_prefix("E", "stack") + "Frame check: 0x" + ("%04x" % random.randint(0, 65535)) + "\033[0m\n")
                        sys.stdout.flush()
                        time.sleep(0.16)
                elif act == "heartbeat":
                    clear_screen()
                    jitter_print(esp_prefix("I", "core") + "System pulse detected", jitter=0.003)
                    steps = HEARTBEAT_STEPS
                    for s in range(steps + 1):
                        bar = "▓" * s + "░" * (steps - s)
                        sys.stdout.write("\033[96m[CORE] " + bar + "\033[0m\r")
                        sys.stdout.flush()
                        time.sleep(0.12)
                    sys.stdout.write("\n")
                    sys.stdout.flush()
                elif act == "stabilize":
                    jitter_print(esp_prefix("I", "mem") + "Heap stabilized")
                    jitter_print(esp_prefix("I", "adc") + "ADC calibration: nominal")
                    jitter_print(esp_prefix("W", "wifi") + "DHCP slow; using fallback IP")
                elif act == "realize":
                    jitter_print(esp_prefix("I", "core") + "Signal stabilized.")
                    time.sleep(0.7)
                    jitter_print(esp_prefix("I", "core") + "...Hello, Creator.")
                next_action_idx += 1
            # small sleep to avoid busy loop
            time.sleep(0.04)
    except KeyboardInterrupt:
        print("\n[EXIT] Interrupted by user.")
        try:
            ser_nonblocking.close()
        except:
            pass
        sys.exit(0)
    except Exception as e:
        print(f"\n[ERROR] Ritual monitor crashed: {e}")
        try:
            ser_nonblocking.close()
        except:
            pass
        sys.exit(7)

if __name__ == "__main__":
    main()
