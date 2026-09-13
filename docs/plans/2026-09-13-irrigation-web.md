# Irrigation Web Interface Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the three-screen React front end that runs in a browser on the LAN and drives `irrigationd` through the nginx `/api` proxy.

**Architecture:** One typed API client is the only thing in the bundle that calls `fetch`, and every response passes through a decoder that throws on a shape mismatch. One polling hook owns the `/api/status` cadence and hands the controller's IANA timezone id to every piece of UI that renders an instant; formatting takes that zone as a required argument, so a laptop sitting in the controller's own timezone cannot make a wrong implementation look right. Three screens — Now, Programs, Settings — hang off a hash-routed shell.

**Tech Stack:** React 19, Vite 7, TypeScript 5.9 (strict), Vitest 3 + jsdom + Testing Library. No CSS framework, no router library, no HTTP library.

**Spec:** `docs/design/2026-09-05-irrigation-design.md` — section 8 is the scope, section 7 is the API consumed, section 5 says what the daemon does with the requests. The plan argues from the spec; executors read both.

**Companion plan:** `docs/plans/2026-09-12-irrigationd.md`. Its Task 9 builds `IrrigationControlServer`, the process this front end talks to. Do not edit that file.

---

## Global Constraints

These apply to every task. A task's requirements implicitly include this section.

- **Repository:** `~/src/punak/irrigation`, branch `feature/superproject`. All work is inside `web/`. Do not touch `IrrigationD/`, the Kanoop submodules, the root `CMakeLists.txt`, or `docs/plans/2026-09-12-irrigationd.md` — another session is building the daemon in this tree.
- **Never `git push`.** Commits stay local. The user controls all remote pushes.
- **Never discard uncommitted changes.** No `git checkout --`, `git restore`, `git clean -f`, `git reset --hard`. Do not delete untracked files you did not create.
- **Node 22 / npm 10.** Verified on this host: `node v22.22.2`, `npm 10.8.2`. Commit `package-lock.json`.
- **TypeScript `strict` plus `noUnusedLocals`, `noUnusedParameters`, `noUncheckedIndexedAccess`, `exactOptionalPropertyTypes`.** `npm run typecheck` is this project's `-Werror`: a task is not done until it exits zero.
- **Every network call goes through `src/api/client.ts`.** No bare `fetch` in a component, a hook, or a test helper other than the stub.
- **Every API instant is UTC and every rendered instant goes through `src/time/zonedformat.ts` with an explicit IANA zone id** taken from `/api/status`. `toLocaleString`, `toLocaleTimeString`, `toLocaleDateString` and a bare `Intl.DateTimeFormat` are banned in application code — they read the browser's zone, which is correct on a laptop in the same timezone as the controller and wrong everywhere else.
- **Tests run under `TZ=UTC`.** The npm scripts set it and `src/test/setup.ts` aborts the suite if the host zone resolves to anything else. A timezone test on a host that already sits in the zone under test proves nothing.
- **No network origin in the bundle.** No CDN scripts, no Google Fonts, no remote images. The controller has no internet and the browser loading this page may not either.
- **Filenames:** `PascalCase.tsx` for components and screens, `camelCase.ts` for modules. Tests are colocated as `<name>.test.ts` / `<name>.test.tsx`.
- **Comments state traps, not reasoning.** A comment earns its place only when a plausible future edit would silently break behaviour and the code cannot show why. Design rationale, pattern names and descriptions of what previous code did wrong belong in the commit message.
- **No "it's X, not Y" antithesis** in code, comments, commit messages or documentation. Delete the negated clause; if the sentence still says everything it said, the construction was doing no work.
- Commit messages are conventional commits and end with:
  ```
  Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
  ```

---

## The API contract

The browser never reaches the daemon directly. nginx serves the bundle from `/` and reverse-proxies `/api/*` to `127.0.0.1:8080/admin/*`. So `/api/status` in the browser is `/admin/status` at the daemon, and the bundle contains the string `/api` and never the string `/admin`.

Spec §7 names the endpoints and the daemon plan's Task 9 pins `/admin/status`, `/admin/zones/{n}/run` and the error codes. The body shapes for zones, programs and settings are pinned here, derived from the model field names in the daemon plan's Task 4 and the `settings` key/value table in spec §6. Where this plan had to choose, the choice is listed under **Deliberate decisions** at the end.

### Two different zone identifiers

`zones` rows carry both `id` (primary key) and `number` (1-8, the valve). They are equal in a freshly seeded database and the daemon's own review process caught three separate transposition bugs that were invisible for exactly that reason.

| Use | Identifier | Endpoint |
|---|---|---|
| Address a valve | `zone.number` | `POST /api/zones/{number}/run`, `PUT /api/zones/{number}` |
| Reference a zone from a program | `zone.id` | `program.zones[].zoneId` |

**Every test fixture in this plan uses zones where `id !== number`.** A fixture with `id === number` cannot tell the two apart, which is how the transposition survived on the daemon side.

### `GET /api/status`

Polled by the whole app. Fields are `ServerStatus` from the daemon plan's Task 9.

```json
{
  "runningZone": 4,
  "secondsRemaining": 120,
  "nextRunUtc": "2026-09-13T13:00:00Z",
  "timezone": "America/Los_Angeles",
  "masterEnabled": true,
  "rainDelayUntilUtc": ""
}
```

`runningZone` is `0` when idle. A Qt `QDateTime` that is null serialises to the empty string, so `""` is the absent value for both timestamps.

### `GET /api/zones` → `Zone[]`

```json
[{ "id": 9, "number": 3, "name": "Front lawn", "enabled": true }]
```

### `PUT /api/zones/{number}`

```json
{ "name": "Front lawn", "enabled": true }
```

### `POST /api/zones/{number}/run` → 202

```json
{ "seconds": 600 }
```

### `GET /api/programs` → `Program[]`

```json
[{
  "id": 1,
  "name": "Morning",
  "enabled": true,
  "dayMode": "DaysOfWeek",
  "dowMask": 5,
  "intervalDays": 0,
  "anchorDate": null,
  "startTimes": [{ "id": 3, "programId": 1, "minutesAfterMidnight": 360, "timezone": "America/Los_Angeles" }],
  "zones": [{ "id": 7, "programId": 1, "zoneId": 9, "sequence": 1, "durationSeconds": 600 }],
  "nextRunUtc": "2026-09-14T13:00:00Z"
}]
```

`dayMode` is one of `DaysOfWeek`, `Odd`, `Even`, `EveryNDays` — verbatim from `Program::dayModeToString()` at `IrrigationD/src/model/program.cpp:17-25`, committed at `f077d57`, and the `day_mode` column default in `schema.sql:17`. Read those strings from the source rather than inferring them from the `FiredInstant::Outcome` strings, which are lower-underscore (`ran`, `skipped_busy`) and follow a different convention. `dowMask` bit 0 is **Monday** and bit 6 is Sunday, matching `QDate::dayOfWeek()` minus one. `anchorDate` is a plain `YYYY-MM-DD` calendar date with no time and no zone; it is `null` unless `dayMode` is `EveryNDays`.

`nextRunUtc` is **optional** — see **Known gaps**.

### `POST /api/programs` → 201, `PUT /api/programs/{id}` → 200

The full program with its nested lists. `id` is omitted on create; nested `id` and `programId` are omitted on create and ignored on update, because the daemon replaces the start-time and zone rows wholesale.

```json
{
  "name": "Morning",
  "enabled": true,
  "dayMode": "DaysOfWeek",
  "dowMask": 5,
  "intervalDays": 0,
  "anchorDate": null,
  "startTimes": [{ "minutesAfterMidnight": 360, "timezone": "America/Los_Angeles" }],
  "zones": [{ "zoneId": 9, "sequence": 1, "durationSeconds": 600 }]
}
```

### `DELETE /api/programs/{id}` → 204, `POST /api/programs/{id}/run` → 202, `POST /api/stop` → 202

`POST /api/stop` takes no body.

### `GET /api/settings` → `Record<string, string>`, `PUT /api/settings`

The `settings` table is key/value TEXT, so **every value is a string**, including the booleans.

```json
{ "rain_delay_until": "", "master_enabled": "true", "max_zone_seconds": "1800", "log_level": "info" }
```

`PUT` takes a partial map of the same shape. The daemon's `settingValue()` returns an empty `QString` both for an absent key and for a key holding an empty value, so `""` and absent both mean "unset" and `""` is how the UI clears the rain delay.

### Errors

Any non-2xx carries `{ "error": "human readable" }`. The daemon plan's Task 9 pins 400 for malformed JSON and 404 for an unknown zone.

---

## File Structure

Everything below `web/`.

| Path | Responsibility |
|---|---|
| `package.json` | Scripts and dependencies. `TZ=UTC` lives in the test scripts. |
| `tsconfig.json` | Strict compiler settings. One project covering `src`, `scripts` and the two config files, so the type gate reaches all of them. |
| `vite.config.ts` | Build and dev-server config. Dev proxy `/api` → `127.0.0.1:8080/admin`. |
| `vitest.config.ts` | jsdom environment, setup file, colocated test glob. Excludes `*.tz.test.ts`. |
| `vitest.wallclock.config.ts` | The non-UTC run. No setup file, `*.tz.test.ts` only. |
| `src/time/wallclock.tz.test.ts` | Wall-clock assertions under `TZ=America/Los_Angeles`. |
| `index.html` | Single page. Viewport meta. No external origins. |
| `src/main.tsx` | Mounts `<App/>`. |
| `src/App.tsx` | Shell: hash routing, tab bar, connection banner, status fan-out. |
| `src/api/apiPath.ts` | `API_PREFIX` and `daemonPath()`, the single definition of the `/api` → `/admin` rewrite. Imported by `vite.config.ts`. |
| `src/api/types.ts` | `Status`, `Zone`, `Program`, `ProgramStartTime`, `ProgramZone`, `DayMode`, `SettingsMap`, `ApiError`, `DecodeError`. |
| `src/api/decode.ts` | Runtime decoders. Every one throws `DecodeError` on a shape mismatch. |
| `src/api/client.ts` | The only module that calls `fetch`. One function per endpoint the three screens use. |
| `src/time/zonedformat.ts` | Instant formatting against an explicit IANA zone, plus wall-clock minute conversion. |
| `src/hooks/useStatus.ts` | Adaptive `/api/status` polling, 2 s running / 15 s idle, with staleness. |
| `src/hooks/useCountdown.ts` | Local per-second decrement between polls. |
| `src/programs/dayRule.ts` | `dowMask` ↔ weekday list, day-rule summary text, total runtime. |
| `src/settings/settingsMap.ts` | Parse and serialise the string-valued settings map. |
| `src/screens/NowScreen.tsx` | Running zone, next run, eight zone tiles, stop control. |
| `src/screens/ProgramsScreen.tsx` | Program list with computed totals and next run. |
| `src/screens/ProgramEditor.tsx` | Create, edit, delete one program. |
| `src/screens/SettingsScreen.tsx` | Rain delay, master enable, max zone runtime, zone names. |
| `src/components/ZoneTile.tsx` | One valve tile. |
| `src/components/StopButton.tsx` | The e-stop. |
| `src/styles/tokens.css` | Colour, spacing and touch-target custom properties. |
| `src/styles/app.css` | Layout and component styles. |
| `src/test/setup.ts` | Testing Library matchers plus the `TZ=UTC` guard. |
| `src/test/fetchStub.ts` | Typed `fetch` stub with a recorded call log. |
| `src/test/fixtures.ts` | Shared `Status`, `Zone[]`, `Program[]` fixtures with `id !== number`. |
| `scripts/checkBundle.mjs` | Fails the build if `dist/` references a network origin. |
| `README.md` | Dev loop, scripts, the API contract, the two zone identifiers. |

---

### Task 1: Scaffold, test harness and the `/api` rewrite

The Vite project, the strict compiler settings, the test harness, and the one piece of production logic the dev server depends on: the `/api` → `/admin` path rewrite. Getting that rewrite wrong produces `/admin/admin/status` or `/status`, both of which 404 against a daemon that is running correctly.

**Files:**
- Create: `web/package.json`, `web/tsconfig.json`, `web/vite.config.ts`, `web/vitest.config.ts`, `web/index.html`, `web/.gitignore`
- Create: `web/src/main.tsx`, `web/src/App.tsx`
- Create: `web/src/styles/tokens.css`, `web/src/styles/app.css`
- Create: `web/src/api/apiPath.ts`
- Create: `web/src/test/setup.ts`
- Test: `web/src/api/apiPath.test.ts`, `web/src/App.test.tsx`

**Interfaces:**
- Produces:
  - `export const API_PREFIX = '/api'`
  - `export function apiUrl(path: string): string` — `apiUrl('/zones')` → `/api/zones`
  - `export function daemonPath(url: string): string` — `/api/zones` → `/admin/zones`; throws on a url that does not start with `/api`

- [ ] **Step 1: Create the project files**

`web/package.json`:

```json
{
  "name": "irrigation-web",
  "private": true,
  "version": "1.0.0",
  "type": "module",
  "scripts": {
    "dev": "vite",
    "build": "tsc --noEmit && vite build && node scripts/checkBundle.mjs",
    "preview": "vite preview",
    "typecheck": "tsc --noEmit",
    "test": "TZ=UTC vitest run && npm run test:wallclock",
    "test:watch": "TZ=UTC vitest",
    "test:wallclock": "TZ=America/Los_Angeles vitest run --config vitest.wallclock.config.ts"
  },
  "dependencies": {
    "react": "^19.1.0",
    "react-dom": "^19.1.0"
  },
  "devDependencies": {
    "@testing-library/jest-dom": "^6.6.0",
    "@testing-library/react": "^16.3.0",
    "@testing-library/user-event": "^14.6.0",
    "@types/react": "^19.1.0",
    "@types/react-dom": "^19.1.0",
    "@vitejs/plugin-react": "^5.0.0",
    "jsdom": "^26.0.0",
    "typescript": "~5.9.0",
    "vite": "^7.0.0",
    "vitest": "^3.2.0"
  }
}
```

`scripts/checkBundle.mjs` does not exist until Task 10, so `npm run build` fails until then. That is correct — the build gate arrives with the task that defines it. Use `npm run typecheck` and `npm test` as this task's gates.

`web/tsconfig.json`:

```json
{
  "compilerOptions": {
    "target": "ES2022",
    "lib": ["ES2022", "DOM", "DOM.Iterable"],
    "module": "ESNext",
    "moduleResolution": "bundler",
    "jsx": "react-jsx",
    "types": ["vitest/globals", "@testing-library/jest-dom"],
    "strict": true,
    "noUnusedLocals": true,
    "noUnusedParameters": true,
    "noUncheckedIndexedAccess": true,
    "exactOptionalPropertyTypes": true,
    "noFallthroughCasesInSwitch": true,
    "isolatedModules": true,
    "verbatimModuleSyntax": true,
    "resolveJsonModule": true,
    "allowJs": true,
    "skipLibCheck": true,
    "noEmit": true
  },
  "include": ["src", "scripts", "vite.config.ts", "vitest.config.ts"]
}
```

**One project, no `references`.** `tsc --noEmit` uses a referenced project only for its
declarations and never type-checks that project's own sources, so a two-project split leaves
`vite.config.ts` outside the gate: `rewrite: 12345` where the type is `(path: string) => string`
compiles clean and exits 0. Measured. The `include` list above is what puts the config files and
`scripts/` under the same `tsc --noEmit` the source is under.

`allowJs` is what lets `src/build/checkBundle.test.ts` import `scripts/checkBundle.mjs` in Task 10.
Without it that import fails with `TS7016: Could not find a declaration file`.

There is no `tsconfig.node.json`. A second project would have to be `composite` to be referenced,
`composite` may not set `noEmit` (`TS6310`), and a non-`noEmit` project emits `vite.config.js` beside
its source on any direct `tsc -p` or `tsc -b` invocation — into a directory `.gitignore` does not
cover.

`web/index.html`:

```html
<!doctype html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover" />
    <meta name="color-scheme" content="light dark" />
    <title>Irrigation</title>
  </head>
  <body>
    <div id="root"></div>
    <script type="module" src="/src/main.tsx"></script>
  </body>
</html>
```

`web/.gitignore`:

```
node_modules/
dist/
```

- [ ] **Step 2: Write the failing rewrite test**

`web/src/api/apiPath.test.ts`:

```ts
import { describe, expect, it } from 'vitest'
import { API_PREFIX, apiUrl, daemonPath } from './apiPath'

describe('apiUrl', () => {
  it('prefixes a daemon route with the proxy mount point', () => {
    expect(apiUrl('/status')).toBe('/api/status')
    expect(apiUrl('/zones/3/run')).toBe('/api/zones/3/run')
  })

  it('is the only place the mount point is spelled', () => {
    expect(API_PREFIX).toBe('/api')
  })
})

describe('daemonPath', () => {
  it('rewrites the proxy prefix to the daemon prefix', () => {
    expect(daemonPath('/api/status')).toBe('/admin/status')
    expect(daemonPath('/api/zones/3/run')).toBe('/admin/zones/3/run')
    expect(daemonPath('/api/programs/12')).toBe('/admin/programs/12')
  })

  it('leaves a nested "api" segment alone', () => {
    expect(daemonPath('/api/zones/api/run')).toBe('/admin/zones/api/run')
  })

  it('rejects a url that is already a daemon path', () => {
    expect(() => daemonPath('/admin/status')).toThrow(/api/)
  })
})
```

The nested-segment case is the discriminator. A rewrite written as `url.replace('/api', '/admin')` passes the first case and turns `/api/zones/api/run` into `/admin/zones/api/run` only by luck of `String.replace` stopping at the first match — swap it for `replaceAll` and this test fails. A rewrite written as `url.replace(/^\/api/, '')` fails the first case. The already-rewritten guard catches a caller that runs the rewrite twice, which produces `/admin/admin/status` and a 404 that looks like a routing bug in the daemon.

- [ ] **Step 3: Run it and verify it fails**

```bash
cd web && npm install && npm test -- apiPath
```

Expected: failure — `./apiPath` does not exist.

- [ ] **Step 4: Write the rewrite**

`web/src/api/apiPath.ts`:

```ts
/** Mount point nginx proxies to the daemon. The bundle never contains the daemon's own prefix. */
export const API_PREFIX = '/api'

const DAEMON_PREFIX = '/admin'

/** Returns the browser-side url for a daemon route, e.g. `/status` -> `/api/status`. */
export function apiUrl(path: string): string {
  return `${API_PREFIX}${path}`
}

/**
 * Rewrites a browser-side url to the daemon route it proxies to. Used by the dev
 * server proxy; nginx performs the same rewrite in production.
 */
export function daemonPath(url: string): string {
  if (url.startsWith(`${API_PREFIX}/`) === false && url !== API_PREFIX) {
    throw new Error(`daemonPath expects a ${API_PREFIX} url, received "${url}"`)
  }
  return `${DAEMON_PREFIX}${url.slice(API_PREFIX.length)}`
}
```

- [ ] **Step 5: Write the Vite and Vitest configs**

`web/vite.config.ts`:

```ts
import react from '@vitejs/plugin-react'
import { defineConfig } from 'vite'
import { API_PREFIX, daemonPath } from './src/api/apiPath'

export default defineConfig({
  base: '/',
  plugins: [react()],
  build: {
    outDir: 'dist',
    emptyOutDir: true,
  },
  server: {
    host: true,
    proxy: {
      [API_PREFIX]: {
        target: 'http://127.0.0.1:8080',
        changeOrigin: false,
        rewrite: daemonPath,
      },
    },
  },
})
```

`web/vitest.config.ts`:

```ts
import react from '@vitejs/plugin-react'
import { defineConfig } from 'vitest/config'

export default defineConfig({
  plugins: [react()],
  test: {
    globals: true,
    environment: 'jsdom',
    setupFiles: ['./src/test/setup.ts'],
    include: ['src/**/*.test.{ts,tsx}'],
    exclude: ['**/node_modules/**', '**/dist/**', 'src/**/*.tz.test.ts'],
    restoreMocks: true,
  },
})
```

`web/src/test/setup.ts`:

```ts
import '@testing-library/jest-dom/vitest'

const hostZone = Intl.DateTimeFormat().resolvedOptions().timeZone

if (hostZone !== 'UTC') {
  throw new Error(
    `Tests require TZ=UTC; this process resolved "${hostZone}". A timezone test run in the ` +
      `zone under test passes whether or not the code passes the zone through. Run "npm test".`,
  )
}
```

That guard is the reason the npm scripts carry `TZ=UTC`. Run `vitest` directly in a shell in America/Los_Angeles and the suite aborts with the instruction rather than going green on tests that prove nothing.

- [ ] **Step 6: Write the shell and styles**

`web/src/styles/tokens.css`:

```css
:root {
  --surface: #ffffff;
  --surface-raised: #f2f4f5;
  --ink: #10161a;
  --ink-dim: #4c5a63;
  --line: #c6d0d6;
  --accent: #0b6fa4;
  --running: #1f8f45;
  --danger: #b3241c;
  --danger-ink: #ffffff;
  --warn: #8a5a00;

  --touch: 56px;
  --gap: 12px;
  --radius: 10px;
  --font: system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
}

@media (prefers-color-scheme: dark) {
  :root {
    --surface: #0e1417;
    --surface-raised: #182126;
    --ink: #f2f6f8;
    --ink-dim: #a8b8c0;
    --line: #2e3d45;
    --accent: #4fb3e8;
    --running: #47d07c;
    --danger: #e0483d;
    --warn: #e0a53a;
  }
}
```

`web/src/styles/app.css`:

```css
* { box-sizing: border-box; }

body {
  margin: 0;
  font-family: var(--font);
  font-size: 17px;
  color: var(--ink);
  background: var(--surface);
}

.app {
  display: flex;
  flex-direction: column;
  min-height: 100vh;
  padding-bottom: calc(var(--touch) + env(safe-area-inset-bottom));
}

.screen {
  flex: 1;
  padding: var(--gap);
  display: flex;
  flex-direction: column;
  gap: var(--gap);
}

.tabs {
  position: fixed;
  inset: auto 0 0 0;
  display: flex;
  border-top: 1px solid var(--line);
  background: var(--surface-raised);
  padding-bottom: env(safe-area-inset-bottom);
}

.tabs button {
  flex: 1;
  min-height: var(--touch);
  border: 0;
  background: none;
  font: inherit;
  color: var(--ink-dim);
}

.tabs button[aria-current='page'] {
  color: var(--accent);
  font-weight: 700;
  box-shadow: inset 0 3px 0 var(--accent);
}

.banner {
  padding: var(--gap);
  border-radius: var(--radius);
  background: var(--warn);
  color: #000;
  font-weight: 600;
}

button, input, select {
  font: inherit;
  min-height: var(--touch);
  border-radius: var(--radius);
  border: 1px solid var(--line);
  background: var(--surface-raised);
  color: var(--ink);
  padding: 0 var(--gap);
}
```

`web/src/main.tsx`:

```tsx
import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import App from './App'
import './styles/tokens.css'
import './styles/app.css'

const container = document.getElementById('root')

if (container === null) {
  throw new Error('index.html is missing #root')
}

createRoot(container).render(
  <StrictMode>
    <App />
  </StrictMode>,
)
```

`web/src/App.tsx` — a stub that Task 5 replaces:

```tsx
export default function App() {
  return <main className="app">Irrigation</main>
}
```

`web/src/App.test.tsx`:

```tsx
import { render, screen } from '@testing-library/react'
import { describe, expect, it } from 'vitest'
import App from './App'

describe('App', () => {
  it('mounts', () => {
    render(<App />)
    expect(screen.getByText('Irrigation')).toBeInTheDocument()
  })
})
```

- [ ] **Step 7: Run the suite and the type check**

```bash
cd web && npm test && npm run typecheck
```

Expected: 6 tests pass, `tsc` exits zero.

- [ ] **Step 8: Verify the dev proxy against the real daemon**

Only if `irrigationd` is running locally. `npm run dev`, then:

```bash
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:5173/api/health
```

Expected: `200`. A `404` means the rewrite dropped or doubled a prefix.

- [ ] **Step 9: Commit**

```bash
cd /home/spunak/src/punak/irrigation
git add web
git commit -m "feat: scaffold the irrigation web front end"
```

