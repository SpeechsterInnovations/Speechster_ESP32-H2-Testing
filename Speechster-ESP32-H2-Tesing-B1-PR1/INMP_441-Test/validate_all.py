#!/usr/bin/env python3
"""
validate_all.py - validate all WAV files in a directory and optionally check SHA1 sidecars.
"""
import os, sys, wave, hashlib
from pathlib import Path

DEFAULT_DIR = "recordings"
EXPECTED_SR = 16000
EXPECTED_CH = 1
EXPECTED_SW = 2  # bytes

def sha1_of_file(path):
    import hashlib
    h = hashlib.sha1()
    with open(path, "rb") as f:
        while True:
            b = f.read(65536)
            if not b: break
            h.update(b)
    return h.hexdigest()

def validate_file(wav_path: Path):
    print(f"Checking {wav_path}")
    try:
        with wave.open(str(wav_path), "rb") as wf:
            ch = wf.getnchannels()
            sw = wf.getsampwidth()
            sr = wf.getframerate()
            nframes = wf.getnframes()
            duration = nframes / float(sr) if sr else 0.0
        ok = True
        if ch != EXPECTED_CH:
            print(f"  channel mismatch: {ch} != {EXPECTED_CH}")
            ok = False
        if sw != EXPECTED_SW:
            print(f"  sampwidth mismatch: {sw} != {EXPECTED_SW}")
            ok = False
        if sr != EXPECTED_SR:
            print(f"  samplerate mismatch: {sr} != {EXPECTED_SR}")
            ok = False
        print(f"  frames={nframes}, duration={duration:.3f}s")
        # check sidecar sha1
        sidecar = wav_path.with_suffix('.sha1')
        if sidecar.exists():
            computed = sha1_of_file(str(wav_path.with_suffix('.raw')) if wav_path.with_suffix('.raw').exists() else str(wav_path))
            # This script assumes .raw sidecar exists when used originally; fallback to wav
            expected = sidecar.read_text().strip()
            if computed == expected:
                print("  SHA1 OK")
            else:
                print(f"  SHA1 MISMATCH (expected {expected}, computed {computed})")
                ok = False
        else:
            print("  no .sha1 sidecar")
        return ok
    except Exception as e:
        print(f"  error validating: {e}")
        return False

def main(directory=DEFAULT_DIR):
    p = Path(directory)
    if not p.exists():
        print(f"Directory {directory} not found")
        return
    wavs = sorted([x for x in p.iterdir() if x.suffix.lower() == '.wav'])
    for w in wavs:
        ok = validate_file(w)
        print("  OK" if ok else "  FAILED")
    print("done")

if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=DEFAULT_DIR)
    args = ap.parse_args()
    main(args.dir)
