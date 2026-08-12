#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <Preferences.h>

// --- WiFi ---
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

#include <WiFiClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// --- MQTT ---
#define MQTT_HOST "YOUR_MQTT_BROKER_IP"
#define MQTT_PORT 1883
#define MQTT_USER ""       // leave empty if broker has no auth
#define MQTT_PASSWORD ""
#define DEPLOYMENT_SLUG "sijagaair-bojong-kulur"
#define DEVICE_ID "node-001"

// --- Runtime-mutable config (defaults; can be changed via MQTT command/config) ---
#define SENSOR_HEIGHT_CM_DEFAULT 150.0  // distance from sensor to dry-channel bottom; calibrate per install site
#define READ_INTERVAL_SEC_DEFAULT 30

float sensorHeightCm = SENSOR_HEIGHT_CM_DEFAULT;
uint32_t readIntervalSec = READ_INTERVAL_SEC_DEFAULT;

float lastDistanceCm = 0.0;
uint32_t lastDistanceAt = 0; // millis() of last successful sensor read; 0 = no reading yet
uint32_t trigger_cnt_reset_flag = 0;

Preferences prefs;

#define STATUS_INTERVAL_SEC_DEFAULT 120
uint32_t statusIntervalSec = STATUS_INTERVAL_SEC_DEFAULT;
uint32_t statusSentAt = 0;
#define FIRMWARE_VERSION "sijagakali-v1.0.0"

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
char topicBase[96];
char mqttClientId[48];

// A01ANY4B ultrasonic sensor, RS485/Modbus RTU variant, via auto-direction TTL<->RS485 module.
// Sensor: red->5-12V (own supply, not ESP32 3.3V), black->GND, yellow->A+, white->B-
// Module: TXD->GPIO5(ESP RX)  RXD->GPIO4(ESP TX)  VCC->5V  GND->GND (common with sensor+ESP32)
#define SENSOR_RX 5
#define SENSOR_TX 4

HardwareSerial sensorSerial(1);

/* Table of CRC values for high-order byte */
static const uint8_t table_crc_hi[] = {
    0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41,
    0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40,
    0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40,
    0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41,
    0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40,
    0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41,
    0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41,
    0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40, 0x01, 0xC0, 0x80, 0x41, 0x01, 0xC0, 0x80, 0x41, 0x00, 0xC1, 0x81, 0x40
};

/* Table of CRC values for low-order byte */
static const uint8_t table_crc_lo[] = {
    0x00, 0xC0, 0xC1, 0x01, 0xC3, 0x03, 0x02, 0xC2, 0xC6, 0x06, 0x07, 0xC7, 0x05, 0xC5, 0xC4, 0x04, 0xCC, 0x0C, 0x0D, 0xCD, 0x0F, 0xCF, 0xCE, 0x0E, 0x0A, 0xCA, 0xCB, 0x0B, 0xC9, 0x09, 0x08, 0xC8,
    0xD8, 0x18, 0x19, 0xD9, 0x1B, 0xDB, 0xDA, 0x1A, 0x1E, 0xDE, 0xDF, 0x1F, 0xDD, 0x1D, 0x1C, 0xDC, 0x14, 0xD4, 0xD5, 0x15, 0xD7, 0x17, 0x16, 0xD6, 0xD2, 0x12, 0x13, 0xD3, 0x11, 0xD1, 0xD0, 0x10,
    0xF0, 0x30, 0x31, 0xF1, 0x33, 0xF3, 0xF2, 0x32, 0x36, 0xF6, 0xF7, 0x37, 0xF5, 0x35, 0x34, 0xF4, 0x3C, 0xFC, 0xFD, 0x3D, 0xFF, 0x3F, 0x3E, 0xFE, 0xFA, 0x3A, 0x3B, 0xFB, 0x39, 0xF9, 0xF8, 0x38,
    0x28, 0xE8, 0xE9, 0x29, 0xEB, 0x2B, 0x2A, 0xEA, 0xEE, 0x2E, 0x2F, 0xEF, 0x2D, 0xED, 0xEC, 0x2C, 0xE4, 0x24, 0x25, 0xE5, 0x27, 0xE7, 0xE6, 0x26, 0x22, 0xE2, 0xE3, 0x23, 0xE1, 0x21, 0x20, 0xE0,
    0xA0, 0x60, 0x61, 0xA1, 0x63, 0xA3, 0xA2, 0x62, 0x66, 0xA6, 0xA7, 0x67, 0xA5, 0x65, 0x64, 0xA4, 0x6C, 0xAC, 0xAD, 0x6D, 0xAF, 0x6F, 0x6E, 0xAE, 0xAA, 0x6A, 0x6B, 0xAB, 0x69, 0xA9, 0xA8, 0x68,
    0x78, 0xB8, 0xB9, 0x79, 0xBB, 0x7B, 0x7A, 0xBA, 0xBE, 0x7E, 0x7F, 0xBF, 0x7D, 0xBD, 0xBC, 0x7C, 0xB4, 0x74, 0x75, 0xB5, 0x77, 0xB7, 0xB6, 0x76, 0x72, 0xB2, 0xB3, 0x73, 0xB1, 0x71, 0x70, 0xB0,
    0x50, 0x90, 0x91, 0x51, 0x93, 0x53, 0x52, 0x92, 0x96, 0x56, 0x57, 0x97, 0x55, 0x95, 0x94, 0x54, 0x9C, 0x5C, 0x5D, 0x9D, 0x5F, 0x9F, 0x9E, 0x5E, 0x5A, 0x9A, 0x9B, 0x5B, 0x99, 0x59, 0x58, 0x98,
    0x88, 0x48, 0x49, 0x89, 0x4B, 0x8B, 0x8A, 0x4A, 0x4E, 0x8E, 0x8F, 0x4F, 0x8D, 0x4D, 0x4C, 0x8C, 0x44, 0x84, 0x85, 0x45, 0x87, 0x47, 0x46, 0x86, 0x82, 0x42, 0x43, 0x83, 0x41, 0x81, 0x80, 0x40
};

