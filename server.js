const express = require('express');
const http = require('http');
const path = require('path');
const socketIo = require('socket.io');
const multer = require('multer');
const { SerialPort } = require('serialport');
const { parseGpsFrame, formatSerialError } = require('./serialParser');

const app = express();
const server = http.createServer(app);
const io = socketIo(server);

const WEB_PORT = process.env.PORT || 3000;
const BAUD_RATE = 115200;

let currentLocation = null;
let coordinateHistory = [];
const MAX_HISTORY = 150;

let serialPort = null;
let serialPortPath = null;
let serialPortStatus = 'disconnected';
let serialPortError = null;
let availablePorts = [];

app.use(express.json());
app.use('/vendor', express.static(path.join(__dirname, 'node_modules')));
app.use(express.static(path.join(__dirname, 'public')));

function parseNumber(payload) {
  const n = parseFloat(payload);
  return Number.isFinite(n) ? n : null;
}

function parseBooleanValue(payload) {
  if (payload === undefined || payload === null) return false;
  const normalized = String(payload).trim().toLowerCase();
  if (!normalized) return false;
  if (['true', '1', 'yes', 'y', 'on'].includes(normalized)) return true;
  if (['false', '0', 'no', 'n', 'off'].includes(normalized)) return false;
  const parsedInt = Number.parseInt(normalized, 10);
  return !Number.isNaN(parsedInt) ? parsedInt !== 0 : false;
}

function emitLocationData(data) {
  const timestamp = data.timestamp || new Date().toISOString();
  const payload = {
    latitude: data.latitude,
    longitude: data.longitude,
    altitude: data.altitude,
    speed: data.speed,
    isBacklog: Boolean(data.isBacklog),
    timestamp
  };

  if (data.voltage !== undefined && data.voltage !== null) {
    payload.voltage = data.voltage;
  }

  if (data.current !== undefined && data.current !== null) {
    payload.current = data.current;
  }

  currentLocation = payload;

  coordinateHistory.push({ ...payload });
  if (coordinateHistory.length > MAX_HISTORY) coordinateHistory.shift();

  console.log('[SOCKET OUT] Emitting complete coordinate package to UI:', payload);
  io.emit('location_update', payload);
}

function broadcastSerialStatus(details) {
  io.emit('serial_status', {
    connected: serialPortStatus === 'connected',
    status: serialPortStatus,
    port: serialPortPath,
    error: serialPortError,
    ...details
  });
}

function setSerialStatus(status, error = null) {
  serialPortStatus = status;
  serialPortError = error;
  broadcastSerialStatus({});
}

async function refreshAvailablePorts() {
  try {
    const ports = await SerialPort.list();
    availablePorts = ports.map((port) => port.path);
    return availablePorts;
  } catch (error) {
    console.error('Unable to inspect serial ports:', error.message);
    availablePorts = [];
    return [];
  }
}

function handleIncomingFrame(rawFrame) {
  const packet = parseGpsFrame(rawFrame);
  if (!packet) {
    console.warn('[SERIAL] Ignoring malformed frame:', rawFrame);
    return;
  }

  emitLocationData({
    ...packet,
    timestamp: new Date().toISOString()
  });
}

function attachSerialHandlers(port) {
  let serialBuffer = '';

  port.on('data', (chunk) => {
    serialBuffer += chunk.toString('utf8');
    const lines = serialBuffer.split(/\r?\n/);
    serialBuffer = lines.pop() || '';

    lines.filter(Boolean).forEach((line) => {
      handleIncomingFrame(line);
    });
  });

  port.on('error', (error) => {
    console.error('Serial port error:', error.message);
    setSerialStatus('error', error.message);
  });

  port.on('close', () => {
    if (serialPort && serialPort.path === port.path) {
      serialPort = null;
      serialPortPath = null;
      setSerialStatus('disconnected');
    }
  });
}