---

### Task 2: Typed API client and runtime decoders

The only module in the bundle that calls `fetch`, plus a decoder per response type. A decoder that casts is worth nothing: the daemon is being written in a different session against a contract this plan wrote down, and the first symptom of a mismatch must be a thrown error naming the field rather than `undefined` rendering as a blank tile.

**Files:**
- Create: `web/src/api/types.ts`, `web/src/api/decode.ts`, `web/src/api/client.ts`
- Create: `web/src/test/fetchStub.ts`, `web/src/test/fixtures.ts`
- Test: `web/src/api/decode.test.ts`, `web/src/api/client.test.ts`

**Interfaces:**
- Consumes: `apiUrl` from Task 1
- Produces:
  - `type DayMode = 'DaysOfWeek' | 'Odd' | 'Even' | 'EveryNDays'`
  - `interface Status { runningZone: number; secondsRemaining: number; nextRunUtc: string | null; timezone: string; masterEnabled: boolean; rainDelayUntilUtc: string | null }`
  - `interface Zone { id: number; number: number; name: string; enabled: boolean }`
  - `interface ProgramStartTime { id: number; minutesAfterMidnight: number; timezone: string }`
  - `interface ProgramZone { id: number; zoneId: number; sequence: number; durationSeconds: number }`
  - `interface Program { id: number; name: string; enabled: boolean; dayMode: DayMode; dowMask: number; intervalDays: number; anchorDate: string | null; startTimes: ProgramStartTime[]; zones: ProgramZone[]; nextRunUtc: string | null }`
  - `type ProgramDraft = Omit<Program, 'id' | 'nextRunUtc' | 'startTimes' | 'zones'> & { startTimes: StartTimeDraft[]; zones: ProgramZoneDraft[] }`
  - `interface StartTimeDraft { minutesAfterMidnight: number; timezone: string }`
  - `interface ProgramZoneDraft { zoneId: number; sequence: number; durationSeconds: number }`
  - `type SettingsMap = Record<string, string>`
  - `class ApiError extends Error { status: number }`, `class DecodeError extends Error { field: string }`
  - `decodeStatus`, `decodeZones`, `decodePrograms`, `decodeSettings`
  - client: `getStatus()`, `getZones()`, `putZone(zone, patch)`, `runZone(zone, seconds)`, `getPrograms()`, `createProgram(draft)`, `updateProgram(id, draft)`, `deleteProgram(id)`, `runProgram(id)`, `stopAll()`, `getSettings()`, `putSettings(patch)`

- [ ] **Step 1: Write the failing decoder tests**

`web/src/api/decode.test.ts`:

```ts
import { describe, expect, it } from 'vitest'
import { decodePrograms, decodeSettings, decodeStatus, decodeZones } from './decode'
import { DecodeError } from './types'

const goodStatus = {
  runningZone: 4,
  secondsRemaining: 120,
  nextRunUtc: '2026-09-13T13:00:00Z',
  timezone: 'America/Los_Angeles',
  masterEnabled: true,
  rainDelayUntilUtc: '',
}

describe('decodeStatus', () => {
  it('reads every field', () => {
    const status = decodeStatus(goodStatus)
    expect(status.runningZone).toBe(4)
    expect(status.secondsRemaining).toBe(120)
    expect(status.nextRunUtc).toBe('2026-09-13T13:00:00Z')
    expect(status.timezone).toBe('America/Los_Angeles')
    expect(status.masterEnabled).toBe(true)
  })

  it('maps the empty timestamp to null', () => {
    expect(decodeStatus(goodStatus).rainDelayUntilUtc).toBeNull()
    expect(decodeStatus({ ...goodStatus, nextRunUtc: '' }).nextRunUtc).toBeNull()
  })

  it('throws naming the field when one is missing', () => {
    const { timezone, ...withoutZone } = goodStatus
    expect(timezone).toBeDefined()
    expect(() => decodeStatus(withoutZone)).toThrow(DecodeError)
    expect(() => decodeStatus(withoutZone)).toThrow(/timezone/)
  })

  it('throws when a number arrives as a string', () => {
    expect(() => decodeStatus({ ...goodStatus, secondsRemaining: '120' })).toThrow(/secondsRemaining/)
  })

  it('throws when a boolean arrives as a string', () => {
    expect(() => decodeStatus({ ...goodStatus, masterEnabled: 'true' })).toThrow(/masterEnabled/)
  })
})

describe('decodeZones', () => {
  it('reads a list', () => {
    const zones = decodeZones([{ id: 9, number: 3, name: 'Front lawn', enabled: true }])
    expect(zones).toHaveLength(1)
    expect(zones[0]?.id).toBe(9)
    expect(zones[0]?.number).toBe(3)
  })

  it('throws when the payload is not an array', () => {
    expect(() => decodeZones({ id: 9 })).toThrow(DecodeError)
  })

  it('names the index of the bad element', () => {
    expect(() =>
      decodeZones([
        { id: 9, number: 3, name: 'Front lawn', enabled: true },
        { id: 10, number: 4, name: 'Roses' },
      ]),
    ).toThrow(/\[1\]\.enabled/)
  })
})

const goodProgram = {
  id: 1,
  name: 'Morning',
  enabled: true,
  dayMode: 'DaysOfWeek',
  dowMask: 5,
  intervalDays: 0,
  anchorDate: null,
  startTimes: [{ id: 3, programId: 1, minutesAfterMidnight: 360, timezone: 'America/Los_Angeles' }],
  zones: [{ id: 7, programId: 1, zoneId: 9, sequence: 1, durationSeconds: 600 }],
  nextRunUtc: '2026-09-14T13:00:00Z',
}

describe('decodePrograms', () => {
  it('reads nested start times and zones', () => {
    const program = decodePrograms([goodProgram])[0]
    expect(program?.startTimes[0]?.minutesAfterMidnight).toBe(360)
    expect(program?.zones[0]?.zoneId).toBe(9)
    expect(program?.zones[0]?.durationSeconds).toBe(600)
  })

  it('rejects an unknown dayMode rather than defaulting', () => {
    expect(() => decodePrograms([{ ...goodProgram, dayMode: 'days_of_week' }])).toThrow(/dayMode/)
    expect(() => decodePrograms([{ ...goodProgram, dayMode: 'everyNDays' }])).toThrow(/dayMode/)
    expect(() => decodePrograms([{ ...goodProgram, dayMode: 'DAYSOFWEEK' }])).toThrow(/dayMode/)
  })

  it('accepts a program with no nextRunUtc', () => {
    const { nextRunUtc, ...withoutNext } = goodProgram
    expect(nextRunUtc).toBeDefined()
    expect(decodePrograms([withoutNext])[0]?.nextRunUtc).toBeNull()
  })

  it('sorts zones by sequence', () => {
    const shuffled = {
      ...goodProgram,
      zones: [
        { id: 8, programId: 1, zoneId: 10, sequence: 2, durationSeconds: 300 },
        { id: 7, programId: 1, zoneId: 9, sequence: 1, durationSeconds: 600 },
      ],
    }
    expect(decodePrograms([shuffled])[0]?.zones.map((zone) => zone.zoneId)).toEqual([9, 10])
  })
})

describe('decodeSettings', () => {
  it('accepts a string map', () => {
    expect(decodeSettings({ master_enabled: 'true', max_zone_seconds: '1800' })).toEqual({
      master_enabled: 'true',
      max_zone_seconds: '1800',
    })
  })

  it('throws when a value is not a string', () => {
    expect(() => decodeSettings({ master_enabled: true })).toThrow(/master_enabled/)
  })
})
```

`rejects an unknown dayMode rather than defaulting` is the one that matters most. The daemon's `Program::dayModeFromString()` at `program.cpp:4-15` returns `DaysOfWeek` for any string it does not recognise, with no error and no log entry. If the two sides disagree on casing and the decoder defaults the same way, every `EveryNDays` program renders as a days-of-week program with a `dowMask` of 0 — the UI says it never runs while the controller waters every third day. Delete the `DAY_MODES.includes(...)` check and this test fails.

`sorts zones by sequence` guards the ordering the daemon promises. `zonesFor()` is documented as ordered by `sequence`, and the Programs screen renders the run order; a client that trusts array order silently shows the wrong sequence the day a query loses its `ORDER BY`.

- [ ] **Step 2: Run it and verify it fails**

```bash
cd web && npm test -- decode
```

Expected: failure — `./decode` does not exist.

- [ ] **Step 3: Write the types**

`web/src/api/types.ts`:

```ts
export type DayMode = 'DaysOfWeek' | 'Odd' | 'Even' | 'EveryNDays'

export const DAY_MODES: readonly DayMode[] = ['DaysOfWeek', 'Odd', 'Even', 'EveryNDays']

export interface Status {
  runningZone: number
  secondsRemaining: number
  nextRunUtc: string | null
  timezone: string
  masterEnabled: boolean
  rainDelayUntilUtc: string | null
}

export interface Zone {
  id: number
  number: number
  name: string
  enabled: boolean
}

export interface ProgramStartTime {
  id: number
  minutesAfterMidnight: number
  timezone: string
}

export interface ProgramZone {
  id: number
  zoneId: number
  sequence: number
  durationSeconds: number
}

export interface Program {
  id: number
  name: string
  enabled: boolean
  dayMode: DayMode
  dowMask: number
  intervalDays: number
  anchorDate: string | null
  startTimes: ProgramStartTime[]
  zones: ProgramZone[]
  nextRunUtc: string | null
}

export interface StartTimeDraft {
  minutesAfterMidnight: number
  timezone: string
}

export interface ProgramZoneDraft {
  zoneId: number
  sequence: number
  durationSeconds: number
}

export interface ProgramDraft {
  name: string
  enabled: boolean
  dayMode: DayMode
  dowMask: number
  intervalDays: number
  anchorDate: string | null
  startTimes: StartTimeDraft[]
  zones: ProgramZoneDraft[]
}

export type SettingsMap = Record<string, string>

/** Thrown for any non-2xx response. `status` is the HTTP code. */
export class ApiError extends Error {
  readonly status: number

  constructor(status: number, message: string) {
    super(message)
    this.name = 'ApiError'
    this.status = status
  }
}

/** Thrown when a response body does not match the contract. `field` is the path that failed. */
export class DecodeError extends Error {
  readonly field: string

  constructor(field: string, detail: string) {
    super(`${field}: ${detail}`)
    this.name = 'DecodeError'
    this.field = field
  }
}
```

- [ ] **Step 4: Write the decoders**

`web/src/api/decode.ts`:

```ts
import {
  DAY_MODES,
  DecodeError,
  type DayMode,
  type Program,
  type ProgramStartTime,
  type ProgramZone,
  type SettingsMap,
  type Status,
  type Zone,
} from './types'

function asRecord(value: unknown, field: string): Record<string, unknown> {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    throw new DecodeError(field, 'expected an object')
  }
  return value as Record<string, unknown>
}

function asArray(value: unknown, field: string): unknown[] {
  if (Array.isArray(value) === false) {
    throw new DecodeError(field, 'expected an array')
  }
  return value as unknown[]
}

function num(source: Record<string, unknown>, key: string, field: string): number {
  const value = source[key]
  if (typeof value !== 'number' || Number.isFinite(value) === false) {
    throw new DecodeError(`${field}.${key}`, `expected a number, received ${typeof value}`)
  }
  return value
}

function str(source: Record<string, unknown>, key: string, field: string): string {
  const value = source[key]
  if (typeof value !== 'string') {
    throw new DecodeError(`${field}.${key}`, `expected a string, received ${typeof value}`)
  }
  return value
}

function bool(source: Record<string, unknown>, key: string, field: string): boolean {
  const value = source[key]
  if (typeof value !== 'boolean') {
    throw new DecodeError(`${field}.${key}`, `expected a boolean, received ${typeof value}`)
  }
  return value
}

/** An absent or empty Qt QDateTime serialises to "". */
function instant(source: Record<string, unknown>, key: string, field: string): string | null {
  const value = source[key]
  if (value === undefined || value === null || value === '') {
    return null
  }
  if (typeof value !== 'string') {
    throw new DecodeError(`${field}.${key}`, `expected an ISO string, received ${typeof value}`)
  }
  return value
}

function dayMode(source: Record<string, unknown>, field: string): DayMode {
  const value = str(source, 'dayMode', field)
  if (DAY_MODES.includes(value as DayMode) === false) {
    throw new DecodeError(`${field}.dayMode`, `unknown day mode "${value}"`)
  }
  return value as DayMode
}

export function decodeStatus(payload: unknown): Status {
  const source = asRecord(payload, 'status')
  return {
    runningZone: num(source, 'runningZone', 'status'),
    secondsRemaining: num(source, 'secondsRemaining', 'status'),
    nextRunUtc: instant(source, 'nextRunUtc', 'status'),
    timezone: str(source, 'timezone', 'status'),
    masterEnabled: bool(source, 'masterEnabled', 'status'),
    rainDelayUntilUtc: instant(source, 'rainDelayUntilUtc', 'status'),
  }
}

export function decodeZones(payload: unknown): Zone[] {
  return asArray(payload, 'zones').map((element, index) => {
    const field = `zones[${index}]`
    const source = asRecord(element, field)
    return {
      id: num(source, 'id', field),
      number: num(source, 'number', field),
      name: str(source, 'name', field),
      enabled: bool(source, 'enabled', field),
    }
  })
}

function decodeStartTime(element: unknown, field: string): ProgramStartTime {
  const source = asRecord(element, field)
  return {
    id: num(source, 'id', field),
    minutesAfterMidnight: num(source, 'minutesAfterMidnight', field),
    timezone: str(source, 'timezone', field),
  }
}

function decodeProgramZone(element: unknown, field: string): ProgramZone {
  const source = asRecord(element, field)
  return {
    id: num(source, 'id', field),
    zoneId: num(source, 'zoneId', field),
    sequence: num(source, 'sequence', field),
    durationSeconds: num(source, 'durationSeconds', field),
  }
}

export function decodePrograms(payload: unknown): Program[] {
  return asArray(payload, 'programs').map((element, index) => {
    const field = `programs[${index}]`
    const source = asRecord(element, field)
    const anchor = source['anchorDate']
    if (anchor !== null && anchor !== undefined && typeof anchor !== 'string') {
      throw new DecodeError(`${field}.anchorDate`, 'expected a YYYY-MM-DD string or null')
    }
    return {
      id: num(source, 'id', field),
      name: str(source, 'name', field),
      enabled: bool(source, 'enabled', field),
      dayMode: dayMode(source, field),
      dowMask: num(source, 'dowMask', field),
      intervalDays: num(source, 'intervalDays', field),
      anchorDate: anchor === undefined || anchor === '' ? null : (anchor as string | null),
      startTimes: asArray(source['startTimes'], `${field}.startTimes`).map((start, i) =>
        decodeStartTime(start, `${field}.startTimes[${i}]`),
      ),
      zones: asArray(source['zones'], `${field}.zones`)
        .map((zone, i) => decodeProgramZone(zone, `${field}.zones[${i}]`))
        .sort((left, right) => left.sequence - right.sequence),
      nextRunUtc: instant(source, 'nextRunUtc', field),
    }
  })
}

export function decodeSettings(payload: unknown): SettingsMap {
  const source = asRecord(payload, 'settings')
  const map: SettingsMap = {}
  for (const [key, value] of Object.entries(source)) {
    if (typeof value !== 'string') {
      throw new DecodeError(`settings.${key}`, `expected a string, received ${typeof value}`)
    }
    map[key] = value
  }
  return map
}
```

- [ ] **Step 5: Write the failing client tests**

`web/src/test/fetchStub.ts`:

```ts
import { vi } from 'vitest'

export interface RecordedCall {
  url: string
  method: string
  body: unknown
  headers: Record<string, string>
}

export interface StubbedResponse {
  status?: number
  body?: unknown
}

/**
 * Replaces globalThis.fetch. `respond` is consulted per call; returning undefined
 * yields 200 with an empty object.
 */
export function installFetchStub(
  respond: (call: RecordedCall) => StubbedResponse | undefined = () => undefined,
) {
  const calls: RecordedCall[] = []

  const stub = vi.fn(async (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    const rawBody = init?.body
    const call: RecordedCall = {
      url: String(input),
      method: init?.method ?? 'GET',
      body: typeof rawBody === 'string' && rawBody.length > 0 ? JSON.parse(rawBody) : null,
      headers: { ...((init?.headers as Record<string, string>) ?? {}) },
    }
    calls.push(call)

    const reply = respond(call) ?? {}
    const status = reply.status ?? 200
    const payload = reply.body === undefined ? {} : reply.body

    return new Response(status === 204 ? null : JSON.stringify(payload), {
      status,
      headers: { 'content-type': 'application/json' },
    })
  })

  vi.stubGlobal('fetch', stub)
  return { calls, stub }
}
```

`web/src/test/fixtures.ts`:

```ts
import type { Program, Status, Zone } from '../api/types'

/**
 * Zone ids are offset from zone numbers by 6. The API addresses a valve by
 * `number` and a program references a zone by `id`; a fixture where the two are
 * equal cannot tell a transposed identifier from a correct one.
 */
export const zoneFixtures: Zone[] = [
  { id: 7, number: 1, name: 'Front lawn', enabled: true },
  { id: 8, number: 2, name: 'Side strip', enabled: true },
  { id: 9, number: 3, name: 'Roses', enabled: true },
  { id: 10, number: 4, name: 'Back lawn', enabled: true },
  { id: 11, number: 5, name: 'Vegetable bed', enabled: true },
  { id: 12, number: 6, name: 'Hedge', enabled: true },
  { id: 13, number: 7, name: 'Planters', enabled: false },
  { id: 14, number: 8, name: 'Orchard', enabled: true },
]

export const idleStatus: Status = {
  runningZone: 0,
  secondsRemaining: 0,
  nextRunUtc: '2026-09-14T13:00:00Z',
  timezone: 'America/Los_Angeles',
  masterEnabled: true,
  rainDelayUntilUtc: null,
}

export const runningStatus: Status = {
  ...idleStatus,
  runningZone: 3,
  secondsRemaining: 120,
}

export const morningProgram: Program = {
  id: 1,
  name: 'Morning',
  enabled: true,
  dayMode: 'DaysOfWeek',
  dowMask: 0b0000101,
  intervalDays: 0,
  anchorDate: null,
  startTimes: [{ id: 3, minutesAfterMidnight: 360, timezone: 'America/Los_Angeles' }],
  zones: [
    { id: 21, zoneId: 7, sequence: 1, durationSeconds: 600 },
    { id: 22, zoneId: 9, sequence: 2, durationSeconds: 300 },
  ],
  nextRunUtc: '2026-09-14T13:00:00Z',
}
```

`web/src/api/client.test.ts`:

```ts
import { afterEach, describe, expect, it, vi } from 'vitest'
import {
  createProgram,
  deleteProgram,
  getSettings,
  getStatus,
  getZones,
  putZone,
  runProgram,
  runZone,
  stopAll,
} from './client'
import { ApiError } from './types'
import { installFetchStub } from '../test/fetchStub'
import { idleStatus, zoneFixtures } from '../test/fixtures'

afterEach(() => {
  vi.unstubAllGlobals()
})

describe('client', () => {
  it('gets status from the proxied path', async () => {
    const { calls } = installFetchStub(() => ({ body: { ...idleStatus, rainDelayUntilUtc: '' } }))
    const status = await getStatus()
    expect(calls[0]?.url).toBe('/api/status')
    expect(calls[0]?.method).toBe('GET')
    expect(status.timezone).toBe('America/Los_Angeles')
  })

  it('addresses a manual run by zone number', async () => {
    const { calls } = installFetchStub(() => ({ status: 202, body: { accepted: true } }))
    const roses = zoneFixtures[2]!
    expect(roses.id).not.toBe(roses.number)

    await runZone(roses, 600)

    expect(calls[0]?.url).toBe('/api/zones/3/run')
    expect(calls[0]?.method).toBe('POST')
    expect(calls[0]?.body).toEqual({ seconds: 600 })
    expect(calls[0]?.headers['content-type']).toBe('application/json')
  })

  it('addresses a zone update by zone number', async () => {
    const { calls } = installFetchStub()
    const roses = zoneFixtures[2]!

    await putZone(roses, { name: 'Rose bed', enabled: false })

    expect(calls[0]?.url).toBe('/api/zones/3')
    expect(calls[0]?.method).toBe('PUT')
    expect(calls[0]?.body).toEqual({ name: 'Rose bed', enabled: false })
  })

  it('posts a program draft with no ids', async () => {
    const { calls } = installFetchStub(() => ({ status: 201, body: { ok: true } }))

    await createProgram({
      name: 'Evening',
      enabled: true,
      dayMode: 'EveryNDays',
      dowMask: 0,
      intervalDays: 3,
      anchorDate: '2026-04-01',
      startTimes: [{ minutesAfterMidnight: 1140, timezone: 'America/Los_Angeles' }],
      zones: [{ zoneId: 9, sequence: 1, durationSeconds: 300 }],
    })

    expect(calls[0]?.url).toBe('/api/programs')
    expect(calls[0]?.body).toEqual({
      name: 'Evening',
      enabled: true,
      dayMode: 'EveryNDays',
      dowMask: 0,
      intervalDays: 3,
      anchorDate: '2026-04-01',
      startTimes: [{ minutesAfterMidnight: 1140, timezone: 'America/Los_Angeles' }],
      zones: [{ zoneId: 9, sequence: 1, durationSeconds: 300 }],
    })
  })

  it('runs and deletes a program by id', async () => {
    const { calls } = installFetchStub((call) => (call.method === 'DELETE' ? { status: 204 } : { status: 202 }))

    await runProgram(12)
    await deleteProgram(12)

    expect(calls[0]?.url).toBe('/api/programs/12/run')
    expect(calls[0]?.method).toBe('POST')
    expect(calls[1]?.url).toBe('/api/programs/12')
    expect(calls[1]?.method).toBe('DELETE')
  })

  it('stops with no body', async () => {
    const { calls } = installFetchStub(() => ({ status: 202 }))
    await stopAll()
    expect(calls[0]?.url).toBe('/api/stop')
    expect(calls[0]?.method).toBe('POST')
    expect(calls[0]?.body).toBeNull()
  })

  it('decodes zones through the decoder', async () => {
    installFetchStub(() => ({ body: zoneFixtures }))
    const zones = await getZones()
    expect(zones.map((zone) => zone.number)).toEqual([1, 2, 3, 4, 5, 6, 7, 8])
  })

  it('throws ApiError carrying the status and the server message', async () => {
    installFetchStub(() => ({ status: 404, body: { error: 'unknown zone' } }))

    await expect(runZone(zoneFixtures[2]!, 600)).rejects.toThrowError(ApiError)
    await expect(runZone(zoneFixtures[2]!, 600)).rejects.toThrow(/unknown zone/)

    const error = await runZone(zoneFixtures[2]!, 600).catch((caught: unknown) => caught)
    expect((error as ApiError).status).toBe(404)
  })

  it('throws ApiError when the body is not JSON at all', async () => {
    vi.stubGlobal(
      'fetch',
      vi.fn(async () => new Response('<html>502 Bad Gateway</html>', { status: 502 })),
    )
    await expect(getSettings()).rejects.toThrowError(ApiError)
  })
})
```

`addresses a manual run by zone number` and `addresses a zone update by zone number` are the transposition guards. The fixture's `id` is 9 and its `number` is 3; a client that reaches for `zone.id` builds `/api/zones/9/run`, the daemon answers 404 because no zone has number 9, and the test fails on the url before it ever gets there.

`throws ApiError when the body is not JSON at all` covers nginx answering 502 while the daemon is restarting. A client that does `await response.json()` before checking `response.ok` throws a `SyntaxError` there, and every screen's error handling has to special-case it.

- [ ] **Step 6: Run it and verify it fails**

```bash
cd web && npm test -- client
```

Expected: failure — `./client` does not exist.

- [ ] **Step 7: Write the client**

`web/src/api/client.ts`:

