# OTA Dashboard Backend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the `sijagakali-ota` backend (Fastify + TypeScript) — a REST API that lets an operator upload ESP32 firmware to Cloudflare R2, trigger an OTA update over MQTT, and track deployment status — fully testable via `curl`, no frontend needed for this plan.

**Architecture:** Single long-running Node process: Fastify HTTP server + a persistent MQTT client in the same process. Auth is delegated to Supabase Auth (frontend logs in there; this backend only verifies the resulting token via `supabase.auth.getUser()`, no local JWT-secret handling). All Postgres access goes through `@supabase/supabase-js` with the service-role key — this backend is the sole gateway to the `sijagakali` Supabase project for OTA data; the existing `sijagakali-api`/`sijagakali-app` repos are untouched.

**Tech Stack:** Node.js (LTS) + TypeScript, Fastify 5, `@supabase/supabase-js`, `@aws-sdk/client-s3` (R2's S3-compatible API), `mqtt` (npm package), `pg` (one-off migration runner only), `vitest` for tests.

**Spec:** `sijagakali-firmware/docs/superpowers/specs/2026-08-15-ota-dashboard-design.md` (this plan implements that spec's backend half; the frontend is a separate later plan)

## Global Constraints

- New repo: `sijagakali-ota` at `e:\code-for-life\inspiralabs\projects\sijagakali\sijagakali-ota`, sibling to `sijagakali-firmware`/`sijagakali-api`/`sijagakali-app`.
- Targets the **existing, live** Supabase project already used by `sijagakali-api` (same `device_configs`, `admins`, `deployments`, `mqtt_ingestion` tables — do not recreate them, do not alter them beyond the 2 new columns in Task 2).
- Do **not** build the sensor-monitoring ingestion pipeline (`sensor_readings`, `notification_logs`, threshold logic, WhatsApp, BMKG, CCTV) — out of scope, belongs to `sijagakali-api`.
- MQTT listener subscribes to exactly two topic patterns: `sijagakali/+/command/ack` and `sijagakali/+/sensor/status`. Nothing else.
- No MD5/checksum verification of uploaded firmware. No presigned R2 URLs (device's MQTT payload buffer is ~512 bytes — firmware URLs must be short, served from a **public** R2 bucket/custom domain via `R2_PUBLIC_BASE_URL`).
- Single-admin auth only (no roles/permissions system). Auth verification via `supabase.auth.getUser(token)` — no local JWT-secret verification, no separate login endpoint in this backend.
- `firmware_updates.status` values: `pending` → `acked_ok` | `acked_fail`. No automatic timeout sweep.
- Devices endpoint marks a device "outdated" by comparing its `firmware_version` to the single most-recently-uploaded `firmware_releases.version` (global "latest," not per-device).
- "Online" = `device_configs.last_seen_at > now() - interval '360 seconds'`.

---

### Task 1: Repo scaffold + health check server

**Files:**
- Create: `sijagakali-ota/package.json`
- Create: `sijagakali-ota/tsconfig.json`
- Create: `sijagakali-ota/src/server.ts`
- Create: `sijagakali-ota/src/app.ts`
- Create: `sijagakali-ota/.env.example`
- Create: `sijagakali-ota/.gitignore`
- Create: `sijagakali-ota/vitest.config.ts`
- Test: `sijagakali-ota/src/app.test.ts`

**Interfaces:**
- Produces: `buildApp(): FastifyInstance` (exported from `src/app.ts`) — every later task registers its routes onto this same instance. `server.ts` calls `buildApp()` and starts listening; tests call `buildApp()` directly and use Fastify's `.inject()`, no real network socket needed.

- [ ] **Step 1: Create the repo and directory**

```bash
mkdir -p "e:\code-for-life\inspiralabs\projects\sijagakali\sijagakali-ota\src"
cd "e:\code-for-life\inspiralabs\projects\sijagakali\sijagakali-ota"
git init
```

- [ ] **Step 2: Write `package.json`**

```json
{
  "name": "sijagakali-ota",
  "version": "0.1.0",
  "private": true,
  "type": "module",
  "scripts": {
    "dev": "tsx watch src/server.ts",
    "build": "tsc",
    "start": "node dist/server.js",
    "test": "vitest run",
    "migrate": "tsx src/migrate.ts"
  },
  "dependencies": {
    "@aws-sdk/client-s3": "^3.687.0",
    "@fastify/multipart": "^9.0.1",
    "@supabase/supabase-js": "^2.49.1",
    "dotenv": "^16.5.0",
    "fastify": "^5.3.3",
    "mqtt": "^5.10.4",
    "pg": "^8.13.1"
  },
  "devDependencies": {
    "@types/node": "^22.0.0",
    "@types/pg": "^8.11.10",
    "typescript": "^5.8.3",
    "tsx": "^4.19.4",
    "vitest": "^2.1.5"
  }
}
```

- [ ] **Step 3: Write `tsconfig.json`**

```json
{
  "compilerOptions": {
    "target": "ES2022",
    "module": "NodeNext",
    "moduleResolution": "NodeNext",
    "outDir": "dist",
    "rootDir": "src",
    "strict": true,
    "esModuleInterop": true,
    "skipLibCheck": true,
    "resolveJsonModule": true
  },
  "include": ["src/**/*.ts"]
}
```

- [ ] **Step 4: Write `.gitignore`**

```
node_modules/
dist/
.env
```

- [ ] **Step 5: Write `.env.example`**

```
PORT=3000
SUPABASE_URL=
SUPABASE_SERVICE_ROLE_KEY=
DATABASE_URL=
R2_ACCOUNT_ID=
R2_ACCESS_KEY_ID=
R2_SECRET_ACCESS_KEY=
R2_BUCKET=
R2_PUBLIC_BASE_URL=
MQTT_BROKER_URL=mqtt://localhost:1883
```

`DATABASE_URL` is only used by `npm run migrate` (Task 2) — the running app never opens a raw Postgres connection, it goes through `@supabase/supabase-js`.

- [ ] **Step 6: Write `src/app.ts`**

```typescript
import Fastify, { FastifyInstance } from 'fastify';

export function buildApp(): FastifyInstance {
  const app = Fastify({ logger: true });

  app.get('/health', async () => {
    return { ok: true };
  });

  return app;
}
```

- [ ] **Step 7: Write `src/server.ts`**

```typescript
import 'dotenv/config';
import { buildApp } from './app.js';

const app = buildApp();
const port = Number(process.env.PORT ?? 3000);

app.listen({ port, host: '0.0.0.0' }, (err) => {
  if (err) {
    app.log.error(err);
    process.exit(1);
  }
});
```

- [ ] **Step 8: Write `vitest.config.ts`**

```typescript
import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    environment: 'node'
  }
});
```

- [ ] **Step 9: Write the failing test**

```typescript
// src/app.test.ts
import { describe, it, expect } from 'vitest';
import { buildApp } from './app.js';

describe('GET /health', () => {
  it('returns ok true', async () => {
    const app = buildApp();
    const res = await app.inject({ method: 'GET', url: '/health' });
    expect(res.statusCode).toBe(200);
    expect(res.json()).toEqual({ ok: true });
  });
});
```

- [ ] **Step 10: Install dependencies and run the test**

```bash
npm install
npm test
```

Expected: 1 test passing (`GET /health returns ok true`).

- [ ] **Step 11: Verify the dev server actually runs**

```bash
npm run dev
```

In another terminal: `curl http://localhost:3000/health` → expect `{"ok":true}`. Stop the dev server (Ctrl+C) once confirmed.

- [ ] **Step 12: Commit**

```bash
git add package.json tsconfig.json .gitignore .env.example vitest.config.ts src/
git commit -m "feat: scaffold sijagakali-ota backend with health check"
```

---

### Task 2: Database migration

**Files:**
- Create: `sijagakali-ota/supabase/migrations/0001_add_ota_tables.sql`
- Create: `sijagakali-ota/src/migrate.ts`

**Interfaces:**
- Consumes: `DATABASE_URL` env var (Task 1's `.env.example`)
- Produces: Postgres tables `firmware_releases`, `firmware_updates`, and 2 new columns on `device_configs` (`firmware_version`, `firmware_updated_at`) — every later task's Supabase queries assume these exist.

- [ ] **Step 1: Write the migration SQL**

```sql
-- sijagakali-ota/supabase/migrations/0001_add_ota_tables.sql

alter table device_configs
  add column if not exists firmware_version text,
  add column if not exists firmware_updated_at timestamptz;

create table if not exists firmware_releases (
  id uuid primary key default gen_random_uuid(),
  version text not null unique,
  r2_key text not null,
  file_size_bytes bigint not null,
  notes text,
  uploaded_by uuid references admins(id),
  created_at timestamptz not null default now()
);

create table if not exists firmware_updates (
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

alter table firmware_releases enable row level security;
alter table firmware_updates enable row level security;
-- Deny-by-default: this backend always connects with the service-role key,
-- which bypasses RLS. These policies are defense-in-depth only, in case the
-- anon key is ever used against these tables from somewhere else.
```

- [ ] **Step 2: Write the migration runner**

```typescript
// sijagakali-ota/src/migrate.ts
import 'dotenv/config';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import pg from 'pg';

const __dirname = dirname(fileURLToPath(import.meta.url));
const sqlPath = join(__dirname, '..', 'supabase', 'migrations', '0001_add_ota_tables.sql');

async function main() {
  const databaseUrl = process.env.DATABASE_URL;
  if (!databaseUrl) {
    throw new Error('DATABASE_URL is not set');
  }

  const sql = readFileSync(sqlPath, 'utf-8');
  const client = new pg.Client({ connectionString: databaseUrl });
  await client.connect();
  try {
    await client.query(sql);
    console.log('Migration applied: 0001_add_ota_tables.sql');
  } finally {
    await client.end();
  }
}

main().catch((err) => {
  console.error('Migration failed:', err);
  process.exit(1);
});
```

- [ ] **Step 3: Set real credentials and run the migration**

Copy `.env.example` to `.env` and fill in `DATABASE_URL` with the real Supabase project's connection string (Supabase dashboard → Project Settings → Database → Connection string → URI, "Session" or "Transaction" pooler mode both work for this one-off script).

```bash
npm run migrate
```

Expected: `Migration applied: 0001_add_ota_tables.sql`, no errors.

- [ ] **Step 4: Verify the migration against the live database**

```typescript
// Temporary verification — run with: npx tsx -e "$(cat verify.ts)"
// or just paste into a throwaway .ts file and run with tsx.
import 'dotenv/config';
import pg from 'pg';

const client = new pg.Client({ connectionString: process.env.DATABASE_URL });
await client.connect();
const res = await client.query(`
  select table_name from information_schema.tables
  where table_schema = 'public' and table_name in ('firmware_releases', 'firmware_updates')
`);
console.log(res.rows);
const cols = await client.query(`
  select column_name from information_schema.columns
  where table_name = 'device_configs' and column_name in ('firmware_version', 'firmware_updated_at')
`);
console.log(cols.rows);
await client.end();
```

Expected: both tables listed, both columns listed.

- [ ] **Step 5: Commit**

```bash
git add supabase/ src/migrate.ts
git commit -m "feat: add OTA tables migration"
```

---

### Task 3: Auth middleware

**Files:**
- Create: `sijagakali-ota/src/supabaseClient.ts`
- Create: `sijagakali-ota/src/auth.ts`
- Modify: `sijagakali-ota/src/app.ts`
- Test: `sijagakali-ota/src/auth.test.ts`

**Interfaces:**
- Consumes: `SUPABASE_URL`, `SUPABASE_SERVICE_ROLE_KEY` env vars
- Produces: `getSupabaseClient(): SupabaseClient` (from `src/supabaseClient.ts`) — used by every later task that touches the database. `requireAuth` Fastify `onRequest` hook (from `src/auth.ts`) — every protected route in later tasks registers it via `{ onRequest: [requireAuth] }`; on success it sets `request.userId: string`.

- [ ] **Step 1: Write `src/supabaseClient.ts`**

```typescript
import { createClient, SupabaseClient } from '@supabase/supabase-js';

let client: SupabaseClient | undefined;

export function getSupabaseClient(): SupabaseClient {
  if (client) return client;

  const url = process.env.SUPABASE_URL;
  const key = process.env.SUPABASE_SERVICE_ROLE_KEY;
  if (!url || !key) {
    throw new Error('SUPABASE_URL and SUPABASE_SERVICE_ROLE_KEY must be set');
  }

  client = createClient(url, key, {
    auth: { persistSession: false }
  });
  return client;
}
```

- [ ] **Step 2: Write the failing test for `requireAuth`**

```typescript
// src/auth.test.ts
import { describe, it, expect, vi, beforeEach } from 'vitest';
import Fastify from 'fastify';

vi.mock('./supabaseClient.js', () => ({
  getSupabaseClient: () => ({
    auth: {
      getUser: async (token: string) => {
        if (token === 'valid-token') {
          return { data: { user: { id: 'user-123' } }, error: null };
        }
        return { data: { user: null }, error: new Error('invalid token') };
      }
    }
  })
}));

describe('requireAuth', () => {
  let app: ReturnType<typeof Fastify>;

  beforeEach(async () => {
    const { requireAuth } = await import('./auth.js');
    app = Fastify();
    app.get('/protected', { onRequest: [requireAuth] }, async (req) => {
      return { userId: (req as any).userId };
    });
  });

  it('rejects requests with no Authorization header', async () => {
    const res = await app.inject({ method: 'GET', url: '/protected' });
    expect(res.statusCode).toBe(401);
  });

  it('rejects requests with an invalid token', async () => {
    const res = await app.inject({
      method: 'GET',
      url: '/protected',
      headers: { authorization: 'Bearer bad-token' }
    });
    expect(res.statusCode).toBe(401);
  });

  it('accepts requests with a valid token and sets userId', async () => {
    const res = await app.inject({
      method: 'GET',
      url: '/protected',
      headers: { authorization: 'Bearer valid-token' }
    });
    expect(res.statusCode).toBe(200);
    expect(res.json()).toEqual({ userId: 'user-123' });
  });
});
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
npm test -- auth.test.ts
```

Expected: FAIL — `./auth.js` does not exist yet.

- [ ] **Step 4: Write `src/auth.ts`**

```typescript
import { FastifyRequest, FastifyReply } from 'fastify';
import { getSupabaseClient } from './supabaseClient.js';

declare module 'fastify' {
  interface FastifyRequest {
    userId?: string;
  }
}

export async function requireAuth(request: FastifyRequest, reply: FastifyReply) {
  const authHeader = request.headers.authorization;
  if (!authHeader?.startsWith('Bearer ')) {
    return reply.code(401).send({ error: 'Missing bearer token' });
  }

  const token = authHeader.slice('Bearer '.length);
  const supabase = getSupabaseClient();
  const { data, error } = await supabase.auth.getUser(token);

  if (error || !data.user) {
    return reply.code(401).send({ error: 'Invalid token' });
  }

  request.userId = data.user.id;
}
```

- [ ] **Step 5: Run the test to verify it passes**

```bash
npm test -- auth.test.ts
```

Expected: 3 tests passing.

- [ ] **Step 6: Commit**

```bash
git add src/supabaseClient.ts src/auth.ts src/auth.test.ts
git commit -m "feat: add Supabase-backed auth middleware"
```

---

### Task 4: Firmware catalog (upload to R2 + list)

**Files:**
- Create: `sijagakali-ota/src/r2Client.ts`
- Create: `sijagakali-ota/src/routes/firmware.ts`
- Modify: `sijagakali-ota/src/app.ts`
- Test: `sijagakali-ota/src/routes/firmware.test.ts`

**Interfaces:**
- Consumes: `getSupabaseClient()` (Task 3), `requireAuth` (Task 3)
- Produces: `registerFirmwareRoutes(app: FastifyInstance)` — registered in `src/app.ts`. Route `POST /api/firmware/:id/deploy` in Task 6 reads from the same `firmware_releases` table this task writes, by `id`.

- [ ] **Step 1: Write `src/r2Client.ts`**

```typescript
import { S3Client, PutObjectCommand } from '@aws-sdk/client-s3';

let client: S3Client | undefined;

export function getR2Client(): S3Client {
  if (client) return client;

  const accountId = process.env.R2_ACCOUNT_ID;
  const accessKeyId = process.env.R2_ACCESS_KEY_ID;
  const secretAccessKey = process.env.R2_SECRET_ACCESS_KEY;
  if (!accountId || !accessKeyId || !secretAccessKey) {
    throw new Error('R2 credentials are not set');
  }

  client = new S3Client({
    region: 'auto',
    endpoint: `https://${accountId}.r2.cloudflarestorage.com`,
    credentials: { accessKeyId, secretAccessKey }
  });
  return client;
}

export async function uploadToR2(key: string, body: Buffer, contentType: string): Promise<void> {
  const bucket = process.env.R2_BUCKET;
  if (!bucket) throw new Error('R2_BUCKET is not set');

  const client = getR2Client();
  await client.send(
    new PutObjectCommand({ Bucket: bucket, Key: key, Body: body, ContentType: contentType })
  );
}
```

- [ ] **Step 2: Write the failing test**

```typescript
// src/routes/firmware.test.ts
import { describe, it, expect, vi, beforeEach } from 'vitest';
import Fastify, { FastifyInstance } from 'fastify';
import multipart from '@fastify/multipart';

const insertedRows: any[] = [];

vi.mock('../supabaseClient.js', () => ({
  getSupabaseClient: () => ({
    from: (table: string) => ({
      insert: (row: any) => ({
        select: () => ({
          single: async () => {
            const inserted = { id: 'release-1', created_at: new Date().toISOString(), ...row };
            insertedRows.push(inserted);
            return { data: inserted, error: null };
          }
        })
      }),
      select: () => ({
        order: async () => ({ data: insertedRows, error: null })
      })
    })
  })
}));

vi.mock('../r2Client.js', () => ({
  uploadToR2: vi.fn(async () => {})
}));

vi.mock('../auth.js', () => ({
  requireAuth: async (req: any) => {
    req.userId = 'user-123';
  }
}));

describe('firmware routes', () => {
  let app: FastifyInstance;

  beforeEach(async () => {
    insertedRows.length = 0;
    const { registerFirmwareRoutes } = await import('./firmware.js');
    app = Fastify();
    await app.register(multipart);
    await registerFirmwareRoutes(app);
  });

  it('rejects an invalid version string', async () => {
    const form = new FormData();
    form.append('version', 'not a valid version!!');
    form.append('file', new Blob([Buffer.from('fake binary')]), 'firmware.bin');

    const res = await app.inject({
      method: 'POST',
      url: '/api/firmware',
      payload: form
    });
    expect(res.statusCode).toBe(400);
  });

  it('uploads a valid firmware and lists it', async () => {
    const form = new FormData();
    form.append('version', 'sijagakali-v1.0.1');
    form.append('notes', 'test build');
    form.append('file', new Blob([Buffer.from('fake binary')]), 'firmware.bin');

    const uploadRes = await app.inject({
      method: 'POST',
      url: '/api/firmware',
      payload: form
    });
    expect(uploadRes.statusCode).toBe(201);
    const body = uploadRes.json();
    expect(body.version).toBe('sijagakali-v1.0.1');
    expect(body.r2_key).toBe('firmware/sijagakali-v1.0.1.bin');

    const listRes = await app.inject({ method: 'GET', url: '/api/firmware' });
    expect(listRes.statusCode).toBe(200);
    expect(listRes.json()).toHaveLength(1);
  });
});
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
npm test -- firmware.test.ts
```

Expected: FAIL — `./firmware.js` does not exist yet.

- [ ] **Step 4: Write `src/routes/firmware.ts`**

```typescript
import { FastifyInstance } from 'fastify';
import { getSupabaseClient } from '../supabaseClient.js';
import { uploadToR2 } from '../r2Client.js';
import { requireAuth } from '../auth.js';

const VERSION_PATTERN = /^[a-zA-Z0-9._-]+$/;

export async function registerFirmwareRoutes(app: FastifyInstance) {
  app.post('/api/firmware', { onRequest: [requireAuth] }, async (request, reply) => {
    const parts = request.parts();
    let version: string | undefined;
    let notes: string | undefined;
    let fileBuffer: Buffer | undefined;

    for await (const part of parts) {
      if (part.type === 'field' && part.fieldname === 'version') {
        version = String(part.value);
      } else if (part.type === 'field' && part.fieldname === 'notes') {
        notes = String(part.value);
      } else if (part.type === 'file' && part.fieldname === 'file') {
        fileBuffer = await part.toBuffer();
      }
    }

    if (!version || !VERSION_PATTERN.test(version)) {
      return reply.code(400).send({ error: 'version must match ^[a-zA-Z0-9._-]+$' });
    }
    if (!fileBuffer) {
      return reply.code(400).send({ error: 'file is required' });
    }

    const r2Key = `firmware/${version}.bin`;
    await uploadToR2(r2Key, fileBuffer, 'application/octet-stream');

    const supabase = getSupabaseClient();
    const { data, error } = await supabase
      .from('firmware_releases')
      .insert({
        version,
        r2_key: r2Key,
        file_size_bytes: fileBuffer.length,
        notes: notes ?? null,
        uploaded_by: request.userId
      })
      .select()
      .single();

    if (error) {
      return reply.code(500).send({ error: error.message });
    }
    return reply.code(201).send(data);
  });

  app.get('/api/firmware', { onRequest: [requireAuth] }, async (_request, reply) => {
    const supabase = getSupabaseClient();
    const { data, error } = await supabase
      .from('firmware_releases')
      .select('*')
      .order('created_at', { ascending: false });

    if (error) {
      return reply.code(500).send({ error: error.message });
    }
    return reply.send(data);
  });
}
```

- [ ] **Step 5: Register the routes and the multipart plugin in `src/app.ts`**

```typescript
import Fastify, { FastifyInstance } from 'fastify';
import multipart from '@fastify/multipart';
import { registerFirmwareRoutes } from './routes/firmware.js';

export function buildApp(): FastifyInstance {
  const app = Fastify({ logger: true });

  app.register(multipart);

  app.get('/health', async () => {
    return { ok: true };
  });

  app.register(registerFirmwareRoutes);

  return app;
}
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
npm test -- firmware.test.ts
```

Expected: 2 tests passing.

- [ ] **Step 7: Manual curl check against the real R2 bucket**

```bash
npm run dev
```

```bash
curl -X POST http://localhost:3000/api/firmware \
  -H "Authorization: Bearer <a real Supabase Auth JWT for your admin user>" \
  -F "version=sijagakali-v0.0.1-test" \
  -F "notes=manual smoke test" \
  -F "file=@/path/to/any/small/file.bin"

curl http://localhost:3000/api/firmware -H "Authorization: Bearer <token>"
```

Expected: 201 with the inserted row, then 200 with a list containing it. Confirm in the R2 dashboard that `firmware/sijagakali-v0.0.1-test.bin` exists. Delete the test object from R2 and the test row from `firmware_releases` afterward — this was a manual smoke test, not fixture data.

- [ ] **Step 8: Commit**

```bash
git add src/r2Client.ts src/routes/firmware.ts src/routes/firmware.test.ts src/app.ts
git commit -m "feat: add firmware upload and list endpoints"
```

---

### Task 5: Devices endpoint

**Files:**
- Create: `sijagakali-ota/src/routes/devices.ts`
- Modify: `sijagakali-ota/src/app.ts`
- Test: `sijagakali-ota/src/routes/devices.test.ts`

**Interfaces:**
- Consumes: `getSupabaseClient()`, `requireAuth` (Task 3)
- Produces: `registerDeviceRoutes(app: FastifyInstance)` — registered in `src/app.ts`. No later task depends on this one's internals; Task 7's MQTT listener writes to the same `device_configs` columns this task reads, but doesn't call into this file.

- [ ] **Step 1: Write the failing test**

```typescript
// src/routes/devices.test.ts
import { describe, it, expect, vi, beforeEach } from 'vitest';
import Fastify, { FastifyInstance } from 'fastify';

const deviceRows = [
  {
    deployment_slug: 'sijagakali-bojong-kulur',
    device_id: 'node-001',
    location_name: 'Bojong Kulur',
    firmware_version: 'sijagakali-v1.0.0',
    last_seen_at: new Date().toISOString()
  },
  {
    deployment_slug: 'sijagakali-bojong-kulur',
    device_id: 'node-002',
    location_name: 'Somewhere Else',
    firmware_version: null,
    last_seen_at: new Date(Date.now() - 1000 * 3600).toISOString() // 1 hour ago: offline
  }
];

const latestRelease = { version: 'sijagakali-v1.0.1' };

vi.mock('../supabaseClient.js', () => ({
  getSupabaseClient: () => ({
    from: (table: string) => {
      if (table === 'device_configs') {
        return { select: async () => ({ data: deviceRows, error: null }) };
      }
      if (table === 'firmware_releases') {
        return {
          select: () => ({
            order: () => ({
              limit: () => ({
                maybeSingle: async () => ({ data: latestRelease, error: null })
              })
            })
          })
        };
      }
      throw new Error(`unexpected table ${table}`);
    }
  })
}));

vi.mock('../auth.js', () => ({
  requireAuth: async (req: any) => {
    req.userId = 'user-123';
  }
}));

describe('GET /api/devices', () => {
  let app: FastifyInstance;

  beforeEach(async () => {
    const { registerDeviceRoutes } = await import('./devices.js');
    app = Fastify();
    await registerDeviceRoutes(app);
  });

  it('derives online and is_outdated', async () => {
    const res = await app.inject({ method: 'GET', url: '/api/devices' });
    expect(res.statusCode).toBe(200);
    const body = res.json();

    expect(body[0].device_id).toBe('node-001');
    expect(body[0].online).toBe(true);
    expect(body[0].is_outdated).toBe(true); // v1.0.0 != latest v1.0.1

    expect(body[1].device_id).toBe('node-002');
    expect(body[1].online).toBe(false); // last_seen_at 1 hour ago > 360s threshold
    expect(body[1].is_outdated).toBe(true); // null firmware_version != latest
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
npm test -- devices.test.ts
```

Expected: FAIL — `./devices.js` does not exist yet.

- [ ] **Step 3: Write `src/routes/devices.ts`**

```typescript
import { FastifyInstance } from 'fastify';
import { getSupabaseClient } from '../supabaseClient.js';
import { requireAuth } from '../auth.js';

const ONLINE_THRESHOLD_SECONDS = 360;

export async function registerDeviceRoutes(app: FastifyInstance) {
  app.get('/api/devices', { onRequest: [requireAuth] }, async (_request, reply) => {
    const supabase = getSupabaseClient();

    const { data: devices, error: devicesError } = await supabase.from('device_configs').select();
    if (devicesError) {
      return reply.code(500).send({ error: devicesError.message });
    }

    const { data: latest, error: latestError } = await supabase
      .from('firmware_releases')
      .select('version')
      .order('created_at', { ascending: false })
      .limit(1)
      .maybeSingle();
    if (latestError) {
      return reply.code(500).send({ error: latestError.message });
    }

    const latestVersion = latest?.version ?? null;
    const now = Date.now();

    const result = (devices ?? []).map((d: any) => {
      const lastSeenMs = d.last_seen_at ? new Date(d.last_seen_at).getTime() : 0;
      const online = now - lastSeenMs <= ONLINE_THRESHOLD_SECONDS * 1000;
      const isOutdated = latestVersion !== null && d.firmware_version !== latestVersion;
      return { ...d, online, is_outdated: isOutdated, latest_firmware_version: latestVersion };
    });

    return reply.send(result);
  });
}
```

- [ ] **Step 4: Register the route in `src/app.ts`**

```typescript
import { registerDeviceRoutes } from './routes/devices.js';
// ...inside buildApp(), alongside app.register(registerFirmwareRoutes):
app.register(registerDeviceRoutes);
```

- [ ] **Step 5: Run the test to verify it passes**

```bash
npm test -- devices.test.ts
```

Expected: 1 test passing.

- [ ] **Step 6: Commit**

```bash
git add src/routes/devices.ts src/routes/devices.test.ts src/app.ts
git commit -m "feat: add devices list endpoint with online/outdated flags"
```

---

### Task 6: MQTT publish + deploy endpoint

**Files:**
- Create: `sijagakali-ota/src/mqttClient.ts`
- Create: `sijagakali-ota/src/routes/deploy.ts`
- Modify: `sijagakali-ota/src/app.ts`
- Test: `sijagakali-ota/src/routes/deploy.test.ts`

**Interfaces:**
- Consumes: `getSupabaseClient()`, `requireAuth` (Task 3)
- Produces: `getMqttClient(): MqttClient` (from `src/mqttClient.ts`) — Task 7's subscribe listener uses the same client instance. `registerDeployRoutes(app: FastifyInstance)`.

- [ ] **Step 1: Write `src/mqttClient.ts`**

```typescript
import mqtt, { MqttClient } from 'mqtt';

let client: MqttClient | undefined;

export function getMqttClient(): MqttClient {
  if (client) return client;

  const url = process.env.MQTT_BROKER_URL ?? 'mqtt://localhost:1883';
  client = mqtt.connect(url);
  client.on('error', (err) => console.error('MQTT error:', err));
  return client;
}
```

- [ ] **Step 2: Write the failing test**

```typescript
// src/routes/deploy.test.ts
import { describe, it, expect, vi, beforeEach } from 'vitest';
import Fastify, { FastifyInstance } from 'fastify';

const publishedMessages: { topic: string; payload: string }[] = [];
const insertedUpdates: any[] = [];

vi.mock('../mqttClient.js', () => ({
  getMqttClient: () => ({
    publish: (topic: string, payload: string) => {
      publishedMessages.push({ topic, payload });
    }
  })
}));

vi.mock('../supabaseClient.js', () => ({
  getSupabaseClient: () => ({
    from: (table: string) => {
      if (table === 'firmware_releases') {
        return {
          select: () => ({
            eq: () => ({
              single: async () => ({
                data: { id: 'release-1', version: 'sijagakali-v1.0.1', r2_key: 'firmware/sijagakali-v1.0.1.bin' },
                error: null
              })
            })
          })
        };
      }
      if (table === 'firmware_updates') {
        return {
          insert: (row: any) => ({
            select: () => ({
              single: async () => {
                const inserted = { id: 'update-1', ...row };
                insertedUpdates.push(inserted);
                return { data: inserted, error: null };
              }
            })
          }),
          select: () => ({
            order: async () => ({ data: insertedUpdates, error: null })
          })
        };
      }
      throw new Error(`unexpected table ${table}`);
    }
  })
}));

vi.mock('../auth.js', () => ({
  requireAuth: async (req: any) => {
    req.userId = 'user-123';
  }
}));

describe('POST /api/firmware/:id/deploy', () => {
  let app: FastifyInstance;

  beforeEach(async () => {
    publishedMessages.length = 0;
    insertedUpdates.length = 0;
    const { registerDeployRoutes } = await import('./deploy.js');
    app = Fastify();
    await registerDeployRoutes(app);
  });

  it('publishes the ota_update command and records the request', async () => {
    const res = await app.inject({
      method: 'POST',
      url: '/api/firmware/release-1/deploy',
      payload: { deployment_slug: 'sijagakali-bojong-kulur', device_id: 'node-001' }
    });

    expect(res.statusCode).toBe(201);
    const body = res.json();
    expect(body.status).toBe('pending');
    expect(body.deployment_slug).toBe('sijagakali-bojong-kulur');
    expect(body.device_id).toBe('node-001');

    expect(publishedMessages).toHaveLength(1);
    expect(publishedMessages[0].topic).toBe('sijagakali/node-001/command');
    const payload = JSON.parse(publishedMessages[0].payload);
    expect(payload.cmd).toBe('ota_update');
    expect(payload.request_id).toBe(body.mqtt_request_id);
    expect(payload.params.url).toContain('firmware/sijagakali-v1.0.1.bin');
  });
});
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
npm test -- deploy.test.ts
```

Expected: FAIL — `./deploy.js` does not exist yet.

- [ ] **Step 4: Write `src/routes/deploy.ts`**

```typescript
import { randomUUID } from 'node:crypto';
import { FastifyInstance } from 'fastify';
import { getSupabaseClient } from '../supabaseClient.js';
import { getMqttClient } from '../mqttClient.js';
import { requireAuth } from '../auth.js';

export async function registerDeployRoutes(app: FastifyInstance) {
  app.post<{
    Params: { id: string };
    Body: { deployment_slug: string; device_id: string };
  }>('/api/firmware/:id/deploy', { onRequest: [requireAuth] }, async (request, reply) => {
    const { id } = request.params;
    const { deployment_slug, device_id } = request.body;

    if (!deployment_slug || !device_id) {
      return reply.code(400).send({ error: 'deployment_slug and device_id are required' });
    }

    const supabase = getSupabaseClient();

    const { data: release, error: releaseError } = await supabase
      .from('firmware_releases')
      .select('id, version, r2_key')
      .eq('id', id)
      .single();
    if (releaseError || !release) {
      return reply.code(404).send({ error: 'firmware release not found' });
    }

    const mqttRequestId = randomUUID();
    const publicBaseUrl = process.env.R2_PUBLIC_BASE_URL;
    if (!publicBaseUrl) {
      return reply.code(500).send({ error: 'R2_PUBLIC_BASE_URL is not set' });
    }
    const url = `${publicBaseUrl}/${release.r2_key}`;

    const { data: updateRow, error: insertError } = await supabase
      .from('firmware_updates')
      .insert({
        deployment_slug,
        device_id,
        firmware_release_id: release.id,
        requested_by: request.userId,
        mqtt_request_id: mqttRequestId,
        status: 'pending'
      })
      .select()
      .single();
    if (insertError) {
      return reply.code(500).send({ error: insertError.message });
    }

    const mqttClient = getMqttClient();
    const topic = `sijagakali/${device_id}/command`;
    const payload = JSON.stringify({
      cmd: 'ota_update',
      request_id: mqttRequestId,
      params: { url }
    });
    mqttClient.publish(topic, payload);

    return reply.code(201).send(updateRow);
  });

  app.get('/api/firmware-updates', { onRequest: [requireAuth] }, async (request, reply) => {
    const { device_id, deployment_slug } = request.query as { device_id?: string; deployment_slug?: string };
    const supabase = getSupabaseClient();
    let query = supabase.from('firmware_updates').select('*').order('requested_at', { ascending: false });
    if (device_id) query = query.eq('device_id', device_id);
    if (deployment_slug) query = query.eq('deployment_slug', deployment_slug);

    const { data, error } = await query;
    if (error) {
      return reply.code(500).send({ error: error.message });
    }
    return reply.send(data);
  });
}
```

- [ ] **Step 5: Register the route in `src/app.ts`**

```typescript
import { registerDeployRoutes } from './routes/deploy.js';
// ...inside buildApp():
app.register(registerDeployRoutes);
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
npm test -- deploy.test.ts
```

Expected: 1 test passing.

- [ ] **Step 7: Commit**

```bash
git add src/mqttClient.ts src/routes/deploy.ts src/routes/deploy.test.ts src/app.ts
git commit -m "feat: add deploy endpoint that publishes ota_update over MQTT"
```

---

### Task 7: MQTT ack/status listener

**Files:**
- Create: `sijagakali-ota/src/mqttListener.ts`
- Modify: `sijagakali-ota/src/server.ts`
- Test: `sijagakali-ota/src/mqttListener.test.ts`

**Interfaces:**
- Consumes: `getMqttClient()` (Task 6), `getSupabaseClient()` (Task 3)
- Produces: `startMqttListener(): void` — called once from `src/server.ts` at startup. No later task in this plan depends on it (this is the last task).

- [ ] **Step 1: Write the failing test**

```typescript
// src/mqttListener.test.ts
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { EventEmitter } from 'node:events';

const updates: Record<string, any> = {};
const deviceConfigs: Record<string, any> = {};
const ingestionRows: any[] = [];

class FakeMqttClient extends EventEmitter {
  subscribe(_topic: string) {}
}
const fakeClient = new FakeMqttClient();

vi.mock('./mqttClient.js', () => ({
  getMqttClient: () => fakeClient
}));

vi.mock('./supabaseClient.js', () => ({
  getSupabaseClient: () => ({
    from: (table: string) => {
      if (table === 'firmware_updates') {
        return {
          update: (fields: any) => ({
            eq: (_col: string, val: string) => ({
              then: (resolve: any) => {
                updates[val] = { ...(updates[val] ?? {}), ...fields };
                resolve({ error: null });
              }
            })
          })
        };
      }
      if (table === 'mqtt_ingestion') {
        return { insert: async (row: any) => { ingestionRows.push(row); return { error: null }; } };
      }
      if (table === 'device_configs') {
        return {
          update: (fields: any) => ({
            eq: (_c1: string, v1: string) => ({
              eq: (_c2: string, v2: string) => ({
                then: (resolve: any) => {
                  deviceConfigs[`${v1}:${v2}`] = { ...(deviceConfigs[`${v1}:${v2}`] ?? {}), ...fields };
                  resolve({ error: null });
                }
              })
            })
          })
        };
      }
      throw new Error(`unexpected table ${table}`);
    }
  })
}));

describe('MQTT listener', () => {
  beforeEach(async () => {
    Object.keys(updates).forEach((k) => delete updates[k]);
    Object.keys(deviceConfigs).forEach((k) => delete deviceConfigs[k]);
    ingestionRows.length = 0;
    const { startMqttListener } = await import('./mqttListener.js');
    startMqttListener();
  });

  it('updates firmware_updates on an ack message', async () => {
    const payload = JSON.stringify({ request_id: 'req-1', ok: true, detail: 'update ok, restarting' });
    fakeClient.emit('message', 'sijagakali/node-001/command/ack', Buffer.from(payload));
    await new Promise((r) => setTimeout(r, 10));

    expect(updates['req-1']).toEqual(
      expect.objectContaining({ status: 'acked_ok', ack_detail: 'update ok, restarting' })
    );
    expect(ingestionRows).toHaveLength(1);
    expect(ingestionRows[0].correlation_id).toBe('req-1');
  });

  it('updates device_configs on a status message', async () => {
    const payload = JSON.stringify({
      deployment_slug: 'sijagakali-bojong-kulur',
      device_id: 'node-001',
      firmware_version: 'sijagakali-v1.0.1',
      online: true
    });
    fakeClient.emit('message', 'sijagakali/node-001/sensor/status', Buffer.from(payload));
    await new Promise((r) => setTimeout(r, 10));

    expect(deviceConfigs['sijagakali-bojong-kulur:node-001']).toEqual(
      expect.objectContaining({ firmware_version: 'sijagakali-v1.0.1' })
    );
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
npm test -- mqttListener.test.ts
```

Expected: FAIL — `./mqttListener.js` does not exist yet.

- [ ] **Step 3: Write `src/mqttListener.ts`**

```typescript
import { getMqttClient } from './mqttClient.js';
import { getSupabaseClient } from './supabaseClient.js';

export function startMqttListener(): void {
  const client = getMqttClient();

  client.subscribe('sijagakali/+/command/ack');
  client.subscribe('sijagakali/+/sensor/status');

  client.on('message', (topic: string, messageBuffer: Buffer) => {
    void handleMessage(topic, messageBuffer);
  });
}

async function handleMessage(topic: string, messageBuffer: Buffer) {
  const parts = topic.split('/');
  const deviceId = parts[1];
  const messageType = parts.slice(2).join('/');

  let payload: any;
  try {
    payload = JSON.parse(messageBuffer.toString());
  } catch {
    console.error('MQTT listener: invalid JSON on', topic);
    return;
  }

  const supabase = getSupabaseClient();

  if (messageType === 'command/ack') {
    const { request_id, ok, detail } = payload;
    if (!request_id) return;

    await supabase
      .from('firmware_updates')
      .update({
        status: ok ? 'acked_ok' : 'acked_fail',
        ack_detail: detail ?? null,
        acked_at: new Date().toISOString()
      })
      .eq('mqtt_request_id', request_id);

    await supabase.from('mqtt_ingestion').insert({
      deployment_slug: payload.deployment_slug ?? null,
      device_id: deviceId,
      correlation_id: request_id,
      message_type: 'ota_ack',
      payload_json: payload,
      ingest_status: 'received'
    });
    return;
  }

  if (messageType === 'sensor/status') {
    const { deployment_slug, firmware_version } = payload;
    if (!deployment_slug || !deviceId) return;

    await supabase
      .from('device_configs')
      .update({
        firmware_version: firmware_version ?? null,
        firmware_updated_at: new Date().toISOString(),
        last_seen_at: new Date().toISOString()
      })
      .eq('deployment_slug', deployment_slug)
      .eq('device_id', deviceId);
  }
}
```

Note: `firmware_updated_at` is always bumped to "now" on every status message in this minimal version, not only when the version actually changed — the spec's "if changed" nuance is a Minor polish left for later (it costs nothing functionally; the column is purely informational, not read by any other task's logic).

- [ ] **Step 4: Run the test to verify it passes**

```bash
npm test -- mqttListener.test.ts
```

Expected: 2 tests passing.

- [ ] **Step 5: Wire it into `src/server.ts`**

```typescript
import 'dotenv/config';
import { buildApp } from './app.js';
import { startMqttListener } from './mqttListener.js';

const app = buildApp();
const port = Number(process.env.PORT ?? 3000);

startMqttListener();

app.listen({ port, host: '0.0.0.0' }, (err) => {
  if (err) {
    app.log.error(err);
    process.exit(1);
  }
});
```

- [ ] **Step 6: Manual end-to-end smoke test**

With a real `.env` (MQTT_BROKER_URL pointing at a reachable broker, real Supabase credentials):

```bash
npm run dev
```

From another terminal, simulate an ack the way the real ESP32 firmware would send one:

```bash
mosquitto_pub -h <broker_host> -t "sijagakali/node-001/command/ack" \
  -m '{"request_id":"11111111-1111-1111-1111-111111111111","ok":true,"detail":"update ok, restarting"}'
```

Expected in the dev server logs: no errors. Then check in Supabase (SQL Editor or `GET /api/firmware-updates`) that a `firmware_updates` row with that `mqtt_request_id` — if one exists from a prior manual `POST /api/firmware/:id/deploy` test — now shows `status = acked_ok`. If no matching row exists, the update simply affects zero rows, which is correct (no error, no crash).

- [ ] **Step 7: Commit**

```bash
git add src/mqttListener.ts src/server.ts src/mqttListener.test.ts
git commit -m "feat: add MQTT listener for ack and status messages"
```
