// server.js
// Speechster-H2 BLE client (Node.js + noble)
// Streams raw PCM16 frames into real-time WAV output

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
let leftover = Buffer.alloc(0);

// === WAV setup ===
const WAV_FILE = 'stream.wav';
const SAMPLE_RATE = 16000;
let wavFd = fs.openSync(WAV_FILE, 'w');
let totalSamples = 0;

writeWavHeader(wavFd);

// Write placeholder WAV header
function writeWavHeader(fd) {
  const header = Buffer.alloc(44);
  header.write('RIFF', 0);
  header.writeUInt32LE(36, 4); // placeholder
  header.write('WAVE', 8);
  header.write('fmt ', 12);
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20); // PCM
  header.writeUInt16LE(1, 22); // mono
  header.writeUInt32LE(SAMPLE_RATE, 24);
  header.writeUInt32LE(SAMPLE_RATE * 2, 28);
  header.writeUInt16LE(2, 32);
  header.writeUInt16LE(16, 34);
  header.write('data', 36);
  header.writeUInt32LE(0, 40);
  fs.writeSync(fd, header, 0, 44, 0);
}

// Update WAV header at exit
function finalizeWav(fd) {
  const dataSize = totalSamples * 2;
  const fileSize = 36 + dataSize;
  const buf = Buffer.alloc(8);
  buf.writeUInt32LE(fileSize, 0);
  fs.writeSync(fd, buf, 0, 4, 4); // RIFF size
  buf.writeUInt32LE(dataSize, 0);
  fs.writeSync(fd, buf, 0, 4, 40); // data size
}

// Helper to extract BLE frame
function extractFrame(buf) {
  if (buf.length < 8) return { frame: null, remaining: buf };
  const type = buf[0],
        flags = buf[1],
        seq = buf.readUInt16LE(2),
        ts = buf.readUInt32LE(4);
  const payload = buf.slice(8);
  return { frame: { type, flags, seq, ts, payload }, remaining: Buffer.alloc(0) };
}

// === BLE scanning ===
console.log('Scanning for Speechster-H2...');

noble.on('stateChange', state => {
  if (state === 'poweredOn') noble.startScanning([], false);
  else noble.stopScanning();
});

noble.on('discover', async peripheral => {
  const name = peripheral.advertisement?.localName || 'Unnamed';
  if (!name.toLowerCase().includes('speechster')) return;
  console.log(`Found device: ${name} (${peripheral.id})`);
  noble.stopScanning();
  await connectDevice(peripheral);
});

async function connectDevice(peripheral) {
  peripheral.connect(err => {
    if (err) return console.error('Connection error:', err);
    console.log(`Connected to ${peripheral.advertisement.localName}`);

    peripheral.discoverAllServicesAndCharacteristics((err, _services, chars) => {
      if (err) return console.error('Discovery error:', err);

      controlChar = chars.find(c =>
        c.properties.includes('write') || c.properties.includes('writeWithoutResponse')
      );
      dataChar = chars.find(c => c.properties.includes('notify'));

      if (dataChar) {
        console.log(`\nSubscribed to stream characteristic: ${dataChar.uuid}`);
        dataChar.subscribe(err => {
          if (err) console.error('Subscribe error:', err);
        });

        let lastSeq = -1;
        dataChar.on('data', buf => {
          leftover = Buffer.concat([leftover, buf]);
          while (leftover.length >= 8) {
            const { frame, remaining } = extractFrame(leftover);
            if (!frame) break;

            // LOGS:
            console.log(
              `Frame seq=${frame.seq} ts=${frame.ts} len=${frame.payload.length} bytes`
            );

            if (lastSeq >= 0 && frame.seq !== (lastSeq + 1) % 0x10000) {
              console.warn(`⚠️ Frame loss? expected ${ (lastSeq + 1) % 0x10000 }, got ${frame.seq}`);
            }
            lastSeq = frame.seq;

            // Write raw PCM16 payload directly
            fs.writeSync(wavFd, frame.payload, 0, frame.payload.length, 44 + totalSamples * 2);
            totalSamples += frame.payload.length / 2;
            leftover = remaining;
          }
        });

      }

      if (controlChar) {
        console.log('\nReady! Type JSON control commands (e.g. {"mic":true}):');
        rl.prompt();

        rl.on('line', line => {
          const t = line.trim();
          if (!t) return rl.prompt();
          try {
            controlChar.write(Buffer.from(t), false, err => {
              if (err) console.error('Write error:', err);
              else console.log(`Sent: ${t}`);
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

// Clean exit
process.on('SIGINT', () => {
  finalizeWav(wavFd);
  console.log('\nWAV file finalized. Exiting.');
  process.exit();
});