function connectToPort(portPathToUse) {
  return new Promise((resolve, reject) => {
    if (serialPort && serialPort.isOpen) {
      if (serialPort.path === portPathToUse) {
        resolve({ port: serialPortPath });
        return;
      }
      serialPort.close(() => {
        serialPort = null;
        serialPortPath = null;
      });
    }

    setSerialStatus('connecting');

    const port = new SerialPort({
      path: portPathToUse,
      baudRate: BAUD_RATE,
      autoOpen: false
    });

    port.open((error) => {
      if (error) {
        const friendly = formatSerialError(error, portPathToUse);
        setSerialStatus('error', friendly.accessibleMessage);
        reject(new Error(friendly.accessibleMessage));
        return;
      }

      attachSerialHandlers(port);
      serialPort = port;
      serialPortPath = portPathToUse;
      setSerialStatus('connected');
      resolve({ port: portPathToUse });
    });
  });
}

function disconnectSerialPort() {
  return new Promise((resolve) => {
    if (!serialPort || !serialPort.isOpen) {
      serialPort = null;
      serialPortPath = null;
      setSerialStatus('disconnected');
      resolve();
      return;
    }

    serialPort.close((error) => {
      if (error) {
        console.warn('Serial close error:', error.message);
      }
      serialPort = null;
      serialPortPath = null;
      setSerialStatus('disconnected');
      resolve();
    });
  });
}

app.get('/api/location', (req, res) => {
  if (!currentLocation) return res.status(404).json({ error: 'No location data available yet' });
  res.json(currentLocation);
});

app.get('/api/history', (req, res) => {
  res.json({ count: coordinateHistory.length, coordinates: coordinateHistory });
});

app.get('/api/stats', (req, res) => {
  if (coordinateHistory.length === 0) return res.status(404).json({ error: 'No location data available' });

  const lats = coordinateHistory.map((c) => c.latitude);
  const lons = coordinateHistory.map((c) => c.longitude);
  const alts = coordinateHistory.map((c) => c.altitude);

  res.json({
    total_points: coordinateHistory.length,
    current: currentLocation,
    bounds: {
      north: Math.max(...lats),
      south: Math.min(...lats),
      east: Math.max(...lons),
      west: Math.min(...lons)
    },
    altitude_range: {
      min: Math.min(...alts),
      max: Math.max(...alts),
      current: currentLocation ? currentLocation.altitude : null
    }
  });
});

app.get('/api/serial/ports', async (req, res) => {
  const ports = await refreshAvailablePorts();
  res.json({
    ports,
    selectedPort: serialPortPath,
    status: serialPortStatus,
    error: serialPortError
  });
});

app.post('/api/serial/connect', async (req, res) => {
  const portPath = req.body.port;
  if (!portPath) {
    return res.status(400).json({ error: 'Missing serial port path' });
  }

  try {
    await connectToPort(portPath);
    return res.json({ ok: true, port: portPath, status: serialPortStatus });
  } catch (error) {
    const friendly = formatSerialError(error, portPath);
    return res.status(500).json({ error: friendly.accessibleMessage, hint: friendly.hint, detail: friendly.detail });
  }
});

app.post('/api/serial/disconnect', async (req, res) => {
  try {
    await disconnectSerialPort();
    return res.json({ ok: true, status: serialPortStatus });
  } catch (error) {
    return res.status(500).json({ error: error.message });
  }
});

const upload = multer({ storage: multer.memoryStorage(), limits: { fileSize: 5 * 1024 * 1024 } });

function parseCSVLine(line) {
  const out = [];
  let cur = '';
  let inQuotes = false;

  for (let i = 0; i < line.length; i++) {
    const ch = line[i];
    if (ch === '"') {
      inQuotes = !inQuotes;
      continue;
    }
    if (ch === ',' && !inQuotes) {
      out.push(cur);
      cur = '';
      continue;
    }
    cur += ch;
  }
  out.push(cur);
  return out;
}

