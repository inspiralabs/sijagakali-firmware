# MQTT-over-WebSocket Migration (mosquitto + ESP32 firmware) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let `sijagakali-firmware` (ESP32-C5) reach the on-prem mosquitto broker over the internet via Cloudflare Tunnel, by adding a secured WebSocket listener to mosquitto and migrating the firmware's MQTT client from raw-TCP `PubSubClient` to ESP-IDF's `esp_mqtt_client` (`wss://`), without changing any backend code in `sijagakali-api` or `sijagakali-ota`.

**Architecture:** mosquitto gets a second listener (`1773`, `protocol websockets`) alongside the existing local one (`1883`), both behind per-listener username/password + topic ACL. Cloudflare Tunnel (configured outside this plan, on the host running mosquitto) forwards the public hostname to `ws://localhost:1773`. Firmware swaps its MQTT transport to `esp_mqtt_client` over `wss://`, keeping every topic name, JSON payload shape, and command-handling behavior byte-for-byte identical to the current `PubSubClient` implementation — only the connection/publish/subscribe *mechanism* changes.

**Tech Stack:** mosquitto 2.1.2 (already installed locally), ESP-IDF `esp_mqtt_client` + `esp_crt_bundle` (called directly from the existing `framework = arduino` PlatformIO project — no framework switch), ArduinoJson (unchanged).

**Spec:** This plan implements the design discussed and recorded in `d:\code-for-life\inspiralabs\projects\sijagakali\architecture-overview\ALUR-SISTEM.md` (system-wide flow) plus the WS-vs-TCP/REST tradeoff discussion that preceded this plan (no separate written spec file — the conversation is the spec of record).

## Global Constraints

- Do not modify `sijagakali-api` or `sijagakali-ota` in this plan — they keep talking to mosquitto on `mqtt://localhost:1883` exactly as today. (Their `.env` files will eventually need `MQTT_USERNAME`/`MQTT_PASSWORD` filled in to match the new password-protected listener — noted as a follow-up, not a task here.)
- Firmware keeps `framework = arduino` in `platformio.ini` — no hybrid `arduino, espidf` switch. `esp_mqtt_client`/`esp_crt_bundle` headers are called directly, the same way arduino-esp32 projects commonly call other ESP-IDF APIs (`esp_wifi_*`, `nvs_flash_init`, etc.) without a framework change.
- **This sandbox has no PlatformIO CLI and no physical ESP32 hardware attached.** Every firmware build/flash verification step in this plan is written for the human executor (or a machine that has `pio`/PlatformIO IDE and the board) to run — it cannot be executed inside this session. Do not report a firmware task "done" without the human confirming the build/flash result.
- Preserve exact current behavior unless explicitly called out as a change: same topics (`sensor/data`, `sensor/status`, `config/interval`, `command`, `command/ack`), same JSON fields, same QoS levels (publish = QoS 0, subscribe = QoS 1 — `PubSubClient` never actually published above QoS 0, so keep parity instead of silently upgrading).
- MQTT keepalive must stay under Cloudflare's free-tier WebSocket idle-connection timeout (~100s) so the tunnel doesn't drop an otherwise-healthy connection.

---

### Task 1: mosquitto config template — WS + TCP listeners, password auth, per-device ACL

**Files:**
- Modify: `sijagakali-firmware/mosquitto-conf/mosquitto.conf`
- Create (outside the repo, on the machine running mosquitto): `C:\Program Files\mosquitto\sijagakali.pwfile`, `C:\Program Files\mosquitto\sijagakali.acl`

**Interfaces:**
- Produces: two mosquitto listeners — `1883` (plain MQTT, local backend) and `1773` (`protocol websockets`, fronted later by Cloudflare Tunnel) — both requiring username/password, both consulting the same ACL file. Backend services connect with a shared `sijagakali-backend` user (full `sijagakali/#` access); each device connects with a username equal to its own `DEVICE_ID` (e.g. `node-001`), scoped by ACL to only `sijagakali/<that device id>/#`.

- [ ] **Step 1: Generate the password file (backend user + one example device user)**

