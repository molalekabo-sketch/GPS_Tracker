ESP32-S3 GPS Telemetry Tracker

This project integrates GNSS (GPS) location tracking, high-precision system power monitoring (Voltage + Current), using a SD card and ESP-NOW. Designed for remote tracking, where data retention is critical even when wireless connections drop.

Key Features:

-Streams live data via low-latency ESP-NOW at 1Hz. If the wireless connection drops, data is safely buffered to the SD card. Once the connection is re-established, the system automatically flushes the backlog file over the air without interrupting live tracking.

-Precision Power Monitoring
Current: Reads a WCS1500 Hall-effect sensor 
Voltage: Uses an M5Stack U087 Voltmeter (ADS1115 16-bit I2C ADC) to measure main battery voltage via a custom voltage divider network.

-AT-Command GNSS: Directly interfaces with a GSM/GNSS modem to parse `+CGNSSINFO` sentences for Latitude, Longitude, Altitude, and Speed.

---

Hardware Architecture

Components:
Microcontroller: ESP32-S3
Modem: UART-controlled GSM/GNSS Module
Current Sensor: WCS1500 Hall Effect Sensor
Voltage Sensor: M5Stack U087 (ADS1115 I2C)
Storage: MicroSD Card (1-Bit SD_MMC Mode)

Data Payload Structure

The system packages all sensor and location data into a lightweight C-struct before logging it to CSV and broadcasting it over ESP-NOW.

struct TelemetryData {
  unsigned long timestamp; // Uptime in milliseconds
  float latitude;          // GPS Latitude
  float longitude;         // GPS Longitude
  float altitude;          // Altitude in meters
  float speed;             // Speed in km/h or knots
  bool isValid;            // GPS Lock status
  bool isBacklog;          // Flag indicating if data is live or recovered
  float voltage;           // Battery voltage (V)
  float current;           // System draw (A)
};

---

🧠 Smart Calibration & Failsafes

The WCS1500 sensor is a $V_{CC}/2$ ratiometric sensor. To ensure high accuracy, this firmware implements:

1. Dynamic VDD Scaling: Sensitivity is calculated against the real-world (4.6V) rather than assuming a perfect 5.0V supply.
2. Moving Average Filter: Raw amperage readings are smoothed through a 20-sample circular buffer.
3. Hardware Sanity Check: During boot-up calibration, if the calculated zero-point falls outside an expected threshold (e.g., if the system booted under heavy load), the code rejects the skewed calibration and safely defaults to the mathematical ideal zero.

more details and in-depth explanation of the sensor at:
https://github.com/viwemqaqa/WCS1500-current-sensor/tree/main
---

Setup & Installation

Dependencies

Ensure you have the following libraries installed in your Arduino IDE or PlatformIO environment:

M5Unified & M5GFX
M5_ADS1115

Built-in ESP32 core libraries: 

WiFi 
esp_now
SD_MMC
Wire
HardwareSerial

Configuration

1. Target MAC Address: Update the receiverAddress array with the MAC address of your ESP-NOW receiver node.
   
uint8_t receiverAddress[] = {0xXX, 0xXX, 0xXX, 0xXX, 0xXX, 0xXX};


SD Card File System

unsent.csv - The active backlog queue. Live data is appended here until a successful ESP-NOW transmission acknowledges receipt.
/temp.csv - Used internally during the backlog flushing process to safely truncate the file without data loss.
