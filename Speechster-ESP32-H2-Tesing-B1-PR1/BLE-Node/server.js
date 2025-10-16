// server.js
// Speechster-H2 BLE PCM client (Node.js + noble)
// Receives chunked PCM notifications, reconstructs frames, writes to WAV or plays live

import noble from '@abandonware/noble';
import fs from 'fs';
import process from 'process';

const SERVICE_UUID = '0000feed00001000800000805f9b34fb';
const STREAM_UUID  = '0000feed00020000800000805f9b34fb';
const CONTROL_UUID = '0000feed00010000800000805f9b34fb';

const OUTPUT_FILE = 'mic_output.wav';  // or 'mic_output.raw'
const writeStream = fs.createWriteStream(OUTPUT_FILE);

let controlChar = null;
let dataChar = null;

/* --- Frame reassembly state --- */
let frameBuffer = Buffer.alloc(0);
let expectedSeq = null;
let incompleteFrame = null;
let lastChunkTime = 0;

function parseHeader(buf) {
  return {
    type: buf[0],
    flags: buf[1],
    seq: buf.readUInt16LE(2),
    ts: buf.readUInt32LE(4)
  };
}

/* --- WAV header writer (16-bit PCM) --- */
function writeWavHeader(stream, sampleRate = 48000, numSamples = 0) {
  const blockAlign = 2;
  const byteRate = sampleRate * blockAlign;
  const header = Buffer.alloc(44);
  header.write('RIFF', 0);
  header.writeUInt32LE(36 + numSamples * 2, 4);
  header.write('WAVEfmt ', 8);
  header.writeUInt32LE(16, 16); // PCM chunk size
  header.writeUInt16LE(1, 20);  // PCM format
  header.writeUInt16LE(1, 22);  // mono
  header.writeUInt32LE(sampleRate, 24);
  header.writeUInt32LE(byteRate, 28);
  header.writeUInt16LE(blockAlign, 32);
  header.writeUInt16LE(16, 34);
  header.write('data', 36);
  header.writeUInt32LE(numSamples * 2, 40);
  stream.write(header);
}

/* --- Stream reassembly --- */
function handleNotify(data) {
  const now = Date.now();
  if (now - lastChunkTime > 1000) {
    // reset if gap too long
    incompleteFrame = null;
    frameBuffer = Buffer.alloc(0);
  }
  lastChunkTime = now;

  if (data.length < 8) return;
  const hdr = parseHeader(data);
  const continuation = (hdr.flags & 0x02) !== 0;

  if (!incompleteFrame) {
    // start of new frame
    incompleteFrame = {
      seq: hdr.seq,
      ts: hdr.ts,
      type: hdr.type,
      flags: hdr.flags,
      payloads: []
    };
  }

  // Append payload
  incompleteFrame.payloads.push(data.slice(8));

  if (!continuation) {
    // final chunk → assemble
    const full = Buffer.concat(incompleteFrame.payloads);
    const pcm = full;

    // sanity check: small frames only (~960 samples)
    if (pcm.length > 2000) {
      console.log(`[WARN] Oversized frame seq=${hdr.seq} (${pcm.length} bytes)`);
    }

    // detect seq jumps
    if (expectedSeq !== null && hdr.seq !== (expectedSeq & 0xffff)) {
      console.log(`[DROP] Seq jump: expected=${expectedSeq} got=${hdr.seq}`);
    }
    expectedSeq = (hdr.seq + 1) & 0xffff;

    // Write PCM data
    writeStream.write(pcm);

    incompleteFrame = null;
    frameBuffer = Buffer.alloc(0);
  }
}

/* --- BLE Setup --- */
noble.on('stateChange', async (state) => {
  console.log('Noble state:', state);
  if (state === 'poweredOn') {
    try {
      console.log('Starting scan (no filters)...');
      await noble.startScanningAsync([], false);
      console.log('Scan started.');
    } catch (e) {
      console.error('Scan start failed:', e);
    }
  } else {
    noble.stopScanning();
  }
});
noble.on('discover', async (peripheral) => {
  console.log(`Found ${peripheral.advertisement.localName || peripheral.id}`);
  await noble.stopScanningAsync();
  // Wait 1–2 seconds to let BlueZ fully stop scanning
  await new Promise(r => setTimeout(r, 2000));
  await peripheral.connectAsync();
  console.log("Waiting for UUIDs")
  const { services, characteristics } = await peripheral.discoverAllServicesAndCharacteristicsAsync();

  for (const c of characteristics) {
    if (c.uuid === CONTROL_UUID.replace(/-/g, '')) controlChar = c;
    if (c.uuid === STREAM_UUID.replace(/-/g, '')) dataChar = c;
  }

  if (!dataChar) {
    console.error('Stream characteristic not found.');
    process.exit(1);
  }

  console.log('Connected. Subscribing to stream...');
  dataChar.on('data', handleNotify);
  await dataChar.subscribeAsync();

  console.log('Enabling mic...');
  const cmd = JSON.stringify({ mic: true });
  await controlChar.writeAsync(Buffer.from(cmd), false);

  writeWavHeader(writeStream, 48000);
  console.log('Recording started.');
});

/* --- Exit handling --- */
process.on('SIGINT', () => {
  console.log('Stopping...');
  writeStream.end();
  process.exit();
});
