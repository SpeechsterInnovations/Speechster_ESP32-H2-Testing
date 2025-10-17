// ─────────────────────────────────────────────────────────────
// Speechster-H2 BLE PCM Stream Logger
// Version: 1.0 (Stable PCM Baseline)
// Date: 2025-10-17
// 
// Description:
// Connects to Speechster-H2 over BLE (NimBLE GATT),
// receives PCM16 audio stream notifications,
// verifies continuity (seq/loss check),
// and saves to valid WAV file (stream.wav).
//
// Usage:
//   1. Run:  node server.js
//   2. Type control JSON (e.g. {"mic":true})
//   3. Stop with Ctrl+C — WAV auto-finalizes
//
// Notes:
//   - Tested with ESP-IDF v5.5.1 GATT stream server
//   - Sample rate: 48 kHz mono PCM16
//   - Writes one WAV per session
// ─────────────────────────────────────────────────────────────


import noble from '@abandonware/noble';
import readline from 'readline';
import fs from 'fs';
import process from 'process';

const rl = readline.createInterface({
  input: process.stdin,
  output: process.stdout,
  prompt: '> '
});

let controlChar = null;
let dataChar = null;

// Store pending partial fragments (for ADPCM or split notifications)

const WAV_FILE = 'stream.wav';
const SAMPLE_RATE = 48000; // matches ESP32 I2S
let wavFd = fs.openSync(WAV_FILE, 'w');

let lastSeq = -1;
let totalFrames = 0;
let lostFrames = 0;
let totalSamples = 0;
let pending = new Map(); // holds partial frame chunks


// ──────────────────────────── WAV HEADER ────────────────────────────
writeWavHeader(wavFd);
function writeWavHeader(fd) {
  const header = Buffer.alloc(44);
  header.write('RIFF', 0);
  header.writeUInt32LE(36, 4);
  header.write('WAVE', 8);
  header.write('fmt ', 12);
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20); // PCM format
  header.writeUInt16LE(1, 22); // mono
  header.writeUInt32LE(SAMPLE_RATE, 24);
  header.writeUInt32LE(SAMPLE_RATE * 2, 28);
  header.writeUInt16LE(2, 32);
  header.writeUInt16LE(16, 34);
  header.write('data', 36);
  header.writeUInt32LE(0, 40);
  fs.writeSync(fd, header, 0, 44, 0);
}

function finalizeWav(fd) {
  const dataSize = totalSamples * 2;
  const fileSize = 36 + dataSize;
  const buf = Buffer.alloc(4);
  buf.writeUInt32LE(fileSize, 0);
  fs.writeSync(fd, buf, 0, 4, 4); // total size
  buf.writeUInt32LE(dataSize, 0);
  fs.writeSync(fd, buf, 0, 4, 40); // data chunk size
}

// ──────────────────────────── BLE STREAM LOGIC ────────────────────────────
function parseHeader(buf) {
  if (buf.length < 8) return null;
  return {
    type: buf[0],
    flags: buf[1],
    seq: buf.readUInt16LE(2),
    ts: buf.readUInt32LE(4),
  };
}

console.log('🔍 Scanning for Speechster-H2...');

noble.on('stateChange', (state) => {
  if (state === 'poweredOn') noble.startScanning([], false);
  else noble.stopScanning();
});

noble.on('discover', async (peripheral) => {
  const name = peripheral.advertisement?.localName || 'Unnamed';
  if (!name.toLowerCase().includes('speechster')) return;
  console.log(`✅ Found device: ${name} (${peripheral.id})`);
  noble.stopScanning();
  await connectDevice(peripheral);
});

async function connectDevice(peripheral) {
  peripheral.connect((err) => {
    if (err) return console.error('Connection error:', err);
    console.log(`🔗 Connected to ${peripheral.advertisement.localName}`);

    peripheral.discoverAllServicesAndCharacteristics((err, _services, chars) => {
      if (err) return console.error('Discovery error:', err);

      controlChar = chars.find(
        (c) =>
          c.properties.includes('write') ||
          c.properties.includes('writeWithoutResponse')
      );
      dataChar = chars.find((c) => c.properties.includes('notify'));

      if (dataChar) {
        console.log(`📡 Subscribed to stream characteristic: ${dataChar.uuid}`);
        pending.clear();
        dataChar.subscribe((err) => {
          if (err) console.error('Subscribe error:', err);
        });

        dataChar.on('data', (data) => {
          if (data.length < 8) {
            console.warn('⚠️ Received malformed frame (<8 bytes)');
            return;
          }

          const header = parseHeader(data);
          const payload = data.slice(8);
          const cont = (header.flags & 0x02) !== 0;
          const compressed = (header.flags & 0x01) !== 0;

          // --- Aggregation-aware logic ---
          const key = header.seq;

          // Start or continue accumulating a frame
          if (!pending.has(key)) {
            pending.set(key, { chunks: [], total: 0, ts: header.ts });
          }

          const frame = pending.get(key);
          frame.chunks.push(payload);
          frame.total += payload.length;

          // (optional) temporary debug log per chunk
          // console.log(`🧩 seq=${header.seq} cont=${cont ? 'yes':'no'} len=${payload.length}`);

          // When cont=0 => frame finished
          if (!cont) {
            const fullPayload = Buffer.concat(frame.chunks);
            pending.delete(key);

            // Verify continuity once per frame
            if (!compressed) {
              if (lastSeq >= 0) {
                const expected = (lastSeq + 1) % 0x10000;
                if (header.seq !== expected) {
                  console.warn(`⚠️ Sequence jump: expected ${expected}, got ${header.seq}`);
                  lostFrames++;
                }
              }
              lastSeq = header.seq;
              totalFrames++;
            }

            // Log one line per full frame
            console.log(
              `✅ Frame ${header.seq} complete: ${frame.chunks.length} chunks, ${frame.total} bytes ` +
              `(ts=${header.ts}, enc=${compressed ? 'ADPCM' : 'PCM'})`
            );

            // Write PCM data to WAV
            if (!compressed) {
              fs.writeSync(wavFd, fullPayload, 0, fullPayload.length, 44 + totalSamples * 2);
              totalSamples += fullPayload.length / 2;
            }

            // Print running stats occasionally
            if (totalFrames % 128 === 0) {
              console.log(
                `📈 Stats: frames=${totalFrames} lost=${lostFrames} lossRate=${(
                  (lostFrames / totalFrames) * 100
                ).toFixed(2)}%`
              );
            }
          }

        });
      }

      if (controlChar) {
        console.log('\n🎛️ Ready! Type JSON control commands (e.g. {"mic":true}):');
        rl.prompt();

        rl.on('line', (line) => {
          const t = line.trim();
          if (!t) return rl.prompt();
          try {
            controlChar.write(Buffer.from(t), false, (err) => {
              if (err) console.error('Write error:', err);
              else console.log(`➡️ Sent: ${t}`);
            });
          } catch (err) {
            console.error('Invalid input:', err);
          }
          rl.prompt();
        });
      }
    });
  });
}

// ──────────────────────────── CLEAN EXIT ────────────────────────────
process.on('SIGINT', () => {
  try {
    finalizeWav(wavFd);
  } catch (e) {
    console.error('Error finalizing WAV:', e);
  } finally {
    process.exit();
  }
});
