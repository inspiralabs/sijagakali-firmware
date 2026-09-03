# OTA Dashboard Frontend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the `sijagakali-ota/frontend` React app — a small single-operator dashboard (login, devices table with deploy button, firmware upload/catalog, deploy modal, update history with polling) that consumes the already-implemented `sijagakali-ota` backend API.

**Architecture:** Vite + React + TypeScript SPA, no router (only two top-level views: Login and Dashboard, switched on Supabase session state) and no server-state library (five endpoints, `fetch` + `useEffect` is enough — the one place that needs repeated fetching, Update History, uses a plain `setInterval`). Frontend never talks to Postgres or Supabase tables directly — only to Supabase Auth (for login) and to the `sijagakali-ota` Fastify backend (for all data), which is already built and tested per `docs/superpowers/plans/2026-08-15-ota-backend.md`. Lives in `sijagakali-ota/frontend/` (a sibling folder to the backend's `src/`, inside the same repo — the user asked for the whole dashboard, backend and frontend, in that one repo).

**Tech Stack:** React 18 + Vite 5 + TypeScript, `@supabase/supabase-js` (Auth only, anon key), Tailwind CSS v4 (`@tailwindcss/vite` plugin — no `tailwind.config.js`/PostCSS needed), `@marsidev/react-turnstile` (optional CAPTCHA, see constraint below), `vitest` + `@testing-library/react` + `jsdom` for tests.

**Spec:** `sijagakali-firmware/docs/superpowers/specs/2026-08-15-ota-dashboard-design.md`, "Frontend (React + Vite)" section. The backend half of the same spec is already implemented; this plan writes against that backend's *actual* shipped routes, not the spec's sketch of them.

## Global Constraints

- Frontend lives at `sijagakali-ota/frontend/` — own `package.json`/`vite.config.ts`/`tsconfig.json`, separate `node_modules` from the backend at `sijagakali-ota/` root. The repo's existing root `.gitignore` (`node_modules/`, `dist/`, `.env`, no leading slash on any of them) already covers `frontend/node_modules/`, `frontend/dist/`, `frontend/.env` — do not add a second `.gitignore`.
- Single-admin only, no roles/multi-user UI, no signup flow — matches backend's single-admin auth.
- No Supabase Realtime — Update History uses polling only (explicit non-goal in the spec).
- No client-side duplicate validation of firmware `version` format — the backend already validates (`^[a-zA-Z0-9._-]+$`) and returns `400 {"error": "..."}"`; the frontend just surfaces that error message.
- **CAPTCHA:** the shared Supabase project (`sijagakali-app/.env.example`, `sijagakali-app/src/components/TurnstileField.tsx`) supports Cloudflare Turnstile "Attack Protection" on Auth, enforced **server-side** by Supabase — if it's turned on for this project, `signInWithPassword` fails without a valid `captchaToken`, regardless of which app calls it. This app must support it the same optional way `sijagakali-app` does: render the Turnstile widget only if `VITE_TURNSTILE_SITE_KEY` is set, attach the token if present, work with no widget at all if the env var is empty (Attack Protection off or not yet configured for this app's domain).
- Backend API base URL is configurable via `VITE_OTA_API_URL` (defaults to `http://localhost:3787` to match the backend's current `.env` `PORT`).
- Auth: frontend authenticates directly against Supabase Auth with the **anon** key (never the service-role key — that lives only in the backend's `.env`) and forwards the resulting `access_token` as `Authorization: Bearer <token>` to every backend call. No login endpoint on the backend itself.
- UI copy in Indonesian (matches `sijagakali-app`'s house style); code identifiers and comments in English.

## Backend API being consumed (already shipped, for reference)

| Method | Path | Body / Query | Response |
|---|---|---|---|
| GET | `/api/devices` | — | `Device[]` (device_configs row + `online`, `is_outdated`, `latest_firmware_version`) |
| GET | `/api/firmware` | — | `FirmwareRelease[]`, newest first |
| POST | `/api/firmware` | multipart: `version`, `notes?`, `file` | `201 FirmwareRelease` |
| POST | `/api/firmware/:id/deploy` | JSON `{deployment_slug, device_id}` | `201 FirmwareUpdate` |
| GET | `/api/firmware-updates` | — | `FirmwareUpdate[]`, newest first |

All require `Authorization: Bearer <supabase-jwt>`. Non-2xx responses are JSON `{"error": "message"}"`.

---

### Task 1: Frontend scaffold

**Files:**
- Create: `sijagakali-ota/frontend/package.json`
- Create: `sijagakali-ota/frontend/tsconfig.json`
- Create: `sijagakali-ota/frontend/vite.config.ts`
- Create: `sijagakali-ota/frontend/vitest.config.ts`
- Create: `sijagakali-ota/frontend/index.html`
- Create: `sijagakali-ota/frontend/src/main.tsx`
- Create: `sijagakali-ota/frontend/src/index.css`
- Create: `sijagakali-ota/frontend/src/setupTests.ts`
- Create: `sijagakali-ota/frontend/src/App.tsx`
- Create: `sijagakali-ota/frontend/.env.example`
- Test: `sijagakali-ota/frontend/src/App.test.tsx`

**Interfaces:**
- Produces: `App` component (exported from `src/App.tsx`) — Task 2 replaces its contents with real session-gating logic; `main.tsx` mounts it.

- [ ] **Step 1: Create the frontend directory**

```bash
mkdir -p "D:\code-for-life\inspiralabs\projects\sijagakali\sijagakali-ota\frontend\src"
```

- [ ] **Step 2: Write `package.json`**

```json
{
  "name": "sijagakali-ota-frontend",
  "version": "0.1.0",
  "private": true,
  "type": "module",
  "scripts": {
    "dev": "vite",
    "build": "vite build",
    "preview": "vite preview",
    "test": "vitest run"
  },
  "dependencies": {
    "@marsidev/react-turnstile": "^1.3.0",
    "@supabase/supabase-js": "^2.49.1",
    "react": "^18.3.1",
    "react-dom": "^18.3.1"
  },
  "devDependencies": {
    "@tailwindcss/vite": "^4.0.0",
    "@testing-library/jest-dom": "^6.6.0",
    "@testing-library/react": "^16.0.0",
    "@types/react": "^18.3.23",
    "@types/react-dom": "^18.3.7",
    "@vitejs/plugin-react-swc": "^3.11.0",
    "jsdom": "^20.0.3",
    "tailwindcss": "^4.0.0",
    "typescript": "^5.8.3",
    "vite": "^5.4.19",
    "vitest": "^3.2.4"
  }
}
```

- [ ] **Step 3: Write `tsconfig.json`**

```json
{
  "compilerOptions": {
    "target": "ES2022",
    "useDefineForClassFields": true,
    "lib": ["ES2022", "DOM", "DOM.Iterable"],
    "module": "ESNext",
    "skipLibCheck": true,
    "moduleResolution": "Bundler",
    "resolveJsonModule": true,
    "isolatedModules": true,
    "noEmit": true,
    "jsx": "react-jsx",
    "strict": true
  },
  "include": ["src"]
}
```

- [ ] **Step 4: Write `vite.config.ts`**

```typescript
import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react-swc';
import tailwindcss from '@tailwindcss/vite';

export default defineConfig({
  plugins: [react(), tailwindcss()]
});
```

- [ ] **Step 5: Write `vitest.config.ts`**

```typescript
import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react-swc';

export default defineConfig({
  plugins: [react()],
  test: {
    environment: 'jsdom',
    setupFiles: ['./src/setupTests.ts']
  }
});
```

- [ ] **Step 6: Write `src/setupTests.ts`**

```typescript
import '@testing-library/jest-dom/vitest';
```

- [ ] **Step 7: Write `index.html`**

```html
<!doctype html>
<html lang="id">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    <title>SiJagaKali OTA Dashboard</title>
  </head>
  <body>
    <div id="root"></div>
    <script type="module" src="/src/main.tsx"></script>
  </body>
</html>
```

- [ ] **Step 8: Write `src/index.css`**

```css
@import "tailwindcss";
```

- [ ] **Step 9: Write `src/main.tsx`**

```tsx
import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import App from './App';
import './index.css';

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <App />
  </StrictMode>
);
```

- [ ] **Step 10: Write the failing test**

```tsx
// src/App.test.tsx
import { describe, it, expect } from 'vitest';
import { render, screen } from '@testing-library/react';
import { App } from './App';

describe('App', () => {
  it('renders the dashboard title', () => {
    render(<App />);
    expect(screen.getByText('SiJagaKali OTA Dashboard')).toBeInTheDocument();
  });
});
```

- [ ] **Step 11: Run the test to verify it fails**

```bash
cd frontend
npm install
npm test -- App.test.tsx
```

Expected: FAIL — `./App` does not exist yet (npm install is required first since this is the first test run; expect it to take a minute).

- [ ] **Step 12: Write `src/App.tsx`**

```tsx
export function App() {
  return (
    <div className="flex min-h-screen items-center justify-center">
      <h1 className="text-2xl font-bold">SiJagaKali OTA Dashboard</h1>
    </div>
  );
}

export default App;
```

- [ ] **Step 13: Run the test to verify it passes**

```bash
npm test -- App.test.tsx
```

Expected: 1 test passing.

- [ ] **Step 14: Write `.env.example`**

```
VITE_SUPABASE_URL=
VITE_SUPABASE_ANON_KEY=
VITE_OTA_API_URL=http://localhost:3787
VITE_TURNSTILE_SITE_KEY=
```

`VITE_SUPABASE_ANON_KEY` is the **anon/public** key (Supabase dashboard → Settings → API) — never the service-role key from the backend's `.env`. `VITE_TURNSTILE_SITE_KEY` is optional — leave blank if Supabase Attack Protection isn't enabled for this app's domain yet.

- [ ] **Step 15: Verify the dev server runs**

```bash
npm run dev
```

In another terminal: `curl -s http://localhost:5173/ | grep '<div id="root">'` → expect a match (the rendered heading itself only appears client-side after React mounts, which the Step 13 test already verified). Stop the dev server (Ctrl+C) once confirmed.

- [ ] **Step 16: Commit**

```bash
git add frontend/package.json frontend/package-lock.json frontend/tsconfig.json frontend/vite.config.ts frontend/vitest.config.ts frontend/index.html frontend/.env.example frontend/src/
git commit -m "feat: scaffold sijagakali-ota frontend with placeholder App"
```

---

### Task 2: Supabase Auth + Login page + session gate

**Files:**
- Create: `sijagakali-ota/frontend/src/lib/supabase.ts`
- Create: `sijagakali-ota/frontend/src/components/TurnstileField.tsx`
- Create: `sijagakali-ota/frontend/src/components/LoginPage.tsx`
- Create: `sijagakali-ota/frontend/src/components/LoginPage.test.tsx`
- Create: `sijagakali-ota/frontend/src/components/Dashboard.tsx`
- Modify: `sijagakali-ota/frontend/src/App.tsx`
- Modify: `sijagakali-ota/frontend/src/App.test.tsx`

**Interfaces:**
- Consumes: nothing from Task 1 besides the project scaffold.
- Produces: `getSupabase(): SupabaseClient` (`src/lib/supabase.ts`) — Task 3 does not need it (API client uses the access token, not the Supabase client, directly), but any future Supabase Auth use (e.g. logout) goes through it. `Dashboard` component (props: `accessToken: string`, `onLogout: () => void`) — a placeholder here, Task 8 fills in its real content; Tasks 4-7's components are not wired in until Task 8.

- [ ] **Step 1: Write `src/lib/supabase.ts`**

```typescript
import { createClient, type SupabaseClient } from '@supabase/supabase-js';

let client: SupabaseClient | undefined;

export function getSupabase(): SupabaseClient {
  if (client) return client;

  const url = import.meta.env.VITE_SUPABASE_URL as string;
  const key = import.meta.env.VITE_SUPABASE_ANON_KEY as string;

  client = createClient(url, key, {
    auth: { persistSession: true, autoRefreshToken: true }
  });
  return client;
}
```

- [ ] **Step 2: Write `src/components/TurnstileField.tsx`**

```tsx
import { Turnstile } from '@marsidev/react-turnstile';

const siteKey = (import.meta.env.VITE_TURNSTILE_SITE_KEY as string | undefined)?.trim() ?? '';

export function isTurnstileConfigured(): boolean {
  return Boolean(siteKey);
}

export function TurnstileField({ onToken }: { onToken: (token: string | null) => void }) {
  if (!siteKey) return null;

  return (
    <Turnstile
      siteKey={siteKey}
      options={{ theme: 'auto', language: 'id' }}
      onSuccess={(token) => onToken(token)}
      onExpire={() => onToken(null)}
      onError={() => onToken(null)}
    />
  );
}
```

- [ ] **Step 3: Write the failing test for `LoginPage`**

```tsx
// src/components/LoginPage.test.tsx
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render, screen, fireEvent, waitFor } from '@testing-library/react';
import { LoginPage } from './LoginPage';

const signInWithPassword = vi.fn();

vi.mock('../lib/supabase', () => ({
  getSupabase: () => ({
    auth: { signInWithPassword }
  })
}));

describe('LoginPage', () => {
  beforeEach(() => {
    signInWithPassword.mockReset();
  });

  it('calls signInWithPassword with entered credentials', async () => {
    signInWithPassword.mockResolvedValue({ error: null });
    render(<LoginPage />);

    fireEvent.change(screen.getByLabelText('Email'), { target: { value: 'admin@sijagakali.com' } });
    fireEvent.change(screen.getByLabelText('Kata Sandi'), { target: { value: 'secret123' } });
    fireEvent.click(screen.getByRole('button', { name: 'Masuk' }));

    await waitFor(() => {
      expect(signInWithPassword).toHaveBeenCalledWith(
        expect.objectContaining({ email: 'admin@sijagakali.com', password: 'secret123' })
      );
    });
  });

  it('shows an error message when sign-in fails', async () => {
    signInWithPassword.mockResolvedValue({ error: { message: 'Invalid login credentials' } });
    render(<LoginPage />);

    fireEvent.change(screen.getByLabelText('Email'), { target: { value: 'admin@sijagakali.com' } });
    fireEvent.change(screen.getByLabelText('Kata Sandi'), { target: { value: 'wrong' } });
    fireEvent.click(screen.getByRole('button', { name: 'Masuk' }));

    await waitFor(() => {
      expect(screen.getByText('Invalid login credentials')).toBeInTheDocument();
    });
  });
});
```

- [ ] **Step 4: Run the test to verify it fails**

```bash
npm test -- LoginPage.test.tsx
```

Expected: FAIL — `./LoginPage` does not exist yet.

- [ ] **Step 5: Write `src/components/LoginPage.tsx`**

```tsx
import { useState, type FormEvent } from 'react';
import { getSupabase } from '../lib/supabase';
import { TurnstileField, isTurnstileConfigured } from './TurnstileField';

export function LoginPage() {
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [captchaToken, setCaptchaToken] = useState<string | null>(null);
  const [submitting, setSubmitting] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const captchaRequired = isTurnstileConfigured();
  const captchaReady = !captchaRequired || Boolean(captchaToken);

  const handleSubmit = async (e: FormEvent) => {
    e.preventDefault();
    setError(null);
    setSubmitting(true);

    const supabase = getSupabase();
    const { error: signInError } = await supabase.auth.signInWithPassword({
      email,
      password,
      options: captchaToken ? { captchaToken } : undefined
    });

    setSubmitting(false);
    if (signInError) {
      setError(signInError.message);
    }
  };

  return (
    <div className="flex min-h-screen items-center justify-center bg-gray-50 p-4">
      <form onSubmit={handleSubmit} className="w-full max-w-sm rounded-lg border border-gray-200 bg-white p-8 shadow-sm">
        <h1 className="mb-6 text-xl font-bold text-gray-900">SiJagaKali OTA Dashboard</h1>

        <label htmlFor="login-email" className="mb-1 block text-xs font-medium text-gray-600">
          Email
        </label>
        <input
          id="login-email"
          type="email"
          value={email}
          onChange={(e) => setEmail(e.target.value)}
          required
          className="mb-4 w-full rounded border border-gray-300 px-3 py-2 text-sm"
        />

        <label htmlFor="login-password" className="mb-1 block text-xs font-medium text-gray-600">
          Kata Sandi
        </label>
        <input
          id="login-password"
          type="password"
          value={password}
          onChange={(e) => setPassword(e.target.value)}
          required
          className="mb-4 w-full rounded border border-gray-300 px-3 py-2 text-sm"
        />

        {isTurnstileConfigured() && (
          <div className="mb-4">
            <TurnstileField onToken={setCaptchaToken} />
          </div>
        )}

        {error && <p className="mb-4 text-sm text-red-600">{error}</p>}

        <button
          type="submit"
          disabled={submitting || !captchaReady}
          className="w-full rounded bg-blue-600 px-4 py-2 text-sm font-medium text-white disabled:opacity-50"
        >
          {submitting ? 'Memproses...' : 'Masuk'}
        </button>
      </form>
    </div>
  );
}
```

`<label htmlFor="login-email">` + `<input id="login-email">` (and the password pair) is what lets the test's `getByLabelText('Email')` find the right input — Testing Library resolves labels by that `for`/`id` association.

- [ ] **Step 6: Run the test to verify it passes**

```bash
npm test -- LoginPage.test.tsx
```

Expected: 2 tests passing.

- [ ] **Step 7: Write `src/components/Dashboard.tsx` (placeholder)**

```tsx
export function Dashboard({ accessToken, onLogout }: { accessToken: string; onLogout: () => void }) {
  void accessToken;

  return (
    <div className="min-h-screen bg-gray-50 p-6">
      <div className="mb-6 flex items-center justify-between">
        <h1 className="text-xl font-bold text-gray-900">SiJagaKali OTA Dashboard</h1>
        <button onClick={onLogout} className="rounded border border-gray-300 px-3 py-1.5 text-sm">
          Keluar
        </button>
      </div>
    </div>
  );
}
```

`void accessToken;` silences the unused-parameter warning until Task 8 actually uses it — deliberately temporary, deleted the moment Task 8 wires in real content.

- [ ] **Step 8: Rewrite the failing `App.test.tsx`**

```tsx
// src/App.test.tsx
import { describe, it, expect, vi } from 'vitest';
import { render, screen, waitFor } from '@testing-library/react';
import { App } from './App';

const getSession = vi.fn();
const onAuthStateChange = vi.fn(() => ({ data: { subscription: { unsubscribe: () => {} } } }));

vi.mock('./lib/supabase', () => ({
  getSupabase: () => ({
    auth: { getSession, onAuthStateChange }
  })
}));

describe('App', () => {
  it('shows the login page when there is no session', async () => {
    getSession.mockResolvedValue({ data: { session: null } });
    render(<App />);
    await waitFor(() => {
      expect(screen.getByRole('button', { name: 'Masuk' })).toBeInTheDocument();
    });
  });

  it('shows the dashboard when a session exists', async () => {
    getSession.mockResolvedValue({ data: { session: { access_token: 'tok-123', user: { id: 'u1' } } } });
    render(<App />);
    await waitFor(() => {
      expect(screen.getByText('SiJagaKali OTA Dashboard')).toBeInTheDocument();
      expect(screen.getByText('Keluar')).toBeInTheDocument();
    });
  });
});
```

- [ ] **Step 9: Run the test to verify it fails**

```bash
npm test -- App.test.tsx
```

Expected: FAIL — `App` still renders the old unconditional placeholder, so `getByRole('button', { name: 'Masuk' })` doesn't exist.

- [ ] **Step 10: Rewrite `src/App.tsx`**

```tsx
import { useEffect, useState } from 'react';
import type { Session } from '@supabase/supabase-js';
import { getSupabase } from './lib/supabase';
import { LoginPage } from './components/LoginPage';
import { Dashboard } from './components/Dashboard';

export function App() {
  const [session, setSession] = useState<Session | null>(null);
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    const supabase = getSupabase();

    supabase.auth.getSession().then(({ data }: { data: { session: Session | null } }) => {
      setSession(data.session);
      setLoading(false);
    });

    const { data: sub } = supabase.auth.onAuthStateChange((_event: string, newSession: Session | null) => {
      setSession(newSession);
    });

    return () => sub.subscription.unsubscribe();
  }, []);

  if (loading) {
    return <div className="flex min-h-screen items-center justify-center text-gray-500">Memuat...</div>;
  }

  if (!session) return <LoginPage />;

  return <Dashboard accessToken={session.access_token} onLogout={() => getSupabase().auth.signOut()} />;
}

export default App;
```

- [ ] **Step 11: Run the test to verify it passes**

```bash
npm test -- App.test.tsx
```

Expected: 2 tests passing.

- [ ] **Step 12: Run the full test suite**

```bash
npm test
```

Expected: all tests passing (4 so far: App x2, LoginPage x2).

- [ ] **Step 13: Commit**

```bash
git add frontend/src/lib/supabase.ts frontend/src/components/TurnstileField.tsx frontend/src/components/LoginPage.tsx frontend/src/components/LoginPage.test.tsx frontend/src/components/Dashboard.tsx frontend/src/App.tsx frontend/src/App.test.tsx
git commit -m "feat: add Supabase Auth login and session-gated App shell"
```

---

### Task 3: Backend API client

**Files:**
- Create: `sijagakali-ota/frontend/src/lib/api.ts`
- Test: `sijagakali-ota/frontend/src/lib/api.test.ts`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: types `Device`, `FirmwareRelease`, `FirmwareUpdate` and functions `getDevices`, `getFirmwareReleases`, `uploadFirmware`, `deployFirmware`, `getFirmwareUpdates` (all `(accessToken: string, ...) => Promise<...>`) from `src/lib/api.ts` — Tasks 4-7 each consume a subset of these.

- [ ] **Step 1: Write the failing test**

```typescript
// src/lib/api.test.ts
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { getDevices, getFirmwareReleases, uploadFirmware, deployFirmware, getFirmwareUpdates } from './api';

const fetchMock = vi.fn();

beforeEach(() => {
  fetchMock.mockReset();
  vi.stubGlobal('fetch', fetchMock);
});

describe('api client', () => {
  it('getDevices attaches the bearer token and returns parsed JSON', async () => {
    fetchMock.mockResolvedValue({
      ok: true,
      json: async () => [{ device_id: 'node-001' }]
    });

    const result = await getDevices('token-abc');

    expect(fetchMock).toHaveBeenCalledWith(
      expect.stringContaining('/api/devices'),
      expect.objectContaining({ headers: expect.objectContaining({ Authorization: 'Bearer token-abc' }) })
    );
    expect(result).toEqual([{ device_id: 'node-001' }]);
  });

  it('throws the backend error message on a non-ok response', async () => {
    fetchMock.mockResolvedValue({
      ok: false,
      status: 500,
      json: async () => ({ error: 'boom' })
    });

    await expect(getFirmwareReleases('token-abc')).rejects.toThrow('boom');
  });

  it('uploadFirmware sends multipart form data without a manual Content-Type header', async () => {
    fetchMock.mockResolvedValue({
      ok: true,
      json: async () => ({ id: 'r1', version: 'sijagakali-v1.0.1' })
    });
    const file = new File(['fake binary'], 'firmware.bin');

    await uploadFirmware('token-abc', file, 'sijagakali-v1.0.1', 'notes here');

    const [, init] = fetchMock.mock.calls[0];
    expect(init.method).toBe('POST');
    expect(init.body).toBeInstanceOf(FormData);
    expect(init.headers.Authorization).toBe('Bearer token-abc');
    expect(init.headers['Content-Type']).toBeUndefined();
  });

  it('deployFirmware posts JSON with deployment_slug and device_id', async () => {
    fetchMock.mockResolvedValue({
      ok: true,
      json: async () => ({ id: 'update-1', status: 'pending' })
    });

    await deployFirmware('token-abc', 'release-1', 'sijagakali-bojong-kulur', 'node-001');

    expect(fetchMock).toHaveBeenCalledWith(
      expect.stringContaining('/api/firmware/release-1/deploy'),
      expect.objectContaining({
        method: 'POST',
        body: JSON.stringify({ deployment_slug: 'sijagakali-bojong-kulur', device_id: 'node-001' })
      })
    );
  });

  it('getFirmwareUpdates fetches the update history', async () => {
    fetchMock.mockResolvedValue({ ok: true, json: async () => [] });
    await getFirmwareUpdates('token-abc');
    expect(fetchMock).toHaveBeenCalledWith(expect.stringContaining('/api/firmware-updates'), expect.anything());
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
npm test -- api.test.ts
```

Expected: FAIL — `./api` does not exist yet.

- [ ] **Step 3: Write `src/lib/api.ts`**

```typescript
const API_BASE_URL = (import.meta.env.VITE_OTA_API_URL as string | undefined) ?? 'http://localhost:3787';

export type Device = {
  deployment_slug: string;
  device_id: string;
  location_name: string;
  firmware_version: string | null;
  last_seen_at: string | null;
  online: boolean;
  is_outdated: boolean;
  latest_firmware_version: string | null;
};

export type FirmwareRelease = {
  id: string;
  version: string;
  r2_key: string;
  file_size_bytes: number;
  notes: string | null;
  uploaded_by: string | null;
  created_at: string;
};

export type FirmwareUpdate = {
  id: string;
  deployment_slug: string;
  device_id: string;
  firmware_release_id: string;
  requested_by: string | null;
  requested_at: string;
  mqtt_request_id: string;
  status: 'pending' | 'acked_ok' | 'acked_fail';
  ack_detail: string | null;
  acked_at: string | null;
};

async function apiFetch<T>(path: string, accessToken: string, init?: RequestInit): Promise<T> {
  const res = await fetch(`${API_BASE_URL}${path}`, {
    ...init,
    headers: {
      ...(init?.headers ?? {}),
      Authorization: `Bearer ${accessToken}`
    }
  });

  if (!res.ok) {
    const body = await res.json().catch(() => ({}) as { error?: string });
    throw new Error(body.error ?? `Request failed with status ${res.status}`);
  }

  return res.json() as Promise<T>;
}

export function getDevices(accessToken: string): Promise<Device[]> {
  return apiFetch('/api/devices', accessToken);
}

export function getFirmwareReleases(accessToken: string): Promise<FirmwareRelease[]> {
  return apiFetch('/api/firmware', accessToken);
}

export function uploadFirmware(
  accessToken: string,
  file: File,
  version: string,
  notes: string
): Promise<FirmwareRelease> {
  const form = new FormData();
  form.append('version', version);
  if (notes) form.append('notes', notes);
  form.append('file', file);
  return apiFetch('/api/firmware', accessToken, { method: 'POST', body: form });
}

export function deployFirmware(
  accessToken: string,
  releaseId: string,
  deploymentSlug: string,
  deviceId: string
): Promise<FirmwareUpdate> {
  return apiFetch(`/api/firmware/${releaseId}/deploy`, accessToken, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ deployment_slug: deploymentSlug, device_id: deviceId })
  });
}

export function getFirmwareUpdates(accessToken: string): Promise<FirmwareUpdate[]> {
  return apiFetch('/api/firmware-updates', accessToken);
}
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
npm test -- api.test.ts
```

Expected: 5 tests passing.

- [ ] **Step 5: Commit**

```bash
git add frontend/src/lib/api.ts frontend/src/lib/api.test.ts
git commit -m "feat: add typed backend API client"
```

---

### Task 4: Devices table

**Files:**
- Create: `sijagakali-ota/frontend/src/components/DevicesTable.tsx`
- Test: `sijagakali-ota/frontend/src/components/DevicesTable.test.tsx`

**Interfaces:**
- Consumes: `getDevices`, `Device` (Task 3).
- Produces: `DevicesTable` component (props: `accessToken: string`, `onDeploy: (device: Device) => void`) — Task 8 renders it and supplies `onDeploy` to open Task 6's `DeployModal`.

- [ ] **Step 1: Write the failing test**

```tsx
// src/components/DevicesTable.test.tsx
import { describe, it, expect, vi } from 'vitest';
import { render, screen, waitFor, fireEvent } from '@testing-library/react';
import { DevicesTable } from './DevicesTable';
import * as api from '../lib/api';

const device = {
  deployment_slug: 'sijagakali-bojong-kulur',
  device_id: 'node-001',
  location_name: 'Bojong Kulur',
  firmware_version: 'sijagakali-v1.0.0',
  last_seen_at: new Date().toISOString(),
  online: true,
  is_outdated: true,
  latest_firmware_version: 'sijagakali-v1.0.1'
};

describe('DevicesTable', () => {
  it('renders devices and shows the outdated badge', async () => {
    vi.spyOn(api, 'getDevices').mockResolvedValue([device]);

    render(<DevicesTable accessToken="tok" onDeploy={() => {}} />);

    await waitFor(() => {
      expect(screen.getByText('Bojong Kulur')).toBeInTheDocument();
      expect(screen.getByText('update tersedia')).toBeInTheDocument();
      expect(screen.getByText('Online')).toBeInTheDocument();
    });
  });

  it('calls onDeploy with the clicked device', async () => {
    vi.spyOn(api, 'getDevices').mockResolvedValue([device]);
    const onDeploy = vi.fn();

    render(<DevicesTable accessToken="tok" onDeploy={onDeploy} />);

    await waitFor(() => screen.getByText('Bojong Kulur'));
    fireEvent.click(screen.getByRole('button', { name: 'Deploy' }));

    expect(onDeploy).toHaveBeenCalledWith(expect.objectContaining({ device_id: 'node-001' }));
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
npm test -- DevicesTable.test.tsx
```

Expected: FAIL — `./DevicesTable` does not exist yet.

- [ ] **Step 3: Write `src/components/DevicesTable.tsx`**

```tsx
import { useEffect, useState } from 'react';
import { getDevices, type Device } from '../lib/api';

export function DevicesTable({
  accessToken,
  onDeploy
}: {
  accessToken: string;
  onDeploy: (device: Device) => void;
}) {
  const [devices, setDevices] = useState<Device[]>([]);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    getDevices(accessToken)
      .then(setDevices)
      .catch((err: Error) => setError(err.message));
  }, [accessToken]);

  if (error) return <p className="text-sm text-red-600">{error}</p>;

  return (
    <table className="w-full text-left text-sm">
      <thead>
        <tr className="border-b border-gray-200 text-gray-500">
          <th className="py-2">Lokasi</th>
          <th className="py-2">Deployment</th>
          <th className="py-2">Device ID</th>
          <th className="py-2">Firmware</th>
          <th className="py-2">Status</th>
          <th className="py-2"></th>
        </tr>
      </thead>
      <tbody>
        {devices.map((device) => (
          <tr key={`${device.deployment_slug}:${device.device_id}`} className="border-b border-gray-100">
            <td className="py-2">{device.location_name}</td>
            <td className="py-2">{device.deployment_slug}</td>
            <td className="py-2">{device.device_id}</td>
            <td className="py-2">
              {device.firmware_version ?? '-'}
              {device.is_outdated && (
                <span className="ml-2 rounded bg-amber-100 px-2 py-0.5 text-xs text-amber-800">
                  update tersedia
                </span>
              )}
            </td>
            <td className="py-2">
              <span className={device.online ? 'text-green-600' : 'text-gray-400'}>
                {device.online ? 'Online' : 'Offline'}
              </span>
            </td>
            <td className="py-2">
              <button
                onClick={() => onDeploy(device)}
                className="rounded border border-gray-300 px-2 py-1 text-xs"
              >
                Deploy
              </button>
            </td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
npm test -- DevicesTable.test.tsx
```

Expected: 2 tests passing.

- [ ] **Step 5: Commit**

```bash
git add frontend/src/components/DevicesTable.tsx frontend/src/components/DevicesTable.test.tsx
git commit -m "feat: add devices table with outdated badge and deploy button"
```

---

### Task 5: Firmware catalog page (list + upload)

**Files:**
- Create: `sijagakali-ota/frontend/src/components/FirmwarePage.tsx`
- Test: `sijagakali-ota/frontend/src/components/FirmwarePage.test.tsx`

**Interfaces:**
- Consumes: `getFirmwareReleases`, `uploadFirmware`, `FirmwareRelease` (Task 3).
- Produces: `FirmwarePage` component (props: `accessToken: string`) — Task 8 renders it directly, no output consumed by other tasks.

- [ ] **Step 1: Write the failing test**

```tsx
// src/components/FirmwarePage.test.tsx
import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render, screen, waitFor, fireEvent } from '@testing-library/react';
import { FirmwarePage } from './FirmwarePage';
import * as api from '../lib/api';

describe('FirmwarePage', () => {
  beforeEach(() => {
    vi.spyOn(api, 'getFirmwareReleases').mockResolvedValue([
      {
        id: 'r1',
        version: 'sijagakali-v1.0.0',
        r2_key: 'firmware/sijagakali-v1.0.0.bin',
        file_size_bytes: 102400,
        notes: null,
        uploaded_by: null,
        created_at: new Date().toISOString()
      }
    ]);
  });

  it('lists existing firmware releases', async () => {
    render(<FirmwarePage accessToken="tok" />);
    await waitFor(() => {
      expect(screen.getByText('sijagakali-v1.0.0')).toBeInTheDocument();
    });
  });

  it('uploads a new firmware and refreshes the list', async () => {
    const uploadSpy = vi.spyOn(api, 'uploadFirmware').mockResolvedValue({
      id: 'r2',
      version: 'sijagakali-v1.0.1',
      r2_key: 'firmware/sijagakali-v1.0.1.bin',
      file_size_bytes: 2048,
      notes: '',
      uploaded_by: null,
      created_at: new Date().toISOString()
    });

    render(<FirmwarePage accessToken="tok" />);
    await waitFor(() => screen.getByText('sijagakali-v1.0.0'));

    fireEvent.change(screen.getByLabelText('Versi'), { target: { value: 'sijagakali-v1.0.1' } });
    const file = new File(['fake binary'], 'firmware.bin', { type: 'application/octet-stream' });
    fireEvent.change(screen.getByLabelText('File .bin'), { target: { files: [file] } });
    fireEvent.click(screen.getByRole('button', { name: 'Unggah' }));

    await waitFor(() => {
      expect(uploadSpy).toHaveBeenCalledWith('tok', file, 'sijagakali-v1.0.1', '');
    });
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
npm test -- FirmwarePage.test.tsx
```

Expected: FAIL — `./FirmwarePage` does not exist yet.

- [ ] **Step 3: Write `src/components/FirmwarePage.tsx`**

```tsx
import { useEffect, useState, type FormEvent } from 'react';
import { getFirmwareReleases, uploadFirmware, type FirmwareRelease } from '../lib/api';

export function FirmwarePage({ accessToken }: { accessToken: string }) {
  const [releases, setReleases] = useState<FirmwareRelease[]>([]);
  const [version, setVersion] = useState('');
  const [notes, setNotes] = useState('');
  const [file, setFile] = useState<File | null>(null);
  const [uploading, setUploading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const loadReleases = () => {
    getFirmwareReleases(accessToken)
      .then(setReleases)
      .catch((err: Error) => setError(err.message));
  };

  useEffect(loadReleases, [accessToken]);

  const handleUpload = async (e: FormEvent) => {
    e.preventDefault();
    if (!file) return;

    setError(null);
    setUploading(true);
    try {
      await uploadFirmware(accessToken, file, version, notes);
      setVersion('');
      setNotes('');
      setFile(null);
      loadReleases();
    } catch (err) {
      setError((err as Error).message);
    } finally {
      setUploading(false);
    }
  };

  return (
    <div>
      <form onSubmit={handleUpload} className="mb-6 flex flex-wrap items-end gap-3">
        <div>
          <label htmlFor="fw-version" className="mb-1 block text-xs font-medium text-gray-600">
            Versi
          </label>
          <input
            id="fw-version"
            value={version}
            onChange={(e) => setVersion(e.target.value)}
            required
            placeholder="sijagakali-v1.0.1"
            className="rounded border border-gray-300 px-3 py-2 text-sm"
          />
        </div>
        <div>
          <label htmlFor="fw-notes" className="mb-1 block text-xs font-medium text-gray-600">
            Catatan
          </label>
          <input
            id="fw-notes"
            value={notes}
            onChange={(e) => setNotes(e.target.value)}
            className="rounded border border-gray-300 px-3 py-2 text-sm"
          />
        </div>
        <div>
          <label htmlFor="fw-file" className="mb-1 block text-xs font-medium text-gray-600">
            File .bin
          </label>
          <input
            id="fw-file"
            type="file"
            accept=".bin"
            onChange={(e) => setFile(e.target.files?.[0] ?? null)}
            required
            className="text-sm"
          />
        </div>
        <button
          type="submit"
          disabled={uploading}
          className="rounded bg-blue-600 px-4 py-2 text-sm font-medium text-white disabled:opacity-50"
        >
          {uploading ? 'Mengunggah...' : 'Unggah'}
        </button>
      </form>

      {error && <p className="mb-4 text-sm text-red-600">{error}</p>}

      <table className="w-full text-left text-sm">
        <thead>
          <tr className="border-b border-gray-200 text-gray-500">
            <th className="py-2">Versi</th>
            <th className="py-2">Ukuran</th>
            <th className="py-2">Catatan</th>
            <th className="py-2">Diunggah</th>
          </tr>
        </thead>
        <tbody>
          {releases.map((release) => (
            <tr key={release.id} className="border-b border-gray-100">
              <td className="py-2">{release.version}</td>
              <td className="py-2">{(release.file_size_bytes / 1024).toFixed(0)} KB</td>
              <td className="py-2">{release.notes ?? '-'}</td>
              <td className="py-2">{new Date(release.created_at).toLocaleString('id-ID')}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
npm test -- FirmwarePage.test.tsx
```

Expected: 2 tests passing.

- [ ] **Step 5: Commit**

```bash
git add frontend/src/components/FirmwarePage.tsx frontend/src/components/FirmwarePage.test.tsx
git commit -m "feat: add firmware catalog page with upload form"
```

---

### Task 6: Deploy modal

**Files:**
- Create: `sijagakali-ota/frontend/src/components/DeployModal.tsx`
- Test: `sijagakali-ota/frontend/src/components/DeployModal.test.tsx`

**Interfaces:**
- Consumes: `getFirmwareReleases`, `deployFirmware`, `Device`, `FirmwareRelease` (Task 3).
- Produces: `DeployModal` component (props: `accessToken: string`, `device: Device`, `onClose: () => void`, `onDeployed: () => void`) — Task 8 renders it conditionally, driven by `DevicesTable`'s `onDeploy` callback.

- [ ] **Step 1: Write the failing test**

```tsx
// src/components/DeployModal.test.tsx
import { describe, it, expect, vi } from 'vitest';
import { render, screen, waitFor, fireEvent } from '@testing-library/react';
import { DeployModal } from './DeployModal';
import * as api from '../lib/api';

const device = {
  deployment_slug: 'sijagakali-bojong-kulur',
  device_id: 'node-001',
  location_name: 'Bojong Kulur',
  firmware_version: 'sijagakali-v1.0.0',
  last_seen_at: null,
  online: true,
  is_outdated: true,
  latest_firmware_version: 'sijagakali-v1.0.1'
};

describe('DeployModal', () => {
  it('confirms deploy with the selected firmware release', async () => {
    vi.spyOn(api, 'getFirmwareReleases').mockResolvedValue([
      {
        id: 'r1',
        version: 'sijagakali-v1.0.1',
        r2_key: 'firmware/sijagakali-v1.0.1.bin',
        file_size_bytes: 1024,
        notes: null,
        uploaded_by: null,
        created_at: new Date().toISOString()
      }
    ]);
    const deploySpy = vi.spyOn(api, 'deployFirmware').mockResolvedValue({
      id: 'u1',
      deployment_slug: device.deployment_slug,
      device_id: device.device_id,
      firmware_release_id: 'r1',
      requested_by: null,
      requested_at: new Date().toISOString(),
      mqtt_request_id: 'req-1',
      status: 'pending',
      ack_detail: null,
      acked_at: null
    });
    const onDeployed = vi.fn();
    const onClose = vi.fn();

    render(<DeployModal accessToken="tok" device={device} onClose={onClose} onDeployed={onDeployed} />);

    await waitFor(() => screen.getByText('sijagakali-v1.0.1'));
    fireEvent.click(screen.getByRole('button', { name: 'Deploy' }));

    await waitFor(() => {
      expect(deploySpy).toHaveBeenCalledWith('tok', 'r1', 'sijagakali-bojong-kulur', 'node-001');
      expect(onDeployed).toHaveBeenCalled();
      expect(onClose).toHaveBeenCalled();
    });
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
npm test -- DeployModal.test.tsx
```

Expected: FAIL — `./DeployModal` does not exist yet.

- [ ] **Step 3: Write `src/components/DeployModal.tsx`**

```tsx
import { useEffect, useState } from 'react';
import { getFirmwareReleases, deployFirmware, type Device, type FirmwareRelease } from '../lib/api';

export function DeployModal({
  accessToken,
  device,
  onClose,
  onDeployed
}: {
  accessToken: string;
  device: Device;
  onClose: () => void;
  onDeployed: () => void;
}) {
  const [releases, setReleases] = useState<FirmwareRelease[]>([]);
  const [releaseId, setReleaseId] = useState('');
  const [submitting, setSubmitting] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    getFirmwareReleases(accessToken)
      .then((data) => {
        setReleases(data);
        if (data.length > 0) setReleaseId(data[0].id);
      })
      .catch((err: Error) => setError(err.message));
  }, [accessToken]);

  const handleConfirm = async () => {
    if (!releaseId) return;

    setError(null);
    setSubmitting(true);
    try {
      await deployFirmware(accessToken, releaseId, device.deployment_slug, device.device_id);
      onDeployed();
      onClose();
    } catch (err) {
      setError((err as Error).message);
      setSubmitting(false);
    }
  };

  return (
    <div className="fixed inset-0 flex items-center justify-center bg-black/40">
      <div className="w-full max-w-sm rounded-lg bg-white p-6">
        <h2 className="mb-4 text-lg font-bold text-gray-900">Deploy ke {device.device_id}</h2>

        <label htmlFor="deploy-release" className="mb-1 block text-xs font-medium text-gray-600">
          Firmware
        </label>
        <select
          id="deploy-release"
          value={releaseId}
          onChange={(e) => setReleaseId(e.target.value)}
          className="mb-4 w-full rounded border border-gray-300 px-3 py-2 text-sm"
        >
          {releases.map((release) => (
            <option key={release.id} value={release.id}>
              {release.version}
            </option>
          ))}
        </select>

        {error && <p className="mb-4 text-sm text-red-600">{error}</p>}

        <div className="flex justify-end gap-2">
          <button onClick={onClose} className="rounded border border-gray-300 px-4 py-2 text-sm">
            Batal
          </button>
          <button
            onClick={handleConfirm}
            disabled={submitting || !releaseId}
            className="rounded bg-blue-600 px-4 py-2 text-sm font-medium text-white disabled:opacity-50"
          >
            {submitting ? 'Mengirim...' : 'Deploy'}
          </button>
        </div>
      </div>
    </div>
  );
}
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
npm test -- DeployModal.test.tsx
```

Expected: 1 test passing.

- [ ] **Step 5: Commit**

```bash
git add frontend/src/components/DeployModal.tsx frontend/src/components/DeployModal.test.tsx
git commit -m "feat: add deploy confirmation modal"
```

---

### Task 7: Update history (polling)

**Files:**
- Create: `sijagakali-ota/frontend/src/components/UpdateHistoryTable.tsx`
- Test: `sijagakali-ota/frontend/src/components/UpdateHistoryTable.test.tsx`

**Interfaces:**
- Consumes: `getFirmwareUpdates`, `FirmwareUpdate` (Task 3).
- Produces: `UpdateHistoryTable` component (props: `accessToken: string`) — Task 8 renders it directly, no output consumed by other tasks (last task before the shell).

- [ ] **Step 1: Write the failing test**

```tsx
// src/components/UpdateHistoryTable.test.tsx
import { describe, it, expect, vi, afterEach } from 'vitest';
import { render, screen, waitFor } from '@testing-library/react';
import { UpdateHistoryTable } from './UpdateHistoryTable';
import * as api from '../lib/api';

describe('UpdateHistoryTable', () => {
  afterEach(() => {
    vi.useRealTimers();
    vi.restoreAllMocks();
  });

  it('renders update rows with a translated status', async () => {
    vi.spyOn(api, 'getFirmwareUpdates').mockResolvedValue([
      {
        id: 'u1',
        deployment_slug: 'sijagakali-bojong-kulur',
        device_id: 'node-001',
        firmware_release_id: 'r1',
        requested_by: null,
        requested_at: new Date().toISOString(),
        mqtt_request_id: 'req-1',
        status: 'acked_ok',
        ack_detail: 'update ok, restarting',
        acked_at: new Date().toISOString()
      }
    ]);

    render(<UpdateHistoryTable accessToken="tok" />);

    await waitFor(() => {
      expect(screen.getByText('node-001')).toBeInTheDocument();
      expect(screen.getByText('Berhasil')).toBeInTheDocument();
    });
  });

  it('polls again while a row is pending', async () => {
    vi.useFakeTimers({ shouldAdvanceTime: true });
    const getUpdates = vi.spyOn(api, 'getFirmwareUpdates').mockResolvedValue([
      {
        id: 'u1',
        deployment_slug: 'sijagakali-bojong-kulur',
        device_id: 'node-001',
        firmware_release_id: 'r1',
        requested_by: null,
        requested_at: new Date().toISOString(),
        mqtt_request_id: 'req-1',
        status: 'pending',
        ack_detail: null,
        acked_at: null
      }
    ]);

    render(<UpdateHistoryTable accessToken="tok" />);
    await waitFor(() => expect(getUpdates).toHaveBeenCalledTimes(1));

    await vi.advanceTimersByTimeAsync(4000);
    expect(getUpdates).toHaveBeenCalledTimes(2);
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
npm test -- UpdateHistoryTable.test.tsx
```

Expected: FAIL — `./UpdateHistoryTable` does not exist yet.

- [ ] **Step 3: Write `src/components/UpdateHistoryTable.tsx`**

```tsx
import { useEffect, useRef, useState } from 'react';
import { getFirmwareUpdates, type FirmwareUpdate } from '../lib/api';

const POLL_INTERVAL_MS = 4000;

const STATUS_LABEL: Record<FirmwareUpdate['status'], string> = {
  pending: 'Menunggu',
  acked_ok: 'Berhasil',
  acked_fail: 'Gagal'
};

export function UpdateHistoryTable({ accessToken }: { accessToken: string }) {
  const [updates, setUpdates] = useState<FirmwareUpdate[]>([]);
  const timerRef = useRef<ReturnType<typeof setInterval> | null>(null);

  useEffect(() => {
    let cancelled = false;

    const load = async () => {
      const data = await getFirmwareUpdates(accessToken);
      if (cancelled) return;
      setUpdates(data);

      const hasPending = data.some((u) => u.status === 'pending');
      if (hasPending && !timerRef.current) {
        timerRef.current = setInterval(load, POLL_INTERVAL_MS);
      } else if (!hasPending && timerRef.current) {
        clearInterval(timerRef.current);
        timerRef.current = null;
      }
    };

    load();

    return () => {
      cancelled = true;
      if (timerRef.current) clearInterval(timerRef.current);
    };
  }, [accessToken]);

  return (
    <table className="w-full text-left text-sm">
      <thead>
        <tr className="border-b border-gray-200 text-gray-500">
          <th className="py-2">Device</th>
          <th className="py-2">Diminta</th>
          <th className="py-2">Status</th>
          <th className="py-2">Detail</th>
        </tr>
      </thead>
      <tbody>
        {updates.map((update) => (
          <tr key={update.id} className="border-b border-gray-100">
            <td className="py-2">{update.device_id}</td>
            <td className="py-2">{new Date(update.requested_at).toLocaleString('id-ID')}</td>
            <td className="py-2">{STATUS_LABEL[update.status]}</td>
            <td className="py-2">{update.ack_detail ?? '-'}</td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
npm test -- UpdateHistoryTable.test.tsx
```

Expected: 2 tests passing.

- [ ] **Step 5: Commit**

```bash
git add frontend/src/components/UpdateHistoryTable.tsx frontend/src/components/UpdateHistoryTable.test.tsx
git commit -m "feat: add update history table that polls while pending"
```

---

### Task 8: Dashboard shell (tabs + deploy wiring)

**Files:**
- Modify: `sijagakali-ota/frontend/src/components/Dashboard.tsx`
- Create: `sijagakali-ota/frontend/src/components/Dashboard.test.tsx`

**Interfaces:**
- Consumes: `DevicesTable` (Task 4), `FirmwarePage` (Task 5), `DeployModal` (Task 6), `UpdateHistoryTable` (Task 7).
- Produces: the finished `Dashboard` — last task in this plan, nothing further consumes it.

- [ ] **Step 1: Write the failing test**

```tsx
// src/components/Dashboard.test.tsx
import { describe, it, expect, vi } from 'vitest';
import { render, screen, waitFor, fireEvent } from '@testing-library/react';
import { Dashboard } from './Dashboard';
import * as api from '../lib/api';

describe('Dashboard', () => {
  it('switches between tabs', async () => {
    vi.spyOn(api, 'getDevices').mockResolvedValue([]);
    vi.spyOn(api, 'getFirmwareReleases').mockResolvedValue([]);
    vi.spyOn(api, 'getFirmwareUpdates').mockResolvedValue([]);

    render(<Dashboard accessToken="tok" onLogout={() => {}} />);

    fireEvent.click(screen.getByRole('button', { name: 'Firmware' }));
    await waitFor(() => expect(screen.getByLabelText('Versi')).toBeInTheDocument());

    fireEvent.click(screen.getByRole('button', { name: 'Riwayat' }));
    await waitFor(() => expect(screen.getByText('Device')).toBeInTheDocument());
  });

  it('opens the deploy modal when a device Deploy button is clicked', async () => {
    vi.spyOn(api, 'getDevices').mockResolvedValue([
      {
        deployment_slug: 'sijagakali-bojong-kulur',
        device_id: 'node-001',
        location_name: 'Bojong Kulur',
        firmware_version: 'sijagakali-v1.0.0',
        last_seen_at: null,
        online: true,
        is_outdated: true,
        latest_firmware_version: 'sijagakali-v1.0.1'
      }
    ]);
    vi.spyOn(api, 'getFirmwareReleases').mockResolvedValue([]);

    render(<Dashboard accessToken="tok" onLogout={() => {}} />);

    await waitFor(() => screen.getByText('Bojong Kulur'));
    fireEvent.click(screen.getByRole('button', { name: 'Deploy' }));

    await waitFor(() => {
      expect(screen.getByText('Deploy ke node-001')).toBeInTheDocument();
    });
  });
});
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
npm test -- Dashboard.test.tsx
```

Expected: FAIL — current `Dashboard` placeholder has no tabs and no devices table.

- [ ] **Step 3: Rewrite `src/components/Dashboard.tsx`**

```tsx
import { useState } from 'react';
import { DevicesTable } from './DevicesTable';
import { FirmwarePage } from './FirmwarePage';
import { UpdateHistoryTable } from './UpdateHistoryTable';
import { DeployModal } from './DeployModal';
import type { Device } from '../lib/api';

type Tab = 'devices' | 'firmware' | 'history';

const TAB_LABEL: Record<Tab, string> = {
  devices: 'Perangkat',
  firmware: 'Firmware',
  history: 'Riwayat'
};

export function Dashboard({ accessToken, onLogout }: { accessToken: string; onLogout: () => void }) {
  const [tab, setTab] = useState<Tab>('devices');
  const [deployTarget, setDeployTarget] = useState<Device | null>(null);
  const [devicesKey, setDevicesKey] = useState(0);

  return (
    <div className="min-h-screen bg-gray-50 p-6">
      <div className="mb-6 flex items-center justify-between">
        <h1 className="text-xl font-bold text-gray-900">SiJagaKali OTA Dashboard</h1>
        <button onClick={onLogout} className="rounded border border-gray-300 px-3 py-1.5 text-sm">
          Keluar
        </button>
      </div>

      <div className="mb-4 flex gap-2 border-b border-gray-200">
        {(['devices', 'firmware', 'history'] as Tab[]).map((t) => (
          <button
            key={t}
            onClick={() => setTab(t)}
            className={`px-3 py-2 text-sm ${
              tab === t ? 'border-b-2 border-blue-600 font-medium text-blue-600' : 'text-gray-500'
            }`}
          >
            {TAB_LABEL[t]}
          </button>
        ))}
      </div>

      {tab === 'devices' && <DevicesTable key={devicesKey} accessToken={accessToken} onDeploy={setDeployTarget} />}
      {tab === 'firmware' && <FirmwarePage accessToken={accessToken} />}
      {tab === 'history' && <UpdateHistoryTable accessToken={accessToken} />}

      {deployTarget && (
        <DeployModal
          accessToken={accessToken}
          device={deployTarget}
          onClose={() => setDeployTarget(null)}
          onDeployed={() => setDevicesKey((k) => k + 1)}
        />
      )}
    </div>
  );
}
```

`devicesKey` forces `DevicesTable` to remount (and thus re-fetch) after a successful deploy, so the outdated badge / firmware version reflect the just-requested update without adding a second data-fetching path.

- [ ] **Step 4: Run the test to verify it passes**

```bash
npm test -- Dashboard.test.tsx
```

Expected: 2 tests passing.

- [ ] **Step 5: Run the full test suite**

```bash
npm test
```

Expected: all tests passing (App x2, LoginPage x2, api x5, DevicesTable x2, FirmwarePage x2, DeployModal x1, UpdateHistoryTable x2, Dashboard x2 = 18 tests).

- [ ] **Step 6: Manual end-to-end check**

Requires real credentials this plan's author doesn't have — do this yourself once `frontend/.env` and the backend's `.env` (R2 keys, etc.) are filled in:

```bash
# terminal 1, from sijagakali-ota/
npm run dev
# terminal 2, from sijagakali-ota/frontend/
npm run dev
```

Open the frontend's dev URL, log in with a real admin account (Supabase `auth.users` + a matching `sijagakali.admins` row), and click through: Devices tab shows real rows → Deploy opens the modal → Firmware tab uploads a `.bin` and shows it in the table → deploying shows a `pending` row in Riwayat that flips to `Berhasil`/`Gagal` once the device (or a simulated `mosquitto_pub` ack) responds.

- [ ] **Step 7: Commit**

```bash
git add frontend/src/components/Dashboard.tsx frontend/src/components/Dashboard.test.tsx
git commit -m "feat: wire devices, firmware, history, and deploy modal into dashboard shell"
```