```ts
import { apiUrl } from './apiPath'
import { decodePrograms, decodeSettings, decodeStatus, decodeZones } from './decode'
import {
  ApiError,
  type Program,
  type ProgramDraft,
  type SettingsMap,
  type Status,
  type Zone,
} from './types'

interface RequestOptions {
  method?: string
  body?: unknown
}

async function request(path: string, options: RequestOptions = {}): Promise<unknown> {
  const method = options.method ?? 'GET'
  const init: RequestInit = { method }

  if (options.body !== undefined) {
    init.headers = { 'content-type': 'application/json' }
    init.body = JSON.stringify(options.body)
  }

  const response = await fetch(apiUrl(path), init)

  if (response.status === 204) {
    return null
  }

  const text = await response.text()
  let payload: unknown = null

  if (text.length > 0) {
    try {
      payload = JSON.parse(text)
    } catch {
      if (response.ok === false) {
        throw new ApiError(response.status, `${response.status} ${response.statusText}`)
      }
      throw new ApiError(response.status, 'response body was not JSON')
    }
  }

  if (response.ok === false) {
    const detail =
      typeof payload === 'object' && payload !== null && typeof (payload as Record<string, unknown>)['error'] === 'string'
        ? ((payload as Record<string, unknown>)['error'] as string)
        : `${response.status} ${response.statusText}`
    throw new ApiError(response.status, detail)
  }

  return payload
}

export async function getStatus(): Promise<Status> {
  return decodeStatus(await request('/status'))
}

export async function getZones(): Promise<Zone[]> {
  return decodeZones(await request('/zones'))
}

export async function putZone(zone: Zone, patch: { name: string; enabled: boolean }): Promise<void> {
  await request(`/zones/${zone.number}`, { method: 'PUT', body: patch })
}

export async function runZone(zone: Zone, seconds: number): Promise<void> {
  await request(`/zones/${zone.number}/run`, { method: 'POST', body: { seconds } })
}

export async function getPrograms(): Promise<Program[]> {
  return decodePrograms(await request('/programs'))
}

export async function createProgram(draft: ProgramDraft): Promise<void> {
  await request('/programs', { method: 'POST', body: draft })
}

export async function updateProgram(id: number, draft: ProgramDraft): Promise<void> {
  await request(`/programs/${id}`, { method: 'PUT', body: draft })
}

export async function deleteProgram(id: number): Promise<void> {
  await request(`/programs/${id}`, { method: 'DELETE' })
}

export async function runProgram(id: number): Promise<void> {
  await request(`/programs/${id}/run`, { method: 'POST' })
}

export async function stopAll(): Promise<void> {
  await request('/stop', { method: 'POST' })
}

export async function getSettings(): Promise<SettingsMap> {
  return decodeSettings(await request('/settings'))
}

export async function putSettings(patch: SettingsMap): Promise<void> {
  await request('/settings', { method: 'PUT', body: patch })
}
```

`stopAll` sends no body, so `request` omits the content-type header and `fetch` sends a zero-length POST. The daemon's stop route parses no body.

- [ ] **Step 8: Run the suite and the type check**

```bash
cd web && npm test && npm run typecheck
```

Expected: all decoder and client tests pass, `tsc` exits zero.

- [ ] **Step 9: Commit**

```bash
cd /home/spunak/src/punak/irrigation
git add web
git commit -m "feat: add the typed irrigation API client and decoders"
```

---

### Task 3: Controller-timezone formatting

Every instant the API returns is UTC, and spec §8 requires rendering in the **controller's** timezone as reported by `/admin/status`. The implementation that reads the browser's zone renders correctly on a laptop sitting in the controller's own timezone, which is where this code will be written and reviewed.

Every function here takes the zone as a **required argument**. There is no overload that defaults to the browser.

**Files:**
- Create: `web/src/time/zonedformat.ts`
- Test: `web/src/time/zonedformat.test.ts`

**Interfaces:**
- Produces:
  - `formatClock(utcIso: string, zone: string): string` — `'6:00 AM'`
  - `formatDayAndClock(utcIso: string, zone: string, nowMs?: number): string` — `'Today 6:00 AM'`, `'Tomorrow 6:00 AM'`, `'Mon 6:00 AM'`, `'Mon 14 Sep 6:00 AM'`
  - `formatDuration(seconds: number): string` — `'10 min'`, `'1 h 30 min'`, `'45 s'`
  - `formatCountdown(seconds: number): string` — `'2:00'`, `'1:05:30'`
  - `minutesToClock(minutesAfterMidnight: number): string` — `'6:00 AM'`
  - `minutesToInputValue(minutesAfterMidnight: number): string` — `'06:00'` for `<input type="time">`
  - `inputValueToMinutes(value: string): number` — `'23:59'` → `1439`
  - `INVALID_ZONE_MARKER = '--'` — returned by `formatClock`, `formatDayAndClock` and `minutesToClock` for anything they cannot render

- [ ] **Step 1: Write the failing tests**

`web/src/time/zonedformat.test.ts`:

```ts
import { describe, expect, it } from 'vitest'
import {
  INVALID_ZONE_MARKER,
  formatClock,
  formatCountdown,
  formatDayAndClock,
  formatDuration,
  inputValueToMinutes,
  minutesToClock,
  minutesToInputValue,
} from './zonedformat'

const LA = 'America/Los_Angeles'
const AUCKLAND = 'Pacific/Auckland'

describe('formatClock', () => {
  it('renders in the zone it is given', () => {
    expect(formatClock('2026-09-13T13:00:00Z', LA)).toBe('6:00 AM')
    expect(formatClock('2026-09-13T13:00:00Z', 'UTC')).toBe('1:00 PM')
  })

  it('renders the same instant differently in two zones', () => {
    const instant = '2026-09-13T13:00:00Z'
    expect(formatClock(instant, LA)).not.toBe(formatClock(instant, AUCKLAND))
  })

  it('tracks a DST transition rather than a fixed offset', () => {
    // Same UTC wall clock either side of the spring transition. PST is UTC-8,
    // PDT is UTC-7, so the local hour differs by one.
    expect(formatClock('2026-01-08T18:00:00Z', LA)).toBe('10:00 AM')
    expect(formatClock('2026-03-12T18:00:00Z', LA)).toBe('11:00 AM')
  })

  it('marks an unusable zone instead of falling back to the browser', () => {
    expect(formatClock('2026-09-13T13:00:00Z', 'Not/AZone')).toBe(INVALID_ZONE_MARKER)
    expect(formatClock('2026-09-13T13:00:00Z', '')).toBe(INVALID_ZONE_MARKER)
  })

  it('marks an unparseable instant', () => {
    expect(formatClock('', LA)).toBe(INVALID_ZONE_MARKER)
    expect(formatClock('not a timestamp', LA)).toBe(INVALID_ZONE_MARKER)
  })
})

describe('formatDayAndClock', () => {
  const now = Date.parse('2026-09-13T20:00:00Z') // 1:00 PM in Los Angeles

  it('says Today for an instant on the same calendar day in the controller zone', () => {
    expect(formatDayAndClock('2026-09-14T01:00:00Z', LA, now)).toBe('Today 6:00 PM')
  })

  it('uses the controller zone for the day boundary, not the host zone', () => {
    // 2026-09-14T01:00:00Z is already the 14th in UTC and still the 13th in
    // Los Angeles. A host-zone day boundary calls this Tomorrow.
    expect(formatDayAndClock('2026-09-14T01:00:00Z', LA, now)).toMatch(/^Today/)
    expect(formatDayAndClock('2026-09-14T01:00:00Z', 'UTC', now)).toMatch(/^Tomorrow/)
  })

  it('says Tomorrow for the next calendar day', () => {
    expect(formatDayAndClock('2026-09-14T13:00:00Z', LA, now)).toBe('Tomorrow 6:00 AM')
  })

  it('names the weekday within the next week', () => {
    expect(formatDayAndClock('2026-09-16T13:00:00Z', LA, now)).toBe('Wed 6:00 AM')
  })

  it('adds the date beyond a week out', () => {
    expect(formatDayAndClock('2026-09-28T13:00:00Z', LA, now)).toBe('Mon 28 Sep 6:00 AM')
  })
})

describe('formatDuration', () => {
  it('renders seconds, minutes and hours', () => {
    expect(formatDuration(45)).toBe('45 s')
    expect(formatDuration(600)).toBe('10 min')
    expect(formatDuration(5400)).toBe('1 h 30 min')
    expect(formatDuration(3600)).toBe('1 h')
    expect(formatDuration(0)).toBe('0 s')
  })
})

describe('formatCountdown', () => {
  it('renders mm:ss under an hour and h:mm:ss above', () => {
    expect(formatCountdown(120)).toBe('2:00')
    expect(formatCountdown(65)).toBe('1:05')
    expect(formatCountdown(9)).toBe('0:09')
    expect(formatCountdown(3930)).toBe('1:05:30')
  })

  it('floors at zero', () => {
    expect(formatCountdown(-5)).toBe('0:00')
  })
})

describe('wall-clock minutes', () => {
  it('converts minutes after midnight without consulting a timezone', () => {
    expect(minutesToClock(0)).toBe('12:00 AM')
    expect(minutesToClock(360)).toBe('6:00 AM')
    expect(minutesToClock(720)).toBe('12:00 PM')
    expect(minutesToClock(1140)).toBe('7:00 PM')
    expect(minutesToClock(1439)).toBe('11:59 PM')
  })

  it('round-trips through the time input format', () => {
    expect(minutesToInputValue(360)).toBe('06:00')
    expect(minutesToInputValue(1439)).toBe('23:59')
    expect(minutesToInputValue(0)).toBe('00:00')
    expect(inputValueToMinutes('06:00')).toBe(360)
    expect(inputValueToMinutes('23:59')).toBe(1439)
    expect(inputValueToMinutes('00:00')).toBe(0)
  })

  it('rejects an unparseable time input', () => {
    expect(inputValueToMinutes('')).toBe(-1)
    expect(inputValueToMinutes('25:00')).toBe(-1)
    expect(inputValueToMinutes('12:60')).toBe(-1)
  })
})
```

What each one kills:

- `renders in the zone it is given` — delete `timeZone: zone` from the formatter options and, with `TZ=UTC` on the host, Los Angeles renders `1:00 PM`. That is the entire spec §8 requirement in one assertion, and the `TZ=UTC` guard in `src/test/setup.ts` is what keeps it honest.
- `tracks a DST transition rather than a fixed offset` — replace the IANA zone with a numeric offset captured once from `/api/status` and one of the two assertions fails. This is why the contract carries a zone id.
- `uses the controller zone for the day boundary` — compute "today" from `new Date().toDateString()` and the first assertion returns `Tomorrow`.
- `converts minutes after midnight without consulting a timezone` — implement `minutesToClock` by constructing a `Date` and formatting it in a zone and every value shifts. A start time is a wall-clock rule; it has no instant to convert.
- `marks an unusable zone instead of falling back to the browser` — a `catch` that retries without the `timeZone` option renders `1:00 PM` and fails. The marker is what makes a bad zone id visible on screen instead of a plausible wrong time.

- [ ] **Step 2: Run it and verify it fails**

```bash
cd web && npm test -- zonedformat
```

Expected: failure — `./zonedformat` does not exist.

- [ ] **Step 3: Write the formatter**

`web/src/time/zonedformat.ts`:

```ts
/** Rendered in place of a time that cannot be formatted. */
export const INVALID_ZONE_MARKER = '--'

const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec']
const WEEKDAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat']

interface ZonedParts {
  year: number
  month: number
  day: number
  hour: number
  minute: number
  weekday: number
}

const partsCache = new Map<string, Intl.DateTimeFormat>()

function partsFormatter(zone: string): Intl.DateTimeFormat | null {
  const cached = partsCache.get(zone)
  if (cached !== undefined) {
    return cached
  }
  try {
    const formatter = new Intl.DateTimeFormat('en-GB', {
      timeZone: zone,
      hourCycle: 'h23',
      year: 'numeric',
      month: '2-digit',
      day: '2-digit',
      hour: '2-digit',
      minute: '2-digit',
      weekday: 'short',
    })
    partsCache.set(zone, formatter)
    return formatter
  } catch {
    return null
  }
}

/**
 * Breaks a UTC instant into calendar fields as they read in `zone`. Returns null
 * when the zone is unknown to the runtime or the instant will not parse.
 */
function zonedParts(utcIso: string, zone: string): ZonedParts | null {
  const ms = Date.parse(utcIso)
  if (Number.isFinite(ms) === false) {
    return null
  }

  const formatter = partsFormatter(zone)
  if (formatter === null) {
    return null
  }

  const found = new Map<string, string>()
  for (const part of formatter.formatToParts(new Date(ms))) {
    found.set(part.type, part.value)
  }

  const weekdayIndex = WEEKDAYS.indexOf(found.get('weekday') ?? '')
  if (weekdayIndex < 0) {
    return null
  }

  return {
    year: Number(found.get('year')),
    month: Number(found.get('month')),
    day: Number(found.get('day')),
    hour: Number(found.get('hour')),
    minute: Number(found.get('minute')),
    weekday: weekdayIndex,
  }
}

function clockFromHourMinute(hour: number, minute: number): string {
  const period = hour < 12 ? 'AM' : 'PM'
  const hour12 = hour % 12 === 0 ? 12 : hour % 12
  return `${hour12}:${String(minute).padStart(2, '0')} ${period}`
}

/** Renders the time of day of a UTC instant as it reads in `zone`. */
export function formatClock(utcIso: string, zone: string): string {
  const parts = zonedParts(utcIso, zone)
  if (parts === null) {
    return INVALID_ZONE_MARKER
  }
  return clockFromHourMinute(parts.hour, parts.minute)
}

function dayNumber(parts: ZonedParts): number {
  return parts.year * 10000 + parts.month * 100 + parts.day
}

/** Renders a UTC instant as a day label plus a time, both read in `zone`. */
export function formatDayAndClock(utcIso: string, zone: string, nowMs: number = Date.now()): string {
  const parts = zonedParts(utcIso, zone)
  const today = zonedParts(new Date(nowMs).toISOString(), zone)
  if (parts === null || today === null) {
    return INVALID_ZONE_MARKER
  }

  const clock = clockFromHourMinute(parts.hour, parts.minute)
  const dayGap = Math.round(
    (Date.UTC(parts.year, parts.month - 1, parts.day) - Date.UTC(today.year, today.month - 1, today.day)) / 86400000,
  )

  if (dayNumber(parts) === dayNumber(today)) {
    return `Today ${clock}`
  }
  if (dayGap === 1) {
    return `Tomorrow ${clock}`
  }
  if (dayGap > 1 && dayGap < 7) {
    return `${WEEKDAYS[parts.weekday]} ${clock}`
  }
  return `${WEEKDAYS[parts.weekday]} ${parts.day} ${MONTHS[parts.month - 1]} ${clock}`
}

/** Renders a run length for display, e.g. 5400 -> "1 h 30 min". */
export function formatDuration(seconds: number): string {
  if (seconds < 60) {
    return `${Math.max(0, Math.round(seconds))} s`
  }

  const totalMinutes = Math.round(seconds / 60)
  const hours = Math.floor(totalMinutes / 60)
  const minutes = totalMinutes % 60

  if (hours === 0) {
    return `${minutes} min`
  }
  if (minutes === 0) {
    return `${hours} h`
  }
  return `${hours} h ${minutes} min`
}

/** Renders remaining seconds as a ticking clock. */
export function formatCountdown(seconds: number): string {
  const total = Math.max(0, Math.floor(seconds))
  const hours = Math.floor(total / 3600)
  const minutes = Math.floor((total % 3600) / 60)
  const secs = total % 60

  if (hours > 0) {
    return `${hours}:${String(minutes).padStart(2, '0')}:${String(secs).padStart(2, '0')}`
  }
  return `${minutes}:${String(secs).padStart(2, '0')}`
}

function isValidMinutes(value: number): boolean {
  return Number.isInteger(value) && value >= 0 && value <= 1439
}

/**
 * A start time is a wall-clock rule with no instant behind it, so this takes no
 * zone. Converting it through a Date shifts every start time by the host offset.
 */
export function minutesToClock(minutesAfterMidnight: number): string {
  if (isValidMinutes(minutesAfterMidnight) === false) {
    return INVALID_ZONE_MARKER
  }
  return clockFromHourMinute(Math.floor(minutesAfterMidnight / 60), minutesAfterMidnight % 60)
}

/** Formats wall-clock minutes for an `<input type="time">` value. */
export function minutesToInputValue(minutesAfterMidnight: number): string {
  if (isValidMinutes(minutesAfterMidnight) === false) {
    return '00:00'
  }
  return `${String(Math.floor(minutesAfterMidnight / 60)).padStart(2, '0')}:${String(minutesAfterMidnight % 60).padStart(2, '0')}`
}

/** Parses an `<input type="time">` value. Returns -1 when it will not parse. */
export function inputValueToMinutes(value: string): number {
  const match = /^(\d{2}):(\d{2})$/.exec(value)
  if (match === null) {
    return -1
  }
  const hours = Number(match[1])
  const minutes = Number(match[2])
  if (hours > 23 || minutes > 59) {
    return -1
  }
  return hours * 60 + minutes
}
```

`isValidMinutes` guards both wall-clock functions ahead of any arithmetic. `inputValueToMinutes`
returns `-1` for input it cannot parse, and a `Math.max`/`Math.min` clamp turns that into a
plausible midnight while passing `NaN` straight through to produce the string `NaN:NaN NaN`.

**A `TZ=UTC` suite cannot prove these two functions ignore the timezone.** A reimplementation through
a `Date` renders identically when the host zone is UTC. Measured: `new Date(local)` + `setMinutes` +
`toLocaleTimeString` round-trips through one zone and cancels out everywhere, while
`new Date(Date.UTC(...))` + `setUTCMinutes` + `toLocaleTimeString` renders minute 0 as `4:00 PM`
under `America/Los_Angeles`. The second shape is what gets written by someone who remembers the API
sends UTC and forgets that a start time is not an instant. `src/time/wallclock.tz.test.ts`, run
under the non-UTC config, is what catches it; the file asserts the host zone is not UTC so it fails
loudly instead of passing vacuously.

The 12-hour string is assembled from `formatToParts` rather than taken from `Intl`'s own `hour12` output. ICU emits a narrow no-break space before the day period in recent versions, so a test comparing against `'6:00 AM'` typed with an ordinary space fails on some runtimes and passes on others.

- [ ] **Step 4: Run the suite and the type check**

```bash
cd web && npm test && npm run typecheck
```

Expected: every case passes.

- [ ] **Step 5: Prove the DST case is not an accident**

Temporarily replace `timeZone: zone` with `timeZone: 'America/Los_Angeles'` hard-coded, run the suite, and confirm `renders the same instant differently in two zones` fails. Revert.

Then delete `timeZone: zone` entirely, run the suite, and confirm `renders in the zone it is given` fails with Los Angeles reading `1:00 PM`. Revert.

- [ ] **Step 6: Commit**

```bash
cd /home/spunak/src/punak/irrigation
git add web
git commit -m "feat: render instants in the controller timezone"
```

---

### Task 4: Adaptive status polling and the local countdown

Spec §8: `/admin/status` every 2 s while a zone is running, every 15 s otherwise. One hook owns that cadence for the whole app. A second hook decrements the running zone's remaining seconds between polls so the number on screen moves once a second instead of jumping every two.

**Files:**
- Create: `web/src/hooks/useStatus.ts`, `web/src/hooks/useCountdown.ts`
- Test: `web/src/hooks/useStatus.test.ts`, `web/src/hooks/useCountdown.test.ts`

**Interfaces:**
- Consumes: `getStatus` from Task 2
- Produces:
  - `const RUNNING_POLL_MS = 2000`, `const IDLE_POLL_MS = 15000`
  - `interface StatusState { status: Status | null; error: string | null; stale: boolean; polls: number; refresh: () => void }`
  - `function useStatus(): StatusState`
  - `function useCountdown(seconds: number, epoch: number): number`

- [ ] **Step 1: Write the failing polling tests**

`web/src/hooks/useStatus.test.ts`:

```ts
import { act, renderHook, waitFor } from '@testing-library/react'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import * as client from '../api/client'
import { idleStatus, runningStatus } from '../test/fixtures'
import { IDLE_POLL_MS, RUNNING_POLL_MS, useStatus } from './useStatus'

beforeEach(() => {
  vi.useFakeTimers()
})

afterEach(() => {
  vi.useRealTimers()
  vi.restoreAllMocks()
})

/** Lets the hook's in-flight promise settle while fake timers are installed. */
async function settle() {
  await act(async () => {
    await vi.advanceTimersByTimeAsync(0)
  })
}

describe('useStatus', () => {
  it('polls once immediately', async () => {
    const getStatus = vi.spyOn(client, 'getStatus').mockResolvedValue(idleStatus)
    const { result } = renderHook(() => useStatus())
    await settle()

    expect(getStatus).toHaveBeenCalledTimes(1)
    expect(result.current.status?.timezone).toBe('America/Los_Angeles')
  })

  it('polls every 15 s while idle', async () => {
    const getStatus = vi.spyOn(client, 'getStatus').mockResolvedValue(idleStatus)
    renderHook(() => useStatus())
    await settle()

    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS - 1)
    })
    expect(getStatus).toHaveBeenCalledTimes(1)

    await act(async () => {
      await vi.advanceTimersByTimeAsync(1)
    })
    expect(getStatus).toHaveBeenCalledTimes(2)
  })

  it('polls every 2 s while a zone is running', async () => {
    const getStatus = vi.spyOn(client, 'getStatus').mockResolvedValue(runningStatus)
    renderHook(() => useStatus())
    await settle()

    await act(async () => {
      await vi.advanceTimersByTimeAsync(RUNNING_POLL_MS)
    })
    expect(getStatus).toHaveBeenCalledTimes(2)

    await act(async () => {
      await vi.advanceTimersByTimeAsync(RUNNING_POLL_MS)
    })
    expect(getStatus).toHaveBeenCalledTimes(3)
  })

  it('tightens the interval as soon as a poll reports a run', async () => {
    const getStatus = vi
      .spyOn(client, 'getStatus')
      .mockResolvedValueOnce(idleStatus)
      .mockResolvedValue(runningStatus)

    renderHook(() => useStatus())
    await settle()

    // The idle poll schedules the next one 15 s out; that one reports a run.
    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS)
    })
    expect(getStatus).toHaveBeenCalledTimes(2)

    // From here the cadence must be 2 s.
    await act(async () => {
      await vi.advanceTimersByTimeAsync(RUNNING_POLL_MS)
    })
    expect(getStatus).toHaveBeenCalledTimes(3)
  })

  it('relaxes the interval when the run ends', async () => {
    const getStatus = vi
      .spyOn(client, 'getStatus')
      .mockResolvedValueOnce(runningStatus)
      .mockResolvedValue(idleStatus)

    renderHook(() => useStatus())
    await settle()

    await act(async () => {
      await vi.advanceTimersByTimeAsync(RUNNING_POLL_MS)
    })
    expect(getStatus).toHaveBeenCalledTimes(2)

    await act(async () => {
      await vi.advanceTimersByTimeAsync(RUNNING_POLL_MS)
    })
    expect(getStatus).toHaveBeenCalledTimes(2)

    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS - RUNNING_POLL_MS)
    })
    expect(getStatus).toHaveBeenCalledTimes(3)
  })

  it('keeps the last good status and marks it stale when a poll fails', async () => {
    vi.spyOn(client, 'getStatus')
      .mockResolvedValueOnce(runningStatus)
      .mockRejectedValue(new Error('Failed to fetch'))

    const { result } = renderHook(() => useStatus())
    await settle()
    expect(result.current.stale).toBe(false)

    await act(async () => {
      await vi.advanceTimersByTimeAsync(RUNNING_POLL_MS)
    })

    expect(result.current.status?.runningZone).toBe(3)
    expect(result.current.stale).toBe(true)
    expect(result.current.error).toMatch(/Failed to fetch/)
  })

  it('clears the error on the next success', async () => {
    vi.spyOn(client, 'getStatus')
      .mockRejectedValueOnce(new Error('Failed to fetch'))
      .mockResolvedValue(idleStatus)

    const { result } = renderHook(() => useStatus())
    await settle()
    expect(result.current.error).not.toBeNull()

    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS)
    })

    expect(result.current.error).toBeNull()
    expect(result.current.stale).toBe(false)
    expect(result.current.status?.runningZone).toBe(0)
  })

  it('keeps polling after a failure rather than giving up', async () => {
    const getStatus = vi.spyOn(client, 'getStatus').mockRejectedValue(new Error('Failed to fetch'))
    renderHook(() => useStatus())
    await settle()

    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS * 3)
    })

    expect(getStatus.mock.calls.length).toBeGreaterThanOrEqual(4)
  })

  it('counts only successful polls', async () => {
    vi.spyOn(client, 'getStatus')
      .mockResolvedValueOnce(idleStatus)
      .mockRejectedValueOnce(new Error('Failed to fetch'))
      .mockResolvedValue(idleStatus)

    const { result } = renderHook(() => useStatus())
    await settle()
    expect(result.current.polls).toBe(1)

    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS)
    })
    expect(result.current.polls).toBe(1)

    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS)
    })
    expect(result.current.polls).toBe(2)
  })

  it('stops polling once unmounted', async () => {
    const getStatus = vi.spyOn(client, 'getStatus').mockResolvedValue(idleStatus)
    const { unmount } = renderHook(() => useStatus())
    await settle()

    unmount()

    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS * 4)
    })

    expect(getStatus).toHaveBeenCalledTimes(1)
  })

  it('refresh() polls immediately and reschedules from now', async () => {
    const getStatus = vi.spyOn(client, 'getStatus').mockResolvedValue(idleStatus)
    const { result } = renderHook(() => useStatus())
    await settle()

    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS / 2)
    })
    expect(getStatus).toHaveBeenCalledTimes(1)

    await act(async () => {
      result.current.refresh()
      await vi.advanceTimersByTimeAsync(0)
    })
    expect(getStatus).toHaveBeenCalledTimes(2)

    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS - 1)
    })
    expect(getStatus).toHaveBeenCalledTimes(2)
  })

  it('never overlaps two requests', async () => {
    let resolveFirst: ((value: typeof idleStatus) => void) | null = null
    const getStatus = vi.spyOn(client, 'getStatus').mockImplementation(
      () =>
        new Promise((resolve) => {
          resolveFirst = resolve
        }),
    )

    renderHook(() => useStatus())
    await settle()
    expect(getStatus).toHaveBeenCalledTimes(1)

    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS * 3)
    })
    expect(getStatus).toHaveBeenCalledTimes(1)

    await act(async () => {
      resolveFirst?.(idleStatus)
      await vi.advanceTimersByTimeAsync(0)
    })
    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS)
    })
    expect(getStatus).toHaveBeenCalledTimes(2)
  })
})
```

