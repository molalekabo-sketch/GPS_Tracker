#include <WiFi.h>
#include <esp_now.h>

// ============================================================================
// 1. TELEMETRY DATA STRUCTURE
// ============================================================================
// MUST EXACTLY MATCH THE TRANSMITTER STRUCT TO ALIGN MEMORY
struct TelemetryData {
  unsigned long timestamp;
  float latitude;
  float longitude;
  float altitude;
  float speed;
  bool isValid;     // Used internally by Tx, ignored in output
  bool isBacklog;
  float voltage;
  float current;
};

TelemetryData incomingData;

// ============================================================================
// 2. ESP-NOW RECEIVE CALLBACK
// ============================================================================
// Updated signature for ESP32 Arduino Core v3.x compatibility
// ============================================================================
// 2. ESP-NOW RECEIVE CALLBACK
// ============================================================================
void OnDataRecv(const esp_now_recv_info_t *esp_now_info, const uint8_t *incomingDataPtr, int len) {
  
  // IF IT IS TELEMETRY: Verify the payload size matches our struct
  if (len == sizeof(TelemetryData)) {
    memcpy(&incomingData, incomingDataPtr, sizeof(incomingData));

    // Format strictly as the 8-field CSV: timestamp,lat,lon,alt,speed,isBacklog,voltage,current
    Serial.printf("%lu,%.6f,%.6f,%.2f,%.2f,%s,%.2f,%.1f\n",
                  incomingData.timestamp,
                  incomingData.latitude,
                  incomingData.longitude,
                  incomingData.altitude,
                  incomingData.speed,
                  incomingData.isBacklog ? "true" : "false",
                  incomingData.voltage,
                  incomingData.current);
                  
  } 
  // IF IT IS TEXT: Handle setup/configuration messages
  else if (len > 0 && len < 200) {
    char msg[200];
    int msgLen = len < 199 ? len : 199;
    memcpy(msg, incomingDataPtr, msgLen);
    msg[msgLen] = '\0'; // Ensure the string is safely null-terminated

    // Prefix with "MSG," so the Node.js parser can easily filter it from the CSV telemetry
    Serial.printf("MSG,%s\n", msg);
  }
}

// ============================================================================
// 3. SETUP & LOOP
// ============================================================================
void setup() {
  // Initialize Serial Monitor at exactly the baud rate Node.js expects
  Serial.begin(115200);

  // Set device as a Wi-Fi Station
  WiFi.mode(WIFI_STA);

  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  // Register the receive callback function
  esp_now_register_recv_cb(OnDataRecv);
}

void loop() {
  // The ESP32 does nothing in the main loop. 
  // All data reception is handled asynchronously by the OnDataRecv callback in the background.
  delay(1000);
}