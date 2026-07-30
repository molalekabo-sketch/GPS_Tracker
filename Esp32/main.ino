#include <Arduino.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <esp_now.h>
#include <SPI.h>
#include <SD.h>

// --- Hardware Pin Definitions ---
#define IO_RXD2        47   // ESP32-S3 RX <- Modem TX
#define IO_TXD2        48   // ESP32-S3 TX -> Modem RX
#define IO_GSM_PWRKEY  4    // Power Key Pin
#define IO_GSM_RST     5    // Reset Pin

// --- SD Card Pin (Adjust CS for your specific board layout if needed) ---
#define SD_CS_PIN      10   

#define DEBUG true

// --- Target ESP-NOW Receiver Configuration ---
uint8_t receiverAddress[] = {0x14, 0xC1, 0x9F, 0xCB, 0x66, 0x70};
esp_now_peer_info_t peerInfo;

HardwareSerial modem(2);    // Use Serial2 for hardware connection

unsigned long currentTime;
unsigned long lastPublishTime = 0;

// Flag and status updated by the ESP-NOW callback
volatile bool callbackReceived = false;
volatile bool lastDeliverySuccess = false;

// Updated payload structure (includes isBacklog flag so receiver knows history vs live)
struct GPSData {
  float latitude;
  float longitude;
  float altitude;
  float speed;
  bool isValid;
  bool isBacklog; 
};

/**
 * Send AT command to modem and wait for response
 */
String sendATCommand(String command, const int timeout, boolean debug)
{
  String response = "";
  modem.println(command);
  long int time = millis();

  while ((time + timeout) > millis())
  {
    while (modem.available())
    {
      char c = modem.read();
      response += c;
    }
  }

  if (debug)
  {
    Serial.print(response);
  }

  return response;
}

/**
 * Parse CGNSSINFO response and extract GPS coordinates
 */
struct GPSData parseGPSResponse(String response)
{
  GPSData data = {0, 0, 0, 0, false, false};
  
  int startIndex = response.indexOf("+CGNSSINFO:");
  if (startIndex == -1) {
    Serial.println("GPS data not found in response");
    return data;
  }
  
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
    
    Serial.print("Valid GPS Fix - Lat: ");
    Serial.print(data.latitude, 6);
    Serial.print(", Lon: ");
    Serial.println(data.longitude, 6);
  } else {
    Serial.println("Waiting for GPS fix...");
  }
  
  return data;
}

/**
 * Callback function to capture transmission confirmation
 */
// --- CORRECT FOOTPRINT FOR ESP32 CORE 3.X ---
// --- COMPATIBLE WITH ESP32 ARDUINO CORE 3.3.10 ---
void OnDataSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  // Check the delivery status using the explicit argument passed by the core
  lastDeliverySuccess = (status == ESP_NOW_SEND_SUCCESS);
  callbackReceived = true;
  
  Serial.print("ESP-NOW Link Status:\t");
  Serial.println(lastDeliverySuccess ? "Delivery Success" : "Delivery Fail");
}



/**
 * Append unsent GPS frame to SD card CSV
 */
void saveToSD(GPSData data) {
  File file = SD.open("/unsent.csv", FILE_APPEND);
  if (file) {
    file.printf("%.6f,%.6f,%.2f,%.2f\n", data.latitude, data.longitude, data.altitude, data.speed);
    file.close(); // Force write to physical storage
    Serial.println("-> Saved point to SD card backlog.");
  } else {
    Serial.println("-> SD write error!");
  }
}

/**
 * Read and transmit accumulated offline points from SD card
 */
