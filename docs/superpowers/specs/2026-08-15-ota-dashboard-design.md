# OTA Dashboard — Design Spec

## Purpose

Replace the current manual OTA workflow (build `.bin` → upload to R2 by hand → `mosquitto_pub` a JSON command by hand) with a small internal web dashboard: upload firmware, pick a device, click deploy, watch the result. Single operator, 1-10 ESP32 devices (sijagakali water-level sensors).

## Repo placement

New, separate repo: `sijagakali-ota`, sibling to `sijagakali-firmware`, `sijagakali-api`, `sijagakali-app` (same parent folder, same naming convention).

**Note on existing infrastructure:** `sijagakali-api` (Fastify + TypeScript + `@supabase/supabase-js` + `mqtt`, npm workspaces: `api`, `mqtt-collector`, `data-processing`, `notification-gateway`, `shared`) and `sijagakali-app` (React + Vite) already exist on disk with a stack matching what this project needs. Confirmed with the human partner this OTA dashboard is deliberately a **separate** repo, not an addition to those — but it targets the **same Supabase project/database** (the schema in the section below was read from that live project). Consequence: `sijagakali-ota`'s MQTT listener (below) runs as an independent process alongside `sijagakali-api`'s `mqtt-collector` — both subscribe to the same broker topics in parallel with distinct client IDs; this is normal MQTT (every subscriber gets its own copy of each message) and requires no coordination between the two codebases.

## Non-goals (explicitly out of scope)

- **Sensor/monitoring ingestion pipeline** (`sensor_readings`, `notification_logs`, threshold-based WhatsApp alerts, BMKG weather, CCTV). That data model already exists in Supabase but no backend code consumes it yet — this project does not build that pipeline. It only touches `device_configs` (adds 2 columns) and `mqtt_ingestion` (reuses as-is, for `command/ack` only).
- **Multi-user auth / roles.** Single admin only, backed by Supabase Auth + the existing `admins` table.
- **MD5/checksum verification of firmware**, or any change to the ESP32 firmware itself (already shipped in the `sijagakali-firmware` repo, unrelated to this project — see `sijagakali-firmware/docs/ota-update-guide.md`).
- **Presigned R2 URLs.** The device's MQTT command buffer is ~512 bytes total; presigned URLs (500-800 chars) risk silent truncation. Firmware objects must be served from a public R2 bucket/custom domain.
- **Supabase Realtime.** Dashboard uses simple polling instead — explicitly chosen over Realtime for this scale.
- **Full ingestion pipeline via this backend's MQTT listener.** The listener built here subscribes to exactly two topic patterns (`command/ack`, `sensor/status`) for OTA purposes only — not a general-purpose MQTT-to-Postgres bridge.

## Existing system context (do not rebuild)

Supabase Postgres already has this schema (confirmed via `information_schema` query against the live project — not designed by this spec, must not be altered beyond the additions below):

- `deployments` (PK `slug`) — physical site metadata (WhatsApp templates, contacts). Matches firmware's `DEPLOYMENT_SLUG`.
- `device_configs` (composite PK `(deployment_slug, device_id)`, FK `deployment_slug → deployments.slug`) — the device registry. Already has `sensor_height_cm`, `read_interval_sec`, thresholds, `is_active`, `last_seen_at`. **This is the device table this project attaches to — no new `devices` table.**
- `mqtt_ingestion` (PK `id` uuid, FK `(deployment_slug, device_id) → device_configs`, `correlation_id uuid NOT NULL`) — generic inbound-MQTT-message log. Reused here for `command/ack` only (has a real `request_id` to use as `correlation_id`). NOT used for `sensor/status` (no correlation_id in that payload — see Task 1).
- `sensor_readings`, `notification_logs`, `threshold_history` — belong to the (not-yet-built) monitoring pipeline. Untouched by this project.
- `admins` (PK `id` uuid, UNIQUE `email`) — admin accounts, `id` intended to match `auth.users.id` (standard Supabase profile-table pattern).

Firmware side (already shipped — see `src/main.cpp` and `docs/ota-update-guide.md` in the separate `sijagakali-firmware` repo, not this one):
- Command topic: `sijagakali/{device_id}/command`, payload `{"cmd":"ota_update","request_id":"...","params":{"url":"https://..."}}`.
- Ack topic: `sijagakali/{device_id}/command/ack`, payload `{"request_id":"...","ok":bool,"detail":"...","timestamp":"..."}`.
- Status heartbeat topic: `sijagakali/{device_id}/sensor/status`, payload includes `deployment_slug`, `device_id`, `firmware_version`, `online`, `timestamp`, published every `statusIntervalSec` (default 120s). **No `request_id`/`correlation_id` field.**
- Device does a plain HTTPS GET with no auth headers — hosting must be publicly readable.