What each one kills:

- `polls every 15 s while idle` / `polls every 2 s while a zone is running` — a single hard-coded interval fails one of the two.
- `tightens the interval as soon as a poll reports a run` — an interval captured once at mount into a `setInterval` fails. Reading the cadence off the status that just arrived is the whole point; a manual run started from the Now screen has to speed the poll up without a remount.
- `keeps the last good status and marks it stale` — a handler that sets `status` to `null` on error blanks the running zone and the remaining time the moment wifi hiccups. The stop control has to stay usable through that.
- `keeps polling after a failure rather than giving up` — a `catch` that returns without rescheduling leaves the page permanently frozen after one dropped packet, and the user sees a plausible stale screen forever.
- `stops polling once unmounted` — no cleanup and every navigation leaks a timer.
- `never overlaps two requests` — `setInterval` instead of a chained `setTimeout` stacks requests against a daemon that is already slow, which is exactly when it is least able to answer them.

- [ ] **Step 2: Run it and verify it fails**

```bash
cd web && npm test -- useStatus
```

Expected: failure — `./useStatus` does not exist.

- [ ] **Step 3: Write the polling hook**

`web/src/hooks/useStatus.ts`:

```ts
import { useCallback, useEffect, useRef, useState } from 'react'
import { getStatus } from '../api/client'
import type { Status } from '../api/types'

export const RUNNING_POLL_MS = 2000
export const IDLE_POLL_MS = 15000

export interface StatusState {
  status: Status | null
  error: string | null
  stale: boolean
  /** Increments on each successful poll. Identifies which poll a value came from. */
  polls: number
  refresh: () => void
}

function intervalFor(status: Status | null): number {
  return status !== null && status.runningZone > 0 ? RUNNING_POLL_MS : IDLE_POLL_MS
}

export function useStatus(): StatusState {
  const [status, setStatus] = useState<Status | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [stale, setStale] = useState(false)
  const [polls, setPolls] = useState(0)

  const timer = useRef<ReturnType<typeof setTimeout> | null>(null)
  const inFlight = useRef(false)
  const mounted = useRef(true)

  const poll = useCallback(async () => {
    if (inFlight.current) {
      return
    }
    inFlight.current = true

    let next = IDLE_POLL_MS
    try {
      const fresh = await getStatus()
      if (mounted.current) {
        setStatus(fresh)
        setError(null)
        setStale(false)
        setPolls((count) => count + 1)
      }
      next = intervalFor(fresh)
    } catch (caught: unknown) {
      if (mounted.current) {
        setError(caught instanceof Error ? caught.message : String(caught))
        setStale(true)
      }
    } finally {
      inFlight.current = false
    }

    if (mounted.current) {
      if (timer.current !== null) {
        clearTimeout(timer.current)
      }
      timer.current = setTimeout(() => {
        void poll()
      }, next)
    }
  }, [])

  useEffect(() => {
    mounted.current = true
    void poll()

    return () => {
      mounted.current = false
      if (timer.current !== null) {
        clearTimeout(timer.current)
        timer.current = null
      }
    }
  }, [poll])

  const refresh = useCallback(() => {
    if (timer.current !== null) {
      clearTimeout(timer.current)
      timer.current = null
    }
    void poll()
  }, [poll])

  return { status, error, stale, polls, refresh }
}
```

A failed poll reschedules at `IDLE_POLL_MS` even when a zone was running. The alternative hammers an unreachable daemon every two seconds.

- [ ] **Step 4: Write the failing countdown tests**

`web/src/hooks/useCountdown.test.ts`:

```ts
import { act, renderHook } from '@testing-library/react'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { useCountdown } from './useCountdown'

beforeEach(() => {
  vi.useFakeTimers()
})

afterEach(() => {
  vi.useRealTimers()
})

describe('useCountdown', () => {
  it('starts at the value it is given', () => {
    const { result } = renderHook(() => useCountdown(120, 1))
    expect(result.current).toBe(120)
  })

  it('decrements once a second between polls', () => {
    const { result } = renderHook(() => useCountdown(120, 1))

    act(() => {
      vi.advanceTimersByTime(3000)
    })

    expect(result.current).toBe(117)
  })

  it('resets when a poll delivers a new epoch', () => {
    const { result, rerender } = renderHook(({ seconds, epoch }) => useCountdown(seconds, epoch), {
      initialProps: { seconds: 120, epoch: 1 },
    })

    act(() => {
      vi.advanceTimersByTime(5000)
    })
    expect(result.current).toBe(115)

    rerender({ seconds: 118, epoch: 2 })
    expect(result.current).toBe(118)
  })

  it('holds at the same value when a poll repeats the epoch', () => {
    const { result, rerender } = renderHook(({ seconds, epoch }) => useCountdown(seconds, epoch), {
      initialProps: { seconds: 120, epoch: 1 },
    })

    act(() => {
      vi.advanceTimersByTime(4000)
    })
    expect(result.current).toBe(116)

    rerender({ seconds: 120, epoch: 1 })
    expect(result.current).toBe(116)
  })

  it('floors at zero', () => {
    const { result } = renderHook(() => useCountdown(2, 1))

    act(() => {
      vi.advanceTimersByTime(10000)
    })

    expect(result.current).toBe(0)
  })

  it('stops the timer once unmounted', () => {
    const { unmount } = renderHook(() => useCountdown(120, 1))
    unmount()
    expect(vi.getTimerCount()).toBe(0)
  })
})
```

`decrements once a second between polls` fails for a display driven only by `/api/status`, which would sit still for two seconds and then jump. `resets when a poll delivers a new epoch` fails for a display driven only locally, which drifts away from the daemon's own deadline. `holds at the same value when a poll repeats the epoch` is the one that catches a reset keyed on the seconds value: two consecutive polls two seconds apart can legitimately report the same `secondsRemaining` after rounding, and a value-keyed reset makes the countdown stall.

- [ ] **Step 5: Write the countdown hook**

`web/src/hooks/useCountdown.ts`:

```ts
import { useEffect, useRef, useState } from 'react'

/**
 * Ticks `seconds` down locally between polls. `epoch` identifies the poll the
 * value came from; a change resets the count. Keying the reset on `seconds`
 * instead stalls the display whenever two polls report the same value.
 */
export function useCountdown(seconds: number, epoch: number): number {
  const [remaining, setRemaining] = useState(seconds)
  const lastEpoch = useRef(epoch)

  if (lastEpoch.current !== epoch) {
    lastEpoch.current = epoch
    setRemaining(seconds)
  }

  useEffect(() => {
    const timer = setInterval(() => {
      setRemaining((value) => Math.max(0, value - 1))
    }, 1000)

    return () => {
      clearInterval(timer)
    }
  }, [])

  return remaining
}
```

- [ ] **Step 6: Run the suite and the type check**

```bash
cd web && npm test && npm run typecheck
```

Expected: all polling and countdown cases pass.

- [ ] **Step 7: Commit**

```bash
cd /home/spunak/src/punak/irrigation
git add web
git commit -m "feat: poll status on an adaptive interval with a local countdown"
```

---

### Task 5: App shell, hash routing and the connection banner

The three screens hang off one shell that owns the status poll and hands it down. Routing is the URL fragment, so a reload or a bookmark lands on the screen it names and no router library enters the bundle.

nginx serves the bundle with `try_files $uri $uri/ /index.html`, so path routing would also survive a deep link. Fragment routing survives it without depending on that config being right, and the whole router is the twenty lines below.

**Files:**
- Modify: `web/src/App.tsx`
- Modify: `web/src/App.test.tsx`
- Create: `web/src/screens/NowScreen.tsx`, `web/src/screens/ProgramsScreen.tsx`, `web/src/screens/SettingsScreen.tsx` (stubs; Tasks 6-9 fill them)
- Modify: `web/src/styles/app.css`

**Interfaces:**
- Consumes: `useStatus` from Task 4
- Produces:
  - `type ScreenName = 'now' | 'programs' | 'settings'`
  - `function screenFromHash(hash: string): ScreenName`
  - Each screen takes `{ status: Status | null; refresh: () => void }`

- [ ] **Step 1: Write the failing shell tests**

`web/src/App.test.tsx` (replacing the Task 1 smoke test):

```tsx
import { render, screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import App, { screenFromHash } from './App'
import * as client from './api/client'
import { idleStatus } from './test/fixtures'

beforeEach(() => {
  window.location.hash = ''
  vi.spyOn(client, 'getStatus').mockResolvedValue(idleStatus)
  vi.spyOn(client, 'getZones').mockResolvedValue([])
  vi.spyOn(client, 'getPrograms').mockResolvedValue([])
  vi.spyOn(client, 'getSettings').mockResolvedValue({})
})

afterEach(() => {
  vi.restoreAllMocks()
})

describe('screenFromHash', () => {
  it('maps the three routes', () => {
    expect(screenFromHash('#/now')).toBe('now')
    expect(screenFromHash('#/programs')).toBe('programs')
    expect(screenFromHash('#/settings')).toBe('settings')
  })

  it('falls back to now for anything else', () => {
    expect(screenFromHash('')).toBe('now')
    expect(screenFromHash('#')).toBe('now')
    expect(screenFromHash('#/nonsense')).toBe('now')
    expect(screenFromHash('#/programs/12')).toBe('now')
  })
})

describe('App', () => {
  it('opens on Now', async () => {
    render(<App />)
    expect(await screen.findByRole('heading', { name: /now/i })).toBeInTheDocument()
  })

  it('navigates by tab and records it in the url', async () => {
    const user = userEvent.setup()
    render(<App />)

    await user.click(screen.getByRole('button', { name: 'Programs' }))

    expect(await screen.findByRole('heading', { name: /programs/i })).toBeInTheDocument()
    expect(window.location.hash).toBe('#/programs')
  })

  it('opens the screen the url names on first render', async () => {
    window.location.hash = '#/settings'
    render(<App />)
    expect(await screen.findByRole('heading', { name: /settings/i })).toBeInTheDocument()
  })

  it('follows a hash change from outside React', async () => {
    render(<App />)
    expect(await screen.findByRole('heading', { name: /now/i })).toBeInTheDocument()

    window.location.hash = '#/settings'
    window.dispatchEvent(new HashChangeEvent('hashchange'))

    expect(await screen.findByRole('heading', { name: /settings/i })).toBeInTheDocument()
  })

  it('marks the active tab for assistive tech', async () => {
    const user = userEvent.setup()
    render(<App />)

    expect(screen.getByRole('button', { name: 'Now' })).toHaveAttribute('aria-current', 'page')

    await user.click(screen.getByRole('button', { name: 'Settings' }))

    expect(screen.getByRole('button', { name: 'Settings' })).toHaveAttribute('aria-current', 'page')
    expect(screen.getByRole('button', { name: 'Now' })).not.toHaveAttribute('aria-current')
  })

  it('shows a banner while the status poll is failing', async () => {
    vi.spyOn(client, 'getStatus').mockRejectedValue(new Error('Failed to fetch'))
    render(<App />)
    expect(await screen.findByRole('status')).toHaveTextContent(/not reaching the controller/i)
  })

  it('hides the banner once the poll recovers', async () => {
    vi.spyOn(client, 'getStatus')
      .mockRejectedValueOnce(new Error('Failed to fetch'))
      .mockResolvedValue(idleStatus)

    render(<App />)
    expect(await screen.findByRole('status')).toBeInTheDocument()

    await waitFor(
      () => {
        expect(screen.queryByRole('status')).toBeNull()
      },
      { timeout: 20000 },
    )
  }, 25000)
})
```

`falls back to now for anything else` covers `#/programs/12`. The editor in Task 8 is a screen state rather than a route, so a fragment with an id in it is not a route this app serves, and the fallback keeps a stale bookmark from rendering an empty shell.

`follows a hash change from outside React` is the back-button case. State-only tabs pass the click test and fail this one — the URL changes, the screen does not, and the user is stuck.

- [ ] **Step 2: Run it and verify it fails**

```bash
cd web && npm test -- App
```

Expected: failure — `screenFromHash` is not exported and there are no tabs.

- [ ] **Step 3: Write the screen stubs**

Each is replaced by its own task. `web/src/screens/NowScreen.tsx`:

```tsx
import type { Status } from '../api/types'

export interface ScreenProps {
  status: Status | null
  /** Identifies the poll `status` came from; the countdown resets when it changes. */
  polls: number
  refresh: () => void
}

export default function NowScreen(_props: ScreenProps) {
  return (
    <section className="screen">
      <h1>Now</h1>
    </section>
  )
}
```

`web/src/screens/ProgramsScreen.tsx` and `web/src/screens/SettingsScreen.tsx` are the same shape with headings `Programs` and `Settings`, each importing `ScreenProps` from `./NowScreen`.

- [ ] **Step 4: Write the shell**

`web/src/App.tsx`:

```tsx
import { useCallback, useEffect, useState } from 'react'
import { useStatus } from './hooks/useStatus'
import NowScreen from './screens/NowScreen'
import ProgramsScreen from './screens/ProgramsScreen'
import SettingsScreen from './screens/SettingsScreen'

export type ScreenName = 'now' | 'programs' | 'settings'

const TABS: { name: ScreenName; label: string }[] = [
  { name: 'now', label: 'Now' },
  { name: 'programs', label: 'Programs' },
  { name: 'settings', label: 'Settings' },
]

/** Maps a URL fragment to a screen. Anything unrecognised lands on Now. */
export function screenFromHash(hash: string): ScreenName {
  const route = hash.replace(/^#\/?/, '')
  const match = TABS.find((tab) => tab.name === route)
  return match === undefined ? 'now' : match.name
}

export default function App() {
  const [screen, setScreen] = useState<ScreenName>(() => screenFromHash(window.location.hash))
  const { status, error, stale, polls, refresh } = useStatus()

  useEffect(() => {
    const onHashChange = () => {
      setScreen(screenFromHash(window.location.hash))
    }
    window.addEventListener('hashchange', onHashChange)
    return () => {
      window.removeEventListener('hashchange', onHashChange)
    }
  }, [])

  const navigate = useCallback((name: ScreenName) => {
    window.location.hash = `#/${name}`
    setScreen(name)
  }, [])

  const props = { status, polls, refresh }

  return (
    <div className="app">
      {stale ? (
        <div className="banner" role="status">
          Not reaching the controller — showing the last known state. {error}
        </div>
      ) : null}

      {screen === 'now' ? <NowScreen {...props} /> : null}
      {screen === 'programs' ? <ProgramsScreen {...props} /> : null}
      {screen === 'settings' ? <SettingsScreen {...props} /> : null}

      <nav className="tabs">
        {TABS.map((tab) => (
          <button
            key={tab.name}
            type="button"
            {...(screen === tab.name ? { 'aria-current': 'page' as const } : {})}
            onClick={() => {
              navigate(tab.name)
            }}
          >
            {tab.label}
          </button>
        ))}
      </nav>
    </div>
  )
}
```

`navigate` sets the state as well as the hash. Assigning `window.location.hash` fires `hashchange` asynchronously in a real browser, and jsdom's timing differs; setting both makes the click test deterministic and the listener idempotent.

The banner is keyed on `stale` rather than on `error` being non-null, so a one-off failure that the next poll clears does not leave a warning on screen.

- [ ] **Step 5: Run the suite and the type check**

```bash
cd web && npm test && npm run typecheck
```

Expected: every shell case passes.

- [ ] **Step 6: Commit**

```bash
cd /home/spunak/src/punak/irrigation
git add web
git commit -m "feat: add the app shell, hash routing and the connection banner"
```

---

### Task 6: Now screen — running zone, zone tiles, e-stop

Spec §8: running zone with time remaining, next scheduled run, eight zone tiles with a quick manual run, and a prominent stop control. Mobile-first, large touch targets, high contrast for outdoor readability.

The stop control is an emergency stop. It takes one tap and runs. No confirmation dialog stands between a wet person with muddy hands and closing the valves, and no polling failure disables it — the moment the daemon is unreachable is exactly when someone is reaching for it.

**Files:**
- Modify: `web/src/screens/NowScreen.tsx`
- Create: `web/src/components/ZoneTile.tsx`, `web/src/components/StopButton.tsx`
- Modify: `web/src/styles/app.css`
- Test: `web/src/screens/NowScreen.test.tsx`

**Interfaces:**
- Consumes: `getZones`, `runZone`, `stopAll` from Task 2; `formatCountdown`, `formatDayAndClock`, `formatDuration` from Task 3; `useCountdown` from Task 4
- Produces:
  - `const QUICK_RUN_CHOICES = [60, 300, 600, 900, 1200, 1800]`
  - `ZoneTile` props: `{ zone: Zone; running: boolean; disabled: boolean; onRun: (zone: Zone) => void }`
  - `StopButton` props: `{ onStop: () => void; busy: boolean }`

- [ ] **Step 1: Write the failing Now screen tests**

`web/src/screens/NowScreen.test.tsx`:

```tsx
import { render, screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import NowScreen from './NowScreen'
import * as client from '../api/client'
import { ApiError } from '../api/types'
import { idleStatus, runningStatus, zoneFixtures } from '../test/fixtures'

const refresh = vi.fn()

beforeEach(() => {
  vi.spyOn(client, 'getZones').mockResolvedValue(zoneFixtures)
  vi.spyOn(client, 'runZone').mockResolvedValue(undefined)
  vi.spyOn(client, 'stopAll').mockResolvedValue(undefined)
  vi.stubGlobal(
    'confirm',
    vi.fn(() => {
      throw new Error('the Now screen must not raise a confirmation dialog')
    }),
  )
})

afterEach(() => {
  vi.restoreAllMocks()
  vi.unstubAllGlobals()
  refresh.mockReset()
})

describe('NowScreen idle', () => {
  it('says nothing is running and shows the next scheduled run in the controller zone', async () => {
    render(<NowScreen status={idleStatus} polls={1} refresh={refresh} />)

    expect(await screen.findByText(/no zone running/i)).toBeInTheDocument()
    // 2026-09-14T13:00:00Z is 06:00 in America/Los_Angeles.
    expect(screen.getByTestId('next-run')).toHaveTextContent('6:00 AM')
  })

  it('renders one tile per zone, ordered by zone number', async () => {
    const shuffled = [zoneFixtures[2]!, zoneFixtures[0]!, zoneFixtures[1]!]
    vi.spyOn(client, 'getZones').mockResolvedValue(shuffled)

    render(<NowScreen status={idleStatus} polls={1} refresh={refresh} />)

    const tiles = await screen.findAllByTestId(/^zone-tile-/)
    expect(tiles.map((tile) => tile.getAttribute('data-testid'))).toEqual([
      'zone-tile-1',
      'zone-tile-2',
      'zone-tile-3',
    ])
  })

  it('names each zone', async () => {
    render(<NowScreen status={idleStatus} polls={1} refresh={refresh} />)
    expect(await screen.findByText('Roses')).toBeInTheDocument()
    expect(screen.getByText('Front lawn')).toBeInTheDocument()
  })
})

describe('NowScreen manual run', () => {
  it('runs the tapped zone by its zone number', async () => {
    const user = userEvent.setup()
    const runZone = vi.spyOn(client, 'runZone').mockResolvedValue(undefined)

    render(<NowScreen status={idleStatus} polls={1} refresh={refresh} />)

    const roses = await screen.findByTestId('zone-tile-3')
    await user.click(within(roses).getByRole('button', { name: /run/i }))

    expect(runZone).toHaveBeenCalledTimes(1)
    const [zoneArgument, seconds] = runZone.mock.calls[0]!
    expect(zoneArgument.number).toBe(3)
    expect(zoneArgument.id).toBe(9)
    expect(seconds).toBe(600)
  })

  it('runs for the duration the selector shows', async () => {
    const user = userEvent.setup()
    const runZone = vi.spyOn(client, 'runZone').mockResolvedValue(undefined)

    render(<NowScreen status={idleStatus} polls={1} refresh={refresh} />)

    await user.selectOptions(await screen.findByLabelText(/run for/i), '1800')
    await user.click(within(screen.getByTestId('zone-tile-1')).getByRole('button', { name: /run/i }))

    expect(runZone.mock.calls[0]![1]).toBe(1800)
  })

  it('refreshes the status straight after a run so the poll tightens', async () => {
    const user = userEvent.setup()
    render(<NowScreen status={idleStatus} polls={1} refresh={refresh} />)

    await user.click(
      within(await screen.findByTestId('zone-tile-1')).getByRole('button', { name: /run/i }),
    )

    await waitFor(() => {
      expect(refresh).toHaveBeenCalled()
    })
  })

  it('will not run a disabled zone', async () => {
    const runZone = vi.spyOn(client, 'runZone').mockResolvedValue(undefined)
    render(<NowScreen status={idleStatus} polls={1} refresh={refresh} />)

    // zoneFixtures[6] is zone number 7, enabled: false.
    const planters = await screen.findByTestId('zone-tile-7')
    expect(within(planters).getByRole('button', { name: /run/i })).toBeDisabled()
    expect(runZone).not.toHaveBeenCalled()
  })

  it('surfaces a rejected run instead of swallowing it', async () => {
    const user = userEvent.setup()
    vi.spyOn(client, 'runZone').mockRejectedValue(new ApiError(404, 'unknown zone'))

    render(<NowScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.click(
      within(await screen.findByTestId('zone-tile-1')).getByRole('button', { name: /run/i }),
    )

    expect(await screen.findByRole('alert')).toHaveTextContent(/unknown zone/i)
  })
})

describe('NowScreen while running', () => {
  it('names the running zone and counts down', async () => {
    render(<NowScreen status={runningStatus} polls={1} refresh={refresh} />)

    const banner = await screen.findByTestId('running-banner')
    expect(banner).toHaveTextContent('Roses')
    expect(banner).toHaveTextContent('2:00')
  })

  it('marks the running tile', async () => {
    render(<NowScreen status={runningStatus} polls={1} refresh={refresh} />)
    expect(await screen.findByTestId('zone-tile-3')).toHaveAttribute('data-running', 'true')
    expect(screen.getByTestId('zone-tile-1')).toHaveAttribute('data-running', 'false')
  })
})

describe('StopButton', () => {
  it('stops on a single tap with no dialog in the way', async () => {
    const user = userEvent.setup()
    const stopAll = vi.spyOn(client, 'stopAll').mockResolvedValue(undefined)

    render(<NowScreen status={runningStatus} polls={1} refresh={refresh} />)
    await user.click(await screen.findByRole('button', { name: /stop/i }))

    expect(stopAll).toHaveBeenCalledTimes(1)
    expect(globalThis.confirm).not.toHaveBeenCalled()
  })

  it('stays live while the status poll is failing', async () => {
    const user = userEvent.setup()
    const stopAll = vi.spyOn(client, 'stopAll').mockResolvedValue(undefined)

    render(<NowScreen status={null} polls={0} refresh={refresh} />)

    const stop = await screen.findByRole('button', { name: /stop/i })
    expect(stop).toBeEnabled()

    await user.click(stop)
    expect(stopAll).toHaveBeenCalledTimes(1)
  })

  it('reports a stop the daemon refused', async () => {
    const user = userEvent.setup()
    vi.spyOn(client, 'stopAll').mockRejectedValue(new ApiError(500, 'could not close the bank'))

    render(<NowScreen status={runningStatus} polls={1} refresh={refresh} />)
    await user.click(await screen.findByRole('button', { name: /stop/i }))

    expect(await screen.findByRole('alert')).toHaveTextContent(/could not close the bank/i)
  })
})
```

What each one kills:

- The `confirm` stub **throws** rather than returning false. Any implementation that puts a dialog in front of the e-stop fails loudly on the throw rather than quietly on an assertion, and the same stub is installed for the manual-run tests so a confirmation cannot creep in there either.
- `stays live while the status poll is failing` renders with `status={null}`. A `disabled={status === null}` on the stop control passes every other test in this file and fails this one. That is the case the control exists for.
- `reports a stop the daemon refused` is the front end's half of the `bool allOff()` change the daemon plan made at `dcd3e09`. A stop that returns 500 because the bank would not close must not look identical to a stop that worked.
- `runs the tapped zone by its zone number` asserts both `number` **and** `id` on the argument. The fixture has `id: 9, number: 3`; reaching for the wrong one changes the url and the assertion names which field was wrong.
- `renders one tile per zone, ordered by zone number` feeds the zones back in a shuffled order. A component that maps the array as it arrives renders 3, 1, 2 and fails.
- `refreshes the status straight after a run` is what makes the poll tighten to 2 s without waiting up to fifteen seconds for the idle tick to notice a run started.

- [ ] **Step 2: Run it and verify it fails**

```bash
cd web && npm test -- NowScreen
```

Expected: failure — the stub screen has no tiles.

- [ ] **Step 3: Write the components**

`web/src/components/ZoneTile.tsx`:

```tsx
import type { Zone } from '../api/types'

export interface ZoneTileProps {
  zone: Zone
  running: boolean
  disabled: boolean
  onRun: (zone: Zone) => void
}

export default function ZoneTile({ zone, running, disabled, onRun }: ZoneTileProps) {
  return (
    <div
      className="zone-tile"
      data-testid={`zone-tile-${zone.number}`}
      data-running={running ? 'true' : 'false'}
    >
      <span className="zone-tile__number">{zone.number}</span>
      <span className="zone-tile__name">{zone.name}</span>
      {zone.enabled ? null : <span className="zone-tile__off">Disabled</span>}
      <button
        type="button"
        className="zone-tile__run"
        disabled={disabled || zone.enabled === false}
        onClick={() => {
          onRun(zone)
        }}
      >
        {running ? 'Running' : 'Run'}
      </button>
    </div>
  )
}
```

`web/src/components/StopButton.tsx`:

```tsx
export interface StopButtonProps {
  onStop: () => void
  busy: boolean
}

/**
 * No confirmation step and no disabled state driven by connectivity. Both are
 * things a maintainer adds while tidying, and both break the control for the
 * person standing in the spray.
 */
export default function StopButton({ onStop, busy }: StopButtonProps) {
  return (
    <button type="button" className="stop" onClick={onStop} aria-busy={busy}>
      STOP
    </button>
  )
}
```

- [ ] **Step 4: Write the Now screen**

`web/src/screens/NowScreen.tsx`:

```tsx
import { useCallback, useEffect, useState } from 'react'
import { getZones, runZone, stopAll } from '../api/client'
import type { Status, Zone } from '../api/types'
import StopButton from '../components/StopButton'
import ZoneTile from '../components/ZoneTile'
import { useCountdown } from '../hooks/useCountdown'
import { formatCountdown, formatDayAndClock, formatDuration } from '../time/zonedformat'

export interface ScreenProps {
  status: Status | null
  /** Identifies the poll `status` came from; the countdown resets when it changes. */
  polls: number
  refresh: () => void
}

export const QUICK_RUN_CHOICES = [60, 300, 600, 900, 1200, 1800]
const DEFAULT_QUICK_RUN = 600

export default function NowScreen({ status, polls, refresh }: ScreenProps) {
  const [zones, setZones] = useState<Zone[]>([])
  const [seconds, setSeconds] = useState(DEFAULT_QUICK_RUN)
  const [error, setError] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)

  useEffect(() => {
    let cancelled = false
    getZones()
      .then((loaded) => {
        if (cancelled === false) {
          setZones([...loaded].sort((left, right) => left.number - right.number))
        }
      })
      .catch((caught: unknown) => {
        if (cancelled === false) {
          setError(caught instanceof Error ? caught.message : String(caught))
        }
      })
    return () => {
      cancelled = true
    }
  }, [])

  const runningZone = status?.runningZone ?? 0
  const remaining = useCountdown(status?.secondsRemaining ?? 0, polls)
  const running = zones.find((zone) => zone.number === runningZone)

  const onRun = useCallback(
    (zone: Zone) => {
      setError(null)
      setBusy(true)
      runZone(zone, seconds)
        .then(() => {
          refresh()
        })
        .catch((caught: unknown) => {
          setError(caught instanceof Error ? caught.message : String(caught))
        })
        .finally(() => {
          setBusy(false)
        })
    },
    [seconds, refresh],
  )

  const onStop = useCallback(() => {
    setError(null)
    setBusy(true)
    stopAll()
      .then(() => {
        refresh()
      })
      .catch((caught: unknown) => {
        setError(caught instanceof Error ? caught.message : String(caught))
      })
      .finally(() => {
        setBusy(false)
      })
  }, [refresh])

  const zone = status?.timezone ?? ''

  return (
    <section className="screen">
      <h1>Now</h1>

      {error === null ? null : (
        <div className="alert" role="alert">
          {error}
        </div>
      )}

      <div className="running" data-testid="running-banner">
        {runningZone > 0 ? (
          <>
            <span className="running__zone">
              Zone {runningZone} — {running?.name ?? ''}
            </span>
            <span className="running__clock">{formatCountdown(remaining)}</span>
          </>
        ) : (
          <span className="running__idle">No zone running</span>
        )}
      </div>

      <StopButton onStop={onStop} busy={busy} />

      <div className="next-run" data-testid="next-run">
        Next run:{' '}
        {status !== null && status.nextRunUtc !== null
          ? formatDayAndClock(status.nextRunUtc, zone)
          : 'none scheduled'}
      </div>

      <label className="quick-run">
        Run for
        <select
          value={seconds}
          onChange={(event) => {
            setSeconds(Number(event.target.value))
          }}
        >
          {QUICK_RUN_CHOICES.map((choice) => (
            <option key={choice} value={choice}>
              {formatDuration(choice)}
            </option>
          ))}
        </select>
      </label>

      <div className="zone-grid">
        {zones.map((entry) => (
          <ZoneTile
            key={entry.number}
            zone={entry}
            running={entry.number === runningZone}
            disabled={busy}
            onRun={onRun}
          />
        ))}
      </div>
    </section>
  )
}
```

`useCountdown` takes the poll counter as its epoch, so every successful poll resets the local count to the daemon's number and the ticks in between fill the gap. Passing `secondsRemaining` as its own epoch would look equivalent and would stop resetting whenever two consecutive polls happened to report the same value.

`StopButton` receives `busy` for its `aria-busy` attribute and never for `disabled`.

`ProgramsScreen` and `SettingsScreen` import `ScreenProps` from this file.

- [ ] **Step 5: Add the styles**

Append to `web/src/styles/app.css`:

```css
.running {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: var(--gap);
  padding: var(--gap);
  border-radius: var(--radius);
  background: var(--surface-raised);
  font-size: 1.3rem;
}

.running__clock {
  font-variant-numeric: tabular-nums;
  font-size: 2rem;
  font-weight: 700;
  color: var(--running);
}

.stop {
  min-height: 96px;
  border: 0;
  border-radius: var(--radius);
  background: var(--danger);
  color: var(--danger-ink);
  font-size: 2rem;
  font-weight: 800;
  letter-spacing: 0.08em;
}

.stop:active {
  filter: brightness(0.85);
}

.zone-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(150px, 1fr));
  gap: var(--gap);
}

