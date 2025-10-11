// server.js
// Speechster-H2 BLE client (Node.js + noble)
// Reconstructs PCM/ADPCM frames into real-time WAV output

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

// WAV setup
const WAV_FILE = 'stream.wav';
const SAMPLE_RATE = 16000;
let wavFd = fs.openSync(WAV_FILE, 'w');
let totalSamples = 0;

// Write initial WAV header placeholder
function writeWavHeader(fd) {
  const header = Buffer.alloc(44);
  header.write('RIFF', 0);
  header.writeUInt32LE(36, 4); // placeholder for file size
  header.write('WAVE', 8);
  header.write('fmt ', 12);
  header.writeUInt32LE(16, 16); // PCM chunk size
  header.writeUInt16LE(1, 20);  // audio format PCM
  header.writeUInt16LE(1, 22);  // mono
  header.writeUInt32LE(SAMPLE_RATE, 24);
  header.writeUInt32LE(SAMPLE_RATE*2, 28); // byte rate
  header.writeUInt16LE(2, 32);  // block align
  header.writeUInt16LE(16, 34); // bits per sample
  header.write('data', 36);
  header.writeUInt32LE(0, 40);  // placeholder for data size
  fs.writeSync(fd, header, 0, 44, 0);
}

// Update WAV header at exit
function finalizeWav(fd) {
  fs.writeSync(fd, Buffer.from('RIFF'), 0, 4, 0);
  fs.writeSync(fd, Buffer.alloc(4, 0), 0, 4, 4); // file size = 36 + data size
  const dataSize = totalSamples * 2;
  fs.writeSync(fd, Buffer.from('WAVE'), 0, 4, 8);
  fs.writeSync(fd, Buffer.from('fmt '), 0, 4, 12);
  const fmtChunk = Buffer.alloc(16);
  fmtChunk.writeUInt16LE(1, 0);  // PCM
  fmtChunk.writeUInt16LE(1, 2);  // mono
  fmtChunk.writeUInt32LE(SAMPLE_RATE, 4);
  fmtChunk.writeUInt32LE(SAMPLE_RATE*2, 8); // byte rate
  fmtChunk.writeUInt16LE(2, 12);
  fmtChunk.writeUInt16LE(16, 14);
  fs.writeSync(fd, fmtChunk, 0, 16, 16);
  fs.writeSync(fd, Buffer.from('data'), 0, 4, 36);
  fs.writeSync(fd, Buffer.alloc(4), 0, 4, 40);
  fs.writeSync(fd, Buffer.alloc(0), 0, 0, 44 + dataSize); // ensure file size
}

// --- IMA ADPCM decoder ---
class IMAADPCMDecoder {
  constructor() { this.index = 0; this.predicted = 0; }
  static stepTable = [ /* same table as before */ 
     7, 8, 9,10,11,12,13,14,16,17,19,21,23,25,28,31,
     34,37,41,45,50,55,60,66,73,80,88,97,107,118,130,143,
    157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,
    724,796,876,963,1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,
    3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,10442,11487,12635,13899,
    15289,16818,18500,20350,22385,24623,27086,29794,32767
  ];
  static indexTable = [-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8];
  decodeNibble(n) { /* same decoding logic as before */
    let step = IMAADPCMDecoder.stepTable[this.index];
    let diff = step >> 3;
    if(n&1) diff += step>>2;
    if(n&2) diff += step>>1;
    if(n&4) diff += step;
    if(n&8) diff=-diff;
    this.predicted+=diff;
    if(this.predicted>32767)this.predicted=32767;
    if(this.predicted<-32768)this.predicted=-32768;
    this.index+=IMAADPCMDecoder.indexTable[n&0x0F];
    if(this.index<0)this.index=0;
    if(this.index>88)this.index=88;
    return this.predicted;
  }
  decodeBuffer(buf){
    const pcm=new Int16Array(buf.length*2);
    for(let i=0;i<buf.length;i++){
      pcm[i*2]=this.decodeNibble(buf[i]&0x0F);
      pcm[i*2+1]=this.decodeNibble(buf[i]>>4);
    }
    return Buffer.from(pcm.buffer);
  }
}

const decoder=new IMAADPCMDecoder();

function fmtHex(buf,count=12){return Array.from(buf.slice(0,count)).map(b=>b.toString(16).padStart(2,'0')).join(' ');}
function extractFrame(buf){if(buf.length<8)return{frame:null,remaining:buf};const type=buf[0],flags=buf[1],seq=buf[2]|(buf[3]<<8),ts=buf[4]|(buf[5]<<8)|(buf[6]<<16)|(buf[7]<<24),payload=buf.slice(8);return{frame:{type,flags,seq,ts,payload},remaining:Buffer.alloc(0)};}

writeWavHeader(wavFd);

// === BLE scanning ===
console.log('Scanning for BLE devices...');
noble.on('stateChange',state=>{if(state==='poweredOn')noble.startScanning([],false);else noble.stopScanning();});
noble.on('discover',async peripheral=>{
  const name=peripheral.advertisement?.localName||'Unnamed';
  if(!name.toLowerCase().includes('speechster'))return;
  console.log(`Found device: ${name} (${peripheral.id})`);
  noble.stopScanning();
  await connectDevice(peripheral);
});

async function connectDevice(peripheral){
  return new Promise(resolve=>{
    peripheral.connect(err=>{
      if(err)return console.error('Connection error:',err);
      console.log(`Connected to ${peripheral.advertisement.localName}`);
      peripheral.discoverAllServicesAndCharacteristics((err,services,chars)=>{
        if(err)return console.error('Discovery error:',err);
        controlChar=chars.find(c=>c.properties.includes('write')||c.properties.includes('writeWithoutResponse'));
        dataChar=chars.find(c=>c.properties.includes('notify'));
        if(dataChar){
          console.log(`\nAuto-detected PCM/ADPCM stream on characteristic: ${dataChar.uuid}`);
          dataChar.subscribe(err=>{if(err)console.error('Subscribe error:',err);});
          let lastSeq=-1;
          dataChar.on('data',buf=>{
            leftover=Buffer.concat([leftover,buf]);
            while(leftover.length>=8){
              const{frame,remaining}=extractFrame(leftover);
              if(!frame)break;
              if(lastSeq>=0&&frame.seq!==(lastSeq+1)%0x10000)console.warn(`Frame loss? expected seq=${(lastSeq+1)%0x10000} got seq=${frame.seq}`);
              lastSeq=frame.seq;
              let audio=frame.payload;if(frame.flags&0x01)audio=decoder.decodeBuffer(audio);
              fs.writeSync(wavFd,audio,null,undefined,44+totalSamples*2);
              totalSamples+=audio.length/2;
              leftover=remaining;
            }
          });
        }
        if(controlChar){
          console.log('\nReady! Type JSON to send:');
          rl.prompt();
          rl.on('line',line=>{
            const t=line.trim();if(!t)return rl.prompt();
            try{controlChar.write(Buffer.from(t),false,err=>{if(err)console.error('Write error:',err);else console.log(`Sent: ${t}`);});}catch(err){console.error('Invalid input:',err);}
            rl.prompt();
          });
        }
        resolve();
      });
    });
  });
}

// Finalize WAV header on Ctrl+C
process.on('SIGINT',()=>{
  finalizeWav(wavFd);
  console.log('\nWAV file finalized. Exiting.');
  process.exit();
});
