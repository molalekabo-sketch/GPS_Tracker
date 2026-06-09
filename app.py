import json
import threading
import time
import panel as pn
import holoviews as hv
import hvplot.pandas
import pandas as pd
import paho.mqtt.client as mqtt
import xyzservices.providers as xyz
import random

# Initialize Panel and Extension
pn.extension(sizing_mode="stretch_width")
hv.extension('bokeh')

# --- CONFIGURATION MATCHING ESP32 HARDWARE ---
MQTT_BROKER = "localhost"
MQTT_TOPIC = "telemetry/data"

# Starting position for GPS coordinate generation (Cape Town, South Africa)
START_LAT = -33.9249
START_LON = 18.4241

# Publisher client instance
publisher_client = None

# --- GPS COORDINATE GENERATION ---
class GPSSimulator:
    def __init__(self, start_lat, start_lon):
        self.current_lat = start_lat
        self.current_lon = start_lon
        self.battery_voltage = 12.60
    
    def generate_next_position(self):
        """Generate next GPS coordinates with slight randomization"""
        # Small random movements to simulate natural drift
        lat_drift = random.uniform(-0.0005, 0.0005)
        lon_drift = random.uniform(-0.0005, 0.0005)
        
        self.current_lat += lat_drift
        self.current_lon += lon_drift
        
        # Simulate battery drain
        self.battery_voltage = max(10.0, self.battery_voltage - 0.01)
        
        return {
            "lat": self.current_lat,
            "lon": self.current_lon,
            "v_ext": round(self.battery_voltage, 2)
        }

gps_simulator = GPSSimulator(START_LAT, START_LON)

# --- GPS PUBLISHING ENGINE ---
def start_gps_publisher():
    """Background thread that generates and publishes GPS coordinates every 5 seconds"""
    global publisher_client
    
    publisher_client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
    
    def on_publish_connect(client, userdata, flags, rc, properties=None):
        print(f"Publisher connected to MQTT Broker")
    
    publisher_client.on_connect = on_publish_connect
    publisher_client.connect(MQTT_BROKER, 1883, 120)
    
    print("Starting GPS coordinate publisher (5 second intervals)...")
    
    while True:
        try:
            # Generate new position
            gps_data = gps_simulator.generate_next_position()
            
            # Publish to MQTT topic
            publisher_client.publish(MQTT_TOPIC, json.dumps(gps_data))
            print(f"Published GPS Data: Lat={gps_data['lat']:.6f}, Lon={gps_data['lon']:.6f}, Voltage={gps_data['v_ext']}V")
            
            # Wait 5 seconds before next publish
            time.sleep(5)
        except Exception as e:
            print(f"Error publishing GPS data: {e}")
            time.sleep(5)

# Panel state variables that trigger UI redraws on alteration
state_lat = pn.widgets.StaticText(name="Latitude", value="Waiting...")
state_lon = pn.widgets.StaticText(name="Longitude", value="Waiting...")
state_volt = pn.widgets.StaticText(name="Battery Voltage", value="Waiting...")

# Global dictionary holding our most recent coordinates (Thread-safe updates)
current_position = {"lat": 0.0, "lon": 0.0}

# --- MQTT NETWORKING STACK ENGINE ---
def on_connect(client, userdata, flags, rc, properties=None):
    print(f"Connected to MQTT Broker. Subscribing to: {MQTT_TOPIC}")
    client.subscribe(MQTT_TOPIC)

def on_message(client, userdata, msg):
    global current_position
    try:
        # Expected incoming ESP32 JSON string: {"lat": -29.102057, "lon": 26.192797, "v_ext": 12.60}
        payload = json.loads(msg.payload.decode('utf-8'))
        
        lat = float(payload.get('lat', 0.0))
        lon = float(payload.get('lon', 0.0))
        volt = payload.get('v_ext', 0.0)
        
        # Update thread-safe data reference
        current_position["lat"] = lat
        current_position["lon"] = lon
        
        # Stream updates directly into Panel widgets via safe event loops
        state_lat.value = f"{lat:.6f}"
        state_lon.value = f"{lon:.6f}"
        state_volt.value = f"{volt:.2f} V" if volt else "N/A"
        
    except Exception as e:
        print(f"Parsing error on incoming tracking payload packet: {e}")

def start_mqtt_thread():
    client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(MQTT_BROKER, 1883, 120)
    client.loop_forever()

# Launch GPS publisher as a daemon thread (generates and publishes coordinates)
threading.Thread(target=start_gps_publisher, daemon=True).start()

# Launch MQTT subscriber as a daemon thread (receives and displays coordinates)
threading.Thread(target=start_mqtt_thread, daemon=True).start()

# --- REAL-TIME MAP RENDER LOOP ---
@pn.depends(state_lat, state_lon)
def dynamic_map_view(lat_val, lon_val):
    """Generates an updating interactive map tile structure when coordinates shift"""
    lat = current_position["lat"]
    lon = current_position["lon"]
    
    # Fallback to zero reference center if no hardware lock exists yet
    if lat == 0.0 and lon == 0.0:
        df = pd.DataFrame({'lon': [0.0], 'lat': [0.0], 'Asset': ['Initializing System...']})
    else:
        df = pd.DataFrame({'lon': [lon], 'lat': [lat], 'Asset': ['Live Tracker Active']})

    # Create a simple scatter plot (without geographic projection)
    map_plot = df.hvplot.scatter(
        x='lon', y='lat', color='red', size=200, 
        hover_cols=['Asset'], alpha=0.8, responsive=True
    )
    
    # Add OpenStreetMap tile background without geoviews requirement
    tile_background = hv.Tiles(xyz.OpenStreetMap.Mapnik)
    
    return (tile_background * map_plot).opts(height=600, responsive=True)

# --- PANEL DASHBOARD LAYOUT BUILDER ---
sidebar_controls = pn.Column(
    pn.pane.Markdown("### Asset Diagnostic Stream"),
    state_lat,
    state_lon,
    state_volt,
    pn.pane.Markdown("--- \n *Updates instantly on cellular hardware ping events.*"),
    width=280,
    styles=dict(background='#f8f9fa', padding='15px')
)

dashboard = pn.template.FastListTemplate(
    title="ESP32 4G LTE Fleet Management Console",
    sidebar=[sidebar_controls],
    main=[pn.panel(dynamic_map_view, loading_indicator=True)],
    header_background="#007bff"
)

# Render block command hook
dashboard.servable()