.zone-tile {
  display: flex;
  flex-direction: column;
  gap: 6px;
  padding: var(--gap);
  border: 1px solid var(--line);
  border-radius: var(--radius);
  background: var(--surface-raised);
}

.zone-tile[data-running='true'] {
  border-color: var(--running);
  box-shadow: inset 0 0 0 2px var(--running);
}

.zone-tile__number {
  font-size: 0.8rem;
  color: var(--ink-dim);
}

.zone-tile__name {
  font-weight: 600;
}

.zone-tile__off {
  font-size: 0.8rem;
  color: var(--warn);
}

.alert {
  padding: var(--gap);
  border-radius: var(--radius);
  background: var(--danger);
  color: var(--danger-ink);
}

.quick-run {
  display: flex;
  align-items: center;
  gap: var(--gap);
}
```

- [ ] **Step 6: Run the suite and the type check**

```bash
cd web && npm test && npm run typecheck
```

Expected: every Now screen case passes.

- [ ] **Step 7: Prove the e-stop guard can fail**

Temporarily wrap `onStop`'s body in `if (confirm('Stop all zones?') === false) { return }`, run `npm test -- NowScreen`, and confirm both `StopButton` tests fail on the thrown dialog. Revert.

Then temporarily add `disabled={status === null}` to `StopButton` and confirm `stays live while the status poll is failing` fails. Revert.

- [ ] **Step 8: Commit**

```bash
cd /home/spunak/src/punak/irrigation
git add web
git commit -m "feat: add the Now screen with zone tiles and the e-stop"
```

---

### Task 7: Programs screen — list, day rules, computed totals

Spec §8: list, day rule, start times, ordered zone list with durations, computed total runtime, next run. Creating and editing arrive in Task 8; this task builds the list and the pure functions the editor will also use.

`dowMask` bit 0 is **Monday**. Sunday-first is the other plausible reading, it is what most JavaScript date APIs use, and picking it waters on the wrong days without failing any test that does not name a specific weekday.

**Files:**
- Create: `web/src/programs/dayRule.ts`
- Modify: `web/src/screens/ProgramsScreen.tsx`
- Modify: `web/src/styles/app.css`
- Test: `web/src/programs/dayRule.test.ts`, `web/src/screens/ProgramsScreen.test.tsx`

**Interfaces:**
- Consumes: `getPrograms`, `updateProgram`, `runProgram`, `getZones` from Task 2; `formatDayAndClock`, `formatDuration`, `minutesToClock` from Task 3
- Produces:
  - `const WEEKDAY_LABELS: readonly string[]` — `['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun']`, index 0 is bit 0
  - `weekdaysFromMask(mask: number): number[]`
  - `maskFromWeekdays(indices: number[]): number`
  - `toggleWeekday(mask: number, index: number): number`
  - `dayRuleSummary(program: Pick<Program, 'dayMode' | 'dowMask' | 'intervalDays' | 'anchorDate'>): string`
  - `totalRuntimeSeconds(zones: { durationSeconds: number }[]): number`
  - `toDraft(program: Program): ProgramDraft`

- [ ] **Step 1: Write the failing day-rule tests**

`web/src/programs/dayRule.test.ts`:

```ts
import { describe, expect, it } from 'vitest'
import type { Program } from '../api/types'
import {
  WEEKDAY_LABELS,
  dayRuleSummary,
  maskFromWeekdays,
  toDraft,
  toggleWeekday,
  totalRuntimeSeconds,
  weekdaysFromMask,
} from './dayRule'
import { morningProgram } from '../test/fixtures'

describe('weekday bits', () => {
  it('puts Monday at bit 0 and Sunday at bit 6', () => {
    expect(WEEKDAY_LABELS[0]).toBe('Mon')
    expect(WEEKDAY_LABELS[6]).toBe('Sun')
    expect(weekdaysFromMask(0b0000001)).toEqual([0])
    expect(weekdaysFromMask(0b1000000)).toEqual([6])
  })

  it('reads a multi-day mask in order', () => {
    expect(weekdaysFromMask(0b0010101)).toEqual([0, 2, 4])
  })

  it('round-trips a mask', () => {
    expect(maskFromWeekdays([0, 2, 4])).toBe(0b0010101)
    expect(maskFromWeekdays(weekdaysFromMask(0b1010101))).toBe(0b1010101)
    expect(weekdaysFromMask(maskFromWeekdays([6]))).toEqual([6])
  })

  it('toggles one bit and leaves the rest alone', () => {
    expect(toggleWeekday(0b0000001, 2)).toBe(0b0000101)
    expect(toggleWeekday(0b0000101, 0)).toBe(0b0000100)
  })

  it('ignores bits outside the week', () => {
    expect(weekdaysFromMask(0b11111111)).toEqual([0, 1, 2, 3, 4, 5, 6])
    expect(toggleWeekday(0, 7)).toBe(0)
    expect(toggleWeekday(0, -1)).toBe(0)
  })
})

describe('dayRuleSummary', () => {
  const base: Pick<Program, 'dayMode' | 'dowMask' | 'intervalDays' | 'anchorDate'> = {
    dayMode: 'DaysOfWeek',
    dowMask: 0,
    intervalDays: 0,
    anchorDate: null,
  }

  it('names the selected weekdays', () => {
    expect(dayRuleSummary({ ...base, dowMask: 0b0000001 })).toBe('Mon')
    expect(dayRuleSummary({ ...base, dowMask: 0b1000000 })).toBe('Sun')
    expect(dayRuleSummary({ ...base, dowMask: 0b0010101 })).toBe('Mon, Wed, Fri')
  })

  it('collapses a full week', () => {
    expect(dayRuleSummary({ ...base, dowMask: 0b1111111 })).toBe('Every day')
  })

  it('says so when no day is selected', () => {
    expect(dayRuleSummary({ ...base, dowMask: 0 })).toBe('No days selected')
  })

  it('names the other three modes', () => {
    expect(dayRuleSummary({ ...base, dayMode: 'Odd' })).toBe('Odd days')
    expect(dayRuleSummary({ ...base, dayMode: 'Even' })).toBe('Even days')
    expect(
      dayRuleSummary({ ...base, dayMode: 'EveryNDays', intervalDays: 3, anchorDate: '2026-04-01' }),
    ).toBe('Every 3 days from 1 Apr 2026')
  })

  it('handles an interval of one', () => {
    expect(
      dayRuleSummary({ ...base, dayMode: 'EveryNDays', intervalDays: 1, anchorDate: '2026-04-01' }),
    ).toBe('Every day from 1 Apr 2026')
  })

  it('ignores the mask when the mode is not DaysOfWeek', () => {
    expect(dayRuleSummary({ ...base, dayMode: 'Odd', dowMask: 0b1111111 })).toBe('Odd days')
  })
})

describe('totalRuntimeSeconds', () => {
  it('sums every zone duration', () => {
    expect(totalRuntimeSeconds([{ durationSeconds: 600 }, { durationSeconds: 300 }])).toBe(900)
  })

  it('is zero for an empty program', () => {
    expect(totalRuntimeSeconds([])).toBe(0)
  })
})

describe('toDraft', () => {
  it('strips the ids the daemon assigns', () => {
    const draft = toDraft(morningProgram)

    expect(draft).toEqual({
      name: 'Morning',
      enabled: true,
      dayMode: 'DaysOfWeek',
      dowMask: 0b0000101,
      intervalDays: 0,
      anchorDate: null,
      startTimes: [{ minutesAfterMidnight: 360, timezone: 'America/Los_Angeles' }],
      zones: [
        { zoneId: 7, sequence: 1, durationSeconds: 600 },
        { zoneId: 9, sequence: 2, durationSeconds: 300 },
      ],
    })
  })

  it('keeps the zone ids and drops the row ids', () => {
    const draft = toDraft(morningProgram)
    expect(draft.zones.map((zone) => zone.zoneId)).toEqual([7, 9])
    expect(JSON.stringify(draft)).not.toContain('"id"')
  })
})
```

`puts Monday at bit 0 and Sunday at bit 6` is the whole trap in two assertions. A Sunday-first implementation maps `0b0000001` to Sunday and passes every test that only checks round-tripping.

`ignores the mask when the mode is not DaysOfWeek` catches a summary that concatenates both, which would tell the user an odd-day program runs every day.

`keeps the zone ids and drops the row ids` is the transposition guard on the write path. `program_zones.zone_id` is a foreign key to `zones.id`, so the editor sends `zone.id` here while the manual-run path sends `zone.number` — the two identifiers travel in opposite directions in the same screen. The `morningProgram` fixture references zone ids 7 and 9, whose zone numbers are 1 and 3; a draft carrying 1 and 3 fails.

- [ ] **Step 2: Run it and verify it fails**

```bash
cd web && npm test -- dayRule
```

Expected: failure — `./dayRule` does not exist.

- [ ] **Step 3: Write the day-rule module**

`web/src/programs/dayRule.ts`:

```ts
import type { Program, ProgramDraft } from '../api/types'

/**
 * Index 0 is bit 0 of `dowMask`, which the daemon defines as Monday
 * (`QDate::dayOfWeek()` minus one). Re-indexing this array to Sunday-first waters
 * on the wrong days and breaks nothing that would fail to compile.
 */
export const WEEKDAY_LABELS: readonly string[] = ['Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat', 'Sun']

const MONTH_LABELS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec']
const FULL_WEEK = 0b1111111

export function weekdaysFromMask(mask: number): number[] {
  const days: number[] = []
  for (let index = 0; index < WEEKDAY_LABELS.length; index += 1) {
    if ((mask & (1 << index)) !== 0) {
      days.push(index)
    }
  }
  return days
}

export function maskFromWeekdays(indices: number[]): number {
  return indices.reduce((mask, index) => {
    if (index < 0 || index >= WEEKDAY_LABELS.length) {
      return mask
    }
    return mask | (1 << index)
  }, 0)
}

export function toggleWeekday(mask: number, index: number): number {
  if (index < 0 || index >= WEEKDAY_LABELS.length) {
    return mask
  }
  return mask ^ (1 << index)
}

/** Formats a `YYYY-MM-DD` calendar date. No instant and no timezone are involved. */
function formatAnchor(anchorDate: string | null): string {
  const match = /^(\d{4})-(\d{2})-(\d{2})$/.exec(anchorDate ?? '')
  if (match === null) {
    return 'an unset date'
  }
  return `${Number(match[3])} ${MONTH_LABELS[Number(match[2]) - 1]} ${match[1]}`
}

export function dayRuleSummary(
  program: Pick<Program, 'dayMode' | 'dowMask' | 'intervalDays' | 'anchorDate'>,
): string {
  switch (program.dayMode) {
    case 'Odd':
      return 'Odd days'
    case 'Even':
      return 'Even days'
    case 'EveryNDays': {
      const every = program.intervalDays === 1 ? 'Every day' : `Every ${program.intervalDays} days`
      return `${every} from ${formatAnchor(program.anchorDate)}`
    }
    case 'DaysOfWeek': {
      const masked = program.dowMask & FULL_WEEK
      if (masked === FULL_WEEK) {
        return 'Every day'
      }
      if (masked === 0) {
        return 'No days selected'
      }
      return weekdaysFromMask(masked)
        .map((index) => WEEKDAY_LABELS[index])
        .join(', ')
    }
  }
}

export function totalRuntimeSeconds(zones: { durationSeconds: number }[]): number {
  return zones.reduce((total, zone) => total + zone.durationSeconds, 0)
}

/** Converts a loaded program into the body shape POST and PUT accept. */
export function toDraft(program: Program): ProgramDraft {
  return {
    name: program.name,
    enabled: program.enabled,
    dayMode: program.dayMode,
    dowMask: program.dowMask,
    intervalDays: program.intervalDays,
    anchorDate: program.anchorDate,
    startTimes: program.startTimes.map((start) => ({
      minutesAfterMidnight: start.minutesAfterMidnight,
      timezone: start.timezone,
    })),
    zones: program.zones.map((zone, index) => ({
      zoneId: zone.zoneId,
      sequence: index + 1,
      durationSeconds: zone.durationSeconds,
    })),
  }
}
```

`toDraft` renumbers `sequence` from the array position. `decodePrograms` already sorted by sequence, so this normalises a program whose stored sequences have gaps.

- [ ] **Step 4: Write the failing list tests**

`web/src/screens/ProgramsScreen.test.tsx`:

```tsx
import { render, screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import ProgramsScreen from './ProgramsScreen'
import * as client from '../api/client'
import type { Program } from '../api/types'
import { idleStatus, morningProgram, zoneFixtures } from '../test/fixtures'

const refresh = vi.fn()

const eveningProgram: Program = {
  id: 2,
  name: 'Evening',
  enabled: false,
  dayMode: 'EveryNDays',
  dowMask: 0,
  intervalDays: 3,
  anchorDate: '2026-04-01',
  startTimes: [{ id: 4, minutesAfterMidnight: 1140, timezone: 'America/New_York' }],
  zones: [{ id: 23, zoneId: 8, sequence: 1, durationSeconds: 1200 }],
  nextRunUtc: null,
}

beforeEach(() => {
  vi.spyOn(client, 'getPrograms').mockResolvedValue([morningProgram, eveningProgram])
  vi.spyOn(client, 'getZones').mockResolvedValue(zoneFixtures)
  vi.spyOn(client, 'updateProgram').mockResolvedValue(undefined)
  vi.spyOn(client, 'runProgram').mockResolvedValue(undefined)
})

afterEach(() => {
  vi.restoreAllMocks()
  refresh.mockReset()
})

describe('ProgramsScreen', () => {
  it('lists every program with its day rule', async () => {
    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)

    const morning = await screen.findByTestId('program-1')
    expect(within(morning).getByText('Morning')).toBeInTheDocument()
    expect(within(morning).getByTestId('day-rule')).toHaveTextContent('Mon, Wed')

    const evening = screen.getByTestId('program-2')
    expect(within(evening).getByTestId('day-rule')).toHaveTextContent('Every 3 days from 1 Apr 2026')
  })

  it('renders start times as wall-clock labels', async () => {
    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)
    const morning = await screen.findByTestId('program-1')
    expect(within(morning).getByTestId('start-times')).toHaveTextContent('6:00 AM')
  })

  it('names the zone of a start time that does not match the controller', async () => {
    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)

    const evening = await screen.findByTestId('program-2')
    expect(within(evening).getByTestId('start-times')).toHaveTextContent('7:00 PM')
    expect(within(evening).getByTestId('start-times')).toHaveTextContent('America/New_York')

    const morning = screen.getByTestId('program-1')
    expect(within(morning).getByTestId('start-times')).not.toHaveTextContent('America/Los_Angeles')
  })

  it('shows the zone sequence with each duration and the computed total', async () => {
    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)

    const morning = await screen.findByTestId('program-1')
    const sequence = within(morning).getByTestId('zone-sequence')

    // zoneIds 7 and 9 are zone numbers 1 and 3.
    expect(sequence).toHaveTextContent('1. Front lawn 10 min')
    expect(sequence).toHaveTextContent('2. Roses 5 min')
    expect(within(morning).getByTestId('total-runtime')).toHaveTextContent('15 min')
  })

  it('renders next run in the controller zone, and a placeholder when absent', async () => {
    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)

    expect(await within(screen.getByTestId('program-1')).findByTestId('next-run')).toHaveTextContent('6:00 AM')
    expect(within(screen.getByTestId('program-2')).getByTestId('next-run')).toHaveTextContent('—')
  })

  it('runs a program by its program id', async () => {
    const user = userEvent.setup()
    const runProgram = vi.spyOn(client, 'runProgram').mockResolvedValue(undefined)

    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.click(within(await screen.findByTestId('program-2')).getByRole('button', { name: /run now/i }))

    expect(runProgram).toHaveBeenCalledWith(2)
  })

  it('sends the whole program when the enable toggle flips', async () => {
    const user = userEvent.setup()
    const updateProgram = vi.spyOn(client, 'updateProgram').mockResolvedValue(undefined)

    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.click(within(await screen.findByTestId('program-1')).getByRole('switch'))

    expect(updateProgram).toHaveBeenCalledTimes(1)
    const [id, draft] = updateProgram.mock.calls[0]!
    expect(id).toBe(1)
    expect(draft).toEqual({
      name: 'Morning',
      enabled: false,
      dayMode: 'DaysOfWeek',
      dowMask: 0b0000101,
      intervalDays: 0,
      anchorDate: null,
      startTimes: [{ minutesAfterMidnight: 360, timezone: 'America/Los_Angeles' }],
      zones: [
        { zoneId: 7, sequence: 1, durationSeconds: 600 },
        { zoneId: 9, sequence: 2, durationSeconds: 300 },
      ],
    })
  })

  it('reloads the list after a toggle so a rejected write cannot look applied', async () => {
    const user = userEvent.setup()
    const getPrograms = vi.spyOn(client, 'getPrograms').mockResolvedValue([morningProgram, eveningProgram])

    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await screen.findByTestId('program-1')
    expect(getPrograms).toHaveBeenCalledTimes(1)

    await user.click(within(screen.getByTestId('program-1')).getByRole('switch'))

    await waitFor(() => {
      expect(getPrograms).toHaveBeenCalledTimes(2)
    })
  })

  it('says so when there are no programs', async () => {
    vi.spyOn(client, 'getPrograms').mockResolvedValue([])
    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)
    expect(await screen.findByText(/no programs yet/i)).toBeInTheDocument()
  })
})
```

`sends the whole program when the enable toggle flips` is the one that matters. `PUT /admin/programs/{id}` replaces the program and its nested rows, so a handler that sends `{ enabled: false }` on its own wipes the name, the day rule, every start time and every zone. The assertion is a deep equality against the full draft — drop one field from the body and it fails naming that field.

`renders start times as wall-clock labels` fails for any implementation that routes `minutesAfterMidnight` through a `Date`, because 360 minutes formatted as an instant in Los Angeles is not 6:00 AM.

`names the zone of a start time that does not match the controller` covers the `program_start_times.timezone` column, which is per start time rather than per controller. A screen that renders every start time in `status.timezone` shows 7:00 PM for a New York rule that fires at 4:00 PM local.

- [ ] **Step 5: Write the Programs screen**

`web/src/screens/ProgramsScreen.tsx`:

```tsx
import { useCallback, useEffect, useState } from 'react'
import { getPrograms, getZones, runProgram, updateProgram } from '../api/client'
import type { Program, Zone } from '../api/types'
import { dayRuleSummary, toDraft, totalRuntimeSeconds } from '../programs/dayRule'
import { formatDayAndClock, formatDuration, minutesToClock } from '../time/zonedformat'
import type { ScreenProps } from './NowScreen'

