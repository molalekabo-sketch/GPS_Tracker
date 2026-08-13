**ESP32-S3 GPS Telemetry Tracker**

This project integrates GNSS (GPS) location tracking, high-precision system power monitoring (Voltage + Current), using a SD card and ESP-NOW. Designed for remote tracking, where data retention is critical even when wireless connections drop.

*Key Features:*

-Streams live data via low-latency ESP-NOW at 1Hz. If the wireless connection drops, data is safely buffered to the SD card. Once the connection is re-established, the system automatically flushes the backlog file over the air without interrupting live tracking.

-Precision Power Monitoring
Current: Reads a WCS1500 Hall-effect sensor 
Voltage: Uses an M5Stack U087 Voltmeter (ADS1115 16-bit I2C ADC) to measure main battery voltage via a custom voltage divider network.

-AT-Command GNSS: Directly interfaces with a GSM/GNSS modem to parse `+CGNSSINFO` sentences for Latitude, Longitude, Altitude, and Speed.

---

**Hardware Architecture**

*Components:*     
|image|Component|
|:---:|:---:|
|<img width="325" height="175" alt="image" src="https://github.com/user-attachments/assets/3f9c653d-26b9-4816-9cc8-dd6601ce581f" /> |Makerfabs ESP32 with 4G LTE CAT1 - A7670E|
|<img width="325" height="175" alt="image" src="https://github.com/user-attachments/assets/f94b732f-319a-43d7-b79c-4ff02cd33fbf" />|UART-controlled GSM/GNSS Module|
| <img width="250" height="250" alt="image" src="https://github.com/user-attachments/assets/77312cd8-6058-4183-91ab-56e3af702260" />|WCS1500 Hall Effect Sensor|
|<img width="320" height="320" alt="image" src="https://github.com/user-attachments/assets/692c051b-c32f-46a0-8df9-dee69c4a2fce" />|M5Stack U087 (ADS1115 I2C)|
|<img width="225" height="257" alt="image" src="https://github.com/user-attachments/assets/ef1a353f-8158-4349-b7ea-3eb5d379e3f4" />|MicroSD Card |

*Data Payload Structure*

The system packages all sensor and location data into a lightweight C-struct before logging it to CSV and broadcasting it over ESP-NOW.

```c
struct TelemetryData {
  unsigned long timestamp;
  float latitude;
  float longitude;
  float altitude;
  float speed;
  bool isValid;
  bool isBacklog;
  float voltage;
  float current;
};
```

---
*Smart Calibration & Failsafes:*

The WCS1500 sensor is a ratiometric sensor. To ensure high accuracy, this firmware implements:

1. Dynamic VDD Scaling: Sensitivity is calculated against the real-world (4.6V) rather than assuming a perfect 5.0V supply.
2. Moving Average Filter: Raw amperage readings are smoothed through a 20-sample circular buffer.
3. Hardware Sanity Check: During boot-up calibration, if the calculated zero-point falls outside an expected threshold (e.g., if the system booted under heavy load), the code rejects the skewed calibration and safely defaults to the mathematical ideal zero.

more details and in-depth explanation of the sensor at:
https://github.com/viwemqaqa/WCS1500-current-sensor/tree/main

---

**Setup & Installation**

*Dependencies*
(Ensure you have the following libraries installed in your Arduino IDE or PlatformIO environment:)

M5Unified & M5GFX
M5_ADS1115

*Built-in ESP32 core libraries:*

WiFi 
esp_now
SD_MMC
Wire
HardwareSerial

**Configuration**

1. Target MAC Address: Update the receiverAddress array with the MAC address of your ESP-NOW receiver node.
   
```c 
uint8_t receiverAddress[] = {0xXX, 0xXX, 0xXX, 0xXX, 0xXX, 0xXX};
```


*SD Card File System:*

```c /unsent.csv``` - The active backlog queue. Live data is appended here until a successful ESP-NOW transmission acknowledges receipt.
```c /temp.csv``` - Used internally during the backlog flushing process to safely truncate the file without data loss.
