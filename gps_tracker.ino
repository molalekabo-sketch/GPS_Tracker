#include <Arduino.h>
#include <HardwareSerial.h>
#include <WiFi.h>

// Developer Hardware Pin Definitions
#define IO_RXD2        47   // ESP32-S3 RX <- Modem TX
#define IO_TXD2        48   // ESP32-S3 TX -> Modem RX
#define IO_GSM_PWRKEY  4    // Power Key Pin
#define IO_GSM_RST     5    // Reset Pin

#define DEBUG true

// --- Wi-Fi Credentials ---
const char* ssid     = "MolaleK";
const char* password = "ncde1975";

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to network: ");
  Serial.println(ssid);

  // 1. Set ESP32 Wi-Fi mode to Station (Client mode)
  WiFi.mode(WIFI_STA); 
  
  // 2. Start the connection process
  WiFi.begin(ssid, password);
}

// MQTT Configuration (HiveMQ Cloud - 4G Modem Native MQTT)
// Public MQTT broker (no TLS)
const char MQTT_BROKER[] = "broker.hivemq.com";
const int MQTT_PORT = 1883;

// Public broker credentials not required
const char MQTT_USER[] = "";
const char MQTT_PASS[] = "";
// Publish each GPS field on its own topic
const char MQTT_TOPIC_LAT[] = "SimuTech/gps/latitude";
const char MQTT_TOPIC_LON[] = "SimuTech/gps/longitude";
const char MQTT_TOPIC_ALT[] = "SimuTech/gps/altitude";
const char MQTT_TOPIC_SPEED[] = "SimuTech/gps/speed";
const char MQTT_CLIENT_ID[] = "ESP32S3_A7670_GPS_Tracker";
const char MQTT_APN[] = "internet";  // Change to your SIM provider's APN

HardwareSerial modem(2);    // Use Serial2 for the hardware connection

unsigned long currentTime;
unsigned long lastPublishTime = 0;

struct GPSData {
  float latitude;
  float longitude;
  float altitude;
  float speed;
  bool isValid;
};

/**
 * Send AT command to modem and wait for response
 * Developer's approach: captures full response
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
 * Format: +CGNSSINFO: <run>,<fix>,<date>,<time>,<lat>,<lon>,<alt>,<speed>,<course>,...
 */