export default function ProgramsScreen({ status }: ScreenProps) {
  const [programs, setPrograms] = useState<Program[] | null>(null)
  const [zones, setZones] = useState<Zone[]>([])
  const [error, setError] = useState<string | null>(null)

  const load = useCallback(async () => {
    try {
      const [loadedPrograms, loadedZones] = await Promise.all([getPrograms(), getZones()])
      setPrograms(loadedPrograms)
      setZones(loadedZones)
      setError(null)
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught))
    }
  }, [])

  useEffect(() => {
    void load()
  }, [load])

  const zoneName = useCallback(
    (zoneId: number) => zones.find((zone) => zone.id === zoneId)?.name ?? `Zone id ${zoneId}`,
    [zones],
  )

  const onToggle = useCallback(
    async (program: Program) => {
      try {
        await updateProgram(program.id, { ...toDraft(program), enabled: program.enabled === false })
      } catch (caught: unknown) {
        setError(caught instanceof Error ? caught.message : String(caught))
      }
      await load()
    },
    [load],
  )

  const onRun = useCallback(async (program: Program) => {
    try {
      await runProgram(program.id)
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught))
    }
  }, [])

  const controllerZone = status?.timezone ?? ''

  return (
    <section className="screen">
      <h1>Programs</h1>

      {error === null ? null : (
        <div className="alert" role="alert">
          {error}
        </div>
      )}

      {programs !== null && programs.length === 0 ? <p>No programs yet.</p> : null}

      {(programs ?? []).map((program) => (
        <article key={program.id} className="program" data-testid={`program-${program.id}`}>
          <header className="program__header">
            <h2>{program.name}</h2>
            <button
              type="button"
              role="switch"
              aria-checked={program.enabled}
              aria-label={`${program.name} enabled`}
              onClick={() => {
                void onToggle(program)
              }}
            >
              {program.enabled ? 'Enabled' : 'Disabled'}
            </button>
          </header>

          <div data-testid="day-rule">{dayRuleSummary(program)}</div>

          <ul data-testid="start-times">
            {program.startTimes.map((start) => (
              <li key={start.id}>
                {minutesToClock(start.minutesAfterMidnight)}
                {start.timezone === controllerZone ? '' : ` (${start.timezone})`}
              </li>
            ))}
          </ul>

          <ol data-testid="zone-sequence">
            {program.zones.map((zone, index) => (
              <li key={zone.id}>
                {index + 1}. {zoneName(zone.zoneId)} {formatDuration(zone.durationSeconds)}
              </li>
            ))}
          </ol>

          <div data-testid="total-runtime">
            Total {formatDuration(totalRuntimeSeconds(program.zones))}
          </div>

          <div data-testid="next-run">
            Next run:{' '}
            {program.nextRunUtc === null ? '—' : formatDayAndClock(program.nextRunUtc, controllerZone)}
          </div>

          <button
            type="button"
            onClick={() => {
              void onRun(program)
            }}
          >
            Run now
          </button>
        </article>
      ))}
    </section>
  )
}
```

The `<ol>` renders its own index rather than relying on list-item numbering, so the assertion reads the sequence position out of the text.

- [ ] **Step 6: Add the styles**

Append to `web/src/styles/app.css`:

```css
.program {
  display: flex;
  flex-direction: column;
  gap: 8px;
  padding: var(--gap);
  border: 1px solid var(--line);
  border-radius: var(--radius);
  background: var(--surface-raised);
}

.program__header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--gap);
}

.program__header h2 {
  margin: 0;
  font-size: 1.2rem;
}

.program ul,
.program ol {
  margin: 0;
  padding: 0;
  list-style: none;
  color: var(--ink-dim);
}

.program [role='switch'][aria-checked='false'] {
  color: var(--ink-dim);
}
```

- [ ] **Step 7: Run the suite and the type check**

```bash
cd web && npm test && npm run typecheck
```

Expected: every day-rule and list case passes.

- [ ] **Step 8: Prove the Monday-first assertion can fail**

Reverse `WEEKDAY_LABELS` to `['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat']`, run `npm test -- dayRule`, and confirm `puts Monday at bit 0 and Sunday at bit 6` fails. Revert.

- [ ] **Step 9: Commit**

```bash
cd /home/spunak/src/punak/irrigation
git add web
git commit -m "feat: add the Programs list with day rules and computed totals"
```

---

### Task 8: Program editor — create, edit, delete

The editor is a state of the Programs screen rather than a route, so a half-finished program cannot be reached by a bookmark and abandoned by a reload.

Two identifiers cross this screen in opposite directions and both are addressed here: the zone picker stores `zone.id` into `zones[].zoneId` because `program_zones.zone_id` is a foreign key, while the Now screen's manual run addresses `zone.number`. The fixtures keep them distinct so a swap cannot pass.

Delete asks for confirmation. That differs from the stop control because a deleted program cannot be recovered and a stopped valve can be restarted.

**Files:**
- Create: `web/src/screens/ProgramEditor.tsx`
- Modify: `web/src/screens/ProgramsScreen.tsx`
- Modify: `web/src/styles/app.css`
- Test: `web/src/screens/ProgramEditor.test.tsx`

**Interfaces:**
- Consumes: `createProgram`, `updateProgram`, `deleteProgram`, `getZones` from Task 2; `minutesToInputValue`, `inputValueToMinutes` from Task 3; `dayRule` helpers from Task 7
- Produces:
  - `ProgramEditor` props: `{ program: Program | null; zones: Zone[]; controllerZone: string; onDone: () => void; onCancel: () => void }`
  - `emptyDraft(controllerZone: string): ProgramDraft`
  - `validationError(draft: ProgramDraft): string | null`

- [ ] **Step 1: Write the failing editor tests**

`web/src/screens/ProgramEditor.test.tsx`:

```tsx
import { render, screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import ProgramEditor, { emptyDraft, validationError } from './ProgramEditor'
import * as client from '../api/client'
import { morningProgram, zoneFixtures } from '../test/fixtures'

const onDone = vi.fn()
const onCancel = vi.fn()

const LA = 'America/Los_Angeles'

function renderEditor(program: Parameters<typeof ProgramEditor>[0]['program']) {
  return render(
    <ProgramEditor
      program={program}
      zones={zoneFixtures}
      controllerZone={LA}
      onDone={onDone}
      onCancel={onCancel}
    />,
  )
}

beforeEach(() => {
  vi.spyOn(client, 'createProgram').mockResolvedValue(undefined)
  vi.spyOn(client, 'updateProgram').mockResolvedValue(undefined)
  vi.spyOn(client, 'deleteProgram').mockResolvedValue(undefined)
  vi.stubGlobal('confirm', vi.fn(() => true))
})

afterEach(() => {
  vi.restoreAllMocks()
  vi.unstubAllGlobals()
  onDone.mockReset()
  onCancel.mockReset()
})

describe('emptyDraft', () => {
  it('defaults a new program to the controller timezone', () => {
    const draft = emptyDraft(LA)
    expect(draft.startTimes).toEqual([{ minutesAfterMidnight: 360, timezone: LA }])
    expect(draft.dayMode).toBe('DaysOfWeek')
    expect(draft.enabled).toBe(true)
  })
})

describe('validationError', () => {
  const base = emptyDraft(LA)

  it('requires a name', () => {
    expect(validationError({ ...base, name: '', dowMask: 1, zones: [{ zoneId: 7, sequence: 1, durationSeconds: 60 }] })).toMatch(/name/i)
  })

  it('requires at least one zone', () => {
    expect(validationError({ ...base, name: 'X', dowMask: 1, zones: [] })).toMatch(/zone/i)
  })

  it('requires at least one start time', () => {
    expect(
      validationError({ ...base, name: 'X', dowMask: 1, startTimes: [], zones: [{ zoneId: 7, sequence: 1, durationSeconds: 60 }] }),
    ).toMatch(/start time/i)
  })

  it('requires at least one weekday in DaysOfWeek mode', () => {
    expect(
      validationError({ ...base, name: 'X', dowMask: 0, zones: [{ zoneId: 7, sequence: 1, durationSeconds: 60 }] }),
    ).toMatch(/day/i)
  })

  it('requires an interval and an anchor in EveryNDays mode', () => {
    const everyN = { ...base, name: 'X', dayMode: 'EveryNDays' as const, zones: [{ zoneId: 7, sequence: 1, durationSeconds: 60 }] }
    expect(validationError({ ...everyN, intervalDays: 0, anchorDate: '2026-04-01' })).toMatch(/interval/i)
    expect(validationError({ ...everyN, intervalDays: 3, anchorDate: null })).toMatch(/date/i)
    expect(validationError({ ...everyN, intervalDays: 3, anchorDate: '2026-04-01' })).toBeNull()
  })

  it('rejects a zero-length zone run', () => {
    expect(
      validationError({ ...base, name: 'X', dowMask: 1, zones: [{ zoneId: 7, sequence: 1, durationSeconds: 0 }] }),
    ).toMatch(/duration/i)
  })
})

describe('creating a program', () => {
  it('posts the draft the form describes', async () => {
    const user = userEvent.setup()
    const createProgram = vi.spyOn(client, 'createProgram').mockResolvedValue(undefined)

    renderEditor(null)

    await user.type(screen.getByLabelText(/program name/i), 'Evening')
    await user.click(screen.getByRole('button', { name: 'Mon' }))
    await user.click(screen.getByRole('button', { name: 'Wed' }))
    await user.clear(screen.getByLabelText(/start time 1/i))
    await user.type(screen.getByLabelText(/start time 1/i), '19:00')

    await user.click(screen.getByRole('button', { name: /add zone/i }))
    await user.selectOptions(screen.getByLabelText(/zone 1 valve/i), '9')
    await user.clear(screen.getByLabelText(/zone 1 minutes/i))
    await user.type(screen.getByLabelText(/zone 1 minutes/i), '5')

    await user.click(screen.getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(createProgram).toHaveBeenCalledTimes(1)
    })
    expect(createProgram.mock.calls[0]![0]).toEqual({
      name: 'Evening',
      enabled: true,
      dayMode: 'DaysOfWeek',
      dowMask: 0b0000101,
      intervalDays: 0,
      anchorDate: null,
      startTimes: [{ minutesAfterMidnight: 1140, timezone: LA }],
      zones: [{ zoneId: 9, sequence: 1, durationSeconds: 300 }],
    })
    expect(onDone).toHaveBeenCalled()
  })

  it('stores the zone database id, not the zone number', async () => {
    const user = userEvent.setup()
    const createProgram = vi.spyOn(client, 'createProgram').mockResolvedValue(undefined)

    renderEditor(null)
    await user.type(screen.getByLabelText(/program name/i), 'Evening')
    await user.click(screen.getByRole('button', { name: 'Mon' }))
    await user.click(screen.getByRole('button', { name: /add zone/i }))

    // Option values are zone ids; the label the user reads carries the number.
    const picker = screen.getByLabelText(/zone 1 valve/i)
    expect(within(picker).getByRole('option', { name: /3 · Roses/ })).toHaveValue('9')

    await user.selectOptions(picker, '9')
    await user.click(screen.getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(createProgram).toHaveBeenCalled()
    })
    expect(createProgram.mock.calls[0]![0].zones[0]!.zoneId).toBe(9)
  })

  it('accepts the last minute of the day', async () => {
    const user = userEvent.setup()
    const createProgram = vi.spyOn(client, 'createProgram').mockResolvedValue(undefined)

    renderEditor(null)
    await user.type(screen.getByLabelText(/program name/i), 'Late')
    await user.click(screen.getByRole('button', { name: 'Mon' }))
    await user.clear(screen.getByLabelText(/start time 1/i))
    await user.type(screen.getByLabelText(/start time 1/i), '23:59')
    await user.click(screen.getByRole('button', { name: /add zone/i }))
    await user.selectOptions(screen.getByLabelText(/zone 1 valve/i), '7')
    await user.click(screen.getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(createProgram).toHaveBeenCalled()
    })
    expect(createProgram.mock.calls[0]![0].startTimes[0]!.minutesAfterMidnight).toBe(1439)
  })

  it('refuses to save an invalid draft and says why', async () => {
    const user = userEvent.setup()
    const createProgram = vi.spyOn(client, 'createProgram').mockResolvedValue(undefined)

    renderEditor(null)
    await user.click(screen.getByRole('button', { name: /save/i }))

    expect(await screen.findByRole('alert')).toHaveTextContent(/name/i)
    expect(createProgram).not.toHaveBeenCalled()
  })
})

describe('editing a program', () => {
  it('loads every field of the existing program', async () => {
    renderEditor(morningProgram)

    expect(screen.getByLabelText(/program name/i)).toHaveValue('Morning')
    expect(screen.getByRole('button', { name: 'Mon' })).toHaveAttribute('aria-pressed', 'true')
    expect(screen.getByRole('button', { name: 'Wed' })).toHaveAttribute('aria-pressed', 'true')
    expect(screen.getByRole('button', { name: 'Tue' })).toHaveAttribute('aria-pressed', 'false')
    expect(screen.getByLabelText(/start time 1/i)).toHaveValue('06:00')
    expect(screen.getByLabelText(/zone 1 valve/i)).toHaveValue('7')
    expect(screen.getByLabelText(/zone 1 minutes/i)).toHaveValue(10)
    expect(screen.getByLabelText(/zone 2 valve/i)).toHaveValue('9')
    expect(screen.getByLabelText(/zone 2 minutes/i)).toHaveValue(5)
  })

  it('puts the full draft under the program id', async () => {
    const user = userEvent.setup()
    const updateProgram = vi.spyOn(client, 'updateProgram').mockResolvedValue(undefined)

    renderEditor(morningProgram)
    await user.clear(screen.getByLabelText(/program name/i))
    await user.type(screen.getByLabelText(/program name/i), 'Morning revised')
    await user.click(screen.getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(updateProgram).toHaveBeenCalledTimes(1)
    })
    const [id, draft] = updateProgram.mock.calls[0]!
    expect(id).toBe(1)
    expect(draft.name).toBe('Morning revised')
    expect(draft.zones).toEqual([
      { zoneId: 7, sequence: 1, durationSeconds: 600 },
      { zoneId: 9, sequence: 2, durationSeconds: 300 },
    ])
  })

  it('renumbers sequences when a zone moves up', async () => {
    const user = userEvent.setup()
    const updateProgram = vi.spyOn(client, 'updateProgram').mockResolvedValue(undefined)

    renderEditor(morningProgram)
    await user.click(screen.getByRole('button', { name: /move zone 2 up/i }))
    await user.click(screen.getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(updateProgram).toHaveBeenCalled()
    })
    expect(updateProgram.mock.calls[0]![1].zones).toEqual([
      { zoneId: 9, sequence: 1, durationSeconds: 300 },
      { zoneId: 7, sequence: 2, durationSeconds: 600 },
    ])
  })

  it('renumbers sequences when a zone is removed from the middle', async () => {
    const user = userEvent.setup()
    const updateProgram = vi.spyOn(client, 'updateProgram').mockResolvedValue(undefined)

    renderEditor(morningProgram)
    await user.click(screen.getByRole('button', { name: /add zone/i }))
    await user.selectOptions(screen.getByLabelText(/zone 3 valve/i), '11')
    await user.click(screen.getByRole('button', { name: /remove zone 2/i }))
    await user.click(screen.getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(updateProgram).toHaveBeenCalled()
    })
    expect(updateProgram.mock.calls[0]![1].zones.map((zone) => [zone.zoneId, zone.sequence])).toEqual([
      [7, 1],
      [11, 2],
    ])
  })

  it('defaults an added start time to the controller timezone', async () => {
    const user = userEvent.setup()
    const updateProgram = vi.spyOn(client, 'updateProgram').mockResolvedValue(undefined)

    renderEditor(morningProgram)
    await user.click(screen.getByRole('button', { name: /add start time/i }))
    await user.click(screen.getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(updateProgram).toHaveBeenCalled()
    })
    const draft = updateProgram.mock.calls[0]![1]
    expect(draft.startTimes).toHaveLength(2)
    expect(draft.startTimes[1]!.timezone).toBe(LA)
  })
})

describe('deleting a program', () => {
  it('asks first and then deletes', async () => {
    const user = userEvent.setup()
    const deleteProgram = vi.spyOn(client, 'deleteProgram').mockResolvedValue(undefined)

    renderEditor(morningProgram)
    await user.click(screen.getByRole('button', { name: /delete/i }))

    expect(globalThis.confirm).toHaveBeenCalled()
    await waitFor(() => {
      expect(deleteProgram).toHaveBeenCalledWith(1)
    })
    expect(onDone).toHaveBeenCalled()
  })

  it('does nothing when the confirmation is declined', async () => {
    const user = userEvent.setup()
    vi.stubGlobal('confirm', vi.fn(() => false))
    const deleteProgram = vi.spyOn(client, 'deleteProgram').mockResolvedValue(undefined)

    renderEditor(morningProgram)
    await user.click(screen.getByRole('button', { name: /delete/i }))

    expect(deleteProgram).not.toHaveBeenCalled()
    expect(onDone).not.toHaveBeenCalled()
  })

  it('offers no delete for a program that does not exist yet', () => {
    renderEditor(null)
    expect(screen.queryByRole('button', { name: /delete/i })).toBeNull()
  })
})
```

What each one kills:

- `stores the zone database id, not the zone number` reads the option's value and the option's visible label separately. The label shows `3 · Roses`, the value is `9`; an implementation that sets `value={zone.number}` renders an option labelled `3 · Roses` with value `3`, the assertion fails on the value, and the daemon would otherwise have inserted a foreign key pointing at a different valve.
- `renumbers sequences when a zone moves up` and `when a zone is removed from the middle` both assert the sequence numbers, not just the order. Reordering the array while keeping the stored sequences produces a program that renders in the new order in the browser and waters in the old order on the controller, because `zonesFor()` sorts by `sequence`.
- `defaults an added start time to the controller timezone` runs with the host at `TZ=UTC` and the controller at `America/Los_Angeles`. Defaulting to `Intl.DateTimeFormat().resolvedOptions().timeZone` writes `UTC` into the row and the program fires seven hours early forever.
- `accepts the last minute of the day` pins the 1439 boundary, the value an off-by-one in the minute conversion lands on.
- `does nothing when the confirmation is declined` fails for a handler that fires the delete and asks afterwards.

- [ ] **Step 2: Run it and verify it fails**

```bash
cd web && npm test -- ProgramEditor
```

Expected: failure — `./ProgramEditor` does not exist.

- [ ] **Step 3: Write the editor**

`web/src/screens/ProgramEditor.tsx`:

```tsx
import { useCallback, useState } from 'react'
import { createProgram, deleteProgram, updateProgram } from '../api/client'
import { DAY_MODES, type DayMode, type Program, type ProgramDraft, type Zone } from '../api/types'
import { WEEKDAY_LABELS, toDraft, toggleWeekday } from '../programs/dayRule'
import { inputValueToMinutes, minutesToInputValue } from '../time/zonedformat'

const DAY_MODE_LABELS: Record<DayMode, string> = {
  DaysOfWeek: 'Days of week',
  Odd: 'Odd days',
  Even: 'Even days',
  EveryNDays: 'Every N days',
}

export function emptyDraft(controllerZone: string): ProgramDraft {
  return {
    name: '',
    enabled: true,
    dayMode: 'DaysOfWeek',
    dowMask: 0,
    intervalDays: 0,
    anchorDate: null,
    startTimes: [{ minutesAfterMidnight: 360, timezone: controllerZone }],
    zones: [],
  }
}

export function validationError(draft: ProgramDraft): string | null {
  if (draft.name.trim().length === 0) {
    return 'Give the program a name.'
  }
  if (draft.startTimes.length === 0) {
    return 'Add at least one start time.'
  }
  if (draft.zones.length === 0) {
    return 'Add at least one zone.'
  }
  if (draft.zones.some((zone) => zone.durationSeconds < 1)) {
    return 'Every zone needs a duration of at least one minute.'
  }
  if (draft.dayMode === 'DaysOfWeek' && (draft.dowMask & 0b1111111) === 0) {
    return 'Select at least one day of the week.'
  }
  if (draft.dayMode === 'EveryNDays') {
    if (draft.intervalDays < 1) {
      return 'Set an interval of at least one day.'
    }
    if (draft.anchorDate === null || draft.anchorDate.length === 0) {
      return 'Set a start date for the interval.'
    }
  }
  return null
}

/** Rewrites `sequence` from array position so a reorder reaches the daemon. */
function resequence(zones: ProgramDraft['zones']): ProgramDraft['zones'] {
  return zones.map((zone, index) => ({ ...zone, sequence: index + 1 }))
}

export interface ProgramEditorProps {
  program: Program | null
  zones: Zone[]
  controllerZone: string
  onDone: () => void
  onCancel: () => void
}