uint16_t crc16(uint8_t *buffer, uint16_t buffer_length) {
  uint8_t crc_hi = 0xFF;
  uint8_t crc_lo = 0xFF;
  unsigned int i;

  while (buffer_length--) {
    i = crc_hi ^ *buffer++;
    crc_hi = crc_lo ^ table_crc_hi[i];
    crc_lo = table_crc_lo[i];
  }

  return (crc_hi << 8 | crc_lo);
}

uint32_t wifiRetryAt = 0;

bool wifiIsUp() {
  return WiFi.status() == WL_CONNECTED;
}

void ensureWifi() {
  if (wifiIsUp()) return;
  if (millis() - wifiRetryAt < 5000) return; // retry every 5s, don't block
  wifiRetryAt = millis();

  if (WiFi.status() != WL_CONNECT_FAILED && WiFi.status() != WL_IDLE_STATUS) {
    Serial.println("WiFi: connecting...");
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

uint32_t mqttRetryAt = 0;

void mqttCallback(char* topic, byte* payload, unsigned int length); // forward decl, implemented in Task 5
void publishCommandAck(const char* requestId, bool ok, const char* detail); // forward decl, implemented in Task 5

void ensureMqtt() {
  if (!wifiIsUp()) return;
  if (mqttClient.connected()) {
    mqttClient.loop();
    return;
  }
  if (millis() - mqttRetryAt < 5000) return; // retry every 5s, don't block
  mqttRetryAt = millis();

  Serial.print("MQTT: connecting...");
  bool ok;
  if (strlen(MQTT_USER) > 0) {
    ok = mqttClient.connect(mqttClientId, MQTT_USER, MQTT_PASSWORD);
  } else {
    ok = mqttClient.connect(mqttClientId);
  }

  if (ok) {
    Serial.println(" connected");
    char subTopic[128];
    snprintf(subTopic, sizeof(subTopic), "%s/config/interval", topicBase);
    mqttClient.subscribe(subTopic, 1);
    snprintf(subTopic, sizeof(subTopic), "%s/command", topicBase);
    mqttClient.subscribe(subTopic, 1);
  } else {
    Serial.print(" failed, rc=");
    Serial.println(mqttClient.state());
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char topicStr[128];
  size_t topicLen = strlen(topic);
  if (topicLen >= sizeof(topicStr)) return; // reject oversized topic, avoid overflow
  memcpy(topicStr, topic, topicLen);
  topicStr[topicLen] = '\0';

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.print("MQTT: JSON parse error on ");
    Serial.println(topicStr);
    return;
  }

  char intervalTopic[128];
  snprintf(intervalTopic, sizeof(intervalTopic), "%s/config/interval", topicBase);
  char commandTopic[128];
  snprintf(commandTopic, sizeof(commandTopic), "%s/command", topicBase);

  if (strcmp(topicStr, intervalTopic) == 0) {
    if (doc["interval_sec"].is<int>()) {
      int newInterval = doc["interval_sec"].as<int>();
      if (newInterval >= 10) {
        readIntervalSec = (uint32_t)newInterval;
        prefs.putUInt("interval", readIntervalSec);
        Serial.print("Config: read_interval_sec updated to ");
        Serial.println(readIntervalSec);
      }
    }
    return;
  }

  if (strcmp(topicStr, commandTopic) == 0) {
    const char* cmd = doc["cmd"] | "";
    const char* requestId = doc["request_id"] | "";

    if (strcmp(cmd, "restart") == 0) {
      publishCommandAck(requestId, true, "restarting");
      delay(200); // let the publish flush before reboot
      ESP.restart();
    } else if (strcmp(cmd, "calibrate") == 0) {
      if (!doc["params"]["reference_water_level_cm"].isNull()) {
        if (lastDistanceAt == 0 || millis() - lastDistanceAt > 120000) {
          publishCommandAck(requestId, false, "no recent sensor reading");
        } else {
          // Operator stands a known water level (reference) below the sensor;
          // sensorHeightCm = that reference + whatever raw distance we're reading right now.
          // Since calibrate is async, we snapshot the last known distance above.
          float referenceCm = doc["params"]["reference_water_level_cm"].as<float>();
          sensorHeightCm = referenceCm + lastDistanceCm;
          prefs.putFloat("sensorH", sensorHeightCm);
          char detail[64];
          snprintf(detail, sizeof(detail), "sensor_height_cm=%.1f", sensorHeightCm);
          publishCommandAck(requestId, true, detail);
        }
      } else {
        publishCommandAck(requestId, false, "missing params.reference_water_level_cm");
      }
    } else if (strcmp(cmd, "sample_now") == 0) {
      trigger_cnt_reset_flag = 1; // force next loop() iteration to read+publish immediately
      publishCommandAck(requestId, true, "sampling on next cycle");
    } else {
      publishCommandAck(requestId, false, "unknown cmd");
    }
    return;
  }
}

void generateUuidV4(char out[37]) {
  uint8_t b[16];
  for (int i = 0; i < 16; i++) b[i] = (uint8_t)(esp_random() & 0xFF);
  b[6] = (b[6] & 0x0F) | 0x40; // version 4
  b[8] = (b[8] & 0x3F) | 0x80; // variant 10xx

  snprintf(out, 37,
    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
    b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

void syncTimeWib() {
  configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com"); // UTC+7 (WIB), no DST
  Serial.print("NTP: syncing time");
  time_t now = time(nullptr);
  uint32_t start = millis();
  while (now < 8 * 3600 * 2 && millis() - start < 10000) { // wait for a plausible epoch time, max 10s
    delay(250);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println();
}

void ensureTimeSynced() {
  if (time(nullptr) < 1600000000) { // implausible epoch: NTP never synced (or WiFi wasn't up in setup())
    syncTimeWib();
  }
}

void isoTimestampWib(char out[26]) {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  snprintf(out, 26, "%04d-%02d-%02dT%02d:%02d:%02d+07:00",
    t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
}

void publishCommandAck(const char* requestId, bool ok, const char* detail) {
  if (!mqttClient.connected()) return;

  JsonDocument doc;
  doc["request_id"] = requestId;
  doc["ok"] = ok;
  doc["detail"] = detail;
  char ts[26];
  isoTimestampWib(ts);
  doc["timestamp"] = ts;

  char buf[256];
  size_t n = serializeJson(doc, buf, sizeof(buf));

  char topic[128];
  snprintf(topic, sizeof(topic), "%s/command/ack", topicBase);
  mqttClient.publish(topic, (const uint8_t*)buf, n, false);
}

void publishSensorData(float waterLevelCm) {
  if (!mqttClient.connected()) return;

  ensureTimeSynced();
  char corrId[37];
  generateUuidV4(corrId);
  char ts[26];
  isoTimestampWib(ts);

  JsonDocument doc;
  doc["deployment_slug"] = DEPLOYMENT_SLUG;
  doc["device_id"] = DEVICE_ID;
  doc["correlation_id"] = corrId;
  doc["water_level_cm"] = roundf(waterLevelCm * 10) / 10.0f; // 1 decimal, matches dummy publisher precision
  doc["timestamp"] = ts;
  doc["rssi"] = WiFi.RSSI();
  // no battery sensor on this board; omit battery_pct (optional field)

  char buf[384];
  size_t n = serializeJson(doc, buf, sizeof(buf));

  char topic[128];
  snprintf(topic, sizeof(topic), "%s/sensor/data", topicBase);
  bool ok = mqttClient.publish(topic, (const uint8_t*)buf, n, false);

  Serial.print("Published sensor/data: ");
  Serial.print(buf);
  Serial.println(ok ? " [ok]" : " [FAILED]");
}

void publishSensorStatus() {
  if (!mqttClient.connected()) return;

  ensureTimeSynced();
  char ts[26];
  isoTimestampWib(ts);

  JsonDocument doc;
  doc["deployment_slug"] = DEPLOYMENT_SLUG;
  doc["device_id"] = DEVICE_ID;
  doc["timestamp"] = ts;
  doc["online"] = true;
  doc["uptime_sec"] = millis() / 1000;
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["last_error"] = nullptr;
  doc["heap_free_bytes"] = ESP.getFreeHeap();

  char buf[320];
  size_t n = serializeJson(doc, buf, sizeof(buf));

  char topic[128];
  snprintf(topic, sizeof(topic), "%s/sensor/status", topicBase);
  mqttClient.publish(topic, (const uint8_t*)buf, n, false);
  Serial.println("Published sensor/status heartbeat");
}

void setup() {
  Serial.begin(115200);
  delay(2000); // let USB-CDC enumerate before first print

  pinMode(LED_BUILTIN, OUTPUT);
  sensorSerial.begin(9600, SERIAL_8N1, SENSOR_RX, SENSOR_TX);

  Serial.println("=== A01ANY4B Modbus RTU sensor starting ===");

  prefs.begin("sijagaair", false);
  sensorHeightCm = prefs.getFloat("sensorH", SENSOR_HEIGHT_CM_DEFAULT);
  readIntervalSec = prefs.getUInt("interval", READ_INTERVAL_SEC_DEFAULT);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi: connecting");
  uint32_t wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (wifiIsUp()) {
    Serial.print("WiFi connected, IP=");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi not connected yet, will keep retrying in loop()");
  }

  if (wifiIsUp()) {
    syncTimeWib();
  }

  snprintf(topicBase, sizeof(topicBase), "sijagaair/%s", DEVICE_ID); // matches backend TOPICS: sijagaair/{device_id}/...
  snprintf(mqttClientId, sizeof(mqttClientId), "esp32-%s", DEVICE_ID);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setBufferSize(512); // default 256 is too small for sensor/data + status JSON
  mqttClient.setCallback(mqttCallback);

  // Backdate so the first loop() heartbeat check fires immediately once MQTT connects,
  // instead of waiting a full statusIntervalSec after boot.
  statusSentAt = millis() - (uint32_t)statusIntervalSec * 1000UL - 1;
}

void loop() {
  ensureWifi();
  ensureMqtt();

  static uint32_t trigger_cnt = 0;
  static uint8_t recv_buf[10] = {0};

  if (millis() - trigger_cnt > (readIntervalSec * 1000UL) || trigger_cnt_reset_flag) {
    trigger_cnt_reset_flag = 0;
    while (sensorSerial.available()) sensorSerial.read(); // drop stale bytes so the read window aligns with this request's reply

    // 01 03 01 01 00 01 D4 36  read real-time value -> reply: 01 03 02 <hi> <lo> <crc_hi> <crc_lo>
    uint8_t tx_buf[] = {0x01, 0x03, 0x01, 0x01, 0x00, 0x01, 0xD4, 0x36};
    sensorSerial.write(tx_buf, sizeof(tx_buf));
    sensorSerial.flush(); // wait for command to fully transmit
    delay(20); // let any TX echo settle and the sensor start replying before we read
    while (sensorSerial.available()) sensorSerial.read(); // drop leftover echo bytes, keep only the fresh reply
    delay(30); // give the full 7-byte reply time to arrive so readBytes doesn't return a partial frame
    uint16_t len = sensorSerial.readBytes(recv_buf, 7);

    Serial.printf("raw[%d]: ", len);
    for (uint16_t i = 0; i < len; i++) Serial.printf("%02X ", recv_buf[i]);
    Serial.println();

    if (len == 7 && recv_buf[1] == 0x03) {
      uint16_t calc_crc = crc16(recv_buf, 7 - 2);
      uint16_t recv_crc = recv_buf[5] << 8 | recv_buf[6];

      if (calc_crc == recv_crc) {
        uint16_t distanceMm = recv_buf[3] << 8 | recv_buf[4];
        if (distanceMm > 280) {
          float distanceCm = distanceMm / 10.0;
          lastDistanceCm = distanceCm;
          lastDistanceAt = millis();
          Serial.print("Distance = ");
          Serial.print(distanceCm);
          Serial.println(" cm");

          float waterLevelCm = sensorHeightCm - distanceCm;
          if (waterLevelCm < 0) waterLevelCm = 0; // clamp: sensor above dry channel bottom reads as 0, not negative
          publishSensorData(waterLevelCm);
        } else {
          Serial.println("Below the lower limit");
        }
      } else {
        Serial.println("checksum mismatch");
      }
    }
    trigger_cnt = millis();
  }

  static uint32_t led_cnt = millis();
  if (millis() - led_cnt > 100) {
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN)); // berkedip tiap loop jalan, indikator board hidup
    led_cnt = millis();
  }

  if (millis() - statusSentAt > (statusIntervalSec * 1000UL)) {
    statusSentAt = millis();
    publishSensorStatus();
  }
}