app.post('/api/upload-csv', upload.single('csv'), (req, res) => {
  try {
    if (!req.file) return res.status(400).json({ error: 'Missing csv file upload (field name: csv)' });

    const text = req.file.buffer.toString('utf8');
    const lines = text.split(/\r?\n/).map((l) => l.trim()).filter(Boolean);
    if (lines.length < 2) return res.status(400).json({ error: 'CSV must contain a header row and at least one data row' });

    const header = parseCSVLine(lines[0]).map((h) => h.trim());
    const expected = ['Timestamp', 'Latitude', 'Longitude', 'Altitude', 'Speed', 'IsBacklog', 'Voltage', 'Current'];
    const normalizedHeader = header.map((h) => h.toLowerCase());
    const indexOfHeader = (name) => normalizedHeader.indexOf(name.toLowerCase());

    const timestampIndex = indexOfHeader('Timestamp');
    const latitudeIndex = indexOfHeader('Latitude');
    const longitudeIndex = indexOfHeader('Longitude');
    const altitudeIndex = indexOfHeader('Altitude');
    const speedIndex = indexOfHeader('Speed');
    const backlogIndex = indexOfHeader('IsBacklog');
    const voltageIndex = indexOfHeader('Voltage');
    const currentIndex = indexOfHeader('Current');

    const headerOk = [timestampIndex, latitudeIndex, longitudeIndex, altitudeIndex, speedIndex].every((index) => index !== -1);
    if (!headerOk) {
      return res.status(400).json({
        error: 'Invalid CSV header. Expected columns: ' + expected.join(', '),
        received: header
      });
    }

    const rows = [];
    for (let i = 1; i < lines.length; i++) {
      const parts = parseCSVLine(lines[i]);
      if (parts.length < 5) continue;

      const timestamp = parts[timestampIndex] || '';
      const latitude = parseNumber(parts[latitudeIndex]);
      const longitude = parseNumber(parts[longitudeIndex]);
      const altitude = parseNumber(parts[altitudeIndex]);
      const speed = parseNumber(parts[speedIndex]);
      const isBacklog = backlogIndex !== -1 ? parseBooleanValue(parts[backlogIndex]) : false;
      const voltage = voltageIndex !== -1 ? parseNumber(parts[voltageIndex]) : null;
      const current = currentIndex !== -1 ? parseNumber(parts[currentIndex]) : null;

      if ([latitude, longitude, altitude, speed].some((value) => value === null)) {
        continue;
      }

      const row = {
        latitude,
        longitude,
        altitude,
        speed,
        isBacklog,
        timestamp: timestamp ? timestamp.trim() : new Date().toISOString()
      };

      if (voltage !== null) {
        row.voltage = voltage;
      }

      if (current !== null) {
        row.current = current;
      }

      rows.push(row);
    }

    if (rows.length === 0) return res.status(400).json({ error: 'No valid rows found in CSV' });

    coordinateHistory = [];
    rows.slice(-MAX_HISTORY).forEach((p) => {
      coordinateHistory.push(p);
    });

    currentLocation = coordinateHistory[coordinateHistory.length - 1];

    coordinateHistory.forEach((p) => {
      io.emit('location_update', p);
    });

    return res.json({ ok: true, points_loaded: coordinateHistory.length });
  } catch (error) {
    console.error('upload-csv failed:', error);
    return res.status(500).json({ error: 'Failed to upload/parse CSV' });
  }
});

io.on('connection', (socket) => {
  console.log('User opened the tracker dashboard.');
  socket.emit('serial_status', {
    connected: serialPortStatus === 'connected',
    status: serialPortStatus,
    port: serialPortPath,
    error: serialPortError
  });

  socket.on('disconnect', () => {
    console.log('User closed the tracker dashboard.');
  });
});

server.listen(WEB_PORT, () => {
  console.log(`Dashboard web server actively running at http://localhost:${WEB_PORT}`);
});