export default function ProgramEditor({
  program,
  zones,
  controllerZone,
  onDone,
  onCancel,
}: ProgramEditorProps) {
  const [draft, setDraft] = useState<ProgramDraft>(() =>
    program === null ? emptyDraft(controllerZone) : toDraft(program),
  )
  const [error, setError] = useState<string | null>(null)
  const [saving, setSaving] = useState(false)

  const patch = useCallback((changes: Partial<ProgramDraft>) => {
    setDraft((current) => ({ ...current, ...changes }))
  }, [])

  const onSave = useCallback(async () => {
    const normalised: ProgramDraft = { ...draft, name: draft.name.trim(), zones: resequence(draft.zones) }
    const invalid = validationError(normalised)
    if (invalid !== null) {
      setError(invalid)
      return
    }

    setSaving(true)
    try {
      if (program === null) {
        await createProgram(normalised)
      } else {
        await updateProgram(program.id, normalised)
      }
      onDone()
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught))
    } finally {
      setSaving(false)
    }
  }, [draft, program, onDone])

  const onDelete = useCallback(async () => {
    if (program === null) {
      return
    }
    if (globalThis.confirm(`Delete "${program.name}"? This cannot be undone.`) === false) {
      return
    }
    try {
      await deleteProgram(program.id)
      onDone()
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught))
    }
  }, [program, onDone])

  const moveZone = useCallback((index: number, delta: number) => {
    setDraft((current) => {
      const target = index + delta
      if (target < 0 || target >= current.zones.length) {
        return current
      }
      const reordered = [...current.zones]
      const [moved] = reordered.splice(index, 1)
      reordered.splice(target, 0, moved!)
      return { ...current, zones: resequence(reordered) }
    })
  }, [])

  return (
    <section className="screen editor">
      <h2>{program === null ? 'New program' : `Edit ${program.name}`}</h2>

      {error === null ? null : (
        <div className="alert" role="alert">
          {error}
        </div>
      )}

      <label>
        Program name
        <input
          type="text"
          value={draft.name}
          onChange={(event) => {
            patch({ name: event.target.value })
          }}
        />
      </label>

      <label>
        Enabled
        <input
          type="checkbox"
          checked={draft.enabled}
          onChange={(event) => {
            patch({ enabled: event.target.checked })
          }}
        />
      </label>

      <label>
        Day rule
        <select
          value={draft.dayMode}
          onChange={(event) => {
            patch({ dayMode: event.target.value as DayMode })
          }}
        >
          {DAY_MODES.map((mode) => (
            <option key={mode} value={mode}>
              {DAY_MODE_LABELS[mode]}
            </option>
          ))}
        </select>
      </label>

      {draft.dayMode === 'DaysOfWeek' ? (
        <div className="weekdays">
          {WEEKDAY_LABELS.map((label, index) => (
            <button
              key={label}
              type="button"
              aria-pressed={(draft.dowMask & (1 << index)) !== 0}
              onClick={() => {
                patch({ dowMask: toggleWeekday(draft.dowMask, index) })
              }}
            >
              {label}
            </button>
          ))}
        </div>
      ) : null}

      {draft.dayMode === 'EveryNDays' ? (
        <>
          <label>
            Interval in days
            <input
              type="number"
              min={1}
              value={draft.intervalDays}
              onChange={(event) => {
                patch({ intervalDays: Number(event.target.value) })
              }}
            />
          </label>
          <label>
            Starting on
            <input
              type="date"
              value={draft.anchorDate ?? ''}
              onChange={(event) => {
                patch({ anchorDate: event.target.value === '' ? null : event.target.value })
              }}
            />
          </label>
        </>
      ) : null}

      <h3>Start times</h3>
      {draft.startTimes.map((start, index) => (
        <div key={index} className="row">
          <label>
            {`Start time ${index + 1}`}
            <input
              type="time"
              value={minutesToInputValue(start.minutesAfterMidnight)}
              onChange={(event) => {
                const minutes = inputValueToMinutes(event.target.value)
                if (minutes < 0) {
                  return
                }
                patch({
                  startTimes: draft.startTimes.map((entry, i) =>
                    i === index ? { ...entry, minutesAfterMidnight: minutes } : entry,
                  ),
                })
              }}
            />
          </label>
          <span className="row__zone">{start.timezone}</span>
          <button
            type="button"
            onClick={() => {
              patch({ startTimes: draft.startTimes.filter((_, i) => i !== index) })
            }}
          >
            {`Remove start time ${index + 1}`}
          </button>
        </div>
      ))}
      <button
        type="button"
        onClick={() => {
          patch({
            startTimes: [...draft.startTimes, { minutesAfterMidnight: 360, timezone: controllerZone }],
          })
        }}
      >
        Add start time
      </button>

      <h3>Zones in run order</h3>
      {draft.zones.map((zone, index) => (
        <div key={index} className="row">
          <label>
            {`Zone ${index + 1} valve`}
            <select
              value={String(zone.zoneId)}
              onChange={(event) => {
                patch({
                  zones: draft.zones.map((entry, i) =>
                    i === index ? { ...entry, zoneId: Number(event.target.value) } : entry,
                  ),
                })
              }}
            >
              {zones.map((candidate) => (
                <option key={candidate.id} value={String(candidate.id)}>
                  {`${candidate.number} · ${candidate.name}`}
                </option>
              ))}
            </select>
          </label>
          <label>
            {`Zone ${index + 1} minutes`}
            <input
              type="number"
              min={1}
              value={Math.round(zone.durationSeconds / 60)}
              onChange={(event) => {
                patch({
                  zones: draft.zones.map((entry, i) =>
                    i === index ? { ...entry, durationSeconds: Number(event.target.value) * 60 } : entry,
                  ),
                })
              }}
            />
          </label>
          <button
            type="button"
            onClick={() => {
              moveZone(index, -1)
            }}
          >
            {`Move zone ${index + 1} up`}
          </button>
          <button
            type="button"
            onClick={() => {
              moveZone(index, 1)
            }}
          >
            {`Move zone ${index + 1} down`}
          </button>
          <button
            type="button"
            onClick={() => {
              patch({ zones: resequence(draft.zones.filter((_, i) => i !== index)) })
            }}
          >
            {`Remove zone ${index + 1}`}
          </button>
        </div>
      ))}
      <button
        type="button"
        onClick={() => {
          const first = zones[0]
          if (first === undefined) {
            return
          }
          patch({
            zones: resequence([
              ...draft.zones,
              { zoneId: first.id, sequence: draft.zones.length + 1, durationSeconds: 600 },
            ]),
          })
        }}
      >
        Add zone
      </button>

      <div className="row">
        <button
          type="button"
          disabled={saving}
          onClick={() => {
            void onSave()
          }}
        >
          Save
        </button>
        <button type="button" onClick={onCancel}>
          Cancel
        </button>
        {program === null ? null : (
          <button
            type="button"
            className="destructive"
            onClick={() => {
              void onDelete()
            }}
          >
            Delete
          </button>
        )}
      </div>
    </section>
  )
}
```

The zone picker's `value` is the zone's database id and its label leads with the zone number, so the person choosing sees the valve and the row stores the foreign key.

- [ ] **Step 4: Wire the editor into the Programs screen**

Four changes to `ProgramsScreen.tsx`.

Add the import:

```tsx
import ProgramEditor from './ProgramEditor'
```

Add the editing state alongside the existing `useState` calls. `undefined` means the list is showing, `null` means the editor is open on a new program, and a `Program` means it is open on that one:

```tsx
const [editing, setEditing] = useState<Program | null | undefined>(undefined)
```

Return the editor before the list, placed after `controllerZone` is computed and before the `return (` that renders the list:

```tsx
if (editing !== undefined) {
  return (
    <ProgramEditor
      program={editing}
      zones={zones}
      controllerZone={controllerZone}
      onDone={() => {
        setEditing(undefined)
        void load()
      }}
      onCancel={() => {
        setEditing(undefined)
      }}
    />
  )
}
```

Add the two entry points. A `New program` button directly under the `<h1>`:

```tsx
<button
  type="button"
  onClick={() => {
    setEditing(null)
  }}
>
  New program
</button>
```

And an `Edit` button in each program card, next to `Run now`:

```tsx
<button
  type="button"
  onClick={() => {
    setEditing(program)
  }}
>
  Edit
</button>
```

Add to `ProgramsScreen.test.tsx`:

```tsx
it('opens the editor on a new program and returns to the list', async () => {
  const user = userEvent.setup()
  render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)
  await screen.findByTestId('program-1')

  await user.click(screen.getByRole('button', { name: /new program/i }))
  expect(await screen.findByRole('heading', { name: /new program/i })).toBeInTheDocument()

  await user.click(screen.getByRole('button', { name: /cancel/i }))
  expect(await screen.findByTestId('program-1')).toBeInTheDocument()
})

it('reloads the list after the editor saves', async () => {
  const user = userEvent.setup()
  const getPrograms = vi.spyOn(client, 'getPrograms').mockResolvedValue([morningProgram])
  vi.spyOn(client, 'updateProgram').mockResolvedValue(undefined)

  render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)
  await user.click(within(await screen.findByTestId('program-1')).getByRole('button', { name: /^edit$/i }))
  await user.click(await screen.findByRole('button', { name: /save/i }))

  await waitFor(() => {
    expect(getPrograms).toHaveBeenCalledTimes(2)
  })
})
```

- [ ] **Step 5: Add the styles**

Append to `web/src/styles/app.css`:

```css
.editor label {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: var(--gap);
}

.editor input[type='text'],
.editor input[type='time'],
.editor input[type='date'],
.editor input[type='number'] {
  flex: 1;
  max-width: 220px;
}

.weekdays {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
}

.weekdays button[aria-pressed='true'] {
  background: var(--accent);
  color: #fff;
  border-color: var(--accent);
}

.row {
  display: flex;
  flex-wrap: wrap;
  align-items: center;
  gap: 6px;
}

.row__zone {
  font-size: 0.8rem;
  color: var(--ink-dim);
}

.destructive {
  background: var(--danger);
  color: var(--danger-ink);
  border-color: var(--danger);
}
```

- [ ] **Step 6: Run the suite and the type check**

```bash
cd web && npm test && npm run typecheck
```

Expected: every editor case passes.

- [ ] **Step 7: Prove the two identifier assertions can fail**

Change the zone picker's `value` to `String(candidate.id)` → `String(candidate.number)` and confirm `stores the zone database id, not the zone number` fails. Revert.

Delete the `resequence(...)` call from `moveZone` and confirm `renumbers sequences when a zone moves up` fails on the sequence numbers while the order assertion would still have passed. Revert.

- [ ] **Step 8: Commit**

```bash
cd /home/spunak/src/punak/irrigation
git add web
git commit -m "feat: add the program editor"
```

---

### Task 9: Settings screen — rain delay, master enable, run ceiling, zone names

Spec §8: rain delay, master enable, maximum zone runtime, zone names.

The `settings` table is key/value TEXT, so every value crosses the wire as a string. `Boolean('false')` is `true` in JavaScript, and a master-enable switch built on that reads a disabled controller as enabled.

The daemon's `settingValue()` returns an empty `QString` both for an absent key and for a key holding an empty value, so `''` and missing are the same thing on both sides and `''` is how the rain delay is cleared.

**Files:**
- Create: `web/src/settings/settingsMap.ts`
- Modify: `web/src/screens/SettingsScreen.tsx`
- Test: `web/src/settings/settingsMap.test.ts`, `web/src/screens/SettingsScreen.test.tsx`

**Interfaces:**
- Consumes: `getSettings`, `putSettings`, `getZones`, `putZone` from Task 2; `formatDayAndClock` from Task 3
- Produces:
  - `const SETTING_KEYS = { rainDelayUntil: 'rain_delay_until', masterEnabled: 'master_enabled', maxZoneSeconds: 'max_zone_seconds', logLevel: 'log_level' } as const`
  - `const DEFAULT_MAX_ZONE_SECONDS = 1800`
  - `parseBoolean(value: string | undefined, fallback: boolean): boolean`
  - `parseInteger(value: string | undefined, fallback: number): number`
  - `parseInstant(value: string | undefined): string | null`
  - `serializeBoolean(value: boolean): string`

- [ ] **Step 1: Write the failing settings-map tests**

`web/src/settings/settingsMap.test.ts`:

```ts
import { describe, expect, it } from 'vitest'
import {
  DEFAULT_MAX_ZONE_SECONDS,
  SETTING_KEYS,
  parseBoolean,
  parseInstant,
  parseInteger,
  serializeBoolean,
} from './settingsMap'

describe('parseBoolean', () => {
  it('reads the string "false" as false', () => {
    expect(parseBoolean('false', true)).toBe(false)
    expect(parseBoolean('False', true)).toBe(false)
    expect(parseBoolean('0', true)).toBe(false)
  })

  it('reads the string "true" as true', () => {
    expect(parseBoolean('true', false)).toBe(true)
    expect(parseBoolean('True', false)).toBe(true)
    expect(parseBoolean('1', false)).toBe(true)
  })

  it('falls back for an absent or empty value', () => {
    expect(parseBoolean(undefined, true)).toBe(true)
    expect(parseBoolean('', true)).toBe(true)
    expect(parseBoolean(undefined, false)).toBe(false)
  })

  it('falls back for anything it does not recognise', () => {
    expect(parseBoolean('maybe', false)).toBe(false)
    expect(parseBoolean('maybe', true)).toBe(true)
  })

  it('round-trips through serializeBoolean', () => {
    expect(parseBoolean(serializeBoolean(false), true)).toBe(false)
    expect(parseBoolean(serializeBoolean(true), false)).toBe(true)
  })
})

describe('parseInteger', () => {
  it('reads a decimal string', () => {
    expect(parseInteger('1800', 60)).toBe(1800)
    expect(parseInteger('0', 60)).toBe(0)
  })

  it('falls back for an absent, empty or unparseable value', () => {
    expect(parseInteger(undefined, DEFAULT_MAX_ZONE_SECONDS)).toBe(1800)
    expect(parseInteger('', DEFAULT_MAX_ZONE_SECONDS)).toBe(1800)
    expect(parseInteger('half an hour', DEFAULT_MAX_ZONE_SECONDS)).toBe(1800)
  })

  it('rejects a partially numeric string rather than truncating it', () => {
    expect(parseInteger('1800s', 60)).toBe(60)
    expect(parseInteger('18 00', 60)).toBe(60)
  })
})

describe('parseInstant', () => {
  it('returns the string when it is a usable timestamp', () => {
    expect(parseInstant('2026-09-15T00:00:00Z')).toBe('2026-09-15T00:00:00Z')
  })

  it('treats absent and empty as unset', () => {
    expect(parseInstant(undefined)).toBeNull()
    expect(parseInstant('')).toBeNull()
  })

  it('treats an unparseable timestamp as unset', () => {
    expect(parseInstant('not a date')).toBeNull()
  })
})

describe('SETTING_KEYS', () => {
  it('matches the schema column names', () => {
    expect(SETTING_KEYS.rainDelayUntil).toBe('rain_delay_until')
    expect(SETTING_KEYS.masterEnabled).toBe('master_enabled')
    expect(SETTING_KEYS.maxZoneSeconds).toBe('max_zone_seconds')
    expect(SETTING_KEYS.logLevel).toBe('log_level')
  })
})
```

`reads the string "false" as false` is the whole point. `Boolean('false')` and `!!'false'` and `JSON.parse` on a bare `false` string each go a different wrong way; only an explicit match against the known spellings gets it right.

`rejects a partially numeric string rather than truncating it` catches `parseInt('1800s')`, which returns 1800 and looks correct until the day the value is `'30m'` and the ceiling silently becomes 30 seconds.

- [ ] **Step 2: Run it and verify it fails**

```bash
cd web && npm test -- settingsMap
```

Expected: failure — `./settingsMap` does not exist.

- [ ] **Step 3: Write the settings map**

`web/src/settings/settingsMap.ts`:

```ts
export const SETTING_KEYS = {
  rainDelayUntil: 'rain_delay_until',
  masterEnabled: 'master_enabled',
  maxZoneSeconds: 'max_zone_seconds',
  logLevel: 'log_level',
} as const

export const DEFAULT_MAX_ZONE_SECONDS = 1800

const TRUE_SPELLINGS = ['true', '1', 'yes', 'on']
const FALSE_SPELLINGS = ['false', '0', 'no', 'off']

/** Every settings value is TEXT. `Boolean('false')` is true, so match explicitly. */
export function parseBoolean(value: string | undefined, fallback: boolean): boolean {
  const normalised = (value ?? '').trim().toLowerCase()
  if (TRUE_SPELLINGS.includes(normalised)) {
    return true
  }
  if (FALSE_SPELLINGS.includes(normalised)) {
    return false
  }
  return fallback
}

export function serializeBoolean(value: boolean): string {
  return value ? 'true' : 'false'
}

export function parseInteger(value: string | undefined, fallback: number): number {
  const normalised = (value ?? '').trim()
  if (/^-?\d+$/.test(normalised) === false) {
    return fallback
  }
  return Number(normalised)
}

/** An absent key and an empty value are the same thing on the daemon side. */
export function parseInstant(value: string | undefined): string | null {
  const normalised = (value ?? '').trim()
  if (normalised.length === 0 || Number.isFinite(Date.parse(normalised)) === false) {
    return null
  }
  return normalised
}
```

- [ ] **Step 4: Write the failing Settings screen tests**

`web/src/screens/SettingsScreen.test.tsx`:

```tsx
import { render, screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import SettingsScreen from './SettingsScreen'
import * as client from '../api/client'
import { idleStatus, zoneFixtures } from '../test/fixtures'

const refresh = vi.fn()

beforeEach(() => {
  vi.useFakeTimers({ shouldAdvanceTime: true })
  vi.setSystemTime(new Date('2026-09-13T20:00:00Z'))
  vi.spyOn(client, 'getZones').mockResolvedValue(zoneFixtures)
  vi.spyOn(client, 'putZone').mockResolvedValue(undefined)
  vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)
  vi.spyOn(client, 'getSettings').mockResolvedValue({
    rain_delay_until: '',
    master_enabled: 'true',
    max_zone_seconds: '1800',
    log_level: 'info',
  })
})

afterEach(() => {
  vi.useRealTimers()
  vi.restoreAllMocks()
  refresh.mockReset()
})

describe('SettingsScreen', () => {
  it('shows the current values', async () => {
    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)

    expect(await screen.findByLabelText(/master enable/i)).toBeChecked()
    expect(screen.getByLabelText(/maximum zone runtime/i)).toHaveValue(30)
    expect(screen.getByTestId('rain-delay-state')).toHaveTextContent(/no rain delay/i)
  })

  it('reads the string "false" as disabled', async () => {
    vi.spyOn(client, 'getSettings').mockResolvedValue({
      master_enabled: 'false',
      max_zone_seconds: '1800',
    })

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    expect(await screen.findByLabelText(/master enable/i)).not.toBeChecked()
  })

  it('falls back to 30 minutes when the ceiling is missing', async () => {
    vi.spyOn(client, 'getSettings').mockResolvedValue({ master_enabled: 'true' })
    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    expect(await screen.findByLabelText(/maximum zone runtime/i)).toHaveValue(30)
  })

  it('sends the master enable as a string', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.click(await screen.findByLabelText(/master enable/i))

    await waitFor(() => {
      expect(putSettings).toHaveBeenCalledWith({ master_enabled: 'false' })
    })
  })

  it('sends the run ceiling in seconds as a string', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    const field = await screen.findByLabelText(/maximum zone runtime/i)
    await user.clear(field)
    await user.type(field, '15')
    await user.click(screen.getByRole('button', { name: /save ceiling/i }))

    await waitFor(() => {
      expect(putSettings).toHaveBeenCalledWith({ max_zone_seconds: '900' })
    })
  })

  it('refuses a zero or negative ceiling', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    const field = await screen.findByLabelText(/maximum zone runtime/i)
    await user.clear(field)
    await user.type(field, '0')
    await user.click(screen.getByRole('button', { name: /save ceiling/i }))

    expect(await screen.findByRole('alert')).toHaveTextContent(/at least one minute/i)
    expect(putSettings).not.toHaveBeenCalled()
  })

  it('sets a rain delay as a UTC instant the requested number of days out', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.click(await screen.findByRole('button', { name: /delay 2 days/i }))

    await waitFor(() => {
      expect(putSettings).toHaveBeenCalledTimes(1)
    })
    expect(putSettings.mock.calls[0]![0]).toEqual({
      rain_delay_until: '2026-09-15T20:00:00.000Z',
    })
  })

  it('clears a rain delay with an empty string', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    vi.spyOn(client, 'getSettings').mockResolvedValue({
      rain_delay_until: '2026-09-15T20:00:00Z',
      master_enabled: 'true',
      max_zone_seconds: '1800',
    })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.click(await screen.findByRole('button', { name: /clear rain delay/i }))

    await waitFor(() => {
      expect(putSettings).toHaveBeenCalledWith({ rain_delay_until: '' })
    })
  })

  it('renders an active rain delay in the controller zone', async () => {
    vi.spyOn(client, 'getSettings').mockResolvedValue({
      rain_delay_until: '2026-09-15T20:00:00Z',
      master_enabled: 'true',
      max_zone_seconds: '1800',
    })

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    // 20:00 UTC is 1:00 PM in America/Los_Angeles.
    expect(await screen.findByTestId('rain-delay-state')).toHaveTextContent('1:00 PM')
  })

  it('shows no rain delay for an empty value rather than an invalid date', async () => {
    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    const state = await screen.findByTestId('rain-delay-state')
    expect(state).toHaveTextContent(/no rain delay/i)
    expect(state).not.toHaveTextContent(/invalid/i)
    expect(state).not.toHaveTextContent(/NaN/)
  })

  it('renames a zone by its zone number', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putZone = vi.spyOn(client, 'putZone').mockResolvedValue(undefined)

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)

    const row = await screen.findByTestId('zone-row-3')
    const field = within(row).getByLabelText(/zone 3 name/i)
    await user.clear(field)
    await user.type(field, 'Rose bed')
    await user.click(within(row).getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(putZone).toHaveBeenCalledTimes(1)
    })
    const [zoneArgument, patch] = putZone.mock.calls[0]!
    expect(zoneArgument.number).toBe(3)
    expect(zoneArgument.id).toBe(9)
    expect(patch).toEqual({ name: 'Rose bed', enabled: true })
  })

  it('carries the enabled flag through a rename', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putZone = vi.spyOn(client, 'putZone').mockResolvedValue(undefined)

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)

    // zoneFixtures[6] is zone number 7 with enabled: false.
    const row = await screen.findByTestId('zone-row-7')
    await user.clear(within(row).getByLabelText(/zone 7 name/i))
    await user.type(within(row).getByLabelText(/zone 7 name/i), 'Pots')
    await user.click(within(row).getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(putZone).toHaveBeenCalled()
    })
    expect(putZone.mock.calls[0]![1]).toEqual({ name: 'Pots', enabled: false })
  })

  it('reports a rejected write', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    vi.spyOn(client, 'putSettings').mockRejectedValue(new Error('database is locked'))

    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.click(await screen.findByLabelText(/master enable/i))

    expect(await screen.findByRole('alert')).toHaveTextContent(/database is locked/i)
  })
})
```

What each one kills:

- `reads the string "false" as disabled` — any truthiness-based read shows a disabled controller as enabled, which is the state where nothing waters and the UI says everything is fine.
- `sends the master enable as a string` — the settings table is TEXT. A JSON boolean stores `1`/`0` or fails the bind depending on how the daemon reads it.
- `sets a rain delay as a UTC instant` — the assertion is an exact ISO string against a frozen clock, so a local-time string without the `Z`, or a date-only string, fails.
- `carries the enabled flag through a rename` — `PUT /admin/zones/{number}` takes the whole row. A body carrying only `{ name }` re-enables a zone the user deliberately turned off, and zone 7 in the fixtures is the one that is off.
- `renames a zone by its zone number` asserts both `number` and `id` on the argument, the same transposition guard as the Now screen.
- `shows no rain delay for an empty value` — treating `''` as a date renders `Invalid Date` or `NaN`, which reads as a bug rather than as "no delay set".

- [ ] **Step 5: Write the Settings screen**

`web/src/screens/SettingsScreen.tsx`:

```tsx
import { useCallback, useEffect, useState } from 'react'
import { getSettings, getZones, putSettings, putZone } from '../api/client'
import type { SettingsMap, Zone } from '../api/types'
import {
  DEFAULT_MAX_ZONE_SECONDS,
  SETTING_KEYS,
  parseBoolean,
  parseInstant,
  parseInteger,
  serializeBoolean,
} from '../settings/settingsMap'
import { formatDayAndClock } from '../time/zonedformat'
import type { ScreenProps } from './NowScreen'

