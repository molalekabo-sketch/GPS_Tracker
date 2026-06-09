const express = require('express');
const http = require('http');
const socketIo = require('socket.io');
const mqtt = require('mqtt');

const app = express();
const server = http.createServer(app);
const io = socketIo(server);

const PORT = 3000;

// Configure your target MQTT broker (Must match your ESP32 target script broker)
const MQTT_BROKER = 'mqtt://broker.hivemq.com'; 
const MQTT_TOPIC = 'telemetry/data';

// Middleware
app.use(express.json());
app.use(express.static('public'));

// In-memory storage for coordinate history
let coordinateHistory = [];
let currentLocation = null;
const MAX_HISTORY = 100; // Keep last 100 coordinates

// Initialize backend connection to the MQTT broker network
const mqttClient = mqtt.connect(MQTT_BROKER);

mqttClient.on('connect', () => {
    console.log(`Connected to MQTT Broker at ${MQTT_BROKER}`);
    mqttClient.subscribe(MQTT_TOPIC, (err) => {
        if (!err) {
            console.log(`Successfully subscribed to topic: ${MQTT_TOPIC}`);
        }
    });
});

mqttClient.on('message', (topic, message) => {
    try {
        // Expecting incoming payload text from ESP32: {"lat": -29.102057, "lon": 26.192797, "v_ext": 12.60}
        const telemetryData = JSON.parse(message.toString());
        console.log('Received Telemetry Packet:', telemetryData);

        // Store location for API access
        currentLocation = {
            ...telemetryData,
            timestamp: new Date().toISOString()
        };

        // Maintain history of coordinates for map overlay
        coordinateHistory.push(currentLocation);
        if (coordinateHistory.length > MAX_HISTORY) {
            coordinateHistory.shift();
        }

        // Instantly stream coordinate data packet straight to open browser sessions
        io.emit('location_update', telemetryData);
    } catch (error) {
        console.error('Error parsing incoming payload package string:', error.message);
    }
});

// ============ API ENDPOINTS FOR MAP OVERLAY ============

// Get current asset location
app.get('/api/location', (req, res) => {
    if (!currentLocation) {
        return res.status(404).json({ error: 'No location data available yet' });
    }
    res.json(currentLocation);
});

// Get complete coordinate history for path/trail overlay
app.get('/api/history', (req, res) => {
    res.json({
        count: coordinateHistory.length,
        coordinates: coordinateHistory
    });
});

// Get summary statistics
app.get('/api/stats', (req, res) => {
    if (coordinateHistory.length === 0) {
        return res.status(404).json({ error: 'No location data available' });
    }

    const lats = coordinateHistory.map(c => c.lat);
    const lons = coordinateHistory.map(c => c.lon);
    const volts = coordinateHistory.map(c => c.v_ext);

    res.json({
        total_points: coordinateHistory.length,
        current: currentLocation,
        bounds: {
            north: Math.max(...lats),
            south: Math.min(...lats),
            east: Math.max(...lons),
            west: Math.min(...lons)
        },
        voltage_range: {
            min: Math.min(...volts),
            max: Math.max(...volts),
            current: currentLocation.v_ext
        }
    });
});

// Manage browser instances opening the dashboard tab
io.on('connection', (socket) => {
    console.log('A user opened the tracker dashboard dashboard interface.');
    socket.on('disconnect', () => {
        console.log('User closed the tracker dashboard connection.');
    });
});

server.listen(PORT, () => {
    console.log(`Dashboard web server actively running at http://localhost:${PORT}`);
});
