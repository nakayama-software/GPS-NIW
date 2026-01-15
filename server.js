// server.js (UPDATED)
// Supports new firmware: {t, ok, x, y, n, ageMax, u}
// Supports old firmware: {timestamp, success, position:{x,y}, beaconUsed}
// Supports: {type:"config"}, {type:"ready"}, {type:"debug"}

const express = require('express');
const http = require('http');
const socketIO = require('socket.io');
const { SerialPort } = require('serialport');
const { ReadlineParser } = require('@serialport/parser-readline');

const app = express();
const server = http.createServer(app);
const io = socketIO(server);

// ===== Configuration =====
const PORT = 3000;
const SERIAL_PORT = '/dev/cu.usbserial-110';
const BAUD_RATE = 115200;

// ===== Serve static files =====
app.use(express.static('public'));

// ===== Helpers =====
function normalizePayload(raw) {
  if (raw && typeof raw === 'object') {
    if (raw.type === 'config' || raw.type === 'ready' || raw.type === 'debug') return { ...raw };

    if (typeof raw.ok === 'boolean') {
      return {
        t: raw.t ?? raw.timestamp ?? Date.now(),
        ok: raw.ok,
        x: raw.x,
        y: raw.y,
        n: raw.n,
        ageMax: raw.ageMax,
        u: raw.u,
        raw
      };
    }

    if (typeof raw.success === 'boolean') {
      return {
        t: raw.timestamp ?? raw.t ?? Date.now(),
        ok: raw.success,
        x: raw.position?.x,
        y: raw.position?.y,
        n: raw.beaconUsed,
        raw
      };
    }
  }
  return null;
}

// ===== Serial Port Setup =====
let serialPort;
let parser;

function initSerialPort() {
  try {
    serialPort = new SerialPort({ path: SERIAL_PORT, baudRate: BAUD_RATE });
    parser = serialPort.pipe(new ReadlineParser({ delimiter: '\n' }));

    serialPort.on('open', () => console.log(`✓ Serial port ${SERIAL_PORT} opened at ${BAUD_RATE} baud`));
    serialPort.on('error', (err) => console.error('Serial port error:', err.message));

    parser.on('data', (line) => {
      const trimmed = String(line).trim();
      if (!trimmed) return;

      try {
        const raw = JSON.parse(trimmed);
        const data = normalizePayload(raw);

        if (data?.type === 'config') console.log('Config received:', data);
        else if (data?.type === 'ready') console.log('✓ ESP32 ready');
        else if (data?.type === 'debug') {/* optional */}
        else if (data && typeof data.ok === 'boolean') {
          if (data.ok) {
            console.log(`Position: (${data.x}, ${data.y}) - Beacons: ${data.n}` +
              (data.ageMax != null ? ` - ageMax:${data.ageMax}ms` : '') +
              (data.u != null ? ` - u:${data.u}` : '')
            );
          } else {
            console.log(`Position failed - Beacons: ${data.n ?? 'N/A'}`);
          }
        }

        io.emit('position-update', raw);
        if (data) io.emit('position-update-normalized', data);
      } catch (e) {
        console.log('Non-JSON:', trimmed);
      }
    });

  } catch (err) {
    console.error('Failed to open serial port:', err.message);
    console.log('\nAvailable ports:');
    SerialPort.list().then(ports => ports.forEach(p => console.log(`  - ${p.path}`)));
  }
}

// ===== WebSocket Connection =====
io.on('connection', (socket) => {
  console.log('✓ Client connected');
  socket.on('disconnect', () => console.log('✗ Client disconnected'));
});

// ===== List available serial ports =====
app.get('/api/ports', async (req, res) => {
  try {
    const ports = await SerialPort.list();
    res.json(ports);
  } catch (err) {
    res.status(500).json({ error: err.message });
  }
});

// ===== Start server =====
server.listen(PORT, () => {
  console.log('\n================================');
  console.log('  BLE TRACKER SERVER');
  console.log('================================');
  console.log(`Server running on http://localhost:${PORT}`);
  console.log(`Serial port: ${SERIAL_PORT}`);
  console.log('================================\n');
  initSerialPort();
});



