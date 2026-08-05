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
uint32_t currentSequence = 0;

// Flag and status updated by the ESP-NOW callback
volatile bool callbackReceived = false;
volatile bool lastDeliverySuccess = false;

String currentLogFilename = "";
bool sdMounted = false;

#define MSG_GPS 0
#define MSG_REQUEST_BACKLOG 1
#define MSG_BACKLOG_EMPTY 2

// Updated payload structure (includes msgType, isBacklog and sequence number)
struct GPSData {
  uint8_t msgType;
  uint8_t isBacklog;
  uint8_t reserved[2];
  uint32_t sequence;
  float latitude;
  float longitude;
  float altitude;
  float speed;
  bool isValid;
};

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len);
void sendBacklogEmptyAck(const uint8_t * mac_addr);

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

String normalizeTimestampForFilename(const String &raw)
{
  String normalized = "";
  for (unsigned int i = 0; i < raw.length(); ++i)
  {
    char c = raw[i];
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-') {
      normalized += c;
    } else if (c == ' ' || c == ':' || c == '/' || c == '.' || c == ',') {
      normalized += '_';
    }
  }
  if (normalized.length() == 0) {
    normalized = "boot_" + String(millis());
  }
  return normalized;
}

String makeBootLogFileName(const String &timeHint)
{
  String safeName = timeHint;
  if (safeName.length() == 0) {
    safeName = "boot_" + String(millis());
  }
  safeName = normalizeTimestampForFilename(safeName);
  if (!safeName.startsWith("track_")) {
    safeName = "track_" + safeName;
  }
  return "/" + safeName + ".csv";
}

String parseTimestampFromCclk(const String &response)
{
  int index = response.indexOf("+CCLK:");
  if (index == -1) return "";
  int firstQuote = response.indexOf('"', index);
  int secondQuote = response.indexOf('"', firstQuote + 1);
  if (firstQuote == -1 || secondQuote == -1) return "";
  String raw = response.substring(firstQuote + 1, secondQuote);
  raw.trim();
  return raw;
}

String parseTimestampFromCgnssInfo(const String &response)
{
  int startIndex = response.indexOf("+CGNSSINFO:");
  if (startIndex == -1) return "";
  String dataStr = response.substring(startIndex + 11);
  dataStr.trim();

  int prev = 0;
  while (prev < dataStr.length()) {
    int next = dataStr.indexOf(',', prev);
    String token = (next == -1) ? dataStr.substring(prev) : dataStr.substring(prev, next);
    token.trim();

    if (token.length() >= 10) {
      bool hasDigit = false;
      for (unsigned int i = 0; i < token.length(); ++i) {
        if (isDigit(token[i])) {
          hasDigit = true;
          break;
        }
      }
      if (hasDigit) {
        return token;
      }
    }

    if (next == -1) break;
    prev = next + 1;
  }

  return "";
}

String getBootTimestampHint()
{
  String response = sendATCommand("AT+CCLK?", 1000, DEBUG);
  String timestamp = parseTimestampFromCclk(response);
  if (timestamp.length()) {
    return timestamp;
  }

  response = sendATCommand("AT+CGNSSINFO", 2000, DEBUG);
  return parseTimestampFromCgnssInfo(response);
}

String normalizeTimestampForCsv(String raw)
{
  String out = raw;
  out.replace(",", " ");
  out.replace("\"", "");
  out.replace(";", " ");
  return out;
}

void createBootLogFile()
{
  if (!sdMounted) return;

  String bootHint = getBootTimestampHint();
  currentLogFilename = makeBootLogFileName(bootHint);

  File file = SD.open(currentLogFilename, FILE_APPEND);
  if (!file) {
    Serial.printf("Failed to create log file '%s'\n", currentLogFilename.c_str());
    return;
  }

  if (file.size() == 0) {
    file.println("timestamp,latitude,longitude,altitude,speed,isBacklog");
  }
  file.close();
  Serial.printf("Created boot log file: %s\n", currentLogFilename.c_str());
}

void appendLogRecord(const GPSData &data, const String &recordTimestamp)
{
  if (!sdMounted || currentLogFilename.length() == 0) return;

  File file = SD.open(currentLogFilename, FILE_APPEND);
  if (!file) {
    Serial.printf("Unable to append to log file '%s'\n", currentLogFilename.c_str());
    return;
  }

  String ts = normalizeTimestampForCsv(recordTimestamp);
  file.printf("%s,%.6f,%.6f,%.2f,%.2f,%u\n", ts.c_str(),
              data.latitude, data.longitude, data.altitude, data.speed,
              data.isBacklog ? 1 : 0);
  file.close();
}

/**
 * Parse CGNSSINFO response and extract GPS coordinates
 */
