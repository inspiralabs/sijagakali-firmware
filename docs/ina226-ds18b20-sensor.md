# Sensor Baterai (INA226) & Suhu (DS18B20) — DITUNDA

> **Status:** tidak dipakai di Fase 1 SiJagaKali kedua. Kode di bawah sudah pernah
> **lolos build** untuk `esp32-c5-devkitc-1` (pioarduino, 2026-10-04), tapi belum diuji di
> hardware. Dikeluarkan dari `src/main.cpp` dan disimpan di sini untuk fase berikutnya.
> Rencananya masuk sebagai `sijagakali-v1.2.0`.

## Hardware

- Baterai: Solstellar LiFePO4 12,8 V 50 Ah (4S, BMS proteksi saja, tanpa komunikasi).
- MPPT: SAMOTO MPPT30A. Port RS485/RS232 **tidak berfungsi** (datasheet resmi:
  "Communication methods RS485/RS232 → Not Function"), jadi data baterai tidak bisa
  diambil dari MPPT.
- INA226 modul I2C, shunt **R100** (0,1 Ω, maks ±0,8 A — cukup untuk arus node). Untuk
  ikut mengukur arus charging, pakai shunt eksternal 20 A / 75 mV.
- DS18B20 versi probe waterproof.

### Wiring ke ESP32-C5

| Perangkat | Pin | ESP32-C5 |
|---|---|---|
| INA226 | VCC / GND | 3V3 / GND |
| INA226 | SDA / SCL | GPIO0 / GPIO1 (I2C default variant esp32c5) |
| INA226 | A0, A1 | GND → alamat 0x40 |
| INA226 | IN+ | BAT+ (lewat sekring 2 A) |
| INA226 | IN− | input + buck 12V→5V |
| INA226 | VBS | kabel kecil terpisah **langsung dari kutub BAT+** (tanpa arus → tanpa drop) |
| DS18B20 | merah / hitam | 3V3 / GND |
| DS18B20 | kuning (DATA) | GPIO3 + pull-up **4,7 kΩ** ke 3V3 |

GPIO4/5 sudah dipakai sensor ultrasonik, GPIO27 = LED RGB onboard. Semua ground jadi satu.

## platformio.ini

`paulstoffregen/OneWire` 2.3.8 **gagal compile di ESP32-C5** (register GPIO berupa struct).
Pakai OneWireNg yang menyediakan `OneWire.h` kompatibel, dan abaikan OneWire bawaan yang
ditarik otomatis oleh DallasTemperature:

```ini
lib_deps =
    bblanchon/ArduinoJson@^7.2.0
    robtillaart/INA226@^0.6.6
    pstolarz/OneWireNg@^0.14.1
    milesburton/DallasTemperature@^4.0.6
lib_ignore = OneWire ; tidak support ESP32-C5; OneWireNg menyediakan OneWire.h yang kompatibel
```

## include/battery_soc.h

```cpp
#pragma once

// SoC kasar LiFePO4 4S (12,8 V) dari tegangan ISTIRAHAT. Saat di-charge tegangan naik
// (14,2-14,6 V) sehingga terbaca 100%; itu memang keterbatasan metode tegangan.
// ponytail: tabel tegangan, kurva LiFePO4 datar -> meleset ±10-20% di tengah. Upgrade ke
// coulomb counting (integrasi arus INA226) kalau butuh % yang akurat.
inline int lifepo4SocPct(float v) {
  static const float V[]   = {10.0f, 12.0f, 12.8f, 13.0f, 13.1f, 13.2f, 13.3f, 13.4f};
  static const float PCT[] = {   0,    10,    20,    30,    40,    70,    90,   100};
  const int n = sizeof(V) / sizeof(V[0]);
  if (v <= V[0]) return 0;
  if (v >= V[n - 1]) return 100;
  int i = 1;
  while (v > V[i]) i++;
  float pct = PCT[i - 1] + (v - V[i - 1]) * (PCT[i] - PCT[i - 1]) / (V[i] - V[i - 1]);
  return (int)(pct + 0.5f);
}
```

