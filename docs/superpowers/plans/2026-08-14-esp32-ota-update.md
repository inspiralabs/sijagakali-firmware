# ESP32 OTA Update Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the deployed ESP32 device pull and flash new firmware over the air, triggered by an MQTT command, without any physical USB access.

**Architecture:** Add one new command (`ota_update`) to the existing MQTT command dispatcher in `src/main.cpp`. The device downloads a firmware `.bin` from a URL supplied in the command payload using the ESP32 Arduino core's built-in `HTTPUpdate` library, then acks the result over MQTT and reboots — following the exact same ack-then-restart pattern the `restart` command already uses.

**Tech Stack:** PlatformIO, Arduino framework (`framework-arduinoespressif32`), `HTTPUpdate`/`WiFiClientSecure`/`Update` (all bundled with the core — no new `lib_deps`).

**Spec:** No separate spec doc — this is a bounded, single-file change; design was agreed in chat (see conversation preceding this plan).

## Global Constraints

- Board is `esp32-c5-devkitc-1`, 4MB flash, no PSRAM. The board's default partition table (`default.csv`, used automatically since `platformio.ini` sets no `board_build.partitions`) already reserves `ota_0`/`ota_1` app slots (1.25MB / 0x140000 bytes each) plus `otadata` — **do not add or change any partition table setting.**
- No new `lib_deps` — `HTTPUpdate.h` and `WiFiClientSecure.h` ship inside `framework-arduinoespressif32` and are available as soon as `#include`d.
- OTA is triggered by publishing an MQTT command manually (`mosquitto_pub`, MQTT Explorer, etc.) to the existing `sijagakali/{device_id}/command` topic — same topic the `restart`/`calibrate`/`sample_now` commands already use. There is no backend/dashboard integration in scope.
- Firmware `.bin` hosting is out of scope for this repo: the operator pastes any public **HTTPS** URL (e.g. a GitHub Release asset) directly into the command payload. The device does not know or care where it's hosted.
- No MD5/checksum verification. The device trusts whatever the URL serves and uses `WiFiClientSecure::setInsecure()` (encrypted transport, no certificate validation) — consistent with trusting the MQTT command channel itself, which can already trigger `restart`.
- Command payload shape: `{"cmd":"ota_update","request_id":"...","params":{"url":"https://.../firmware.bin"}}`.

---

### Task 1: Add `ota_update` MQTT command

**Files:**
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: existing `publishCommandAck(const char* requestId, bool ok, const char* detail)` (src/main.cpp:252), existing `topicBase`/command dispatch pattern in `mqttCallback()` (src/main.cpp:144-211).
- Produces: `void performOtaUpdate(const char* requestId, const char* url)` — not consumed by any other task in this plan, single task.

- [ ] **Step 1: Add the OTA includes**

In `src/main.cpp`, right after the existing `#include <ArduinoJson.h>` (line 12), add:

```cpp
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
```

- [ ] **Step 2: Add the forward declaration**

Find the existing forward-declaration block (src/main.cpp:111-112):

```cpp
void mqttCallback(char* topic, byte* payload, unsigned int length); // forward decl, implemented in Task 5
void publishCommandAck(const char* requestId, bool ok, const char* detail); // forward decl, implemented in Task 5
```

Add a third line right after it:

```cpp
void performOtaUpdate(const char* requestId, const char* url); // forward decl, implemented below
```

- [ ] **Step 3: Wire the new command into `mqttCallback()`**

Find the command dispatch block in `mqttCallback()` (src/main.cpp:203-208):

```cpp
    } else if (strcmp(cmd, "sample_now") == 0) {
      trigger_cnt_reset_flag = 1; // force next loop() iteration to read+publish immediately
      publishCommandAck(requestId, true, "sampling on next cycle");
    } else {
      publishCommandAck(requestId, false, "unknown cmd");
    }
```

Replace it with:

```cpp
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
```

- [ ] **Step 4: Implement `performOtaUpdate()`**

Add this new function right after `publishCommandAck()` ends (src/main.cpp:269, right before `void publishSensorData(float waterLevelCm) {`):

```cpp
void performOtaUpdate(const char* requestId, const char* url) {
  Serial.print("OTA: updating from ");
  Serial.println(url);

  WiFiClientSecure client;
  client.setInsecure(); // trust the URL supplied via MQTT command; no fixed CA to pin against

  httpUpdate.rebootOnUpdate(false); // let us ack over MQTT before rebooting, same pattern as "restart"
  t_httpUpdate_return result = httpUpdate.update(client, url);

  switch (result) {
    case HTTP_UPDATE_OK:
      publishCommandAck(requestId, true, "update ok, restarting");
      delay(200); // let the publish flush before reboot
      ESP.restart();
      break;
    case HTTP_UPDATE_NO_UPDATES:
      publishCommandAck(requestId, false, "no update needed");
      break;
    case HTTP_UPDATE_FAILED:
    default: {
      char detail[128];
      snprintf(detail, sizeof(detail), "update failed: %s", httpUpdate.getLastErrorString().c_str());
      publishCommandAck(requestId, false, detail);
      break;
    }
  }
}
```

- [ ] **Step 5: Compile check**

Run: `pio run`
Expected: `SUCCESS`, and the flash-usage line (e.g. `Flash: [== ] XX.X% (used NNNNNN bytes from 1310720 bytes)`) reports usage **under 1,310,720 bytes** (the 1.25MB `ota_0`/`ota_1` partition size) — confirming the added code still fits the existing OTA partitions with no table changes.

- [ ] **Step 6: Flash the device and confirm boot is unaffected**

Run: `pio run -t upload -t monitor` (device connected via USB)
Expected serial output: same startup sequence as before (`=== A01ANY4B Modbus RTU sensor starting ===`, WiFi connects, `MQTT: connecting... connected`) — the new code must not change existing boot behavior.

- [ ] **Step 7: Manual end-to-end OTA test**

1. Build a second firmware you can tell apart from the first — e.g. temporarily change `FIRMWARE_VERSION` (src/main.cpp:38) to `"sijagakali-v1.0.1-ota-test"`, run `pio run`, and upload the resulting `.pio/build/esp32-c5-devkitc-1/firmware.bin` to a public HTTPS URL (e.g. a GitHub Release asset).
2. Flash the **original** (`v1.0.0`) firmware to the device via USB and let it connect to WiFi/MQTT.
3. From a machine with `mosquitto_pub`, trigger the update (replace host/URL/device id as appropriate):

   ```bash
   mosquitto_pub -h <broker_host> -t "sijagakali/node-001/command" -m '{"cmd":"ota_update","request_id":"test-1","params":{"url":"https://.../firmware.bin"}}'
   ```

4. Expected on the device's serial monitor: `OTA: updating from https://...`, then a reboot.
5. Expected on MQTT: a message on `sijagakali/node-001/command/ack` with `"ok":true,"detail":"update ok, restarting"` published *before* the reboot.
6. After reboot, confirm the update landed: watch `sijagakali/node-001/sensor/status` (published every `statusIntervalSec`, default 120s) and check `firmware_version` now reads `"sijagakali-v1.0.1-ota-test"`.
7. Revert the temporary `FIRMWARE_VERSION` test change (it was only for telling builds apart in this test) — this revert is not committed.

- [ ] **Step 8: Commit**

```bash
git add src/main.cpp
git commit -m "feat: add ota_update MQTT command using HTTPUpdate"
```