struct GPSData parseGPSResponse(String response)
{
  GPSData data;
  data.msgType = MSG_GPS;
  data.isBacklog = 0;
  data.reserved[0] = data.reserved[1] = 0;
  data.sequence = 0;
  data.latitude = 0;
  data.longitude = 0;
  data.altitude = 0;
  data.speed = 0;
  data.isValid = false;
  
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

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len) {
  if (len < 1) return;

  uint8_t msgType = incomingData[0];
  if (msgType == MSG_REQUEST_BACKLOG) {
    Serial.println("Received backlog request from receiver.");
    const uint8_t *mac_addr = (info && info->src_addr) ? info->src_addr : receiverAddress;
    if (SD.exists("/unsent.csv")) {
      File check = SD.open("/unsent.csv", FILE_READ);
      if (check && check.size() > 0) {
        check.close();
        flushBacklog();
      } else {
        if (check) check.close();
        sendBacklogEmptyAck(mac_addr);
      }
    } else {
      sendBacklogEmptyAck(mac_addr);
    }
  }
}

void sendBacklogEmptyAck(const uint8_t * mac_addr) {
  GPSData ack = {};
  ack.msgType = MSG_BACKLOG_EMPTY;
  ack.isBacklog = 0;
  ack.reserved[0] = ack.reserved[1] = 0;
  ack.sequence = 0;
  ack.latitude = 0;
  ack.longitude = 0;
  ack.altitude = 0;
  ack.speed = 0;
  ack.isValid = true;

  esp_err_t result = esp_now_send(mac_addr, (uint8_t *)&ack, sizeof(ack));
  Serial.printf("Sent backlog-empty response (%d)\n", result);
}

/**
 * Append unsent GPS frame to SD card CSV
 */
void saveToSD(GPSData data, const String &recordTimestamp) {
  File file = SD.open("/unsent.csv", FILE_APPEND);
  if (file) {
    String ts = normalizeTimestampForCsv(recordTimestamp);
    file.printf("%s,%u,%.6f,%.6f,%.2f,%.2f\n", ts.c_str(), data.sequence, data.latitude, data.longitude, data.altitude, data.speed);
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

    // Parse CSV line into payload struct (timestamp is preserved but not sent)
    GPSData backlogData;
    backlogData.msgType = MSG_GPS;
    backlogData.isBacklog = 1;
    backlogData.reserved[0] = backlogData.reserved[1] = 0;
    backlogData.isValid = true;

    String parts[6];
    int partIndex = 0;
    int start = 0;
    while (partIndex < 6) {
      int comma = line.indexOf(',', start);
      if (comma == -1) {
        parts[partIndex++] = line.substring(start);
        break;
      }
      parts[partIndex++] = line.substring(start, comma);
      start = comma + 1;
    }

    if (partIndex >= 6) {
      backlogData.sequence = currentSequence++;
      if (parts[1].length() > 0 && parts[1].indexOf('.') == -1 && parts[1].indexOf('-') == -1 && parts[1].indexOf('+') == -1 && parts[1].toInt() >= 0) {
        backlogData.sequence = parts[1].toInt();
        backlogData.latitude = parts[2].toFloat();
        backlogData.longitude = parts[3].toFloat();
        backlogData.altitude = parts[4].toFloat();
        backlogData.speed = parts[5].toFloat();
      } else {
        backlogData.latitude = parts[1].toFloat();
        backlogData.longitude = parts[2].toFloat();
        backlogData.altitude = parts[3].toFloat();
        backlogData.speed = parts[4].toFloat();
        backlogData.isBacklog = parts[5].toInt() != 0;
      }
    } else if (partIndex >= 4) {
      backlogData.sequence = currentSequence++;
      backlogData.latitude = parts[0].toFloat();
      backlogData.longitude = parts[1].toFloat();
      backlogData.altitude = parts[2].toFloat();
      backlogData.speed = parts[3].toFloat();
    } else {
      continue;
    }

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
      tempFile.println(line);

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
void publishGPSData(GPSData gpsData, const String &recordTimestamp)
{
  if (!gpsData.isValid) {
    Serial.println("Cannot transmit invalid GPS data");
    return;
  }

  gpsData.msgType = MSG_GPS;
  gpsData.isBacklog = 0;
  gpsData.reserved[0] = gpsData.reserved[1] = 0;
  gpsData.sequence = currentSequence++;

  appendLogRecord(gpsData, recordTimestamp);
  saveToSD(gpsData, recordTimestamp);

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
    } else {
      Serial.println("Delivery failed at receiver end. Backing up to SD...");
      saveToSD(gpsData, recordTimestamp);
    }
  } else {
    Serial.println("Error triggering ESP-NOW stack. Backing up to SD...");
    saveToSD(gpsData, recordTimestamp);
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
  sdMounted = SD.begin(SD_CS_PIN);
  if (!sdMounted) {
    Serial.println("SD Card initialization failed! Check CS pin assignment.");
  } else {
    Serial.println("SD Card mounted successfully.");
    createBootLogFile();
  }

  // ESP-NOW SETUP
  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW stack");
    return;
  }

  esp_now_register_send_cb(OnDataSent);
  esp_now_register_recv_cb(OnDataRecv);

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
    String dataTimestamp = parseTimestampFromCgnssInfo(response);
    GPSData gpsData = parseGPSResponse(response);
    
    publishGPSData(gpsData, dataTimestamp);
  }
}