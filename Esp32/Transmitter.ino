#include <Arduino.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <esp_now.h>
#include "FS.h"
#include <SD_MMC.h>
#include "SPI.h"
#include <Wire.h>
#include "M5Unified.h"
#include "M5GFX.h"
#include "M5_ADS1115.h"

//--- M5Stack U087 Voltmeter  ---
#define M5_UNIT_VMETER_I2C_ADDR             0x49
#define M5_UNIT_VMETER_EEPROM_I2C_ADDR      0x53
#define M5_UNIT_VMETER_PRESSURE_COEFFICIENT 0.015918958F

ADS1115 Vmeter;

float resolution         = 0.0;
float calibration_factor = 0.0;

// --- Hardware Pin Definitions ---
#define IO_RXD2        47   
#define IO_TXD2        48   
#define IO_GSM_PWRKEY  4    
#define IO_GSM_RST     5    

// --- SD_MMC Pins (1-bit mode) ---
#define PIN_SD_CMD     11
#define PIN_SD_CLK     12
#define PIN_SD_D0      13

// --- Sensor Pins & Configuration ---
#define CURRENT_SENSOR_PIN 9             // Verified ESP32-S3 pin
float WCS1500_OFFSET = 2.3;              // Will be auto-calibrated in setup
const int SDA_PIN = 17;
const int SCL_PIN = 18;

const float VDD = 4.6;
const float SENSITIVITY = 0.011 * (VDD / 5.0); // True sensitivity scaled to VDD
const float DIVIDER = 6800.0 / (10000.0 + 6800.0); 

const int FILTER_SIZE = 20;
float currentReadings[FILTER_SIZE];
int readIndex = 0;
float readTotal = 0;

#define DEBUG false 

// --- ESP-NOW Configuration ---
uint8_t receiverAddress[] = {} //The receivers address here
esp_now_peer_info_t peerInfo;
volatile bool espNowInitialized = false; // Tracks dynamic boot state

HardwareSerial modem(2);

unsigned long currentTime;
unsigned long lastLogTime = 0;
unsigned long lastTxTime = 0;

volatile bool callbackReceived = false;
volatile bool lastDeliverySuccess = false;

// --- Telemetry Data Structure (8 Fields) ---
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

TelemetryData latestData = {0, 0, 0, 0, 0, false, false, 0.0, 0.0};

// --- SENSOR HELPER FUNCTIONS ---

float readRawCurrentVoltage() {
  long total_mV = 0;
  for (int i = 0; i < 100; i++) {
    total_mV += analogReadMilliVolts(CURRENT_SENSOR_PIN);
    delayMicroseconds(50); 
  }
  float avg_mV = (float)total_mV / 100.0;
  float pinVoltage = avg_mV / 1000.0; // Convert to Volts
  return pinVoltage / DIVIDER;        // Reconstruct true pre-divider voltage
}

float getFilteredCurrent() {
  long total_mV = 0;
  const int samples = 100;
  
  // Micro-sample to kill ADC noise
  for (int i = 0; i < samples; i++) {
    total_mV += analogReadMilliVolts(CURRENT_SENSOR_PIN);
    delayMicroseconds(50); 
  }
  
  // Convert mV to Volts, then reconstruct true sensor voltage
  float pinVoltage = (total_mV / (float)samples) / 1000.0;
  float trueSensorV = pinVoltage / DIVIDER;
  
  // FIX: Using the VDD-corrected SENSITIVITY variable
  float rawCurrent = (trueSensorV - WCS1500_OFFSET) / SENSITIVITY;

  // Moving Average Filter
  readTotal -= currentReadings[readIndex];
  currentReadings[readIndex] = rawCurrent;
  readTotal += currentReadings[readIndex];
  readIndex = (readIndex + 1) % FILTER_SIZE;

  // FIX: Removed the deadband. Small currents will now register.
  return readTotal / FILTER_SIZE;
}

float getBatteryVoltage() {
  // 1. Get raw ADC data
  int16_t adc_raw = Vmeter.getSingleConversion();
  
  // 2. M5Stack EEPROM Math (Outputs in MILLIVOLTS)
  float voltage_mV = adc_raw * resolution * calibration_factor;
  
  // 3. Convert Millivolts to Volts
  float m5_voltage = voltage_mV / 1000.0; 
  
  // 4. Reverse the External Breadboard Divider
  const float EXTERNAL_DIVIDER_RATIO = 2.2222;
  float true_battery_voltage = m5_voltage * EXTERNAL_DIVIDER_RATIO;
  
  return true_battery_voltage;
}

// --- AT Command & Parsing ---

String sendATCommand(String command, const int timeout, boolean debug) {
  String response = "";
  modem.println(command);
  long int time = millis();
  while ((time + timeout) > millis()) {
    while (modem.available()) {
      char c = modem.read();
      response += c;
    }
  }
  if (debug) Serial.print(response);
  return response;
}

