const express = require('express');
const http = require('http');
const WebSocket = require('ws');
const noble = require('@abandonware/noble');

const app = express();
const server = http.createServer(app);
const wss = new WebSocket.Server({ server });

app.use(express.static(__dirname + '/public'));

let devices = {};
let connectedPeripheral = null;
let writeCharacteristic = null;
let notifyCharacteristic = null;

// Send list of available devices
function simplifyDevices() {
  return Object.values(devices)
    .filter(d => d.advertisement.localName && d.advertisement.localName.includes('Speechster-H2'))
    .map(d => ({
      id: d.id,
      name: d.advertisement.localName,
      address: d.address,
      rssi: d.rssi
    }));
}

wss.on('connection', ws => {
  console.log('WebSocket client connected');
  ws.send(JSON.stringify({ type: 'deviceList', devices: simplifyDevices() }));

  ws.on('message', async msg => {
    const data = JSON.parse(msg);

    if (data.type === 'connect') {
      const device = devices[data.deviceId];
      if (!device) return;
      console.log(`Connecting to ${device.advertisement.localName}...`);
      connectedPeripheral = device;
      try {
        await connectPeripheral(device, ws);
      } catch (e) {
        console.error('Connection error:', e);
      }
    }

    if (data.type === 'send') {
      if (writeCharacteristic) {
        const buf = Buffer.from(JSON.stringify(data.payload));
        writeCharacteristic.write(buf, false, err => {
          if (err) console.error('Write error:', err);
        });
      }
    }
  });
});

// BLE Connection Logic
async function connectPeripheral(peripheral, ws) {
  peripheral.removeAllListeners('disconnect');
  peripheral.on('disconnect', () => {
    console.log('Disconnected from device');
    ws.send(JSON.stringify({ type: 'disconnected' }));
  });

  await peripheral.connectAsync();
  console.log('Connected! Discovering services...');
  const { characteristics } = await peripheral.discoverSomeServicesAndCharacteristicsAsync([], []);

  ws.send(JSON.stringify({
    type: 'services',
    services: characteristics.map(c => ({ uuid: c.uuid, props: c.properties }))
  }));

  writeCharacteristic = characteristics.find(c => c.properties.includes('write') || c.properties.includes('writeWithoutResponse'));
  notifyCharacteristic = characteristics.find(c => c.properties.includes('notify'));

  if (notifyCharacteristic) {
    await notifyCharacteristic.subscribeAsync();
    notifyCharacteristic.on('data', data => {
      ws.send(JSON.stringify({ type: 'notification', payload: data.toString('hex') }));
    });
  }
}

// BLE Scanning
noble.on('stateChange', state => {
  if (state === 'poweredOn') {
    console.log('Starting BLE scan...');
    noble.startScanning([], true);
  } else {
    noble.stopScanning();
  }
});

noble.on('discover', device => {
  if (device.advertisement.localName && device.advertisement.localName.includes('Speechster-H2')) {
    devices[device.id] = device;
    wss.clients.forEach(client => {
      if (client.readyState === WebSocket.OPEN) {
        client.send(JSON.stringify({ type: 'deviceList', devices: simplifyDevices() }));
      }
    });
  }
});

server.listen(8080, () => console.log('Server running at http://localhost:8080'));

