const express = require('express');
const http = require('http');
const socketIo = require('socket.io');
const mqtt = require('mqtt');

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
const MQTT_TOPICS = {
  latitude: 'SimuTech/gps/latitude',
  longitude: 'SimuTech/gps/longitude',
  altitude: 'SimuTech/gps/altitude',
  speed: 'SimuTech/gps/speed'
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

// Assemble latest telemetry from 4 topics
const partial = {
  latitude: null,
  longitude: null,
  altitude: null,
  speed: null,
};

function emitIfReady() {
  if (
    partial.latitude === null ||
    partial.longitude === null ||
    partial.altitude === null ||
    partial.speed === null
  ) {
    return;
  }

  const timestamp = new Date().toISOString();
  const data = {
    latitude: partial.latitude,
    longitude: partial.longitude,
    altitude: partial.altitude,
    speed: partial.speed,
    timestamp
  };

  currentLocation = data;

  coordinateHistory.push({
    ...data
  });
  if (coordinateHistory.length > MAX_HISTORY) coordinateHistory.shift();

  // 🟢 NEW: Log the assembled data payload being sent to the browser
  console.log('[SOCKET OUT] Emitting complete coordinate package to UI:', data);

  // Stream to all open browser sessions
  io.emit('location_update', data);
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
  
  // 🟢 NEW: Log the exact topic and payload as soon as it arrives
  console.log(`[MQTT IN] Topic: ${topic} | Payload: ${rawPayload}`);

  const n = parseNumber(rawPayload);
  if (n === null) {
    console.warn(`[WARNING] Non-numeric payload for ${topic}: '${rawPayload}'`);
    return;
  }

  switch (topic) {
    case MQTT_TOPICS.latitude:
      partial.latitude = n;
      break;
    case MQTT_TOPICS.longitude:
      partial.longitude = n;
      break;
    case MQTT_TOPICS.altitude:
      partial.altitude = n;
      break;
    case MQTT_TOPICS.speed:
      partial.speed = n;
      break;
    default:
      return;
  }

  emitIfReady();
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
      current: currentLocation.altitude
    }
  });
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