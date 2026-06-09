"""
Standalone MQTT Publisher and Subscriber
Demonstrates the complete publish/subscribe pattern for GPS telemetry
"""

import json
import threading
import time
import paho.mqtt.client as mqtt
import random

# --- MQTT CONFIGURATION ---
MQTT_BROKER = "localhost"
MQTT_TOPIC = "telemetry/data"

# --- GPS SIMULATOR ---
class GPSSimulator:
    def __init__(self, start_lat=-33.9249, start_lon=18.4241):
        self.current_lat = start_lat
        self.current_lon = start_lon
        self.battery_voltage = 12.60
    
    def generate_next_position(self):
        """Generate next GPS coordinates with slight randomization"""
        lat_drift = random.uniform(-0.0005, 0.0005)
        lon_drift = random.uniform(-0.0005, 0.0005)
        
        self.current_lat += lat_drift
        self.current_lon += lon_drift
        self.battery_voltage = max(10.0, self.battery_voltage - 0.01)
        
        return {
            "lat": round(self.current_lat, 6),
            "lon": round(self.current_lon, 6),
            "v_ext": round(self.battery_voltage, 2)
        }

# --- PUBLISHER SETUP ---
class MQTTPublisher:
    def __init__(self, broker, topic):
        self.broker = broker
        self.topic = topic
        self.client = None
    
    def on_connect(self, client, userdata, flags, rc, properties=None):
        if rc == 0:
            print("[PUBLISHER] Connected to MQTT Broker")
        else:
            print(f"[PUBLISHER] Connection failed with code {rc}")
    
    def connect(self):
        """Connect to MQTT broker"""
        self.client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
        self.client.on_connect = self.on_connect
        self.client.connect(self.broker, 8883, 60)
        self.client.loop_start()
    
    def publish_message(self, data):
        """Publish GPS data to topic"""
        if self.client:
            result = self.client.publish(self.topic, json.dumps(data))
            print(f"[PUBLISHER] Published: {data}")
    
    def disconnect(self):
        """Disconnect from broker"""
        if self.client:
            self.client.loop_stop()
            self.client.disconnect()

# --- SUBSCRIBER SETUP ---
class MQTTSubscriber:
    def __init__(self, broker, topic):
        self.broker = broker
        self.topic = topic
        self.client = None
        self.received_messages = []
    
    def on_connect(self, client, userdata, flags, rc, properties=None):
        if rc == 0:
            print(f"[SUBSCRIBER] Connected to MQTT Broker")
            print(f"[SUBSCRIBER] Subscribing to topic: {self.topic}")
            self.client.subscribe(self.topic)
        else:
            print(f"[SUBSCRIBER] Connection failed with code {rc}")
    
    def on_message(self, client, userdata, msg):
        """Handle incoming messages"""
        try:
            payload = json.loads(msg.payload.decode('utf-8'))
            self.received_messages.append(payload)
            print(f"[SUBSCRIBER] Received: Lat={payload['lat']}, Lon={payload['lon']}, Voltage={payload['v_ext']}V")
        except Exception as e:
            print(f"[SUBSCRIBER] Error parsing message: {e}")
    
    def connect(self):
        """Connect to MQTT broker"""
        self.client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
        self.client.on_connect = self.on_connect
        self.client.on_message = self.on_message
        self.client.connect(self.broker, 1883, 60)
        self.client.loop_forever()
    
    def start_listening(self):
        """Start listening in a background thread"""
        thread = threading.Thread(target=self.connect, daemon=True)
        thread.start()

# --- MAIN APPLICATION ---
def main():
    print("=" * 60)
    print("GPS Tracker - MQTT Publisher & Subscriber Demo")
    print("=" * 60)
    
    # Initialize GPS simulator
    gps_sim = GPSSimulator()
    
    # Initialize publisher and subscriber
    publisher = MQTTPublisher(MQTT_BROKER, MQTT_TOPIC)
    subscriber = MQTTSubscriber(MQTT_BROKER, MQTT_TOPIC)
    
    # Connect publisher
    print("\n[SETUP] Connecting Publisher...")
    publisher.connect()
    time.sleep(1)
    
    # Start subscriber in background thread
    print("[SETUP] Starting Subscriber...")
    subscriber.start_listening()
    time.sleep(2)
    
    # Main loop: Generate and publish GPS data every 5 seconds
    print("\n[RUNNING] Publishing GPS coordinates every 5 seconds...")
    print("-" * 60)
    
    try:
        iteration = 0
        while iteration < 20:  # Publish 20 times (100 seconds total)
            iteration += 1
            
            # Generate GPS data
            gps_data = gps_sim.generate_next_position()
            
            # Publish to MQTT broker
            publisher.publish_message(gps_data)
            
            # Wait 5 seconds
            time.sleep(5)
        
        print("-" * 60)
        print("\n[SUMMARY] Publish/Subscribe cycle completed!")
        print(f"Total messages received: {len(subscriber.received_messages)}")
        
    except KeyboardInterrupt:
        print("\n[SHUTDOWN] Interrupted by user")
    
    finally:
        # Cleanup
        publisher.disconnect()
        print("[SHUTDOWN] Publisher disconnected")
        print("[SHUTDOWN] Application stopped")

if __name__ == "__main__":
    main()
