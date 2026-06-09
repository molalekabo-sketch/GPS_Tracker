import time
import json
import paho.mqtt.client as mqtt

BROKER = "broker.hivemq.com"
TOPIC = "telemetry/data"

client = mqtt.Client(callback_api_version=mqtt.CallbackAPIVersion.VERSION2)
client.connect(BROKER, 1883)

print("Simulating active vehicle track. Streaming mock steps to Panel...")

# Starting position coordinate grid baseline (Cape Town region)
start_lat = -33.9249
start_lon = 18.4241

for i in range(20):
    # Simulate step changes to coordinates
    mock_lat = start_lat + (i * 0.0005)
    mock_lon = start_lon + (i * 0.0003)
    mock_volts = 12.60 - (i * 0.01)  # Show mock engine draw decay over time
    
    payload = {
        "lat": mock_lat,
        "lon": mock_lon,
        "v_ext": mock_volts
    }
    
    client.publish(TOPIC, json.dumps(payload))
    print(f"Sent Packet {i+1}: {payload}")
    time.sleep(3)  # Send update frame block loop sequence every 3 seconds
