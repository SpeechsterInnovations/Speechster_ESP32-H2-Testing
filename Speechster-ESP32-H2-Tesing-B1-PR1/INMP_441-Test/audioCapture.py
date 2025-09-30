import serial
import time
import os
import sys

# === CONFIG ===
PORT = "/dev/ttyACM0"   # Change to your serial port (e.g. "COM3" on Windows)
BAUD = 115200
OUTPUT_DIR = "recordings"

FRAME_START = b"FRAME_START"
FRAME_END   = b"FRAME_END"

# === MAIN ===
def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    ser = serial.Serial(PORT, BAUD, timeout=1)

    buffer = b""
    file_index = 0
    recording = False

    print("[*] Listening on", PORT, "at", BAUD, "baud...")

    while True:
        data = ser.read(1024)
        if not data:
            continue

        # Print ALL raw data immediately (in hex for readability)
        sys.stdout.buffer.write(data)
        sys.stdout.flush()

        buffer += data

        # Look for FRAME_START
        if not recording:
            start_idx = buffer.find(FRAME_START)
            if start_idx != -1:
                buffer = buffer[start_idx + len(FRAME_START):]
                recording = True
                file_index += 1
                print(f"\n[+] Detected FRAME_START (file {file_index})")

        # Look for FRAME_END
        if recording:
            end_idx = buffer.find(FRAME_END)
            if end_idx != -1:
                wav_data = buffer[:end_idx]
                buffer = buffer[end_idx + len(FRAME_END):]
                recording = False

                filename = os.path.join(OUTPUT_DIR, f"capture_{file_index:03d}.wav")
                with open(filename, "wb") as f:
                    f.write(wav_data)

                print(f"\n[✔] Saved {filename} ({len(wav_data)} bytes)")

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[!] Exiting.")