## Architecture

```
┌─────────────┐         ┌──────────────────────────────────┐
│   Browser    │◀───────▶│  VPS                              │
│  (React app) │  HTTPS  │  ┌────────────┐   ┌─────────────┐ │
└─────────────┘         │  │  Fastify   │──▶│  Mosquitto  │ │
                         │  │  backend   │   │  (broker,   │ │
                         │  │ (TS)       │◀──│  localhost) │ │
                         │  └─────┬──────┘   └──────┬──────┘ │
                         │        │                  │        │
                         └────────┼──────────────────┼────────┘
                                  │                   │ MQTT
                          ┌───────┴────────┐          │
                          │   Supabase     │          ▼
                          │  (Postgres +   │   ┌──────────────┐
                          │   Auth)        │   │ ESP32 devices │
                          └────────────────┘   └──────────────┘
                          ┌────────────────┐
                          │  Cloudflare R2 │
                          │  (public       │
                          │   bucket)      │
                          └────────────────┘
```

Backend is one long-running Node process: Fastify HTTP server + a persistent MQTT client connection in the same process (not serverless — needs to stay subscribed).

## Data model

### Modify `device_configs`

```sql
alter table device_configs
  add column firmware_version text,
  add column firmware_updated_at timestamptz;
```

Populated when a `sensor/status` message arrives (see MQTT listener below). `last_seen_at` (already exists) is also bumped there — the dashboard derives "online" as `last_seen_at > now() - interval '360 seconds'`. The firmware's status-heartbeat interval (`statusIntervalSec`, `sijagakali-firmware/src/main.cpp:35`) defaults to 120s and is **not** currently configurable per device (only the sensor `read_interval_sec` is, via the existing `config/interval` MQTT command) — 360s is 3x that fixed 120s default, enough buffer for one missed heartbeat without flapping the status on every network hiccup.

### New table `firmware_releases`

```sql
create table firmware_releases (
  id uuid primary key default gen_random_uuid(),
  version text not null unique,
  r2_key text not null,
  file_size_bytes bigint not null,
  notes text,
  uploaded_by uuid references admins(id),
  created_at timestamptz not null default now()
);
```

`version` must be filename-safe (validated at upload time — task defines the exact regex) since it becomes part of the R2 object key: `r2_key = 'firmware/' || version || '.bin'`.

### New table `firmware_updates`

```sql
create table firmware_updates (
  id uuid primary key default gen_random_uuid(),
  deployment_slug text not null,
  device_id text not null,
  foreign key (deployment_slug, device_id) references device_configs (deployment_slug, device_id),
  firmware_release_id uuid not null references firmware_releases(id),
  requested_by uuid references admins(id),
  requested_at timestamptz not null default now(),
  mqtt_request_id uuid not null,
  status text not null default 'pending',
  ack_detail text,
  acked_at timestamptz
);
```

Named `firmware_updates`, not `ota_deployments` — `deployments` already means "physical site" in this schema; reusing the word for "an act of pushing firmware" would collide.

`status` values: `pending` (row just created, command published) → `acked_ok` | `acked_fail` (ack received) . No `timeout` state is written automatically by this spec — see Task-level plan for whether a stale-pending sweep is in scope; if not, `pending` rows that never get an ack just stay `pending`, which is honest (nothing timed it out; the dashboard can still show "requested Xh ago, no response").

## Backend API (Fastify, TypeScript)

All routes under `/api`, require `Authorization: Bearer <supabase-jwt>` verified against the Supabase project's JWT secret. No custom login endpoint — the frontend authenticates directly against Supabase Auth and forwards the resulting JWT.