void flushBacklog() {
  if (!SD.exists("/unsent.csv")) return;

  File file = SD.open("/unsent.csv", FILE_READ);
  if (!file || file.size() == 0) {
    if (file) file.close();
    SD.remove("/unsent.csv");
    return;
  }

  Serial.println("\n--- Syncing historical SD card records over ESP-NOW ---");
  File tempFile = SD.open("/temp.csv", FILE_WRITE);

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    // Parse CSV line into payload struct
    GPSData backlogData;
    sscanf(line.c_str(), "%f,%f,%f,%f", 
           &backlogData.latitude, &backlogData.longitude, 
           &backlogData.altitude, &backlogData.speed);
    
    backlogData.isValid = true;
    backlogData.isBacklog = true;

    // Attempt transmission
    callbackReceived = false;
    esp_err_t result = esp_now_send(receiverAddress, (uint8_t *)&backlogData, sizeof(backlogData));

    // Wait for callback receipt
    unsigned long waitStart = millis();
    while (!callbackReceived && (millis() - waitStart < 60)) {
      delay(1);
    }

    // If packet delivery failed, hold unsent points in temp file and abort flush loop
    if (result != ESP_OK || !lastDeliverySuccess) {
      Serial.println("-> Link lost during backlog flush. Preserving remaining records.");
      tempFile.printf("%.6f,%.6f,%.2f,%.2f\n", 
                      backlogData.latitude, backlogData.longitude, 
                      backlogData.altitude, backlogData.speed);

      while (file.available()) {
        tempFile.println(file.readStringUntil('\n'));
      }
      break;
    }

    delay(15); // Short delay to keep RF buffer clean
  }

  file.close();
  tempFile.close();

  // Atomically replace old file with unsent residual file
  SD.remove("/unsent.csv");
  if (SD.exists("/temp.csv")) {
    File check = SD.open("/temp.csv", FILE_READ);
    if (check.size() > 0) {
      check.close();
      SD.rename("/temp.csv", "/unsent.csv");
    } else {
      check.close();
      SD.remove("/temp.csv");
    }
  }
}

/**
 * Transmit raw GPS metrics over ESP-NOW & handle SD fallback
 */
void publishGPSData(GPSData gpsData)
{
  if (!gpsData.isValid) {
    Serial.println("Cannot transmit invalid GPS data");
    return;
  }

  Serial.println("\n--- Broadcasting GPS structural payload over ESP-NOW ---");

  callbackReceived = false;
  esp_err_t result = esp_now_send(receiverAddress, (uint8_t *) &gpsData, sizeof(gpsData));
  
  if (result == ESP_OK) {
    // Wait for physical antenna delivery confirmation callback
    unsigned long waitStart = millis();
    while (!callbackReceived && (millis() - waitStart < 60)) {
      delay(1);
    }

    if (lastDeliverySuccess) {
      Serial.println("Live frame delivered successfully.");
      // Connection confirmed active — sync any stored backlog points
      flushBacklog();
    } else {
      Serial.println("Delivery failed at receiver end. Backing up to SD...");
      saveToSD(gpsData);
    }
  } else {
    Serial.println("Error triggering ESP-NOW stack. Backing up to SD...");
    saveToSD(gpsData);
  }
}

void setup()
{
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\n\n--- Makerfabs ESP32-S3 A7670E Tracker (OFFLINE + SD BACKLOG MODE) ---\n"));

  // Set up modem control pins
  pinMode(IO_GSM_RST, OUTPUT);
  digitalWrite(IO_GSM_RST, LOW);
  
  pinMode(IO_GSM_PWRKEY, OUTPUT);
  digitalWrite(IO_GSM_PWRKEY, HIGH);
  delay(3000);
  digitalWrite(IO_GSM_PWRKEY, LOW);

  // Initialize hardware serial connection to modem
  modem.begin(115200, SERIAL_8N1, IO_RXD2, IO_TXD2);
  delay(500);

  Serial.println("Waiting for modem hardware to boot...");
  delay(12000);

  Serial.println("Testing modem serial connection...");
  sendATCommand("AT", 1000, DEBUG);
  delay(500);
  sendATCommand("AT", 1000, DEBUG);
  delay(500);

  // Turn on GNSS/GPS power inside A7670 modem
  Serial.println("Activating Modem GNSS Engine...");
  sendATCommand("AT+CGNSSPWR=1", 1000, DEBUG);
  delay(1000);

  // Initialize SD Card
  Serial.println("Initializing SD Card...");
  if (!SD.begin(SD_CS_PIN)) {
    Serial.println("SD Card initialization failed! Check CS pin assignment.");
  } else {
    Serial.println("SD Card mounted successfully.");
  }

  // ESP-NOW SETUP
  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW stack");
    return;
  }

    // Add this block inside setup() right after your esp_now_init() verification check:
  esp_now_register_send_cb(OnDataSent);
  memcpy(peerInfo.peer_addr, receiverAddress, 6);
  peerInfo.channel = 0;  
  peerInfo.encrypt = false;      
  if (esp_now_add_peer(&peerInfo) != ESP_OK){
    Serial.println("Failed to add peer");
    return;
  }
} // end setup

void loop()
{
  currentTime = millis();
  
  // Periodically query modem for coordinates every 10 seconds
  if (currentTime - lastPublishTime >= 10000) {
    lastPublishTime = currentTime;
    
    String response = sendATCommand("AT+CGNSSINFO", 2000, DEBUG);
    GPSData gpsData = parseGPSResponse(response);
    
    publishGPSData(gpsData);
  }
}