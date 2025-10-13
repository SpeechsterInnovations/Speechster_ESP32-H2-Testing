// ------------------- Config -------------------
const SERVICE_UUID       = '0000feed-0000-1000-8000-00805f9b34fb';
const CONTROL_CHAR_UUID  = 'feed0001-0010-0000-8000-00805f9b34fb';
const DATA_CHAR_UUID     = 'feed0002-0010-0000-8000-00805f9b34fb';

// ------------------- State -------------------
let device = null, server = null, service = null, controlChar = null, dataChar = null;
let frameBuffer = [];       // buffer for chunked frames
let expectedSeq = null;     // current expected sequence number for chunks

// ------------------- Logging -------------------
const logBox = document.getElementById('logBox');
function log(msg, type = 'info') {
  const now = new Date().toLocaleTimeString();
  const div = document.createElement('div');
  div.innerHTML = `<span class="timestamp">[${now}]</span> ${msg}`;
  div.classList.add(type);
  logBox.appendChild(div);
  logBox.scrollTop = logBox.scrollHeight;
  console.log(`${now} ${msg}`);
}

function clearLog() {
  logBox.innerHTML = '';
  console.clear();
}

function logFunc(name) {
  console.log(`🔹 Function fired: ${name}`);
  log(`🔹 Function fired: ${name}`, 'info');
}


// ------------------- ADPCM decoder -------------------
class IMAADPCMDecoder {
  constructor() {
    this.predictor = 0;
    this.index = 0;
    this.stepTable = [
      7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
      34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
      157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
      598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552
    ];
    this.indexTable = [
      -1, -1, -1, -1, 2, 4, 6, 8,
      -1, -1, -1, -1, 2, 4, 6, 8
    ];
  }

  decodeNibble(nibble) {
    let step = this.stepTable[this.index];
    let diff = step >> 3;
    if (nibble & 1) diff += step >> 2;
    if (nibble & 2) diff += step >> 1;
    if (nibble & 4) diff += step;
    if (nibble & 8) diff = -diff;

    this.predictor += diff;
    if (this.predictor > 32767) this.predictor = 32767;
    else if (this.predictor < -32768) this.predictor = -32768;

    this.index += this.indexTable[nibble & 0x0F];
    if (this.index < 0) this.index = 0;
    else if (this.index > 88) this.index = 88;

    return this.predictor;
  }

  decode(buffer) {
    const out = new Int16Array(buffer.length * 2);
    let outIdx = 0;
    for (let byte of buffer) {
      out[outIdx++] = this.decodeNibble(byte & 0x0F);
      out[outIdx++] = this.decodeNibble(byte >> 4);
    }
    return out;
  }
}

const adpcmDecoder = new IMAADPCMDecoder();

// ------------------- BLE notification -------------------
function handleDataNotify(event) {
  const dv = event.target.value;
  const bytes = new Uint8Array(dv.buffer);

  if (bytes.length < 8) {
    console.warn('[⚠️ notify] Frame too short, ignoring:', bytes);
    return;
  }

  const type = bytes[0];
  const flags = bytes[1];
  const seq = bytes[2] | (bytes[3] << 8);
  const ts = bytes[4] | (bytes[5] << 8) | (bytes[6] << 16) | (bytes[7] << 24);
  const payload = bytes.slice(8);

  console.log(`[📦 notify] seq=${seq} ts=${ts} type=${type} flags=${flags} bytes=${payload.length}`);

  // Sequence check
  if (expectedSeq !== null && seq !== expectedSeq) {
    console.warn(`[⚠️ sequence] Expected ${expectedSeq} but got ${seq}`);
  }

  // Append payload
  frameBuffer.push(...payload);

  const isADPCM = (flags & 0x01) !== 0;
  const isLastChunk = (flags & 0x02) === 0;

  if (isLastChunk) {
    const frameData = new Uint8Array(frameBuffer);
    if (type === 0x01) { // audio
      let pcm16;
      try {
        pcm16 = isADPCM ? adpcmDecoder.decode(frameData) : new Int16Array(frameData.buffer);
      } catch (err) {
        console.error('[❌ decode] Failed to decode audio chunk:', err);
      }

      console.log(`[🎵 audio] ${isADPCM ? 'ADPCM' : 'PCM16'} decoded ${pcm16?.length || 0} samples seq=${seq} ts=${ts}`);

      // Optional: store for AI backend
      if (!window.pcmBuffer) window.pcmBuffer = [];
      if (pcm16) window.pcmBuffer.push(pcm16);
    }

    // Reset for next frame
    frameBuffer = [];
    expectedSeq = null;
  } else {
    expectedSeq = seq + 1;
  }
}