Run (adjust the passwords before running — these are examples, not real secrets to commit anywhere):
```
"C:\Program Files\mosquitto\mosquitto_passwd.exe" -c -b "C:\Program Files\mosquitto\sijagakali.pwfile" sijagakali-backend "CHANGE_ME_BACKEND_PASSWORD"
"C:\Program Files\mosquitto\mosquitto_passwd.exe" -b "C:\Program Files\mosquitto\sijagakali.pwfile" node-001 "CHANGE_ME_NODE001_PASSWORD"
```
The `-c` on the first command creates/overwrites the file; every device added after that must omit `-c` (or it will wipe prior entries). Repeat the second command (new username = new `DEVICE_ID`, e.g. `node-002`) for every additional device you flash later.

Expected: `type "C:\Program Files\mosquitto\sijagakali.pwfile"` shows two lines, each `username:$7$...` (bcrypt/PBKDF2 hash, never plaintext).

- [ ] **Step 2: Write the ACL file**

Create `C:\Program Files\mosquitto\sijagakali.acl`:
```
# sijagakali-backend (mqtt-collector, sijagakali-ota) needs the full tree.
user sijagakali-backend
topic readwrite sijagakali/#

# Every other authenticated user is scoped to its own device namespace by
# matching its username (%u) against the device_id segment of the topic.
# A device logging in as "node-001" can only read/write sijagakali/node-001/#.
pattern readwrite sijagakali/%u/#
```

- [ ] **Step 3: Replace `mosquitto-conf/mosquitto.conf` with the new listener config**

```conf
# Sijagakali MQTT broker
# - listener 1883: plain MQTT, for local backend services (sijagakali-api,
#   sijagakali-ota) running on this same machine. Never exposed outside it.
# - listener 1773: MQTT over WebSockets, for ESP32 devices reached through
#   Cloudflare Tunnel (tunnel forwards the public hostname to
#   ws://localhost:1773; Cloudflare terminates TLS at its edge, so this
#   listener itself stays plain WS on loopback).
#
# Both listeners require a username/password (sijagakali.pwfile) and are
# restricted per-user by topic (sijagakali.acl) — see Task 1 in
# docs/superpowers/plans/2026-09-02-mqtt-websocket-migration.md for how
# those two files are generated.

per_listener_settings true

listener 1883 127.0.0.1
protocol mqtt
allow_anonymous false
password_file C:\Program Files\mosquitto\sijagakali.pwfile
acl_file C:\Program Files\mosquitto\sijagakali.acl

listener 1773 127.0.0.1
protocol websockets
allow_anonymous false
password_file C:\Program Files\mosquitto\sijagakali.pwfile
acl_file C:\Program Files\mosquitto\sijagakali.acl

# Optional: uncomment only if you also want mosquitto itself to terminate
# TLS (e.g. to double-encrypt end to end, or to expose 8883 through a path
# other than the Cloudflare Tunnel). Not required for the WS-over-tunnel
# setup above — Cloudflare already terminates TLS for dev-mqtt.inspiralabs.id.
#listener 8883 127.0.0.1
#protocol mqtt
#allow_anonymous false
#password_file C:\Program Files\mosquitto\sijagakali.pwfile
#acl_file C:\Program Files\mosquitto\sijagakali.acl
#certfile C:\path\to\fullchain.pem
#keyfile C:\path\to\privkey.pem

log_dest stderr
log_type error
log_type warning
log_type notice
```

- [ ] **Step 4: Start mosquitto with the new config and confirm it loads cleanly**

Run (foreground, from `sijagakali-firmware/`):
```
"C:\Program Files\mosquitto\mosquitto.exe" -c mosquitto-conf\mosquitto.conf -v
```
Expected: log lines showing `Opening ipv4 listen socket on port 1883` and `Opening websockets listen socket on port 1773`, no `Error` lines, process stays running (does not exit).

- [ ] **Step 5: Verify the TCP listener (1883) — auth + ACL scoping both work**