## test/test_battery_soc/test_main.cpp

```cpp
// Jalankan di board: pio test -e esp32-c5-devkitc-1 -f test_battery_soc
#include <Arduino.h>
#include <unity.h>
#include "battery_soc.h"

void test_soc() {
  TEST_ASSERT_EQUAL(0, lifepo4SocPct(9.0f));      // di bawah cutoff BMS
  TEST_ASSERT_EQUAL(0, lifepo4SocPct(10.0f));
  TEST_ASSERT_EQUAL(10, lifepo4SocPct(12.0f));    // titik tabel persis
  TEST_ASSERT_EQUAL(55, lifepo4SocPct(13.15f));   // interpolasi 40..70
  TEST_ASSERT_EQUAL(100, lifepo4SocPct(13.4f));
  TEST_ASSERT_EQUAL(100, lifepo4SocPct(14.6f));   // sedang di-charge
}

void setup() {
  delay(2000);
  UNITY_BEGIN();
  RUN_TEST(test_soc);
  UNITY_END();
}

void loop() {}
```

## Perubahan di src/main.cpp

### 1. Include (setelah `#include "esp_crt_bundle.h"`)

```cpp
#include <Wire.h>
#include <INA226.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "battery_soc.h"
```

### 2. Global + pembacaan (setelah `HardwareSerial sensorSerial(1);`)

```cpp
// INA226 (I2C 0x40) high-side antara BAT+ dan input buck: tegangan baterai + arus node.
// SDA->GPIO0  SCL->GPIO1  VCC->3V3. VBS ditarik kabel terpisah langsung dari kutub BAT+.
// DS18B20 probe: DATA->GPIO3 + pull-up 4,7k ke 3V3.
#define I2C_SDA 0
#define I2C_SCL 1
#define INA226_SHUNT_OHM 0.1f   // modul bertanda R100; ganti ke 0.01 untuk R010
#define INA226_MAX_CURRENT_A 0.8f
#define DS18B20_PIN 3

INA226 ina(0x40);
OneWire oneWire(DS18B20_PIN);
DallasTemperature tempSensor(&oneWire);

// Faktor koreksi tegangan = multimeter / INA226, diset lewat command "calibrate_battery".
float batteryVCal = 1.0f;
bool inaReady = false;
// Hasil baca terakhir; NAN = tidak tersedia (field dikirim null + *_error, bukan angka palsu).
float batteryV = NAN, batteryMa = NAN, batteryVRaw = NAN, tempC = NAN;
const char* batteryError = nullptr;
const char* tempError = nullptr;

void readAuxSensors() {
  if (!inaReady) inaReady = ina.begin() && ina.setMaxCurrentShunt(INA226_MAX_CURRENT_A, INA226_SHUNT_OHM) == 0;
  if (inaReady && ina.isConnected()) {
    batteryVRaw = ina.getBusVoltage();
    batteryV = batteryVRaw * batteryVCal;
    batteryMa = ina.getCurrent_mA();
    batteryError = nullptr;
  } else {
    inaReady = false; // coba init ulang di siklus berikutnya (modul bisa dicolok belakangan)
    batteryV = batteryMa = batteryVRaw = NAN;
    batteryError = "ina226_not_found";
  }

  tempSensor.requestTemperatures(); // blok ~750 ms (resolusi 12-bit)
  float t = tempSensor.getTempCByIndex(0);
  if (t == DEVICE_DISCONNECTED_C) {
    tempSensor.begin(); // enumerasi ulang kalau probe baru dicolok
    tempC = NAN;
    tempError = "ds18b20_disconnected";
  } else if (t == 85.0f) {
    tempC = NAN; // nilai power-on, bukan suhu asli
    tempError = "ds18b20_power_on_reset";
  } else {
    tempC = t;
    tempError = nullptr;
  }
  Serial.printf("Baterai: %.3f V (raw %.3f) %.1f mA | Suhu: %.2f C\n", batteryV, batteryVRaw, batteryMa, tempC);
}

void addAuxFields(JsonDocument& doc) {
  if (isnan(batteryV)) {
    doc["battery_v"] = nullptr;
    doc["battery_error"] = batteryError;
  } else {
    doc["battery_v"] = roundf(batteryV * 100) / 100.0f;
    doc["battery_ma"] = roundf(batteryMa * 10) / 10.0f;
    doc["battery_pct"] = lifepo4SocPct(batteryV);
  }
  if (isnan(tempC)) {
    doc["temp_c"] = nullptr;
    doc["temp_error"] = tempError;
  } else {
    doc["temp_c"] = roundf(tempC * 10) / 10.0f;
  }
}
```

