#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>

#define COORDINATE_INTERVAL_MS 10000
#define MISSING_GRACE_MS 5000
#define REQUEST_RETRY_MS 15000
#define MAX_REQUEST_RETRIES 3
#define OFFLINE_TIMEOUT_MS 30000

static const uint8_t broadcastAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

enum MessageType : uint8_t {
  MSG_GPS = 0,
  MSG_REQUEST_BACKLOG = 1,
  MSG_BACKLOG_EMPTY = 2
};

struct EspNowFrame {
  uint8_t msgType;
  uint8_t isBacklog;
  uint8_t reserved[2];
  uint32_t sequence;
  float latitude;
  float longitude;
  float altitude;
  float speed;
};

esp_now_peer_info_t peerInfo;

volatile bool sendComplete = false;
volatile bool sendSuccess = false;

unsigned long lastReceiveMillis = 0;
unsigned long nextExpectedMillis = 0;
unsigned long lastRequestMillis = 0;

uint32_t expectedSequence = 0;
bool hasSequence = false;
bool waitingForReply = false;
uint8_t requestRetries = 0;

uint8_t lastSenderMac[6] = {0};
bool hasSenderMac = false;
bool senderPeerAdded = false;

struct MissingEntry {
  uint32_t sequence;
  unsigned long expectedAt;
  bool resolved;
};

MissingEntry missingEntries[32];
size_t missingCount = 0;

void printMac(const uint8_t *mac)
{
  for (int i = 0; i < 6; ++i) {
    if (mac[i] < 0x10) Serial.print('0');
    Serial.print(mac[i], HEX);
    if (i < 5) Serial.print(':');
  }
}

void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status)
{
  sendComplete = true;
  sendSuccess = (status == ESP_NOW_SEND_SUCCESS);
}