In a second terminal, with mosquitto still running:
```
"C:\Program Files\mosquitto\mosquitto_sub.exe" -h localhost -p 1883 -u node-001 -P CHANGE_ME_NODE001_PASSWORD -t "sijagakali/node-001/sensor/data" -C 1 &
"C:\Program Files\mosquitto\mosquitto_pub.exe" -h localhost -p 1883 -u node-001 -P CHANGE_ME_NODE001_PASSWORD -t "sijagakali/node-001/sensor/data" -m "{\"test\":true}"
```
Expected: the `mosquitto_sub` command prints `{"test":true}` and exits (the `-C 1` makes it quit after one message).

Then confirm ACL denies cross-device access:
```
"C:\Program Files\mosquitto\mosquitto_pub.exe" -h localhost -p 1883 -u node-001 -P CHANGE_ME_NODE001_PASSWORD -t "sijagakali/node-002/sensor/data" -m "{\"should\":\"be denied\"}"
```
Expected: mosquitto's own log (Step 4's terminal) shows a `Denied PUBLISH` (or similar ACL-denied) line for `node-001` on that topic; the publish does not reach any subscriber of `sijagakali/node-002/...`.

- [ ] **Step 6: Verify the WebSocket listener (1773) end-to-end**

`mosquitto_pub`/`mosquitto_sub` don't speak WebSocket, so use the `mqtt` npm package already installed under `sijagakali-api` to prove the WS listener actually accepts MQTT traffic. From `sijagakali-firmware/`:
```
node -e "
const mqtt = require('../sijagakali-api/node_modules/mqtt');
const client = mqtt.connect('ws://localhost:1773', { username: 'node-001', password: 'CHANGE_ME_NODE001_PASSWORD' });
client.on('connect', () => {
  console.log('WS connected');
  client.subscribe('sijagakali/node-001/sensor/data', () => {
    client.publish('sijagakali/node-001/sensor/data', JSON.stringify({ test: true }));
  });
});
client.on('message', (topic, payload) => {
  console.log('received', topic, payload.toString());
  process.exit(0);
});
"
```
Expected: prints `WS connected` then `received sijagakali/node-001/sensor/data {"test":true}`.

- [ ] **Step 7: Stop the manual mosquitto process, reinstall/restart it as the Windows service so it picks up this config**

However mosquitto currently runs in production on this machine (Windows service, task scheduler, etc.), point it at this same `mosquitto-conf/mosquitto.conf` path and restart it. Confirm with `Step 4`'s log check again (or the service's own log file) that both listeners come up.

- [ ] **Step 8: Commit**