GPSData parseGPSResponse(String response)
{
  GPSData data = {0, 0, 0, 0, false};
  
  // Look for +CGNSSINFO: in response
  int startIndex = response.indexOf("+CGNSSINFO:");
  if (startIndex == -1) {
    Serial.println("GPS data not found in response");
    return data;
  }
  
  // Extract the data portion
  String dataStr = response.substring(startIndex + 11); // Skip "+CGNSSINFO:"
  
  // Split by comma and extract values
  int commaCount = 0;
  int prevComma = 0;
  int currentComma = 0;
  String parts[9];
  
  while (currentComma != -1 && commaCount < 9) {
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
  
  // Check if we have valid GPS fix (fix_stat should be 1, 2, 3, or 4)
  int fixStatus = parts[1].toInt();
  if (fixStatus > 0) {
    data.latitude = parts[4].toFloat();
    data.longitude = parts[5].toFloat();
    data.altitude = parts[6].toFloat();
    data.speed = parts[7].toFloat();
    data.isValid = true;
    
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
 * Publish GPS data to MQTT broker using modem's native MQTT
 * Format: AT+CMQPUB=client_index,"topic",qos,retain,dup,message_len,"message"
 */
void publishGPSData(GPSData gpsData)
{
  if (!gpsData.isValid) {
    Serial.println("Cannot publish invalid GPS data");
    return;
  }

  // Create JSON payload with Google Maps URL
  char payload[256];
  snprintf(payload, sizeof(payload),
    "{\"latitude\":%.6f,\"longitude\":%.6f,\"altitude\":%.2f,\"speed\":%.2f,\"maps_url\":\"https://maps.google.com/?q=%.6f,%.6f\"}",
    gpsData.latitude, gpsData.longitude, gpsData.altitude, gpsData.speed,
    gpsData.latitude, gpsData.longitude
  );

  int msgLen = strlen(payload);
  
  // Build AT command for MQTT publish
  // Format: AT+CMQPUB=0,"topic",qos,retain,dup,len,"message"
  // Publish the 4 fields as separate topics with plain string values.

  char valueBuf[32];

  // Latitude
  snprintf(valueBuf, sizeof(valueBuf), "%.6f", gpsData.latitude);
  int latLen = strlen(valueBuf);
  String pubLatCmd = "AT+CMQPUB=0,\"" + String(MQTT_TOPIC_LAT) + "\",0,0,0," + String(latLen) + ",\"" + String(valueBuf) + "\"";
  sendATCommand(pubLatCmd, 2000, DEBUG);

  // Longitude
  snprintf(valueBuf, sizeof(valueBuf), "%.6f", gpsData.longitude);
  int lonLen = strlen(valueBuf);
  String pubLonCmd = "AT+CMQPUB=0,\"" + String(MQTT_TOPIC_LON) + "\",0,0,0," + String(lonLen) + ",\"" + String(valueBuf) + "\"";
  sendATCommand(pubLonCmd, 2000, DEBUG);

  // Altitude
  snprintf(valueBuf, sizeof(valueBuf), "%.2f", gpsData.altitude);
  int altLen = strlen(valueBuf);
  String pubAltCmd = "AT+CMQPUB=0,\"" + String(MQTT_TOPIC_ALT) + "\",0,0,0," + String(altLen) + ",\"" + String(valueBuf) + "\"";
  sendATCommand(pubAltCmd, 2000, DEBUG);

  // Speed
  snprintf(valueBuf, sizeof(valueBuf), "%.2f", gpsData.speed);
  int speedLen = strlen(valueBuf);
  String pubSpeedCmd = "AT+CMQPUB=0,\"" + String(MQTT_TOPIC_SPEED) + "\",0,0,0," + String(speedLen) + ",\"" + String(valueBuf) + "\"";
  sendATCommand(pubSpeedCmd, 2000, DEBUG);

  // (Single JSON publish removed)
  
  
  Serial.println("\n--- Publishing GPS Data via 4G MQTT ---");
  sendATCommand(pubCmd, 2000, DEBUG);
}

void setup()
{
  // Initialize USB Serial for debugging
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\n\n--- Makerfabs ESP32-S3 A7670E GPS Tracker (4G MQTT) ---\n"));

  // Set up modem control pins (Developer's approach)
  pinMode(IO_GSM_RST, OUTPUT);
  digitalWrite(IO_GSM_RST, LOW);
  
  pinMode(IO_GSM_PWRKEY, OUTPUT);
  digitalWrite(IO_GSM_PWRKEY, HIGH);
  delay(3000);
  digitalWrite(IO_GSM_PWRKEY, LOW);

  // Initialize hardware serial connection to modem
  modem.begin(115200, SERIAL_8N1, IO_RXD2, IO_TXD2);
  delay(500);

  // Wait for modem to fully boot
  Serial.println("Waiting for modem to boot...");
  delay(12000);

  // Test basic AT command responsiveness
  Serial.println("Testing modem connection...");
  sendATCommand("AT", 1000, DEBUG);
  delay(500);
  sendATCommand("AT", 1000, DEBUG);
  delay(500);

  // Get modem information
  Serial.println("\nGetting modem information...");
  sendATCommand("AT+CICCID", 1000, DEBUG);
  delay(500);
  sendATCommand("AT+SIMCOMATI", 1000, DEBUG);
  delay(500);
  sendATCommand("AT+COPS?", 1000, DEBUG);
  delay(500);
  sendATCommand("AT+GMR", 1000, DEBUG);
  delay(500);

  // Configure network APN
  Serial.println("\nConfiguring cellular network...");
  String apnCmd = "AT+CGDCONT=1,\"IP\",\"" + String(MQTT_APN) + "\"";
  sendATCommand(apnCmd, 2000, DEBUG);
  delay(500);

  // Verify network registration
  sendATCommand("AT+CGREG?", 2000, DEBUG);
  delay(1000);

  // Initialize MQTT via modem's native MQTT
  Serial.println("\n[MQTT] Initializing MQTT connection via 4G Modem...");
  
  // Create MQTT instance: AT+CMQNEW="broker","port",keep_alive,buffer_size
  String mqNewCmd = "AT+CMQNEW=\"" + String(MQTT_BROKER) + "\",\"" + String(MQTT_PORT) + "\",60,1024";
  sendATCommand(mqNewCmd, 4000, DEBUG);
  delay(1000);

  // Connect to MQTT broker with credentials
  // Format: AT+CMQCON=client_index,mqtt_version,"client_id",keep_alive,clean_session,will_flag
  String mqConCmd = "AT+CMQCON=0,3,\"" + String(MQTT_CLIENT_ID) + "\",60,1,0";
  sendATCommand(mqConCmd, 5000, DEBUG);
  delay(1000);

  // Enable and initialize GPS/GNSS Engine
  Serial.println("\n[GPS] Enabling GNSS Engine...");
  sendATCommand("AT+CGNSSPWR=1", 1000, DEBUG);
  delay(2000);

  // Wait for GPS to be ready
  Serial.println("Waiting for GPS to acquire position...");
  Serial.println("This may take 30-60 seconds for first fix...");
  delay(12000);

  // Configure GPS parameters
  sendATCommand("AT+CGNSSIPR=9600", 1000, DEBUG);
  delay(500);
  sendATCommand("AT+CGNSSTST=1", 1000, DEBUG);
  delay(500);

  // Get initial GPS information
  Serial.println("\nQuerying initial GPS data...");
  sendATCommand("AT+CGNSSINFO", 1000, DEBUG);

  currentTime = millis();
  lastPublishTime = millis();
  Serial.println("\n--- Setup Complete - Entering Loop ---\n");
}

void loop()
{
  // Query and publish GPS position data every 5 seconds
  if (millis() - currentTime > 5000)
  {
    currentTime = millis(); // Refresh timer
    Serial.println("\n--- Querying GPS Data ---");
    String gpsResponse = sendATCommand("AT+CGNSSINFO", 1000, DEBUG);
    
    // Parse GPS response and publish to MQTT
    GPSData gpsData = parseGPSResponse(gpsResponse);
    if (gpsData.isValid) {
      publishGPSData(gpsData);
    }
  }

  // Pass-through serial communication between USB and modem
  // Allows manual AT commands from Serial Monitor
  while (Serial.available() > 0) {
    modem.write(Serial.read());
    yield();
  }

  while (modem.available() > 0) {
    Serial.write(modem.read());
    yield();
  }
}
