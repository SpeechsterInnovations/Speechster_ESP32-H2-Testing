#!/usr/bin/env python3
"""
serial_wav_capture.py

Host-side serial listener that:
 - waits for FRAME_START / FRAME_END markers from ESP,
 - collects raw PCM16LE mono @ 16 kHz between markers,
 - writes a 5-second WAV file per captured frame (pads or truncates),
 - prints a lot of debugging information (configurable),
 - robust to marker-splits across reads and partial writes.

Designed for the ESP32-H2 firmware which sends:
  - ASCII marker "<FRAME_START>\n"
  - raw PCM16LE bytes (mono, 16 kHz)
  - ASCII marker "<FRAME_END>\n"

Author: ChatGPT (adapted to your project)
Date: 2025-09-30
"""

import argparse
import os
import sys
import time
import wave
import logging
import threading
from datetime import datetime
from collections import deque

try:
    import serial
    from serial.serialutil import SerialException
except Exception as e:
    print("ERROR: pyserial not installed. Install with: pip install pyserial")
    raise

# -------------------------
# Constants (match your firmware)
# -------------------------
FRAME_START_MARKER = b"<FRAME_START>\n"
FRAME_END_MARKER   = b"<FRAME_END>\n"

SAMPLE_RATE    = 16000
CHANNELS       = 1
SAMPLE_WIDTH   = 2   # bytes per sample (16-bit)
RECORD_SECONDS = 5
EXPECTED_PCM_BYTES = SAMPLE_RATE * SAMPLE_WIDTH * RECORD_SECONDS

# -------------------------
# Defaults
# -------------------------
DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_BAUD = 115200  # you used higher earlier; override with --baud
READ_CHUNK = 4096      # bytes to read per serial operation
SERIAL_TIMEOUT = 0.5   # seconds; non-blocking-ish read

# -------------------------
# Helpers / utils
# -------------------------
def human_time():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]

def safe_mkdir(path):
    try:
        os.makedirs(path, exist_ok=True)
    except Exception as e:
        logging.error("Failed to create dir %s: %s", path, e)
        raise

def pad_or_trim_pcm(pcm_bytes, expected_len=EXPECTED_PCM_BYTES):
    """Return bytes exactly expected_len long. If shorter -> pad with zeros. If longer -> truncate."""
    if len(pcm_bytes) == expected_len:
        return pcm_bytes
    if len(pcm_bytes) < expected_len:
        pad = bytes(expected_len - len(pcm_bytes))
        return pcm_bytes + pad
    # longer -> truncate
    return pcm_bytes[:expected_len]

def write_wav_file(filename, pcm_bytes, sample_rate=SAMPLE_RATE, sampwidth=SAMPLE_WIDTH, channels=CHANNELS):
    """Write PCM16LE mono into a .wav using wave module (handles header)."""
    with wave.open(filename, 'wb') as wf:
        wf.setnchannels(channels)
        wf.setsampwidth(sampwidth)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_bytes)

def bytes_summary(b):
    """Short printable summary for a bytes object"""
    return "<RAW {} bytes>".format(len(b))