| Method | Path | Behavior |
|---|---|---|
| GET | `/api/devices` | Join `device_configs` (+ derived `online` from `last_seen_at`, + `latest_firmware_version` = the most recently created row in `firmware_releases`, + derived `is_outdated = firmware_version !== latest_firmware_version`) |
| GET | `/api/firmware` | List `firmware_releases`, newest first |
| POST | `/api/firmware` | Multipart: file (`.bin`) + `version` + optional `notes`. Validates version format and uniqueness, uploads to R2 (`@aws-sdk/client-s3`, R2's S3-compatible endpoint), inserts `firmware_releases` row |
| POST | `/api/firmware/:id/deploy` | Body `{deployment_slug, device_id}`. Generates a `uuid` as `mqtt_request_id`, inserts `firmware_updates` (status=`pending`), publishes the `ota_update` MQTT command with `url = R2_PUBLIC_BASE_URL + '/' + r2_key`, returns the created row |
| GET | `/api/firmware-updates` | List `firmware_updates`, optional `?device_id=&deployment_slug=` filter, newest first |

## MQTT listener

Runs inside the same Fastify process, connects to the local broker on startup (same VPS, so plain `mqtt://localhost:1883`, no TLS needed for this hop).

- **Subscribe `sijagakali/+/command/ack`** (QoS 1): parse `device_id` from the topic, `request_id`/`ok`/`detail` from the JSON payload. Find the `firmware_updates` row where `mqtt_request_id = request_id`; update `status` (`ok:true` → `acked_ok`, `ok:false` → `acked_fail`), `ack_detail = detail`, `acked_at = now()`. Also insert one row into `mqtt_ingestion` (`message_type = 'ota_ack'`, `correlation_id = request_id`, `payload_json` = the raw payload) — reusing the existing table, since it already fits this message shape.
- **Subscribe `sijagakali/+/sensor/status`** (QoS 1): parse `device_id` from the topic, `deployment_slug`/`firmware_version` from the payload. Update `device_configs` (`firmware_version`, `firmware_updated_at = now()` if changed, `last_seen_at = now()` always) where `(deployment_slug, device_id)` matches. **Do not** insert into `mqtt_ingestion` for this topic — the payload has no `correlation_id` and that column is `NOT NULL`; inventing one would be a meaningless log entry.

An ack or status message for a `(deployment_slug, device_id)` pair with no matching `device_configs` row is dropped (logged to stderr, not persisted) — device registration in `device_configs` is a precondition, not something this listener creates on the fly.

## Frontend (React + Vite)

- **Login** — Supabase Auth email/password.
- **Devices** — table of `device_configs`: location, deployment_slug, device_id, current `firmware_version`, online (derived), **"update available" badge when `is_outdated`** (compares against the most recently uploaded `firmware_releases` row — global "latest," not a per-device target version; deploying is still always a manual click, this is just a visual hint), "Deploy" button per row.
- **Firmware** — table of `firmware_releases` (version, size, uploaded_at, notes) + upload form.
- **Deploy modal** — pick a `firmware_release` from a dropdown, confirm, `POST /api/firmware/:id/deploy`.
- **Update history** — table of `firmware_updates`, polls every few seconds while any row is `status = 'pending'` (stops polling once nothing is pending, to avoid needless load).

## Configuration (env vars, backend)

- `SUPABASE_URL`, `SUPABASE_SERVICE_ROLE_KEY` (backend uses service role, bypasses RLS — it's the sole gateway; frontend never talks to Supabase directly for this data)
- `SUPABASE_JWT_SECRET` (to verify the JWT the frontend forwards)
- `R2_ACCOUNT_ID`, `R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY`, `R2_BUCKET`, `R2_PUBLIC_BASE_URL`
- `MQTT_BROKER_URL` (default `mqtt://localhost:1883`)

## Delivery plan

Two implementation plans, in order:
1. **Backend** — new `sijagakali-ota` repo scaffold, DB migration, Fastify API, MQTT listener, R2 upload. Done when every endpoint above is testable via `curl`/Postman without any UI.
2. **Frontend** — React app consuming the finished backend API. Written after Plan 1 ships, so it can reference the API's actual response shapes instead of guessing ahead of implementation.

This spec document lives in `sijagakali-firmware/docs/superpowers/specs/` (where the brainstorming happened) even though it describes a different repo — once `sijagakali-ota` exists (Plan 1, Task 1), copy this file into that repo too so it travels with the code it actually describes.

## Security notes

- Even though the backend uses the Supabase service-role key (bypassing RLS), RLS should still be enabled with a deny-by-default policy on the two new tables — defense in depth in case the anon key ever ends up somewhere it shouldn't (per Supabase's own guidance: RLS is the CRITICAL-priority default, not optional even for backend-mediated access).
- The R2 bucket serving firmware must be public-read only for the `firmware/` prefix used here — do not make the whole bucket public if it holds anything else.
- Anyone who can publish to `sijagakali/{device_id}/command` can already trigger an OTA update directly (bypassing this dashboard entirely) — this dashboard does not change that MQTT-level trust boundary, already noted in `sijagakali-firmware/docs/ota-update-guide.md`.