```bash
cd "d:/code-for-life/inspiralabs/projects/sijagakali/sijagakali-firmware"
git add mosquitto-conf/mosquitto.conf
git commit -m "feat: add password-protected websocket listener to mosquitto config"
```
(The generated `.pwfile`/`.acl` live outside the repo in `C:\Program Files\mosquitto\` — nothing secret gets committed.)

---

### Task 2: Migrate firmware MQTT client from PubSubClient to `esp_mqtt_client` over `wss://`

**Files:**
- Modify: `sijagakali-firmware/src/main.cpp`
- Modify: `sijagakali-firmware/platformio.ini`

**Interfaces:**
- Consumes: the WS listener from Task 1 (`ws://localhost:1773`, reachable publicly as `wss://<your-cloudflare-hostname>` once Cloudflare Tunnel is configured on the broker host — that tunnel setup itself is outside this plan, already covered conceptually in `architecture-overview/ALUR-SISTEM.md`), and the per-device username/password created in Task 1 Step 1.
- Produces: same public topic contract as before (`sijagakali/{DEVICE_ID}/sensor/data`, `.../sensor/status`, `.../config/interval` [sub], `.../command` [sub], `.../command/ack`) — nothing downstream (`sijagakali-api`, `sijagakali-ota`) needs to change.

- [ ] **Step 1: Add ESP-IDF MQTT/TLS includes and switch the connection `#define`s — build-only gate before touching any logic**

In `sijagakali-firmware/src/main.cpp`, replace lines 1–22 (the includes and WiFi/MQTT `#define` block) with:

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <Preferences.h>

// --- WiFi ---
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

#include <ArduinoJson.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include "mqtt_client.h"
#include "esp_crt_bundle.h"

// --- MQTT ---
// Public hostname routed through Cloudflare Tunnel to mosquitto's
// "listener 1773 / protocol websockets" (see mosquitto-conf/mosquitto.conf).
// No port needed here: Cloudflare Tunnel serves this on the standard 443,
// the internal 1773 is only used between cloudflared and mosquitto locally.
#define MQTT_BROKER_URI "wss://YOUR_CLOUDFLARE_HOSTNAME/mqtt"
// Must equal this device's mosquitto username (see Task 1 Step 1) so the
// broker's ACL pattern "sijagakali/%u/#" scopes it to its own topics.
#define MQTT_USER "node-001"
#define MQTT_PASSWORD "YOUR_DEVICE_MQTT_PASSWORD"
#define DEPLOYMENT_SLUG "sijagakali-bojong-kulur"
#define DEVICE_ID "node-001"
```

This removes `#include <WiFiClient.h>` and `#include <PubSubClient.h>` (no longer used) and drops `MQTT_HOST`/`MQTT_PORT` (replaced by `MQTT_BROKER_URI`).

- [ ] **Step 2: Build to confirm `mqtt_client.h`/`esp_crt_bundle.h` resolve for this board — do not proceed until this passes**

Run (human executor, in `sijagakali-firmware/`, with PlatformIO installed):
```
pio run
```
Expected: compiles (will still fail later at link time or on missing symbols like `mqttClient`/`ensureMqtt` referenced elsewhere in the file that Steps 3–6 haven't touched yet — that's fine, this step is only checking the two new `#include` lines resolve). If it fails with `fatal error: mqtt_client.h: No such file or directory` or similar, **stop and report back** — this means the `esp-mqtt`/cert-bundle components aren't exposed to this specific `pioarduino` ESP32-C5 Arduino-framework build, and the plan needs a different approach (e.g. hybrid `framework = arduino, espidf`) before continuing to Step 3.

- [ ] **Step 3: Replace the MQTT client globals and connection-retry function with an ESP-IDF event handler**

Replace `sijagakali-firmware/src/main.cpp` lines 42–46 (was: `WiFiClient wifiClient; PubSubClient mqttClient(wifiClient); char topicBase[96]; char mqttClientId[48];`) with:

```cpp
esp_mqtt_client_handle_t mqttClient = nullptr;
volatile bool mqttConnected = false;
char topicBase[96];
char mqttClientId[48];
```

Replace lines 111–145 (the whole `uint32_t mqttRetryAt = 0;` global plus `ensureMqtt()` function) and lines 113–115's forward declarations with:

```cpp
void mqttCallback(const char* topicStr, const uint8_t* payload, size_t length); // forward decl
void publishCommandAck(const char* requestId, bool ok, const char* detail); // forward decl
void performOtaUpdate(const char* requestId, const char* url); // forward decl, unchanged below

static void mqttEventHandler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

  switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
      mqttConnected = true;
      Serial.println("MQTT: connected");
      char subTopic[128];
      snprintf(subTopic, sizeof(subTopic), "%s/config/interval", topicBase);
      esp_mqtt_client_subscribe(mqttClient, subTopic, 1);
      snprintf(subTopic, sizeof(subTopic), "%s/command", topicBase);
      esp_mqtt_client_subscribe(mqttClient, subTopic, 1);
      break;
    }
    case MQTT_EVENT_DISCONNECTED:
      mqttConnected = false;
      Serial.println("MQTT: disconnected");
      break;
    case MQTT_EVENT_DATA: {
      // Our JSON payloads are always well under the buffer size, so a message
      // never arrives split across multiple DATA events. If that assumption
      // is ever violated, drop it rather than parse a partial payload.
      if (event->current_data_offset != 0 || event->data_len != event->total_data_len) {
        Serial.println("MQTT: dropping unexpectedly fragmented message");
        break;
      }
      char topicStr[128];
      size_t topicLen = event->topic_len;
      if (topicLen >= sizeof(topicStr)) break; // reject oversized topic, avoid overflow
      memcpy(topicStr, event->topic, topicLen);
      topicStr[topicLen] = '\0';
      mqttCallback(topicStr, (const uint8_t*)event->data, event->data_len);
      break;
    }
    default:
      break;
  }
}
```

This deletes the polling-based `ensureMqtt()` entirely — `esp_mqtt_client` reconnects on its own in a background task, so nothing needs to call it from `loop()`.

- [ ] **Step 4: Adapt `mqttCallback` to the new signature (same body, same command dispatch)**

Replace lines 147–221 (old `void mqttCallback(char* topic, byte* payload, unsigned int length) { ... }`, which built `topicStr` from the raw `topic`/`length` args) with:

```cpp
void mqttCallback(const char* topicStr, const uint8_t* payload, size_t length) {
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
    } else if (strcmp(cmd, "ota_update") == 0) {
      const char* url = doc["params"]["url"] | "";
      if (strlen(url) == 0) {
        publishCommandAck(requestId, false, "missing params.url");
      } else {
        performOtaUpdate(requestId, url);
      }
    } else {
      publishCommandAck(requestId, false, "unknown cmd");
    }
    return;
  }
}
```

Note this is the *same* body as before — only the function signature and the removed manual `topicStr` construction (now done once, in `mqttEventHandler`) changed.

- [ ] **Step 5: Switch the three publish functions to `esp_mqtt_client_publish`**

In `publishCommandAck` (was lines 262–279), replace:
```cpp
  if (!mqttClient.connected()) return;
```
with:
```cpp
  if (!mqttConnected) return;
```
and replace:
```cpp
  mqttClient.publish(topic, (const uint8_t*)buf, n, false);
```
with:
```cpp
  esp_mqtt_client_publish(mqttClient, topic, buf, n, 0, 0); // qos=0, retain=0 — same as before
```

In `publishSensorData` (was lines 310–338), replace:
```cpp
  if (!mqttClient.connected()) return;
```
with:
```cpp
  if (!mqttConnected) return;
```
and replace:
```cpp
  bool ok = mqttClient.publish(topic, (const uint8_t*)buf, n, false);

  Serial.print("Published sensor/data: ");
  Serial.print(buf);
  Serial.println(ok ? " [ok]" : " [FAILED]");
```
with:
```cpp
  int msgId = esp_mqtt_client_publish(mqttClient, topic, buf, n, 0, 0);

  Serial.print("Published sensor/data: ");
  Serial.print(buf);
  Serial.println(msgId >= 0 ? " [ok]" : " [FAILED]");
```

In `publishSensorStatus` (was lines 340–364), replace:
```cpp
  if (!mqttClient.connected()) return;
```
with:
```cpp
  if (!mqttConnected) return;
```
and replace:
```cpp
  mqttClient.publish(topic, (const uint8_t*)buf, n, false);
```
with:
```cpp
  esp_mqtt_client_publish(mqttClient, topic, buf, n, 0, 0);
```

- [ ] **Step 6: Replace the MQTT setup block in `setup()`**

Replace lines 399–403:
```cpp
  snprintf(topicBase, sizeof(topicBase), "sijagakali/%s", DEVICE_ID); // matches backend TOPICS: sijagakali/{device_id}/...
  snprintf(mqttClientId, sizeof(mqttClientId), "esp32-%s", DEVICE_ID);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setBufferSize(512); // default 256 is too small for sensor/data + status JSON
  mqttClient.setCallback(mqttCallback);
```
with:
```cpp
  snprintf(topicBase, sizeof(topicBase), "sijagakali/%s", DEVICE_ID); // matches backend TOPICS: sijagakali/{device_id}/...
  snprintf(mqttClientId, sizeof(mqttClientId), "esp32-%s", DEVICE_ID);

  esp_mqtt_client_config_t mqttCfg = {};
  mqttCfg.broker.address.uri = MQTT_BROKER_URI;
  mqttCfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  mqttCfg.credentials.username = MQTT_USER;
  mqttCfg.credentials.authentication.password = MQTT_PASSWORD;
  mqttCfg.credentials.client_id = mqttClientId;
  mqttCfg.session.keepalive = 45; // stay well under Cloudflare's free-tier ~100s idle websocket timeout
  mqttCfg.buffer.size = 512; // default 256 is too small for sensor/data + status JSON

  mqttClient = esp_mqtt_client_init(&mqttCfg);
  esp_mqtt_client_register_event(mqttClient, MQTT_EVENT_ANY, mqttEventHandler, nullptr);
  esp_mqtt_client_start(mqttClient);
```

- [ ] **Step 7: Remove the now-unused `ensureMqtt()` call from `loop()`**

In `loop()` (was lines 410–412):
```cpp
void loop() {
  ensureWifi();
  ensureMqtt();
```
becomes:
```cpp
void loop() {
  ensureWifi();
```
(`esp_mqtt_client` reconnects on its own in a background FreeRTOS task; nothing in `loop()` needs to drive it.)

- [ ] **Step 8: Drop the now-unused PubSubClient dependency**

In `sijagakali-firmware/platformio.ini`, change:
```ini
lib_deps =
    knolleary/PubSubClient@^2.8
    bblanchon/ArduinoJson@^7.2.0
```
to:
```ini
lib_deps =
    bblanchon/ArduinoJson@^7.2.0
```

- [ ] **Step 9: Full build**

Run (human executor):
```
pio run
```
Expected: clean build, no errors. If you get an undefined-reference/link error naming an `esp_mqtt_client_*` or `esp_crt_bundle_attach` symbol (as opposed to a missing-header error from Step 2), the header resolved but the corresponding library/component isn't linked into this board's precompiled Arduino core — stop and report back with the exact error before trying further workarounds.

- [ ] **Step 10: Flash to real hardware and verify against Task 1's broker (human executor — needs the physical ESP32-C5 + PlatformIO)**

1. Fill in the real `WIFI_SSID`, `WIFI_PASSWORD`, `MQTT_BROKER_URI` (your actual Cloudflare Tunnel hostname), and `MQTT_PASSWORD` (matching what you generated for `node-001` in Task 1 Step 1).
2. `pio run -t upload` then `pio device monitor`.
3. Expected serial output: `WiFi connected, IP=...`, then within a few seconds `MQTT: connected`, then every `readIntervalSec` a `Published sensor/data: {...} [ok]` line.
4. From a machine that can reach the broker's `1883` listener (e.g. Task 1's test terminal), run:
   ```
   "C:\Program Files\mosquitto\mosquitto_pub.exe" -h localhost -p 1883 -u sijagakali-backend -P CHANGE_ME_BACKEND_PASSWORD -t "sijagakali/node-001/config/interval" -m "{\"interval_sec\":15}"
   ```
   Expected: the device's serial monitor prints `Config: read_interval_sec updated to 15` within a second or two, and the sensor-read cadence visibly speeds up.

- [ ] **Step 11: Commit**

```bash
cd "d:/code-for-life/inspiralabs/projects/sijagakali/sijagakali-firmware"
git add src/main.cpp platformio.ini
git commit -m "feat: migrate MQTT client to esp_mqtt_client over wss for Cloudflare Tunnel"
```

---

## Follow-ups (explicitly out of scope for this plan)

- Set up the actual Cloudflare Tunnel (`cloudflared` config + `tunnel route dns`) pointing `dev-mqtt.inspiralabs.id` (or whatever hostname you choose) at `ws://localhost:1773` — already described conceptually in `architecture-overview/ALUR-SISTEM.md`, not re-derived here.
- Fill in `MQTT_USERNAME`/`MQTT_PASSWORD` in `sijagakali-api/mqtt-collector/.env` and add the equivalent to `sijagakali-ota/.env` (currently has none), both using the `sijagakali-backend` credentials created in Task 1.
- Repeat Task 1 Step 1's `mosquitto_passwd` command (new username = new `DEVICE_ID`) for every device beyond `node-001`.