TelemetryData parseGPSResponse(String response) {
  TelemetryData data = {0, 0, 0, 0, 0, false, false, 0.0, 0.0};
  int startIndex = response.indexOf("+CGNSSINFO:");
  if (startIndex == -1) return data;
  
  String dataStr = response.substring(startIndex + 11); 
  int commaCount = 0;
  int prevComma = 0;
  int currentComma = 0;
  String parts[15];
  
  while (currentComma != -1 && commaCount < 15) {
    currentComma = dataStr.indexOf(',', prevComma);
    if (currentComma == -1) {
      parts[commaCount] = dataStr.substring(prevComma);
    } else {
      parts[commaCount] = dataStr.substring(prevComma, currentComma);
    }
    parts[commaCount].trim();
    prevComma = currentComma + 1;
    commaCount++;
  }
  
  if (parts[5].length() > 2) {
    float lat = parts[5].toFloat();
    if (parts[6] == "S") lat = lat * -1.0;
    data.latitude = lat;
    
    float lon = parts[7].toFloat();
    if (parts[8] == "W") lon = lon * -1.0;
    data.longitude = lon;

    data.altitude = parts[11].toFloat();
    data.speed = parts[12].toFloat();
    data.isValid = true;
    data.isBacklog = false;
  }
  return data;
}

// --- ESP-NOW Callbacks ---

void OnDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  lastDeliverySuccess = (status == ESP_NOW_SEND_SUCCESS);
  callbackReceived = true;
}

// --- SD_MMC File Operations ---

void saveToSD(TelemetryData data) {
  File file = SD_MMC.open("/unsent.csv", FILE_APPEND);
  if (file) {
    file.printf("%lu,%.6f,%.6f,%.2f,%.2f,%d,%.2f,%.1f\n", 
                data.timestamp, data.latitude, data.longitude, 
                data.altitude, data.speed, data.isBacklog ? 1 : 0, 
                data.voltage, data.current);
    file.close(); 
    Serial.println("-> Saved live telemetry to SD card."); 
  } else {
    Serial.println("-> SD Write Error: Could not open /unsent.csv");
  }
}

void flushBacklog() {
  if (!SD_MMC.exists("/unsent.csv")) return;

  File file = SD_MMC.open("/unsent.csv", FILE_READ);
  if (!file || file.size() == 0) {
    if (file) file.close();
    SD_MMC.remove("/unsent.csv");
    return;
  }

  Serial.println("--- Syncing Backlog via ESP-NOW ---");
  File tempFile = SD_MMC.open("/temp.csv", FILE_WRITE);

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    TelemetryData backlogData;
    int backlogFlag;
    
    int parsed = sscanf(line.c_str(), "%lu,%f,%f,%f,%f,%d,%f,%f", 
           &backlogData.timestamp, &backlogData.latitude, &backlogData.longitude, 
           &backlogData.altitude, &backlogData.speed, &backlogFlag,
           &backlogData.voltage, &backlogData.current);
    
    if (parsed != 8) continue; 

    backlogData.isValid = true;
    backlogData.isBacklog = true; 

    callbackReceived = false;
    esp_err_t result = esp_now_send(receiverAddress, (uint8_t *)&backlogData, sizeof(backlogData));

    unsigned long waitStart = millis();
    while (!callbackReceived && (millis() - waitStart < 60)) { delay(1); }

    if (result != ESP_OK || !lastDeliverySuccess) {
      tempFile.printf("%lu,%.6f,%.6f,%.2f,%.2f,%d,%.2f,%.1f\n", 
                      backlogData.timestamp, backlogData.latitude, backlogData.longitude, 
                      backlogData.altitude, backlogData.speed, backlogData.isBacklog ? 1 : 0,
                      backlogData.voltage, backlogData.current);
                      
      while (file.available()) {
        tempFile.println(file.readStringUntil('\n'));
      }
      break;
    }
    delay(15); 
  }

  file.close();
  tempFile.close();

  SD_MMC.remove("/unsent.csv");
  if (SD_MMC.exists("/temp.csv")) {
    File check = SD_MMC.open("/temp.csv", FILE_READ);
    if (check.size() > 0) {
      check.close();
      SD_MMC.rename("/temp.csv", "/unsent.csv");
    } else {
      check.close();
      SD_MMC.remove("/temp.csv");
    }
  }
}

void publishGPSData(TelemetryData tData) {
  if (!tData.isValid) return;

  callbackReceived = false;
  esp_err_t result = esp_now_send(receiverAddress, (uint8_t *) &tData, sizeof(tData));
  
  if (result == ESP_OK) {
    unsigned long waitStart = millis();
    while (!callbackReceived && (millis() - waitStart < 60)) { delay(1); }

    if (lastDeliverySuccess) {
      flushBacklog();
    }
  }
}

