const express = require('express');
const http = require('http');
const socketIo = require('socket.io');
const mqtt = require('mqtt');
const multer = require('multer');


const app = express();
const server = http.createServer(app);
const io = socketIo(server);

// Web server port (separated from MQTT port 1883 to prevent conflicts)
const WEB_PORT = 3000; 
const brokerUrl = "mqtt://broker.hivemq.com";

const options = {
    port: 1883,                          // Standard unencrypted MQTT port
    clientId: `server_client_${Math.random().toString(16).slice(3)}`, 
    // No username/password since broker.hivemq.com is open/public
    keepalive: 60,                       // Seconds between heartbeat packets
    clean: true,                         // Forget transient subscriptions on disconnect
    reconnectPeriod: 5000,               // Inter-reconnect interval in milliseconds
    connectTimeout: 30 * 1000,           // Time to wait for a connack before failing
};

// ESP32 publishes GPS fields to 4 separate topics as plain strings (numbers)
// Single JSON payload topic published by gps_tracker.ino
const MQTT_TOPICS = {
  location: 'SimuTech/gps/location'
};

// Keep latest complete state for the UI
let currentLocation = null; // { latitude, longitude, altitude, speed, timestamp }
let coordinateHistory = []; // keep last 100 points
const MAX_HISTORY = 100;

// Middleware
app.use(express.json());
app.use(express.static('public'));

function parseNumber(payload) {
  const n = parseFloat(payload);
  return Number.isFinite(n) ? n : null;
}

function emitLocationData(data) {
  const timestamp = data.timestamp || new Date().toISOString();
  const payload = {
    latitude: data.latitude,
    longitude: data.longitude,
    altitude: data.altitude,
    speed: data.speed,
    timestamp
  };

  currentLocation = payload;

  coordinateHistory.push({ ...payload });
  if (coordinateHistory.length > MAX_HISTORY) coordinateHistory.shift();

  console.log('[SOCKET OUT] Emitting complete coordinate package to UI:', payload);
  io.emit('location_update', payload);
}


// MQTT connection
const mqttClient = mqtt.connect(brokerUrl, options);

mqttClient.on('connect', () => {
  console.log(`Connected to MQTT Broker at ${brokerUrl}`);

  const topics = Object.values(MQTT_TOPICS);
  topics.forEach((t) => {
    mqttClient.subscribe(t, { qos: 1 }, (err) => {
      if (err) {
        console.error(`Failed to subscribe to ${t}:`, err.message);
      } else {
        console.log(`Subscribed to ${t}`);
      }
    });
  });
});

mqttClient.on('message', (topic, message) => {
  const rawPayload = message.toString().trim();

  console.log(`[MQTT IN] Topic: ${topic} | Payload: ${rawPayload}`);

  if (topic !== MQTT_TOPICS.location) return;

  // gps_tracker.ino publishes unified JSON:
  // {"latitude":...,"longitude":...,"altitude":...,"speed":...}
  let obj;
  try {
    obj = JSON.parse(rawPayload);
  } catch (e) {
    console.warn('[WARNING] Failed to parse JSON from MQTT payload:', e.message);
    return;
  }

  const lat = parseNumber(obj.latitude);
  const lon = parseNumber(obj.longitude);
  const alt = parseNumber(obj.altitude);
  const speed = parseNumber(obj.speed);

  if (![lat, lon, alt, speed].every((v) => v !== null)) {
    console.warn('[WARNING] Missing/invalid fields in MQTT JSON payload:', obj);
    return;
  }

  emitLocationData({ latitude: lat, longitude: lon, altitude: alt, speed: speed });
});


mqttClient.on('error', (err) => {
  console.error('MQTT Client Error:', err.message);
});

// ================== API ENDPOINTS FOR MAP OVERLAY ==================

app.get('/api/location', (req, res) => {
  if (!currentLocation) return res.status(404).json({ error: 'No location data available yet' });
  res.json(currentLocation);
});

app.get('/api/history', (req, res) => {
  res.json({ count: coordinateHistory.length, coordinates: coordinateHistory });
});

app.get('/api/stats', (req, res) => {
  if (coordinateHistory.length === 0) return res.status(404).json({ error: 'No location data available' });

  const lats = coordinateHistory.map(c => c.latitude);
  const lons = coordinateHistory.map(c => c.longitude);
  const alts = coordinateHistory.map(c => c.altitude);

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

// ================== CSV UPLOAD ==================

const upload = multer({ storage: multer.memoryStorage(), limits: { fileSize: 5 * 1024 * 1024 } });

function parseCSVLine(line) {
  // Minimal CSV parser for our expected format:
  // Latitude,Longitude,Altitude,Speed,Timestamp
  // Timestamp may be wrapped in double-quotes.
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
    const lines = text.split(/\r?\n/).map(l => l.trim()).filter(Boolean);
    if (lines.length < 2) return res.status(400).json({ error: 'CSV must contain a header row and at least one data row' });

    const header = parseCSVLine(lines[0]).map(h => h.trim());
    const expected = ['Latitude', 'Longitude', 'Altitude', 'Speed', 'Timestamp'];
    const headerOk = expected.every((h, idx) => header[idx] === h);
    if (!headerOk) {
      return res.status(400).json({
        error: 'Invalid CSV header. Expected: ' + expected.join(', '),
        received: header
      });
    }

    const rows = [];
    for (let i = 1; i < lines.length; i++) {
      const parts = parseCSVLine(lines[i]);
      if (parts.length < 5) continue;

      const [Latitude, Longitude, Altitude, Speed, Timestamp] = parts;

      const lat = parseNumber(Latitude);
      const lon = parseNumber(Longitude);
      const alt = parseNumber(Altitude);
      const speed = parseNumber(Speed);

      if ([lat, lon, alt, speed].some(v => v === null)) {
        continue;
      }

      rows.push({
        latitude: lat,
        longitude: lon,
        altitude: alt,
        speed: speed,
        timestamp: Timestamp ? Timestamp.trim() : new Date().toISOString()
      });
    }

    if (rows.length === 0) return res.status(400).json({ error: 'No valid rows found in CSV' });

    // Overwrite existing track with uploaded data
    coordinateHistory = [];
    rows.slice(-MAX_HISTORY).forEach(p => {
      coordinateHistory.push(p);
    });

    currentLocation = coordinateHistory[coordinateHistory.length - 1];

    // Emit every uploaded point in order so the UI can draw the full polyline.
    // (UI will also update marker/stats per point.)
    coordinateHistory.forEach((p) => {
      io.emit('location_update', p);
    });

    return res.json({ ok: true, points_loaded: coordinateHistory.length });
  } catch (e) {
    console.error('upload-csv failed:', e);
    return res.status(500).json({ error: 'Failed to upload/parse CSV' });
  }
});


io.on('connection', (socket) => {
  console.log('User opened the tracker dashboard.');
  socket.on('disconnect', () => {
    console.log('User closed the tracker dashboard.');
  });
});

server.listen(WEB_PORT, () => {
  console.log(`Dashboard web server actively running at http://localhost:${WEB_PORT}`);
});