# -------------------------
# Main capturing class
# -------------------------
class SerialWavCapture:
    def __init__(self, port, baud, out_dir="captures", print_all=False, max_debug=False, filter_logs=False, timeout=SERIAL_TIMEOUT):
        self.port = port
        self.baud = baud
        self.out_dir = out_dir
        self.print_all = print_all
        self.max_debug = max_debug
        self.filter_logs = filter_logs
        self.timeout = timeout

        # internal buffers/state
        self.serial = None
        self._stop_event = threading.Event()
        self._recv_thread = None

        # stream buffer used for searching markers. We keep a deque for efficiency
        self.buffer = bytearray()

        # inside_frame flag & frame buffer
        self.inside_frame = False
        self.frame_buffer = bytearray()

        # counters
        self.frame_count = 0
        self.total_received = 0
        self.last_print_time = time.time()

        # a small set of strings (bytes) to optionally filter out repeated ASCII boot logs
        # If filter_logs enabled, such lines will be omitted from normal printing to reduce noise.
        self._common_esp_prefixes = [
            b"ESP-ROM:", b"Build:", b"rst:", b"SPIWP:", b"load:", b"entry", b"Boot", b"Detected size",
            b"I (", b"heap_init:", b"spi_flash:", b"sleep_gpio:", b"main_task:", b"App init:", b"INMP441"
        ]

        # safety: maximum frame size we allow buffering (avoid OOM). Should be >= EXPECTED_PCM_BYTES + markers
        self.max_frame_buffer = max(EXPECTED_PCM_BYTES * 3, 10 * 1024 * 1024)  # 10MB default cap

    # ---------------------
    # Serial open/close
    # ---------------------
    def open(self):
        """Open serial port."""
        try:
            self.serial = serial.Serial(self.port, self.baud, timeout=self.timeout)
            # flush input to avoid old bytes being immediately processed
            try:
                self.serial.reset_input_buffer()
                self.serial.reset_output_buffer()
            except Exception:
                pass
        except SerialException as e:
            logging.error("Failed to open serial port %s @ %d: %s", self.port, self.baud, e)
            raise

        logging.info("Opened serial: %s @ %d", self.port, self.baud)

    def close(self):
        """Close serial port."""
        if self.serial and self.serial.is_open:
            try:
                self.serial.close()
                logging.info("Closed serial port.")
            except Exception as e:
                logging.warning("Error closing serial: %s", e)
        self.serial = None

    # ---------------------
    # Start/Stop loop
    # ---------------------
    def start(self):
        """Start background reader thread."""
        safe_mkdir(self.out_dir)
        self._stop_event.clear()
        self._recv_thread = threading.Thread(target=self._run_reader, daemon=True, name="serial-reader")
        self._recv_thread.start()
        logging.info("Reader thread started.")

    def stop(self):
        """Request stop and wait for thread."""
        self._stop_event.set()
        if self._recv_thread:
            self._recv_thread.join(timeout=2.0)
        self.close()

    # ---------------------
    # Internal helpers for printing
    # ---------------------
    def _should_filter_line(self, data_line):
        """Return True if data_line appears to be a repetitive ASCII boot/info message to be filtered when filter_logs True."""
        # only attempt filter on ASCII-like lines (no embedded nulls and printable)
        if b'\x00' in data_line:
            return False
        try:
            s = data_line.decode('utf-8', errors='ignore').strip()
        except Exception:
            return False
        if not s:
            return False
        # check prefixes
        for p in self._common_esp_prefixes:
            if data_line.startswith(p):
                return True
            # also check substring
            if p in data_line:
                return True
        return False

    def _print_chunk(self, chunk_bytes):
        """Print chunk according to verbosity settings."""
        ts = human_time()
        if self.print_all:
            # print exact bytes representation — this prints everything like earlier
            # Use repr so special bytes shown.
            sys.stdout.buffer.write(b"[" + ts.encode() + b"] ")
            sys.stdout.buffer.write(repr(chunk_bytes).encode() + b"\n")
            sys.stdout.flush()
            return

        # default / concise printing: if max_debug -> print hex preview (first 64 bytes)
        if self.max_debug:
            hex_preview = chunk_bytes.hex()[:128]
            logging.debug("[%s] %s %s", ts, bytes_summary(chunk_bytes), hex_preview + ("..." if len(hex_preview) < len(chunk_bytes)*2 else ""))
            # also dump ASCII printable part
            try:
                printable = chunk_bytes.decode('utf-8', errors='replace')
                logging.debug("ASCII preview: %s", printable.replace("\n", "\\n")[:200])
            except Exception:
                pass
            return

        # filter logs (non-destructive)
        if self.filter_logs and self._should_filter_line(chunk_bytes):
            # for filtered lines show a short marker occasionally (to indicate suppressed content)
            now = time.time()
            if now - self.last_print_time > 5.0:
                logging.info("[%s] %s (filtered repetitive ASCII line)", ts, bytes_summary(chunk_bytes))
                self.last_print_time = now
            return

        # baseline concise print
        logging.info("[%s] %s", ts, bytes_summary(chunk_bytes))

    # ---------------------
    # Marker handling and frame extraction
    # ---------------------
    def _process_incoming(self, new_bytes):
        """
        Append new_bytes to a rolling buffer, look for markers, and extract frames.

        Handles markers split across read boundaries.
        """
        if not new_bytes:
            return

        self.total_received += len(new_bytes)
        self.buffer.extend(new_bytes)

        # To avoid unbounded growth in the unlikely event of no markers, cap buffer length
        if len(self.buffer) > self.max_frame_buffer:
            logging.warning("Buffer exceeded max_frame_buffer (%d bytes). Truncating older data.", self.max_frame_buffer)
            # keep only tail
            self.buffer = self.buffer[-self.max_frame_buffer:]

        # fast path: if not inside frame, search for start marker and discard preceding stuff
        while True:
            if not self.inside_frame:
                idx = self.buffer.find(FRAME_START_MARKER)
                if idx == -1:
                    # no start marker yet; optionally print and drop old bytes to keep buffer small
                    # Print a summary of what's received (but not hex, unless max_debug)
                    # We'll print only up to first 128 bytes for brevity
                    # If marker not found and buffer very large, we keep tail to avoid starving memory
                    if len(self.buffer) > 8192:
                        # show concise snippet
                        snippet = bytes(self.buffer[:64])
                        self._print_chunk(snippet)
                        # keep last 4096 bytes only
                        self.buffer = self.buffer[-4096:]
                    else:
                        # if small, print snippet
                        if new_bytes:
                            # print the latest chunk that caused this invocation
                            self._print_chunk(new_bytes)
                    break
                else:
                    # Found start marker. Discard anything before it (but print if debug)
                    prefix = bytes(self.buffer[:idx])
                    if prefix:
                        # Print the prefix so you can see stray logs before marker
                        self._print_chunk(prefix)
                    # remove everything up to after start marker
                    del self.buffer[:idx + len(FRAME_START_MARKER)]
                    self.inside_frame = True
                    self.frame_buffer = bytearray()
                    # Continue loop to attempt to locate end marker in remainder of buffer
                    continue
            else:
                # we're inside a frame: look for end marker in buffer
                idx_end = self.buffer.find(FRAME_END_MARKER)
                if idx_end == -1:
                    # No end marker yet: consume whole buffer into frame buffer and clear buffer
                    if self.buffer:
                        # append all to frame_buffer and clear
                        self.frame_buffer.extend(self.buffer)
                        self.buffer.clear()
                    break
                else:
                    # Found end. Append up to idx_end to frame_buffer
                    self.frame_buffer.extend(self.buffer[:idx_end])
                    # remove the consumed part + the marker
                    del self.buffer[:idx_end + len(FRAME_END_MARKER)]
                    # we have a complete frame -> process it
                    self._handle_complete_frame(bytes(self.frame_buffer))
                    # reset inside_frame to look for next start
                    self.inside_frame = False
                    self.frame_buffer = bytearray()
                    # continue to see if another start marker exists in the leftover buffer, loop around
                    continue

    def _handle_complete_frame(self, pcm_bytes):
        """Sanity check and write to WAV file (5 seconds)."""
        self.frame_count += 1
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        filename = os.path.join(self.out_dir, f"recording_{self.frame_count}_{ts}.wav")

        logging.info("Frame %d received: %d bytes. Expected: %d bytes.", self.frame_count, len(pcm_bytes), EXPECTED_PCM_BYTES)
        if len(pcm_bytes) != EXPECTED_PCM_BYTES:
            # error / weird size: show detailed info. If not max_debug, only print concise unless error
            logging.warning("Frame size mismatch: got %d, expected %d. Will pad/trim to 5 seconds and save. (Enable --max-debug to see hexdump on error)", len(pcm_bytes), EXPECTED_PCM_BYTES)
            if self.max_debug:
                # hexdump first 256 bytes for debugging
                hex_preview = pcm_bytes[:256].hex()
                logging.debug("HEX preview (first 256 bytes): %s ...", hex_preview)
        else:
            if self.max_debug:
                logging.debug("Frame length OK (%d bytes).", len(pcm_bytes))

        pcm_fixed = pad_or_trim_pcm(pcm_bytes, EXPECTED_PCM_BYTES)
        try:
            write_wav_file(filename, pcm_fixed)
            logging.info("Saved WAV: %s  (duration: %ds)", filename, RECORD_SECONDS)
        except Exception as e:
            logging.error("Failed to write WAV file %s: %s", filename, e)
            # on write error, if max_debug, dump a hexdump to help diagnose
            if self.max_debug:
                logging.exception("Saving failed: first 512 bytes (hex): %s", pcm_bytes[:512].hex())

    # ---------------------
    # Reader loop
    # ---------------------
    def _run_reader(self):
        """Main loop executed in a background thread to read serial data and process."""
        last_stats = time.time()
        stat_interval = 5.0  # seconds
        try:
            while not self._stop_event.is_set():
                try:
                    data = self.serial.read(READ_CHUNK)
                except SerialException as e:
                    logging.error("Serial read error: %s", e)
                    # try to reopen?
                    break

                if data:
                    # print chunk according to verbosity
                    self._print_chunk(data)
                    # process markers and frames
                    self._process_incoming(data)
                else:
                    # no data read within timeout
                    pass

                # periodic stats print
                now = time.time()
                if now - last_stats >= stat_interval:
                    logging.info("Status: total_received=%d bytes, frames_saved=%d, inside_frame=%s, buffer=%d bytes",
                                 self.total_received, self.frame_count, bool(self.inside_frame), len(self.buffer))
                    last_stats = now

            logging.info("Reader loop exiting (stop requested).")
        except Exception as e:
            logging.exception("Unhandled exception in reader thread: %s", e)
        finally:
            # on exit, attempt to flush any partial frame (not recommended)
            if self.inside_frame and self.frame_buffer:
                logging.warning("Exiting while inside a frame and %d bytes buffered. Will attempt to save as partial frame.", len(self.frame_buffer))
                self._handle_complete_frame(bytes(self.frame_buffer))

