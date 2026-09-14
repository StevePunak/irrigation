# Irrigation web interface

React 19 + Vite + TypeScript. Three screens against the `irrigationd` REST API.
Spec: `../docs/design/2026-09-05-irrigation-design.md` §8.

## Scripts

| Command | Does |
|---|---|
| `npm run dev` | Vite dev server on 5173, proxying `/api` to `127.0.0.1:8080/admin` |
| `npm test` | Vitest under `TZ=UTC`, then the wall-clock suite under `TZ=America/Los_Angeles` |
| `npm run typecheck` | `tsc --noEmit` |
| `npm run build` | typecheck, build to `dist/`, then the bundle guard |

`npm run build` writes `dist/`. It is gitignored, it is what the `irrigation-web` Yocto recipe
installs, and the recipe fails the image build when `dist/index.html` is missing, so build here
before building an image.

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
from the `DayModeToStringMap` behind `Program::dayModeToString()`, in
`IrrigationD/src/model/program.h`.
