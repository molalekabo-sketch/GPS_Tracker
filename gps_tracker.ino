#include <Arduino.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <PubSubClient.h>

// Developer Hardware Pin Definitions
#define IO_RXD2        47   // ESP32-S3 RX <- Modem TX
#define IO_TXD2        48   // ESP32-S3 TX -> Modem RX
#define IO_GSM_PWRKEY  4    // Power Key Pin
#define IO_GSM_RST     5    // Reset Pin

#define DEBUG true

// --- Wi-Fi Credentials (ACTIVE FOR TESTING) ---
const char* ssid     = "Android LC";
const char* password = "ncde1975";

// --- MQTT Configuration ---
const char MQTT_BROKER[] = "broker.hivemq.com";
const int MQTT_PORT = 1883;

// Publish each GPS field on its own topic
const char MQTT_TOPIC_LAT[] = "SimuTech/gps/latitude";
const char MQTT_TOPIC_LON[] = "SimuTech/gps/longitude";
const char MQTT_TOPIC_ALT[] = "SimuTech/gps/altitude";
const char MQTT_TOPIC_SPEED[] = "SimuTech/gps/speed";
const char MQTT_CLIENT_ID[] = "ESP32S3_A7670_GPS_Tracker";
const char MQTT_APN[] = "internet";  // Change to your SIM provider's APN

HardwareSerial modem(2);    // Use Serial2 for the hardware connection

// --- Wi-Fi MQTT Objects (ACTIVE FOR TESTING) ---
WiFiClient espClient;
PubSubClient client(espClient);

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
  GPSData data = {0, 0, 0, 0, false};
  
  int startIndex = response.indexOf("+CGNSSINFO:");
  if (startIndex == -1) {
    Serial.println("GPS data not found in response");
    return data;
  }
  
  String dataStr = response.substring(startIndex + 11); 
  
  // The A7670E returns a lot of commas. We need to parse at least 15 parts.
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
  
  // A7670E format places Latitude at index 5 and Longitude at index 7.
  // Check if Latitude field has data (meaning we have a satellite fix)
  if (parts[5].length() > 2) {
    
    // 1. Latitude (Index 5, N/S is Index 6)
    float lat = parts[5].toFloat();
    if (parts[6] == "S") lat = lat * -1.0; // South is negative
    data.latitude = lat;
    
    // 2. Longitude (Index 7, E/W is Index 8)
    float lon = parts[7].toFloat();
    if (parts[8] == "W") lon = lon * -1.0; // West is negative
    data.longitude = lon;

    // 3. Altitude and Speed
    data.altitude = parts[11].toFloat();
    data.speed = parts[12].toFloat();
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

// --- WI-FI SETUP ROUTINE (ACTIVE FOR TESTING) ---
void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to Wi-Fi: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA); 
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi Connected.");
}