# -------------------------
# CLI + main
# -------------------------
def parse_args():
    ap = argparse.ArgumentParser(description="Serial -> WAV capture utility with high debugability.\n"
                                         "Detects FRAME_START/FRAME_END, saves 5s WAVs and prints debug info.")
    ap.add_argument("--port", "-p", default=DEFAULT_PORT, help=f"Serial port (default: {DEFAULT_PORT})")
    ap.add_argument("--baud", "-b", default=DEFAULT_BAUD, type=int, help=f"Baud rate (default: {DEFAULT_BAUD})")
    ap.add_argument("--out", "-o", default="captures", help="Directory to save recordings (default: ./captures)")
    ap.add_argument("--print-all", action="store_true", help="Print every raw chunk received (repr), very verbose")
    ap.add_argument("--max-debug", action="store_true", help="Enable maximum debug (hexdumps on errors, extra info)")
    ap.add_argument("--filter-logs", action="store_true", help="Filter common ASCII boot/info lines from normal printing (still captured)")
    ap.add_argument("--timeout", type=float, default=SERIAL_TIMEOUT, help=f"Serial read timeout in seconds (default: {SERIAL_TIMEOUT})")
    ap.add_argument("--one-shot", action="store_true", help="Exit after saving the first valid frame (useful for batch capture)")
    ap.add_argument("--no-sanity", action="store_true", help="Disable size sanity-check logging (still pads/trims to 5s)")
    return ap.parse_args()

