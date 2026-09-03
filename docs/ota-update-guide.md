# Panduan Upload Firmware via OTA

Device yang **belum pernah** di-flash tidak bisa menerima OTA — minimal sekali harus lewat USB dulu (lihat bagian "Jalur 1" di bawah). Setelah itu, update selanjutnya bisa lewat OTA (Jalur 2).

## Jalur 1: USB manual (wajib untuk pertama kali)

```bash
pio run -t upload -t monitor
```

Device connect ke WiFi & MQTT. Baru sejak titik ini OTA bisa dipakai.

## Jalur 2: OTA (untuk update selanjutnya)

```
┌─────────────┐    1. build .bin     ┌──────────────┐
│   Kamu di    │ ───────────────────▶│  firmware.bin │
│   laptop     │    (pio run)         └──────┬───────┘
└─────────────┘                              │ 2. upload manual
                                              ▼
                                     ┌──────────────────┐
                                     │  Hosting publik   │
                                     │  (GitHub Release, │
                                     │   dst — HTTPS)    │
                                     └────────┬──────────┘
                                              │ URL
┌─────────────┐   3. publish command MQTT     │
│   Kamu di    │──────────────────────────────┼────────▶ ┌─────────────────┐
│  mosquitto_  │  {"cmd":"ota_update",         │          │  MQTT Broker    │
│  pub / MQTT  │   "params":{"url":"..."}}     │          └────────┬────────┘
│  Explorer    │                                          topic:  │
└─────────────┘                                sijagakali/{device_id}/command
                                                                    │
                                                                    ▼
                                                          ┌───────────────────┐
                                                          │   Device ESP32-C5  │
                                                          └─────────┬───────────┘
                                                                    │ 4. download via HTTPS
                                                                    ▼
                                                          5. flash ke partisi OTA
                                                          6. ack ke command/ack
                                                          7. reboot ke firmware baru
```

### Langkah detail

**1. Build firmware baru**

```bash
pio run
```

Hasilnya ada di `.pio/build/esp32-c5-devkitc-1/firmware.bin`.

**2. Upload `.bin` ke hosting publik HTTPS**

Tidak ada backend untuk ini — murni manual. Contoh: bikin GitHub Release, attach file `.bin`, copy link asset-nya.

**3. Kirim command MQTT**

Topic: `sijagakali/{device_id}/command` (device_id sesuai `DEVICE_ID` di `src/main.cpp`, default `node-001`).

```bash
mosquitto_pub -h <broker_host> -t "sijagakali/node-001/command" \
  -m '{"cmd":"ota_update","request_id":"upd-001","params":{"url":"https://github.com/.../firmware.bin"}}'
```

**4-7. Yang terjadi otomatis di device** (`mqttCallback()` → `performOtaUpdate()` di `src/main.cpp`):

- Device download `.bin` dari URL lewat HTTPS (redirect GitHub Release diikuti otomatis)
- `HTTPUpdate` cek header image valid, tulis ke partisi OTA yang tidak sedang dipakai (`ota_0`/`ota_1` bergantian)
- **Berhasil:** publish `{"ok":true,"detail":"update ok, restarting"}` ke `sijagakali/{device_id}/command/ack`, lalu reboot ke firmware baru
- **Gagal** (download putus, file korup, dsb): publish `{"ok":false,"detail":"update failed: ..."}`, device **tetap jalan pakai firmware lama** — tidak reboot

**8. Verifikasi berhasil**

Setelah device reboot dan connect ulang, dia publish heartbeat ke `sijagakali/{device_id}/sensor/status` — cek field `firmware_version` sudah berubah sesuai build baru.

## Safety net: rollback otomatis

Kalau firmware baru gagal connect WiFi/MQTT sama sekali (misal ada bug), bootloader ESP32 otomatis **rollback ke firmware sebelumnya** — device tidak akan stuck di firmware rusak tanpa bisa dihubungi dari jarak jauh.

## Checklist sebelum OTA pertama di lapangan

- [ ] Broker MQTT sudah pakai auth (bukan anonymous) — sejak ada `ota_update`, siapapun yang bisa publish ke topic command bisa flash firmware apa saja ke device secara permanen
- [ ] URL firmware pakai HTTPS
- [ ] Sudah pernah dites end-to-end di bench sebelum dipakai ke device yang sudah terpasang di lokasi

## Batasan yang disengaja (bukan bug)

- **Tidak ada verifikasi MD5/checksum** — device percaya HTTPS transport saja (`WiFiClientSecure::setInsecure()`, tanpa validasi sertifikat)
- **Tidak ada integrasi backend** — trigger command harus manual lewat MQTT client
- **URL command dibatasi ~400 karakter** (buffer MQTT 512 byte) — link presigned S3/GCS yang panjang bisa gagal diam-diam; GitHub Release URL aman