// --- Setup & Loop ---

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\n\n--- Tracker + Sensors---\n"));

  // 1. INITIALIZE ADC & I2C FIRST
  for (int i = 0; i < FILTER_SIZE; i++) currentReadings[i] = 0;
  analogReadResolution(12);
  analogSetPinAttenuation(CURRENT_SENSOR_PIN, ADC_11db);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setTimeOut(100); 

  M5.begin();
  while (!Vmeter.begin(&Wire, M5_UNIT_VMETER_I2C_ADDR, 17, 18, 400000U)) {
      Serial.println("Unit Vmeter Init Fail");
      delay(1000);
  }
  Vmeter.setEEPROMAddr(M5_UNIT_VMETER_EEPROM_I2C_ADDR);
  Vmeter.setMode(ADS1115_MODE_SINGLESHOT);
  Vmeter.setRate(ADS1115_RATE_8);
  Vmeter.setGain(ADS1115_PGA_2048);
  resolution = Vmeter.getCoefficient() / M5_UNIT_VMETER_PRESSURE_COEFFICIENT;
  calibration_factor = Vmeter.getFactoryCalibration();

  // 2. CALIBRATE CURRENT SENSOR (Modem is still OFF)
  Serial.println("Calibrating current sensor...");
  
  float sum = 0;
  for (int i = 0; i < 500; i++) {
      sum += readRawCurrentVoltage();
      delay(5); 
  }
  WCS1500_OFFSET = sum / 500.0;
  
  // Sanity Check Logic Ported from Test Script
  float expectedZero = VDD / 2.0; 
  if (WCS1500_OFFSET < (expectedZero - 0.8) || WCS1500_OFFSET > (expectedZero + 0.5)) {
    Serial.println("WARNING: Zero offset out of expected range! (Is system under load?)");
    Serial.println("Reverting to mathematical ideal zero...");
    WCS1500_OFFSET = expectedZero;
  } else {
    Serial.println("Calibration OK!");
  }
  Serial.printf("Current Calibrated. Zero offset saved as: %.3f V\n", WCS1500_OFFSET);


  // 3. WAKE UP THE MODEM (Now safe to draw spikes)
  pinMode(IO_GSM_RST, OUTPUT);
  digitalWrite(IO_GSM_RST, LOW);
  pinMode(IO_GSM_PWRKEY, OUTPUT);
  digitalWrite(IO_GSM_PWRKEY, HIGH);
  delay(3000);
  digitalWrite(IO_GSM_PWRKEY, LOW);
  
  Serial.println("Modem power key triggered. LED should be ON.");
  unsigned long bootWaitStart = millis();

  while(millis() - bootWaitStart < 12000) {
      delay(10);
  }

  // 4. ACTIVATE GPS & SD CARD
  modem.begin(115200, SERIAL_8N1, IO_RXD2, IO_TXD2);
  Serial.println("Activating GNSS Engine...");
  sendATCommand("AT+CGNSSPWR=1", 1000, DEBUG);
  delay(1000);

  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
  if (!SD_MMC.begin("/sdcard", true, true)) {
    Serial.println("SD Card Mount Failed! Check pins.");
  } else {
    Serial.println("SD Mounted. System running master logging file.");
  }
  
  Serial.println("Setup Complete. Searching for GNSS satellites...");
}

void loop() {
  currentTime = millis();
  
  // 1. 5Hz Timer (Every 200ms) - Fetch & Update Sensors
  if (currentTime - lastLogTime >= 200) {
    lastLogTime = currentTime;
    
    String response = sendATCommand("AT+CGNSSINFO", 150, DEBUG); 
    TelemetryData newData = parseGPSResponse(response);
    
    newData.timestamp = millis();
    newData.voltage = getBatteryVoltage();
    newData.current = getFilteredCurrent();
    
    // Only process SD and Wi-Fi if we have a valid GPS lock
    if (newData.isValid) {
      latestData = newData; 
      saveToSD(latestData); 

      // --- DYNAMIC WI-FI BOOT ---
      if (!espNowInitialized) {
        Serial.println("\n*** GPS LOCK ACHIEVED! ***");
        Serial.println("Initializing Wi-Fi and ESP-NOW...");
        
        WiFi.mode(WIFI_STA);
        if (esp_now_init() == ESP_OK) {
          esp_now_register_send_cb(OnDataSent);
          memcpy(peerInfo.peer_addr, receiverAddress, 6);
          peerInfo.channel = 0;  
          peerInfo.encrypt = false;      
          esp_now_add_peer(&peerInfo);
          
          espNowInitialized = true;
          Serial.println("ESP-NOW Active. Broadcasting Telemetry.");
        } else {
          Serial.println("ESP-NOW Init Failed during dynamic boot.");
        }
      }
    }
  }

  // 2. 1Hz Timer (Every 1000ms) - Transmit over ESP-NOW
  if (currentTime - lastTxTime >= 1000) {
    lastTxTime = currentTime;
    
    if (espNowInitialized && latestData.isValid) {
      publishGPSData(latestData);
    }
  }
}