def setup_logging(max_debug=False):
    lvl = logging.DEBUG if max_debug else logging.INFO
    fmt = "%(asctime)s %(levelname)s %(message)s"
    logging.basicConfig(level=lvl, format=fmt, datefmt="%H:%M:%S")

def main():
    args = parse_args()
    setup_logging(args.max_debug)

    print("="*80)
    print("Serial WAV Capture - listening for markers and saving 5-second WAVs")
    print("FRAME_START_MARKER:", FRAME_START_MARKER)
    print("FRAME_END_MARKER  :", FRAME_END_MARKER)
    print("Expected PCM bytes per frame:", EXPECTED_PCM_BYTES, "(= {}s @ {}Hz {} bytes)".format(RECORD_SECONDS, SAMPLE_RATE, SAMPLE_WIDTH))
    print("Saving files to:", args.out)
    print("Print-all mode:", args.print_all)
    print("Max-debug:", args.max_debug)
    print("Filter logs:", args.filter_logs)
    print("Serial port:", args.port, "baud:", args.baud)
    print("="*80)

    cap = SerialWavCapture(port=args.port, baud=args.baud, out_dir=args.out,
                          print_all=args.print_all, max_debug=args.max_debug,
                          filter_logs=args.filter_logs, timeout=args.timeout)

    try:
        cap.open()
    except Exception as e:
        logging.error("Cannot open serial port. Exiting.")
        return 2

    cap.start()

    try:
        # main loop: wait until user ctrl+c or one-shot triggers
        while True:
            time.sleep(0.25)
            # if one-shot and we saved at least one frame -> exit
            if args.one_shot and cap.frame_count > 0:
                logging.info("One-shot mode: frame saved, exiting.")
                break
    except KeyboardInterrupt:
        logging.info("Ctrl-C received; stopping...")
    finally:
        cap.stop()
        logging.info("Exited cleanly. Frames saved: %d", cap.frame_count)

if __name__ == "__main__":
    main()