const RAIN_DELAY_CHOICES = [1, 2, 3, 7]

export default function SettingsScreen({ status, refresh }: ScreenProps) {
  const [settings, setSettings] = useState<SettingsMap>({})
  const [zones, setZones] = useState<Zone[]>([])
  const [names, setNames] = useState<Record<number, string>>({})
  const [ceilingMinutes, setCeilingMinutes] = useState(DEFAULT_MAX_ZONE_SECONDS / 60)
  const [error, setError] = useState<string | null>(null)

  const load = useCallback(async () => {
    try {
      const [loadedSettings, loadedZones] = await Promise.all([getSettings(), getZones()])
      setSettings(loadedSettings)
      setZones([...loadedZones].sort((left, right) => left.number - right.number))
      setNames(Object.fromEntries(loadedZones.map((zone) => [zone.number, zone.name])))
      setCeilingMinutes(
        Math.round(parseInteger(loadedSettings[SETTING_KEYS.maxZoneSeconds], DEFAULT_MAX_ZONE_SECONDS) / 60),
      )
      setError(null)
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught))
    }
  }, [])

  useEffect(() => {
    void load()
  }, [load])

  const write = useCallback(
    async (patch: SettingsMap) => {
      setError(null)
      try {
        await putSettings(patch)
        setSettings((current) => ({ ...current, ...patch }))
        refresh()
      } catch (caught: unknown) {
        setError(caught instanceof Error ? caught.message : String(caught))
      }
    },
    [refresh],
  )

  const masterEnabled = parseBoolean(settings[SETTING_KEYS.masterEnabled], true)
  const rainDelayUntil = parseInstant(settings[SETTING_KEYS.rainDelayUntil])
  const controllerZone = status?.timezone ?? ''

  const onSaveCeiling = useCallback(() => {
    if (ceilingMinutes < 1) {
      setError('The maximum zone runtime must be at least one minute.')
      return
    }
    void write({ [SETTING_KEYS.maxZoneSeconds]: String(ceilingMinutes * 60) })
  }, [ceilingMinutes, write])

  const onRename = useCallback(
    async (zone: Zone) => {
      setError(null)
      try {
        await putZone(zone, { name: names[zone.number] ?? zone.name, enabled: zone.enabled })
        await load()
      } catch (caught: unknown) {
        setError(caught instanceof Error ? caught.message : String(caught))
      }
    },
    [names, load],
  )

  return (
    <section className="screen">
      <h1>Settings</h1>

      {error === null ? null : (
        <div className="alert" role="alert">
          {error}
        </div>
      )}

      <h2>Watering</h2>

      <label>
        Master enable
        <input
          type="checkbox"
          checked={masterEnabled}
          onChange={(event) => {
            void write({ [SETTING_KEYS.masterEnabled]: serializeBoolean(event.target.checked) })
          }}
        />
      </label>

      <label>
        Maximum zone runtime (minutes)
        <input
          type="number"
          min={1}
          value={ceilingMinutes}
          onChange={(event) => {
            setCeilingMinutes(Number(event.target.value))
          }}
        />
      </label>
      <button type="button" onClick={onSaveCeiling}>
        Save ceiling
      </button>

      <h2>Rain delay</h2>

      <div data-testid="rain-delay-state">
        {rainDelayUntil === null
          ? 'No rain delay'
          : `Watering paused until ${formatDayAndClock(rainDelayUntil, controllerZone)}`}
      </div>

      <div className="row">
        {RAIN_DELAY_CHOICES.map((days) => (
          <button
            key={days}
            type="button"
            onClick={() => {
              const until = new Date(Date.now() + days * 86400000).toISOString()
              void write({ [SETTING_KEYS.rainDelayUntil]: until })
            }}
          >
            {`Delay ${days} ${days === 1 ? 'day' : 'days'}`}
          </button>
        ))}
        <button
          type="button"
          onClick={() => {
            void write({ [SETTING_KEYS.rainDelayUntil]: '' })
          }}
        >
          Clear rain delay
        </button>
      </div>

      <h2>Zone names</h2>

      {zones.map((zone) => (
        <div key={zone.number} className="row" data-testid={`zone-row-${zone.number}`}>
          <label>
            {`Zone ${zone.number} name`}
            <input
              type="text"
              value={names[zone.number] ?? ''}
              onChange={(event) => {
                setNames((current) => ({ ...current, [zone.number]: event.target.value }))
              }}
            />
          </label>
          <button
            type="button"
            onClick={() => {
              void onRename(zone)
            }}
          >
            Save
          </button>
        </div>
      ))}
    </section>
  )
}
```

The rain delay is computed from `Date.now()`, which is an instant and carries no timezone question. Rendering it does, and that goes through `formatDayAndClock` with the controller's zone.

- [ ] **Step 6: Run the suite and the type check**

```bash
cd web && npm test && npm run typecheck
```

Expected: every settings case passes.

- [ ] **Step 7: Prove the boolean guard can fail**

Replace `parseBoolean(settings[SETTING_KEYS.masterEnabled], true)` with `Boolean(settings[SETTING_KEYS.masterEnabled])`, run `npm test -- SettingsScreen`, and confirm `reads the string "false" as disabled` fails. Revert.

- [ ] **Step 8: Commit**

```bash
cd /home/spunak/src/punak/irrigation
git add web
git commit -m "feat: add the Settings screen"
```

---

### Task 10: Production build, offline guard and README

The bundle is installed by the Yocto recipe `irrigation-web_1.0.bb` into `/var/www/irrigation/html` and served by nginx from `/`. The controller has no internet and the browser opening the page may not either, so a single CDN font or script turns into a request that hangs and a page that renders wrong with no error anyone will see.

The recipe hard-fails the image build when `web/dist/` is missing or holds no `index.html`. This task catches both conditions at `npm run build` time, where the person who caused them is standing.

**Files:**
- Create: `web/scripts/checkBundle.mjs`
- Create: `web/README.md`
- Test: `web/src/build/checkBundle.test.ts`

**Interfaces:**
- Produces:
  - `scanForExternalOrigins(text: string): string[]`
  - `checkBundleDir(dir: string): string[]` — returns the list of problems; empty means the bundle is fit to install

- [ ] **Step 1: Confirm the compiler already covers `scripts/`**

Task 1's `tsconfig.json` carries `"allowJs": true` and lists `scripts` in `include`, which is what
lets the test below import `checkBundle.mjs`. No change is needed here. Confirm both are present
before writing the test; without them the import fails with `TS7016: Could not find a declaration
file for module '../../scripts/checkBundle.mjs'`.

- [ ] **Step 2: Write the failing bundle-guard tests**

`web/src/build/checkBundle.test.ts`:

```ts
import { mkdtempSync, mkdirSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'
import { checkBundleDir, scanForExternalOrigins } from '../../scripts/checkBundle.mjs'

function bundle(files: Record<string, string>): string {
  const dir = mkdtempSync(join(tmpdir(), 'irrigation-bundle-'))
  for (const [name, content] of Object.entries(files)) {
    const path = join(dir, name)
    mkdirSync(join(path, '..'), { recursive: true })
    writeFileSync(path, content)
  }
  return dir
}

describe('scanForExternalOrigins', () => {
  it('finds a stylesheet on a remote host', () => {
    const found = scanForExternalOrigins(
      '<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Inter">',
    )
    expect(found).toContain('https://fonts.googleapis.com')
  })

  it('finds a script on a remote host', () => {
    expect(scanForExternalOrigins('<script src="http://cdn.example.com/react.js"></script>')).toContain(
      'http://cdn.example.com',
    )
  })

  it('finds a protocol-relative url', () => {
    expect(scanForExternalOrigins('<img src="//images.example.com/logo.png">')).toContain(
      '//images.example.com',
    )
  })

  it('finds a remote origin inside bundled javascript', () => {
    expect(scanForExternalOrigins('fetch("https://api.weather.example.com/v1")')).toContain(
      'https://api.weather.example.com',
    )
  })

  it('allows the XML namespace urls that svg markup carries', () => {
    expect(
      scanForExternalOrigins('<svg xmlns="http://www.w3.org/2000/svg"><path d="M0 0"/></svg>'),
    ).toEqual([])
    expect(scanForExternalOrigins('<html xmlns="http://www.w3.org/1999/xhtml">')).toEqual([])
  })

  it('allows root-relative and same-origin urls', () => {
    expect(
      scanForExternalOrigins('<script type="module" src="/assets/index-abc123.js"></script>'),
    ).toEqual([])
    expect(scanForExternalOrigins('fetch("/api/status")')).toEqual([])
  })
})

describe('checkBundleDir', () => {
  it('passes a clean bundle', () => {
    const dir = bundle({
      'index.html': '<!doctype html><script type="module" src="/assets/app.js"></script>',
      'assets/app.js': 'fetch("/api/status")',
      'assets/app.css': 'body { font-family: system-ui; }',
    })
    expect(checkBundleDir(dir)).toEqual([])
  })

  it('reports a missing directory', () => {
    expect(checkBundleDir(join(tmpdir(), 'irrigation-bundle-does-not-exist'))).toEqual([
      expect.stringMatching(/does not exist/i),
    ])
  })

  it('reports a directory with no index.html', () => {
    const dir = bundle({ 'assets/app.js': 'fetch("/api/status")' })
    expect(checkBundleDir(dir)).toEqual([expect.stringMatching(/index\.html/i)])
  })

  it('reports an external origin in the entry document', () => {
    const dir = bundle({
      'index.html': '<link href="https://fonts.googleapis.com/css2?family=Inter" rel="stylesheet">',
    })
    expect(checkBundleDir(dir).join('\n')).toMatch(/fonts\.googleapis\.com/)
  })

  it('reports an external origin in an emitted asset', () => {
    const dir = bundle({
      'index.html': '<!doctype html><script src="/assets/app.js"></script>',
      'assets/app.js': 'new Image().src = "https://tracker.example.com/pixel.gif"',
    })
    expect(checkBundleDir(dir).join('\n')).toMatch(/tracker\.example\.com/)
  })

  it('ignores source maps', () => {
    const dir = bundle({
      'index.html': '<!doctype html><script src="/assets/app.js"></script>',
      'assets/app.js': '//# sourceMappingURL=app.js.map',
      'assets/app.js.map': '{"sources":["https://internal.example.com/src/app.ts"]}',
    })
    expect(checkBundleDir(dir)).toEqual([])
  })
})
```

`reports a directory with no index.html` is the condition the Yocto recipe fails on. An interrupted `npm run build` leaves `dist/` present and empty, the recipe installs nothing, and nginx answers a silent 403 with no clue where it came from.

`allows the XML namespace urls that svg markup carries` is the false positive that would otherwise make this check useless the first time anyone inlines an icon. `ignores source maps` is the second — a map file records absolute source paths and is never fetched by the page.

- [ ] **Step 3: Run it and verify it fails**

```bash
cd web && npm test -- checkBundle
```

Expected: failure — `scripts/checkBundle.mjs` does not exist.

- [ ] **Step 4: Write the bundle guard**

`web/scripts/checkBundle.mjs`:

```js
import { existsSync, readFileSync, readdirSync, statSync } from 'node:fs'
import { join, relative } from 'node:path'

const ORIGIN_PATTERN = /(?:https?:)?\/\/[a-z0-9.-]+\.[a-z]{2,}/gi

const ALLOWED_ORIGINS = ['http://www.w3.org', 'https://www.w3.org']

const SCANNED_EXTENSIONS = ['.html', '.js', '.mjs', '.css', '.json', '.svg']

/** Returns every remote origin referenced in `text`, deduplicated. */
export function scanForExternalOrigins(text) {
  const found = new Set()
  for (const match of text.matchAll(ORIGIN_PATTERN)) {
    const origin = match[0]
    if (ALLOWED_ORIGINS.some((allowed) => origin.startsWith(allowed))) {
      continue
    }
    found.add(origin)
  }
  return [...found]
}

function walk(dir) {
  const entries = []
  for (const name of readdirSync(dir)) {
    const path = join(dir, name)
    if (statSync(path).isDirectory()) {
      entries.push(...walk(path))
    } else {
      entries.push(path)
    }
  }
  return entries
}

/** Returns every reason this directory is unfit to install. Empty means it is fit. */
export function checkBundleDir(dir) {
  if (existsSync(dir) === false) {
    return [`${dir} does not exist — run "npm run build" first`]
  }

  const problems = []
  const indexPath = join(dir, 'index.html')

  if (existsSync(indexPath) === false) {
    problems.push(`${dir} has no index.html — the irrigation-web recipe fails the image build on this`)
    return problems
  }

  for (const path of walk(dir)) {
    if (path.endsWith('.map')) {
      continue
    }
    if (SCANNED_EXTENSIONS.some((extension) => path.endsWith(extension)) === false) {
      continue
    }

    const origins = scanForExternalOrigins(readFileSync(path, 'utf8'))
    for (const origin of origins) {
      problems.push(
        `${relative(dir, path)} references ${origin} — the controller has no internet and the request will hang`,
      )
    }
  }

  return problems
}

const invokedDirectly = process.argv[1] !== undefined && import.meta.url.endsWith(process.argv[1].replace(/\\/g, '/'))

if (invokedDirectly) {
  const target = process.argv[2] ?? 'dist'
  const problems = checkBundleDir(target)
  if (problems.length > 0) {
    for (const problem of problems) {
      console.error(`bundle check: ${problem}`)
    }
    process.exit(1)
  }
  console.log(`bundle check: ${target} is fit to install`)
}
```

- [ ] **Step 5: Run the production build**

```bash
cd web && npm run build
```

`npm run build` is `tsc --noEmit && vite build && node scripts/checkBundle.mjs`, so a type error, a build failure or an external origin each stop it.

Expected: `dist/index.html` exists and the check prints `dist is fit to install`.

```bash
ls -l dist/index.html && cat dist/index.html
```

Expected: every `src` and `href` is a root-relative `/assets/...` path.

- [ ] **Step 6: Prove the guard can fail**

Add `<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Inter">` to `web/index.html`, run `npm run build`, and confirm it exits non-zero naming `fonts.googleapis.com`. Revert.

Then `rm dist/index.html && node scripts/checkBundle.mjs dist` and confirm it exits non-zero naming the recipe. Rebuild.

- [ ] **Step 7: Write the README**

`web/README.md`:

```markdown
# Irrigation web interface

React 19 + Vite + TypeScript. Three screens against the `irrigationd` REST API.
Spec: `../docs/design/2026-09-05-irrigation-design.md` §8.

## Scripts

| Command | Does |
|---|---|
| `npm run dev` | Vite dev server on 5173, proxying `/api` to `127.0.0.1:8080/admin` |
| `npm test` | Vitest under `TZ=UTC` |
| `npm run typecheck` | `tsc --noEmit` |
| `npm run build` | typecheck, build to `dist/`, then the bundle guard |

`TZ=UTC` is not optional. `src/test/setup.ts` aborts the suite on any other host
zone: the timezone tests compare a rendering in `America/Los_Angeles` against the
host zone, and a host already in that zone passes them whether or not the code
passes the zone through.

## How the browser reaches the daemon

The daemon binds `127.0.0.1:8080` and serves its routes under `/admin`. nginx on
the controller serves this bundle from `/var/www/irrigation/html` and
reverse-proxies `/api/` to the daemon. The browser can never reach `:8080`
directly, so every request in this bundle is a same-origin relative url built by
`apiUrl()` in `src/api/apiPath.ts`. An absolute `http://<host>:8080` url works on
a development machine and fails on the target.

`daemonPath()` in the same file is the `/api` → `/admin` rewrite, used by the dev
server proxy so development and production agree on one definition.

## Two zone identifiers

`zones` rows carry `id` and `number`, and they are equal in a freshly seeded
database.

| Use | Identifier |
|---|---|
| `POST /api/zones/{n}/run`, `PUT /api/zones/{n}` | `zone.number` |
| `program.zones[].zoneId` | `zone.id` |

Every fixture in `src/test/fixtures.ts` uses zones whose `id` and `number`
differ, because a fixture where they match cannot tell a transposed identifier
from a correct one.

## Time

Every timestamp the API returns is UTC. Every rendering of one goes through
`src/time/zonedformat.ts` with the IANA zone id `/api/status` reports. A start
time is a wall-clock rule with no instant behind it and is converted by
`minutesToClock()`, which takes no zone at all.

## Day-of-week bits

`dowMask` bit 0 is **Monday**, bit 6 is Sunday — `QDate::dayOfWeek()` minus one.
`dayMode` strings are `DaysOfWeek`, `Odd`, `Even`, `EveryNDays`, taken verbatim
from `Program::dayModeToString()` in `IrrigationD/src/model/program.cpp`.
```

- [ ] **Step 8: Run the whole suite one more time**

```bash
cd web && npm test && npm run typecheck && npm run build
```

Expected: every test passes, `tsc` exits zero, the bundle guard passes.

- [ ] **Step 9: Commit**

```bash
cd /home/spunak/src/punak/irrigation
git add web
git commit -m "feat: add the production build guard and the web README"
```

---

## Deferred to later plans

- **Run history and reporting** built on `fired_instants` (spec §11). The table is populated from day one and the API exposes nothing that reads it.
- **Weather-aware skip** (spec §11). Its UI would live on Settings next to the rain delay.
- Authentication, remote access, and any screen beyond the three in spec §8.

---

## Spec coverage

| Spec | Requirement | Task |
|---|---|---|
| §3 | `web/`, React 19 + Vite + TypeScript, built to `web/dist/` | 1, 10 |
| §7 | Requests reach the daemon through the nginx `/api` proxy | 1, 2 |
| §7 | Every endpoint the three screens need | 2 |
| §7 | `/admin/status` carries the controller's timezone | 2, 3 |
| §8 | Now — running zone with time remaining | 4, 6 |
| §8 | Now — next scheduled run | 6 |
| §8 | Now — eight zone tiles with a quick manual run | 6 |
| §8 | Now — prominent stop control | 6 |
| §8 | Mobile-first, large touch targets, high contrast | 1, 6 |
| §8 | Programs — list | 7 |
| §8 | Programs — create, edit | 8 |
| §8 | Programs — day rule | 7, 8 |
| §8 | Programs — start times | 7, 8 |
| §8 | Programs — ordered zone list with durations | 7, 8 |
| §8 | Programs — computed total runtime | 7 |
| §8 | Programs — next run | 7 |
| §8 | Settings — rain delay | 9 |
| §8 | Settings — master enable | 9 |
| §8 | Settings — maximum zone runtime | 9 |
| §8 | Settings — zone names | 9 |
| §8 | `/admin/status` every 2 s running, 15 s otherwise | 4 |
| §8 | All times render in the controller's timezone | 3, 6, 7, 9 |
| §9 | `web/dist` installable by `irrigation-web_1.0.bb` | 10 |

---

## Deliberate decisions

Spec §8 is three screens, a polling interval and a timezone rule. Everything below was decided by this plan.

**1. No router library.** Routing is the URL fragment, twenty lines in `App.tsx`. nginx's `try_files $uri $uri/ /index.html` would also support path routing, so the choice buys one fewer dependency in a bundle that has to be audited for network origins rather than any capability.

**2. No server-state library.** `useStatus` is a chained `setTimeout` in about sixty lines. The adaptive interval spec §8 asks for is the thing a caching library would be brought in to provide, and it is the part that would still have to be configured by hand.

**3. No CSS framework.** Custom properties in `tokens.css`, a `--touch` floor of 56px, and a palette that responds to `prefers-color-scheme`. A framework loaded from a CDN is barred outright by the offline constraint, and a bundled one is weight for three screens.

**4. No ESLint.** The gates are `tsc --noEmit` under `strict` plus `noUnusedLocals`, `noUnusedParameters`, `noUncheckedIndexedAccess` and `exactOptionalPropertyTypes`, and the test suite. That is the analogue of the daemon's `-Wextra -Wall -Werror`.

**5. `fetch` is stubbed directly in tests** rather than through a mock service worker. The API surface is thirteen functions and the stub records every call, which is what the transposition assertions read.

**6. The quick manual run is one duration selector above eight run buttons**, defaulting to 10 minutes, offering 1/5/10/15/20/30. Per-tile duration controls would double the number of targets on the screen spec §8 wants operable with large touch targets.

**7. The program editor is a screen state rather than a route.** A half-finished program cannot be reached by a bookmark or survive a reload, and `screenFromHash` sends `#/programs/12` to Now rather than rendering an editor with no data.

**8. Delete confirms; stop does not.** A deleted program cannot be recovered; a stopped valve can be restarted. The `confirm` stub in `NowScreen.test.tsx` throws so a dialog added to the e-stop fails loudly.

**9. `PUT /api/programs/{id}` sends the whole program**, including start times and zones, on every write — the enable toggle on the list included. The daemon replaces the nested rows, so a partial body deletes them.

**10. Settings values are strings in both directions**, because the `settings` table is key/value TEXT and `settingValue()` returns a `QString`.

**11. Zone tiles render exactly the zones `/api/zones` returns**, sorted by `number`. Rendering a fixed eight would let a tap POST to a zone the daemon does not have.

**12. The request body shapes for zones, programs and settings** are defined by this plan's **API contract** section, derived from the daemon plan's Task 4 model field names. Spec §7 lists the routes and specifies no payloads. The decoders throw on a mismatch, so a disagreement with the daemon surfaces as a named field rather than a blank screen.

---

## Cross-session facts

Three sessions work this tree on disjoint subtrees. These were established by the others and are recorded here so this plan's executor does not re-derive them.

- **`dayMode` strings are PascalCase.** `DaysOfWeek`, `Odd`, `Even`, `EveryNDays`, from `IrrigationD/src/model/program.cpp:17-25` (committed at `f077d57`) and the `day_mode` column default in `schema.sql:17`. `FiredInstant::Outcome` uses a different convention (`ran`, `skipped_busy`); neither generalises to the other.
- **`dayModeFromString()` returns `DaysOfWeek` for any unrecognised string**, with no error and no log entry. The decoder in Task 2 throws instead, and that throw is the only place a casing disagreement becomes visible.
- **`GET /admin/programs` will carry a per-program `nextRunUtc`.** Ruled on the daemon side and recorded in its ledger for the Task 9 dispatch. This plan still decodes the field as optional, so the UI is correct whether or not that lands first.
- **The document root is `/var/www/irrigation/html`** and the `irrigation-web_1.0.bb` recipe hard-fails the image build when `web/dist/` is missing or holds no `index.html`. There is no `nodejs` in the image; the bundle is static.
- **nginx does `try_files $uri $uri/ /index.html`** and proxies `/api/` to `http://127.0.0.1:8080` with `Host`, `X-Real-IP`, `X-Forwarded-For` and `X-Forwarded-Proto` set. The daemon binds loopback only.

---

## Known gaps

- **Per-program next run depends on a daemon field that is not yet written.** Until `GET /admin/programs` carries `nextRunUtc`, the Programs screen renders `—` in that position. Computing it in the browser would mean a second implementation of the scheduler's day rules and DST resolution, which is the part of the daemon most likely to drift.
- **`/admin/health` and `/admin/version` are consumed by nothing.** The three screens in spec §8 have nowhere to put a version string, and adding a place for one is scope this plan does not have.
- **The `log_level` setting is rendered by no screen.** Spec §8 lists four things on Settings and this is not one of them. The daemon plan carries the same row as a known gap on its side.
- **There is no optimistic update anywhere.** Every write is followed by a reload or a status refresh. On a LAN with a loopback daemon that costs one round trip and removes a class of bug where the screen shows a state the controller rejected.
- **Nothing tests the real bundle against the real daemon.** Task 1 Step 8 is a manual `curl` through the dev proxy and Task 10 Step 5 is a manual look at `dist/index.html`. An end-to-end check belongs with the Yocto layer's image test, which is a different plan.