// --- WI-FI MQTT RECONNECT ROUTINE (ACTIVE FOR TESTING) ---
void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting Wi-Fi MQTT connection...");
    if (client.connect(MQTT_CLIENT_ID)) {
      Serial.println("connected!");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

/**
 * Publish GPS data
 */
void publishGPSData(GPSData gpsData)
{
  if (!gpsData.isValid) {
    Serial.println("Cannot publish invalid GPS data");
    return;
  }

  Serial.println("\n--- Publishing JSON GPS Data ---");

  // Create one unified JSON string
  char payload[256];
  snprintf(payload, sizeof(payload), 
    "{\"latitude\":%.6f,\"longitude\":%.6f,\"altitude\":%.2f,\"speed\":%.2f}",
    gpsData.latitude, gpsData.longitude, gpsData.altitude, gpsData.speed
  );

  // ========================================================
  // WI-FI MQTT PUBLISH (ACTIVE FOR TESTING)
  // ========================================================
  client.publish("SimuTech/gps/location", payload);

  // ========================================================
  // CELLULAR AT-COMMAND PUBLISH (COMMENTED OUT FOR TESTING)
  // ========================================================
  /*
  int payloadLen = strlen(payload);
  String pubCmd = "AT+CMQPUB=0,\"SimuTech/gps/location\",0,0,0," + String(payloadLen) + ",\"" + String(payload) + "\"";
  sendATCommand(pubCmd, 2000, DEBUG);
  */


  // ========================================================
  // CELLULAR AT-COMMAND PUBLISH (COMMENTED OUT FOR TESTING)
  // ========================================================
  /*
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
  */
}

void setup()
{
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\n\n--- Makerfabs ESP32-S3 A7670E Tracker (WI-FI TEST MODE) ---\n"));

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

  // ========================================================
  // WI-FI & MQTT INITIALIZATION (ACTIVE FOR TESTING)
  // ========================================================
  setup_wifi();
  client.setServer(MQTT_BROKER, MQTT_PORT);


  // ========================================================
  // CELLULAR NETWORK & MQTT INIT (COMMENTED OUT FOR TESTING)
  // ========================================================
  /*
  Serial.println("\nGetting modem information...");
  sendATCommand("AT+CICCID", 1000, DEBUG);
  delay(500);
  sendATCommand("AT+SIMCOMATI", 1000, DEBUG);
  delay(500);
  sendATCommand("AT+COPS?", 1000, DEBUG);
  delay(500);
  sendATCommand("AT+GMR", 1000, DEBUG);
  delay(500);

  Serial.println("\nConfiguring cellular network...");
  String apnCmd = "AT+CGDCONT=1,\"IP\",\"" + String(MQTT_APN) + "\"";
  sendATCommand(apnCmd, 2000, DEBUG);
  delay(500);

  sendATCommand("AT+CGREG?", 2000, DEBUG);
  delay(1000);

  Serial.println("\n[MQTT] Initializing MQTT connection via 4G Modem...");
  String mqNewCmd = "AT+CMQNEW=\"" + String(MQTT_BROKER) + "\",\"" + String(MQTT_PORT) + "\",60,1024";
  sendATCommand(mqNewCmd, 4000, DEBUG);
  delay(1000);

  String mqConCmd = "AT+CMQCON=0,3,\"" + String(MQTT_CLIENT_ID) + "\",60,1,0";
  sendATCommand(mqConCmd, 5000, DEBUG);
  delay(1000);
  */

  // ========================================================
  // GNSS/GPS INITIALIZATION (MUST REMAIN ACTIVE)
  // ========================================================
  Serial.println("\n[GPS] Enabling GNSS Engine...");
  sendATCommand("AT+CGNSSPWR=1", 1000, DEBUG);
  delay(2000);

  Serial.println("Waiting for GPS to acquire position...");
  Serial.println("This may take 30-60 seconds for first fix (ensure you are outside!)...");
  delay(12000);

  sendATCommand("AT+CGNSSIPR=9600", 1000, DEBUG);
  delay(500);
  sendATCommand("AT+CGNSSTST=1", 1000, DEBUG);
  delay(500);

  Serial.println("\nQuerying initial GPS data...");
  sendATCommand("AT+CGNSSINFO", 1000, DEBUG);

  currentTime = millis();
  lastPublishTime = millis();
  Serial.println("\n--- Setup Complete - Entering Loop ---\n");
}

void loop()
{
  // --- WI-FI MQTT KEEP-ALIVE ---
  if (!client.connected()) {
    reconnect();
  }
  client.loop();


  // --- QUERY GPS AND PUBLISH ---
  if (millis() - currentTime > 5000)
  {
    currentTime = millis(); 
    Serial.println("\n--- Querying GPS Data ---");
    String gpsResponse = sendATCommand("AT+CGNSSINFO", 1000, DEBUG);
    
    GPSData gpsData = parseGPSResponse(gpsResponse);
    if (gpsData.isValid) {
      publishGPSData(gpsData);
    }
  }

  // --- SERIAL PASS-THROUGH FOR DEBUGGING ---
  while (Serial.available() > 0) {
    modem.write(Serial.read());
    yield();
  }

  while (modem.available() > 0) {
    Serial.write(modem.read());
    yield();
  }
}