const SERVICE_UUID       = '0000feed-0000-1000-8000-00805f9b34fb';
const CONTROL_CHAR_UUID  = 'feed0001-0010-0000-8000-00805f9b34fb';
const DATA_CHAR_UUID     = 'feed0002-0010-0000-8000-00805f9b34fb';


let device = null, server = null, service = null, controlChar = null, dataChar = null;

const logBox = document.getElementById('logBox');
const connectBtn = document.getElementById('connectBtn');
const disconnectBtn = document.getElementById('disconnectBtn');
const clearBtn = document.getElementById('clearBtn');
const sendBtn = document.getElementById('sendBtn');
const commandInput = document.getElementById('commandInput');

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

async function connectBLE() {
  try {
    clearLog();
    log('Requesting BLE device...');
    device = await navigator.bluetooth.requestDevice({
      filters: [{ name: 'Speechster-H2' }],
      optionalServices: [SERVICE_UUID]
    });

    log('Connecting to GATT server...');
    server = await device.gatt.connect();
    device.addEventListener('gattserverdisconnected', onDisconnected);

    log('Getting primary service...');
    service = await server.getPrimaryService(SERVICE_UUID);

    log('Getting characteristics...');
    controlChar = await service.getCharacteristic(CONTROL_CHAR_UUID);
    dataChar = await service.getCharacteristic(DATA_CHAR_UUID);

    log('Enabling notifications...');
    await dataChar.startNotifications();
    dataChar.addEventListener('characteristicvaluechanged', e => {
      const dv = e.target.value;
      const type = dv.getUint8(0);
      const flags = dv.getUint8(1);
      const seq = dv.getUint16(2, true);
      const ts = dv.getUint32(4, true);
      const len = dv.byteLength;

      // Optional: distinguish fake vs real
      let label = "[📥 recv]";
      if (len === 12 && type === 0x01 && flags === 0x00) label += " (FAKE)";
      console.log(`${label} len=${len} type=0x${type.toString(16)} seq=${seq} ts=${ts} flags=${flags}`);
    });

    log('✅ Connected and ready!');
  } catch (error) {
    log(`❌ Connection error: ${error}`, 'error');
  }
}

function handleDataNotify(event) {
  const value = event.target.value;
  const bytes = new Uint8Array(value.buffer);
  log(`[📥 recv] ${bytes.length} bytes`, 'recv');
  console.log('[Raw]', bytes);
}

async function sendCommand() {
  if (!controlChar) return log('❌ Not connected', 'error');
  const txt = commandInput.value.trim();
  if (!txt) return;

  try {
    const encoder = new TextEncoder();
    const data = encoder.encode(txt);
    await controlChar.writeValue(data);

    // Attempt to parse as JSON for pretty print
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

function disconnectBLE() {
  if (device?.gatt?.connected) {
    log('Disconnecting...');
    device.gatt.disconnect();
  }
}

function onDisconnected() {
  log('🔌 Disconnected.');
  device = controlChar = dataChar = null;
}

connectBtn.addEventListener('click', connectBLE);
disconnectBtn.addEventListener('click', disconnectBLE);
clearBtn.addEventListener('click', clearLog);
sendBtn.addEventListener('click', sendCommand);