bool macEqual(const uint8_t *a, const uint8_t *b)
{
  for (int i = 0; i < 6; ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

bool ensureSenderPeer(const uint8_t *mac)
{
  if (hasSenderMac && senderPeerAdded && macEqual(mac, lastSenderMac)) {
    return true;
  }

  memcpy(lastSenderMac, mac, 6);
  hasSenderMac = true;
  senderPeerAdded = false;

  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  esp_err_t result = esp_now_add_peer(&peerInfo);
  if (result == ESP_OK) {
    senderPeerAdded = true;
    return true;
  }

  Serial.printf("Failed to add sender peer (%d); will still try broadcast if needed\n", result);
  return false;
}

void rememberMissingSequences(uint32_t startSeq, uint32_t endSeq, unsigned long firstExpectedAt)
{
  for (uint32_t seq = startSeq; seq <= endSeq && missingCount < sizeof(missingEntries) / sizeof(missingEntries[0]); ++seq) {
    MissingEntry &entry = missingEntries[missingCount++];
    entry.sequence = seq;
    entry.expectedAt = firstExpectedAt + (seq - startSeq) * COORDINATE_INTERVAL_MS;
    entry.resolved = false;
    Serial.printf("Missing predicted seq %u at ~%lu ms\n", entry.sequence, entry.expectedAt);
  }
}

void resolveMissingSequence(uint32_t sequence)
{
  for (size_t i = 0; i < missingCount; ++i) {
    if (!missingEntries[i].resolved && missingEntries[i].sequence == sequence) {
      missingEntries[i].resolved = true;
      Serial.printf("Recovered missing seq %u from backlog\n", sequence);
      return;
    }
  }
}

void printMissingStatus()
{
  if (missingCount == 0) {
    Serial.println("No missing packets currently predicted.");
    return;
  }

  Serial.printf("Missing predicted entries: %u\n", missingCount);
  for (size_t i = 0; i < missingCount; ++i) {
    Serial.printf("  seq %u at ~%lu ms %s\n", missingEntries[i].sequence,
                  missingEntries[i].expectedAt,
                  missingEntries[i].resolved ? "(resolved)" : "(pending)");
  }
}

void clearResolvedMissingEntries()
{
  size_t writeIndex = 0;
  for (size_t readIndex = 0; readIndex < missingCount; ++readIndex) {
    if (!missingEntries[readIndex].resolved) {
      missingEntries[writeIndex++] = missingEntries[readIndex];
    }
  }
  missingCount = writeIndex;
}

void processSerialInput();
void requestBacklog();

void handleLiveGps(const EspNowFrame &frame, const uint8_t *mac_addr)
{
  unsigned long now = millis();

  if (!hasSenderMac || !macEqual(lastSenderMac, mac_addr)) {
    ensureSenderPeer(mac_addr);
  }

  if (waitingForReply) {
    waitingForReply = false;
    requestRetries = 0;
  }

  if (hasSequence) {
    if (frame.sequence == expectedSequence) {
      // All good
    } else if (frame.sequence > expectedSequence) {
      uint32_t gap = frame.sequence - expectedSequence;
      Serial.printf("Missed %u expected frame(s): expected seq %u but got %u\n", gap, expectedSequence, frame.sequence);
      unsigned long firstExpectedAt = nextExpectedMillis - COORDINATE_INTERVAL_MS;
      rememberMissingSequences(expectedSequence, frame.sequence - 1, firstExpectedAt);
      requestBacklog();
    } else {
      Serial.println("Sequence went backwards; sender likely restarted or reset.");
    }
  }

  hasSequence = true;
  expectedSequence = frame.sequence + 1;
  lastReceiveMillis = now;
  nextExpectedMillis = now + COORDINATE_INTERVAL_MS;

  Serial.print("Live GPS packet seq ");
  Serial.print(frame.sequence);
  Serial.print(frame.isBacklog ? " (backlog) " : " ");
  Serial.print("received from ");
  printMac(mac_addr);
  Serial.print(" -> Lat: ");
  Serial.print(frame.latitude, 6);
  Serial.print(", Lon: ");
  Serial.print(frame.longitude, 6);
  Serial.print(", Alt: ");
  Serial.print(frame.altitude, 2);
  Serial.print(", Spd: ");
  Serial.println(frame.speed, 2);
}

void handleBacklogGps(const EspNowFrame &frame, const uint8_t *mac_addr)
{
  if (!hasSenderMac || !macEqual(lastSenderMac, mac_addr)) {
    ensureSenderPeer(mac_addr);
  }

  resolveMissingSequence(frame.sequence);
  clearResolvedMissingEntries();

  Serial.print("Backlog GPS packet seq ");
  Serial.print(frame.sequence);
  Serial.print(" received from ");
  printMac(mac_addr);
  Serial.print(" -> Lat: ");
  Serial.print(frame.latitude, 6);
  Serial.print(", Lon: ");
  Serial.print(frame.longitude, 6);
  Serial.print(", Alt: ");
  Serial.print(frame.altitude, 2);
  Serial.print(", Spd: ");
  Serial.println(frame.speed, 2);
}

void onDataRecv(const uint8_t * mac_addr, const uint8_t *incomingData, int len)
{
  if (len < 1) return;

  EspNowFrame packet;
  memset(&packet, 0, sizeof(packet));
  memcpy(&packet, incomingData, min(len, (int)sizeof(packet)));

  if (packet.msgType == MSG_GPS) {
    if (packet.isBacklog) {
      handleBacklogGps(packet, mac_addr);
    } else {
      handleLiveGps(packet, mac_addr);
    }
  } else if (packet.msgType == MSG_REQUEST_BACKLOG) {
    Serial.println("Received unexpected backlog request on receiver; ignoring.");
  } else if (packet.msgType == MSG_BACKLOG_EMPTY) {
    Serial.println("Sender responded that no backlog exists for missing entries.");
    if (missingCount > 0) {
      Serial.println("Assuming sender was offline during predicted missing interval.");
      printMissingStatus();
      missingCount = 0;
    }
    waitingForReply = false;
    requestRetries = 0;
  } else {
    Serial.print("Unknown ESP-NOW msgType ");
    Serial.println(packet.msgType);
  }
}

void requestBacklog()
{
  if (waitingForReply && requestRetries >= MAX_REQUEST_RETRIES) {
    Serial.println("Already waiting for a backlog reply; will not send another request yet.");
    return;
  }

  EspNowFrame request = {};
  request.msgType = MSG_REQUEST_BACKLOG;

  const uint8_t *targetMac = nullptr;
  if (hasSenderMac && ensureSenderPeer(lastSenderMac)) {
    targetMac = lastSenderMac;
  }

  sendComplete = false;
  esp_err_t result;
  if (targetMac) {
    result = esp_now_send(targetMac, (uint8_t *)&request, sizeof(request));
  } else {
    result = esp_now_send(broadcastAddress, (uint8_t *)&request, sizeof(request));
  }

  if (result == ESP_OK) {
    waitingForReply = true;
    lastRequestMillis = millis();
    requestRetries = min<uint8_t>(requestRetries + 1, MAX_REQUEST_RETRIES);
    Serial.printf("Backlog request sent (attempt %u)\n", requestRetries);
  } else {
    Serial.printf("Failed to send backlog request: %d\n", result);
    if (requestRetries >= MAX_REQUEST_RETRIES) {
      waitingForReply = false;
      Serial.println("No reply received yet; assuming sender may be out of range. Will wait for the next packet or manual request.");
    }
  }
}

void printStatus()
{
  Serial.println("--- Receiver Status ---");
  Serial.printf("Has sequence: %s\n", hasSequence ? "yes" : "no");
  if (hasSequence) {
    Serial.printf("Expected seq: %u\n", expectedSequence);
    Serial.printf("Next expected at: %lu ms\n", nextExpectedMillis);
  }
  Serial.printf("Waiting for reply: %s\n", waitingForReply ? "yes" : "no");
  Serial.printf("Request retries: %u\n", requestRetries);
  Serial.printf("Known sender MAC: %s\n", hasSenderMac ? "yes" : "no");
  if (hasSenderMac) {
    printMac(lastSenderMac);
    Serial.println();
  }
  printMissingStatus();
}

String serialCommandBuffer = "";

void processSerialInput()
{
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      String command = serialCommandBuffer;
      serialCommandBuffer = "";
      command.trim();
      command.toLowerCase();

      if (command.length() == 0) {
        return;
      }

      if (command == "req" || command == "request" || command == "backlog") {
        requestBacklog();
      } else if (command == "status" || command == "stat") {
        printStatus();
      } else if (command == "help") {
        Serial.println("Commands: help, status, request, backlog, req");
      } else {
        Serial.print("Unknown command: ");
        Serial.println(command);
      }
      return;
    }

    if (serialCommandBuffer.length() < 128) {
      serialCommandBuffer += c;
    }
  }
}

void setup()
{
  Serial.begin(115200);
  delay(500);
  Serial.println("\n--- ESP32 Backlog Receiver ---\n");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW stack");
    return;
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  nextExpectedMillis = millis() + COORDINATE_INTERVAL_MS;
}

void loop()
{
  unsigned long now = millis();
  processSerialInput();

  if (hasSequence && !waitingForReply && now > nextExpectedMillis + MISSING_GRACE_MS) {
    Serial.println("No data arrived by expected time; requesting missed backlog.");
    requestBacklog();
  }

  if (waitingForReply && now > lastRequestMillis + REQUEST_RETRY_MS) {
    if (requestRetries < MAX_REQUEST_RETRIES) {
      Serial.println("Retrying backlog request...");
      requestBacklog();
    } else {
      waitingForReply = false;
      Serial.println("No reply to backlog request; assuming sender out of range and waiting for next packet.");
    }
  }

  if (sendComplete) {
    if (!sendSuccess) {
      Serial.println("Backlog request could not be delivered at the RF layer.");
    }
    sendComplete = false;
  }
}