### 3. Command kalibrasi (di `mqttCallback`, sebelum cabang `sample_now`)

```cpp
    } else if (strcmp(cmd, "calibrate_battery") == 0) {
      // Ukur kutub baterai dengan multimeter (sebaiknya saat istirahat/malam), kirim angkanya.
      float refV = doc["params"]["reference_battery_v"] | 0.0f;
      if (refV < 8.0f || refV > 16.0f) {
        publishCommandAck(requestId, false, "params.reference_battery_v must be 8..16");
      } else if (isnan(batteryVRaw) || batteryVRaw < 1.0f) {
        publishCommandAck(requestId, false, "no ina226 reading");
      } else {
        batteryVCal = refV / batteryVRaw;
        prefs.putFloat("batCal", batteryVCal);
        char detail[64];
        snprintf(detail, sizeof(detail), "battery_v_cal=%.4f", batteryVCal);
        publishCommandAck(requestId, true, detail);
      }
```

Contoh: `{"cmd":"calibrate_battery","request_id":"...","params":{"reference_battery_v":13.28}}`
ke `sijagakali/<device_id>/command`.

### 4. Payload

- `publishSensorData()`: ganti komentar `// no battery sensor on this board...` dengan
  `addAuxFields(doc);`, buffer `char buf[384]` → `char buf[480]`.
- `publishSensorStatus()`: tambah `addAuxFields(doc);` setelah `heap_free_bytes`, buffer
  `char buf[320]` → `char buf[480]` (supaya baterai/suhu tetap terpantau walau ultrasonik gagal).
- `mqttCfg.buffer.size = 512` sudah cukup.

### 5. setup() (setelah baca `interval` dari Preferences) dan loop()

```cpp
  batteryVCal = prefs.getFloat("batCal", 1.0f);

  Wire.begin(I2C_SDA, I2C_SCL);
  tempSensor.begin();
  readAuxSensors(); // isi nilai awal agar heartbeat pertama sudah memuat baterai/suhu
```

Di `loop()`, panggil `readAuxSensors();` tepat setelah `trigger_cnt_reset_flag = 0;`.

## Payload yang dihasilkan

```json
{ "battery_v": 13.28, "battery_ma": 61.5, "battery_pct": 82, "temp_c": 29.4 }
```

Saat sensor tidak ada/rusak (tidak pernah mengirim angka palsu):

```json
{ "battery_v": null, "battery_error": "ina226_not_found",
  "temp_c": null, "temp_error": "ds18b20_disconnected" }
```

Backend (`sijagakali-api`): `battery_pct` sudah tersimpan ke `sensor_readings.battery_pct`.
`battery_v`, `battery_ma`, `temp_c` hanya ada di payload mentah (`mqtt_ingestion.payload_json`)
sampai kolomnya ditambahkan.

## Catatan kalibrasi

Angka INA226 boleh beda 0,1-0,2 V dari layar MPPT (akurasi layar ±1-2%, drop kabel saat
charging). Patokannya multimeter di kutub baterai saat istirahat, lalu kirim `calibrate_battery`.