// ------------------- BLE connect -------------------
async function connectBLE() {
  logFunc('connectBLE');
  clearLog();

  try {
    log('Requesting BLE device...');
    device = await navigator.bluetooth.requestDevice({
      filters: [{ name: 'Speechster-H2' }],
      optionalServices: [SERVICE_UUID]
    });
    log('Device selected: ' + device.name);

    log('Connecting to GATT server...');
    server = await device.gatt.connect();
    log('GATT server connected');
    device.addEventListener('gattserverdisconnected', onDisconnected);

    log('Getting all primary services...');
    const services = await server.getPrimaryServices();
    log(`Found ${services.length} services: ${services.map(s => s.uuid).join(', ')}`);

    // Pick the correct service
    service = services.find(s => s.uuid.toLowerCase() === SERVICE_UUID.toLowerCase());
    if (!service) {
      log('Service not found!', 'error');
      return;
    }
    log(`Using service: ${service.uuid}`);

    log('Getting characteristics for service...');
    const chars = await service.getCharacteristics();
    log(`Service has ${chars.length} characteristics: ${chars.map(c => c.uuid).join(', ')}`);

    // Pick by UUID
    controlChar = chars.find(c => c.uuid.toLowerCase() === CONTROL_CHAR_UUID.toLowerCase());
    dataChar    = chars.find(c => c.uuid.toLowerCase() === DATA_CHAR_UUID.toLowerCase());

    if (!controlChar || !dataChar) {
      log('Could not find control/data characteristics by UUID', 'error');
      return;
    }

    log(`Control char: ${controlChar.uuid}`);
    log(`Data char: ${dataChar.uuid}`);

    log('Enabling notifications on data characteristic...');
    await dataChar.startNotifications();
    dataChar.addEventListener('characteristicvaluechanged', handleDataNotify);
    log('Notifications enabled');

    log('✅ Connected and ready!');
  } catch (error) {
    log(`Connection error: ${error}`, 'error');
  }
}

// ------------------- Command send -------------------
async function sendCommand() {
  if (!controlChar) return log('Not connected', 'error');
  const txt = commandInput.value.trim();
  if (!txt) return;

  try {
    const encoder = new TextEncoder();
    const data = encoder.encode(txt);
    await controlChar.writeValue(data);

    try {
      const obj = JSON.parse(txt);
      log(`[📤 send] JSON: ${JSON.stringify(obj, null, 2)}`, 'send');
    } catch {
      log(`[📤 send] Raw: ${txt}`, 'send');
    }
  } catch (err) {
    log(`Write failed: ${err}`, 'error');
  }
}

// ------------------- Disconnect -------------------
function disconnectBLE() {
  if (device?.gatt?.connected) device.gatt.disconnect();
}

function onDisconnected() {
  log('Disconnected.');
  device = controlChar = dataChar = null;
}

// ------------------- Event Listeners -------------------
connectBtn.addEventListener('click', connectBLE);
disconnectBtn.addEventListener('click', disconnectBLE);
clearBtn.addEventListener('click', clearLog);
sendBtn.addEventListener('click', sendCommand);
