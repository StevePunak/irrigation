# Irrigation Controller — Design

**Date:** 2026-09-05
**Status:** Approved for implementation planning

## 1. Overview

Replace an Orbit 57894 four-station irrigation controller with a Raspberry Pi 4B
driving eight 24VAC solenoid valves, scheduled by a headless Qt daemon and
configured through a locally served web interface.

### Goals

- Feature parity with the Orbit: multiple named programs, per-zone run
  durations, day selection (days-of-week, odd, even, every-N-days), multiple
  start times per program, sequential zone execution.
- Manual zone runs, rain delay, global disable, and a physical stop control.
- Web UI reachable from any device on the LAN, no authentication.
- Unattended operation across power cuts and network outages.

### Non-goals for v1

Designed around but deliberately deferred: weather-aware skip, run history and
reporting, flow sensing and leak detection, master valve or pump-start output,
remote access beyond the LAN, user accounts.

## 2. Hardware

### 2.1 Existing system

The Orbit 57894 is a 120V plug-in indoor/outdoor controller with an internal
transformer driving standard 24VAC solenoids. Maximum load is 250mA per station
and 500mA total. Three of its four stations are in use. The system shutoff is a
manual ball valve teed from a hose bib; there is no master valve and no pump.

The Orbit is retained intact as a fallback controller. Nothing is salvaged from
it.

### 2.2 Bill of materials

| Item | Part |
|---|---|
| Controller | Raspberry Pi 4B |
| Relay board | SunFounder 5V 8-channel, opto-isolated, SPDT |
| Transformer | 24VAC 40VA control transformer, multi-tap 120/208/240V primary, foot mount |
| Logic supply | Mean Well HDR-15-5 (5V, 2.4A, DIN rail) |
| Pi power | USB-C pigtail, bare wire to USB-C male, 20AWG |
| RTC | DS3231 + AT24C32 module (ZS-042), CR2032 cell |
| Enclosure | QILIPSU IP67 ABS, 285 x 195 x 130 mm, opaque grey hinged cover, plastic mounting plate |
| Rail | 35mm slotted aluminium DIN rail, cut to ~250 mm |
| Terminals | DIN terminal block kit with connection bars and end stops |
| Mains entry | 14/3 pigtail cord, NEMA 5-15P, open end |
| Glands | IP68 nylon cord grips, PG7-PG16 assortment |
| Vent | IP68 M12x1.5 breather vent |
| Protection | 5x20mm inline fuse holders; 1A slow-blow (primary), 1A fast-blow (secondary) |
| Stop control | 16mm IP67 momentary pushbutton, 1NO, 304 stainless |

### 2.3 Power distribution

A single 120V cord enters through a cord grip on the bottom face and lands on a
fused terminal block, 1A slow-blow on the hot leg. Slow-blow is required: a
40VA transformer's inrush current will destroy a fast-blow fuse of this rating
on first energisation.

Two loads run in parallel from that block:

- **Transformer primary**, landed on the **120V tap**. The 208V and 240V taps
  are capped and heat-shrunk.
- **HDR-15-5 primary.** Its 5V output feeds the Pi through a USB-C pigtail so
  the Pi's own input protection stays in circuit, and feeds the relay board's
  `JD-VCC` directly with the board jumper removed, so relay coil inrush cannot
  sag the Pi's rail. Trim the supply to approximately 5.1V to offset cable drop.

The transformer's metal frame is bonded to the cord ground. The enclosure is
non-conductive and requires no bonding. The circuit feeding the box must be
GFCI protected.

Load budget: Pi 4B headless 0.6-0.9A steady with a ~1.2A boot peak, one relay
coil and opto ~90mA, RTC negligible. Approximately 1.0A against a 2.4A supply.

### 2.4 Valve wiring

One transformer secondary leg lands on the valve-common bus terminal. The other
is bussed across all eight relay COM poles. Each relay NO output goes to one
zone wire on a DIN terminal block. Closing a relay completes the circuit and
opens the solenoid.

The secondary is fused at 1A. The open-frame transformer has no thermal
protection of its own; a shorted solenoid or a severed field wire would
otherwise be dissipated inside a sealed enclosure.

Zones are numbered 1-8 and consume all eight relay channels. Adding a master
valve later requires either reducing to seven zones or a second relay board.

### 2.5 GPIO assignment

| Function | BCM line |
|---|---|
| Zones 1-8 | 5, 6, 12, 13, 16, 19, 20, 21 |
| Stop button | 25 |
| RTC I2C | 2 (SDA), 3 (SCL) |

Zone lines are chosen to avoid boot-time alternate functions and to leave the
I2C and UART console pins free.

The eight are also the rightmost eight pads on the proto-HAT breakout strip,
in silkscreen order, so zone N lands on pad N and the ribbon to the relay
board runs straight with no crossings.

BCM 26 carried zone 8 until 2026-09-13. The GeeekPi proto-HAT does not break
it out: the strip labels 25 signals and 26 is not among them.

### 2.6 Boot-time valve safety

The relay board is **low-level trigger with no polarity jumper**: pulling an IN
pin low energises its relay and closes NO. Raspberry Pi GPIO lines come up as
inputs, and lines 9-27 default to an internal pull-down, which the board reads
as asserted. Without mitigation, six of the eight zones open at power-up
and stay open until the daemon starts. BCM 5 and 6 default to pull-up and
come up de-asserted.

Two independent mitigations, both required:

1. **10k pull-up resistors** from each IN pin to the relay board VCC. These
   dominate the Pi's ~50k internal pulls and hold the inputs de-asserted during
   the window before the bootloader runs, and whenever the daemon is not holding
   the lines.
2. **`gpio=5,6,12,13,16,19,20,21=op,dh` in `config.txt`**, applied by the
   bootloader before userspace exists.

The SunFounder datasheet contradicts itself on trigger polarity: its feature
list claims a low input leaves the relay off, while its pinout section states a
low input connects NO to COM. The pinout is correct. Verify on the bench with a
meter before connecting 24V.

### 2.7 Enclosure and environment

Mounted outdoors but sheltered — under an eave, out of direct sun and rain.

- Non-metallic enclosure. A metal box around the Pi's antenna makes the wifi
  unusable. If the supplied mounting plate turns out to be steel, mount the Pi
  toward a side wall on standoffs rather than centred on the plate, and confirm
  signal strength before sealing the box.
- All cable glands on the **bottom** face; a gland on the top face channels
  water inward. Leave a drip loop in the mains cord outside the box.
- Fit the **M12 breather vent** in the bottom face. A fully sealed box cycles
  thermally each day, draws in humid air through any imperfection, and condenses
  it on the coldest interior surface. The vent equalises pressure and passes
  water vapour while blocking liquid and insects.
- Do not over-torque the lid screws and do not seal the gasket with silicone.
- Mount the transformer low, for weight and to keep its field away from the Pi.
  The combined ~7W of waste heat holds the interior above the dew point.
- Keep the mains section physically separated from the 24V and logic sections,
  with its own covered terminal block.

Internal panel area is roughly 265 x 175 mm and must accommodate the
transformer, relay board (135 x 54 mm), Pi, DIN PSU, and terminal blocks. Plan
the layout before drilling.

### 2.8 Verify on assembly

- Relay trigger polarity, with a meter, before 24V is connected.
- Transformer primary tap.
- Remove the ZS-042 charging resistor before fitting a CR2032. The module
  trickle-charges its cell for a rechargeable LIR2032; a primary lithium cell on
  that circuit leaks or vents.
- Power the RTC from **3.3V**. Its SDA and SCL pull-ups tie to VCC, and 5V on
  the Pi's I2C lines damages the GPIO bank.
- Gland sizes against the actual cord and field bundle diameters.
- Mounting plate material, and wifi signal strength with the lid closed.

### 2.9 GPIO bring-up on the target

The IO library's failure paths are unit tested, but nothing exercises
`LibGpiodBackend`'s request, release, write or edge paths on a success path,
because a development host has no accessible GPIO chip. These run once on the Pi
before the daemon drives anything.

1. Open by label — `pinctrl-rp1` on a Pi 5, `pinctrl-bcm2711` on a Pi 4B — and
   request the STOP line as an input with `Gpio::Edge::Both`.
2. **Press the STOP button and confirm `asserted()` fires on the press rather
   than the release.** This is the one behaviour the in-memory backend certifies
   independently of the kernel, so a green unit suite is not evidence for it.
3. The self-destruct case: a slot that calls `InputPin::release()` on the first
   event, with at least two edges in one read. Drive the input from a spare
   output with a short square burst so more than one event is guaranteed in a
   single wake. Repeat with `closeChip()` and with deleting the `InputPin`.
   Pass means no crash and every emitted offset is the STOP line's.
4. **Run step 3 under ASAN** (`-fsanitize=address`). A use-after-free that
   happens to survive is indistinguishable from a fixed one without it. If only
   one item on this list gets done, do this one.
5. Re-entrancy: a slot that spins a nested `QEventLoop` on the first event while
   more edges are pending. Each edge must be reported exactly once.
6. Overflow the event buffer: bounce more than sixteen edges. Confirm the
   notifier re-fires for the remainder and the settled logical state matches the
   physical button.
7. Cross-check the valve side: request all eight outputs, then assert no emitted
   edge offset ever falls in the valve offset set. A fabricated offset landing on
   a solenoid is the consequence the event-path fixes exist to prevent.
8. Unbind the chip driver with a notifier armed. Confirm error text is set, and
   record CPU usage — the error path returns without draining a level-triggered
   descriptor, so a persistent read error spins.

## 3. Repositories and components

| Path | Contents |
|---|---|
| `~/src/punak/irrigation` | CMake superproject, `MAIN_PROJ = irrigation` |
| `~/src/punak/irrigation/IrrigationD` | Daemon source, binary `irrigationd` |
| `~/src/punak/irrigation/web` | React 19 + Vite + TypeScript frontend |
| `~/src/punak/KanoopPiQt` | IO library, overhauled in place, added as a submodule |
| `~/src/punak/rpi/meta-rpi4-irrigation` | Yocto layer |

Submodules built `EXCLUDE_FROM_ALL`, following `meta-qt-mains`:

| Submodule | Used by the daemon for |
|---|---|
| `KanoopCommonQt` | `Log` and `LoggingBaseClass`; `AppSettings` as the settings base; `AbstractThreadClass` for worker threads; `LockingQueue`, `MutexEvent`, `PathUtil`, `DateTimeUtil` |
| `KanoopDatabaseQt` | `DataSource` and the versioned-migration framework |
| `KanoopPiQt` | GPIO |

`KanoopCommonQt` is a direct dependency of `irrigationd`, not only of
`KanoopPiQt`.

The frontend lives inside the superproject rather than a separate `-web`
repository. It is a static bundle with no deploy story of its own and no use
without `irrigationd`.

### 3.1 Reference implementations

All patterns come from `~/src/epc/meta-qt-mains`.

| Concern | Reference |
|---|---|
| Superproject CMake | `CMakeLists.txt` |
| C++ style | `.claude/docs/codestyle-cpp.md` |
| Threaded REST server on `QHttpServer` | `libEpcSimQt/include/epc/sim/tau/somrestserver.h` and its `.cpp` |
| Worker threads | `AbstractThreadClass` subclasses under `libEpcCommonQt/include/epc/network/` |
| Thread teardown | `.claude/docs/httpop-teardown-contract.md` |
| Schema versioning and migration | `libEpcCommonQt/include/epc/database/epcdatasource.h`, `.claude/docs/db-migration-architecture.md` |

## 4. KanoopPiQt v2

### 4.1 Build conversion

qmake to CMake, matching the `KanoopCommonQt` layout: `qt_add_library`,
`-Wextra -Wall -Werror`, public headers under `include/Kanoop/pi/` consumed as
`<Kanoop/pi/outputpin.h>`, a `libKanoopPi.pc.in`, and a `tests/` directory
behind `BUILD_TESTING`. Links `Qt6::Core` and libgpiod v2 through pkg-config,
and depends on `KanoopCommonQt` for `Log`.

### 4.2 Legacy code

`pigs.*`, `pigcommand.*`, and `gpioreader.*` are removed. They target the pigpio
daemon, which does not function on Raspberry Pi 5.

`i2c.*` and `devices/` (ADS1115, BMP280) move to `legacy/`, excluded from the
build, to be ported when a consuming project needs them.

### 4.3 API

Five types, digital IO only:

- **Chip ownership lives in the backend**, not a separate `GpioChip` type.
  `IGpioBackend::openChipByLabel()` opens **by label** (e.g. `pinctrl-bcm2711`)
  rather than by device index. The Pi 5 moves GPIO to the RP1 southbridge and
  renumbers every chip on the system, so index-based lookup silently targets
  different silicon across board revisions. A standalone `GpioChip` was dropped
  during implementation because it would have exposed `gpiod_chip` through the
  seam the interface exists to close.
- **`OutputBank`** — requests multiple lines in a single `gpiod_line_request`,
  so a multi-line transition is one atomic `set_values()` call.
- **`OutputPin`** — single-line convenience over the same machinery, with
  `activeLow` as a construction parameter.
- **`InputPin`** — pull-up bias, edge detection, and kernel-side debounce via
  `gpiod_line_settings_set_debounce_period_us()`. The request file descriptor is
  wrapped in a `QSocketNotifier` so edges arrive as Qt signals on the event loop.

**Polarity lives in the kernel, at every layer of this library.** libgpiod and
the GPIO uAPI speak logical values throughout: `gpiod.h` documents
`gpiod_line_value` as "Logical line state" and describes active-low as inverting
the logical value relative to the physical pin. So `activeLow` is set once, on
the line request, and every value and every edge that comes back is already
logical.

`OutputBank::setValue(offset, true)` therefore means "energise the load" and
never inverts. `InputPin` emits `asserted()` on a logical rising edge and
`deasserted()` on a logical falling edge, with no reference to `activeLow`.
A 1NO button wired to ground reads physical LOW when pressed, which under
`activeLow` is logical ACTIVE — a press is a rising edge.

Applying the inversion a second time in software silently cancels the kernel's.
On the valve side that opens every zone when asked to close it; on the STOP
button it fires `asserted()` on release instead of press. This was built wrong
once and caught by review, so state the convention rather than re-deriving it.
- **`IGpioBackend`** with `LibGpiodBackend` and `MockBackend`. The mock allows
  the entire daemon to be unit tested on a development host with no GPIO
  hardware present.

### 4.4 Line release semantics

libgpiod releases requested lines when the owning process exits, and a released
line reverts to input. On an active-low relay board this opens every valve when
the daemon terminates. The kernel offers no mechanism to latch an output state
across process exit.

The external pull-up resistors in section 2.6 are therefore load-bearing rather
than defensive. This constraint belongs in the `OutputPin` header as a hardware
contract.

## 5. Daemon architecture

Source layout under `IrrigationD/src/`:

```
main.cpp                        QCoreApplication, CLI flags, logging, signal handling
irrigationdaemon.{h,cpp}        Lifecycle owner
zonecontroller.{h,cpp}          Sole owner of GPIO
scheduler.{h,cpp}               Resolves programs into due instants
programrunner.{h,cpp}           Executes one program's zone sequence
stopbutton.{h,cpp}              InputPin wrapper
irrigationcontrolserver.{h,cpp} QHttpServer REST surface
settings.{h,cpp}                Kanoop::AppSettings subclass
database/                       DataSource, schema.sql, migrate/
json/                           Request and response bodies
```

### 5.1 ZoneController safety contract

`ZoneController` is the only component that touches GPIO. It enforces five
invariants regardless of caller:

1. **Concurrency cap.** An open request that would put more than
   `max_concurrent_zones` zones open together fails. All line changes for one
   request go out in a single `OutputBank` write. Re-opening a zone that is
   already open takes no slot and sets its deadline to now plus the requested
   duration. Lowering the cap closes nothing; it refuses new opens until the
   count falls below it.
2. **No open without a deadline.** Every open zone has its own deadline and its
   own single-shot close timer. There is no overload that opens a zone
   indefinitely.
3. **Duration clamp.** Each requested duration is clamped to a configured
   ceiling.
4. **Watchdog.** A periodic tick reads the bank back and compares it with the
   expected *set* of open zones. A zone found past its deadline with its close
   timer still armed is closed through the normal deadline path, so a coarse
   event-loop stall between the timer's firing and the watchdog's tick does
   not read as a fault. The watchdog trips — closes the bank and latches a
   fault that refuses every open until a later tick reads back exactly the
   expected set with no zone open — for a zone past its deadline with no
   close timer armed, a line that differs from the expected set, or a failed
   read-back.
5. **Count check.** The watchdog also trips if more lines read back asserted
   than the cap in force when the open zones were opened.

A zone whose close write fails is marked closing and stays tracked until a
retry lands; it cannot be re-opened, and every later bank write — another
zone's open, another zone's close, the zone's own retried close — carries it
as inactive, so it goes dark the next time anything touches the bank. No zone
can open while an all-off or watchdog close is itself pending a retry; a
failed all-off retries every zone together in a single bank write.

The read-back uses `IGpioBackend::getValues()` through `OutputBank::readValues()`;
`InputPin::isAsserted()` gives the STOP button its level at startup.

Every close reports its reason — deadline, per-zone stop, all-off, or watchdog —
so the runner can tell a finished step zone from a STOP. `allOff()` is callable
from any component and always takes precedence.

Construction order is a hard requirement: `ZoneController` is constructed and
drives all eight lines de-asserted before the scheduler or HTTP server exist.
The systemd unit uses `Restart=always`.

### 5.2 Threading

`ZoneController` owns every GPIO line and `Scheduler` owns the firing decision.
Both live on the main event loop along with the libgpiod edge descriptor and all
timers, so valve state has a single owner that never races itself.

Everything that blocks, listens, or reaches the network is an
`AbstractThreadClass` subclass from `KanoopCommonQt`, shaped like `SomRestServer`
in `libEpcSimQt`: `threadStarted()` constructs the owned objects on the worker
thread and `threadFinished()` tears them down. `IrrigationControlServer` is one
of these, running its `QHttpServer` and every route handler on its own thread.
The deferred weather client is the next.

Traffic in both directions goes through signals. A route handler never calls
`ZoneController`; it emits, and the slot executes on the thread that owns the
valves. A setter called from another thread emits a request signal rather than
writing member state that a route handler also touches.

`QSqlDatabase` connections cannot be shared across threads, so the control
server opens its own named connection through `DataSource::setConnectionName()`.

Three `AbstractThreadClass` contracts govern teardown:

- `start()` reports that the thread started, not that initialisation succeeded.
  Post-start readiness is a separate query on the object.
- `stop()`'s default timeout blocks until the worker signals. Destructors depend
  on that, because they delete members the worker may still be reading.
- Never destroy one with `deleteLater()`. Its thread is gone by then and the
  deferred delete is posted to a queue nothing will drain.

See `.claude/docs/httpop-teardown-contract.md` in `meta-qt-mains`.

### 5.3 Time handling

Two distinct kinds of value, handled differently.

**Instants are UTC.** Every recorded moment — the fired-instants ledger, log
entries, API timestamps, and all future run history — is stored and transmitted
as UTC. The scheduler's internal comparisons are UTC. The DS3231 and the system
clock both run UTC.

**Schedule rules are wall-clock.** A start time is stored as
`minutes_after_midnight` in local time together with an IANA timezone
identifier. Normalising a recurring rule to UTC at write time makes it drift by
an hour at each DST transition, because the rule expresses an intention anchored
to the local clock rather than a fixed offset.

The timezone is consulted at exactly two boundaries: resolving a rule into
today's UTC instant, and formatting for display.

DST transitions are resolved explicitly using `QDateTime::TransitionResolution`
(Qt 6.7+):

- Spring forward, where the local time does not exist: `Reject`. The occurrence
  is logged with outcome `missed`.
- Fall back, where the local time occurs twice: `PreferBefore`. The program runs
  once, on the earlier offset.

**`tzdata` must be present in the image.** `QTimeZone` on a system with no zone
database falls back to UTC without raising an error, which shifts every start
time by the local offset while producing internally consistent logs.

### 5.4 Scheduler

A one-second tick evaluates each enabled program. For each start time, the
scheduler resolves the rule into today's UTC instant, checks the day rule, the
rain delay, and the global enable, then consults `fired_instants`.

- Every instant that fires is persisted, making the scheduler idempotent across
  restarts and backward clock steps.
- An instant fires only within a two-minute grace window. A missed occurrence is
  recorded with outcome `missed` and never caught up. Catch-up watering after an
  outage delivers water at an arbitrary time of day.

Day rules: `DaysOfWeek` (bitmask), `Odd`, `Even`, `EveryNDays` (interval plus
anchor date).

### 5.5 ProgramRunner and the program queue

A program is an ordered list of steps; a step is a set of zones and one
duration. `ProgramRunner` walks the steps, advanced only by `ZoneController`'s
close signal, so timing has a single authority.

- The runner opens every zone of the step that fits under the cap. A zone that
  does not fit, or whose close is pending a retry, waits and opens when a slot
  frees and its close has landed, then runs the step's full duration from its
  own open. Waiting zones open in ascending zone number.
- A step completes when every one of its zones has opened and closed. A
  disabled zone is skipped; a step of only disabled zones completes at once.
- A zone already open when its step starts (a manual run) is taken over: its
  deadline becomes now plus the step duration.
- A per-zone stop of a step zone counts as that zone finishing its share of the
  step. While a program is running, an all-off or watchdog close aborts it.
- A program's steps are read once, at start. A read failure, or a failed open
  in the first step, aborts the program before `ProgramQueue` records the
  firing, so the firing is recorded `failed`.

One program runs at a time. `ProgramQueue` holds the rest, first in, first out,
at most one entry per program; a running program may hold one queued entry. A
start time that comes due while its program is already queued is recorded
`skipped_duplicate`. A manual "run program now" joins the same queue and is
refused while that program is running or queued. When an entry reaches the
head, the master enable and, for scheduled entries, the rain delay are checked
again: master off records `skipped_disabled`, a rain delay records
`skipped_rain`. The queue lives in memory; entries lost to a restart are
recorded `dropped_restart` at the next startup.

A manual zone run never queues and never stops the running program: it opens
alongside it if a slot is free and is refused otherwise.

### 5.6 Stop button

`InputPin` on BCM 25 with pull-up bias, falling-edge detection, and 20ms
kernel-side debounce. A press empties the program queue (each scheduled entry
recorded `dropped_stop`), aborts the running program, and calls
`ZoneController::allOff()`, in that order. While the button is held every open
request is refused and every program that comes due is recorded `skipped_stop`.

## 6. Data model

SQLite via `KanoopDatabaseQt`'s `DataSource`. `IrrigationDataSource` supplies the
versioning layer in the shape of `EpcDataSource`: the compiled version is a
constant in `IrrigationD/CMakeLists.txt`, the version last migrated to is stored
in an info table in the database, and scripts live at
`src/database/migrate/irrigation/<version>/NN-<name>.sql`, registered in a `.qrc`.

Every schema change touches two paths that must stay in sync: the fresh-install
script `schema.sql` and the migration script. Updating only one produces a
database that looks healthy until the first query against the column that is
missing from it.

A migration that throws renames the database to `<file>.<utc>.backup` and
recreates it from `schema.sql`. Test each migration against a populated database
before it ships.

```
zones                 id, number (1-8), name, enabled

programs              id, name, enabled, day_mode, dow_mask,
                      interval_days, anchor_date

program_start_times   id, program_id, minutes_after_midnight, timezone

program_steps         id, program_id, sequence, duration_seconds

program_step_zones    id, step_id, zone_id

fired_instants        id, program_id, start_time_id,
                      scheduled_at_utc, outcome

settings              key, value
```

`outcome` is one of `queued`, `ran`, `skipped_rain`, `skipped_stop`,
`skipped_duplicate`, `skipped_disabled`, `dropped_stop`, `dropped_restart`,
`missed`, `failed`. A due firing is inserted `queued` and updated to its final
outcome when it starts or leaves the queue. `skipped_busy` appears only in rows
written before schema 1.1.0.

`scheduled_at_utc` is an ISO-8601 UTC string. The unique key over
(`program_id`, `start_time_id`, `scheduled_at_utc`) is what makes firing
idempotent.

`fired_instants` rows older than 90 days are pruned at startup.

Settings keys: `rain_delay_until` (UTC), `master_enabled`, `max_zone_seconds`,
`log_level`, `max_concurrent_zones` (1–8, default 2).

Zone-to-GPIO mapping lives in the daemon's INI settings rather than the
database. It describes the wiring of a particular box, so changing it must not
require a schema migration.

Schema 1.1.0 replaced `program_zones` with `program_steps` and
`program_step_zones`; its migration turned each `program_zones` row into a
one-zone step with the same id, sequence and duration, dropping any row whose
program or zone no longer existed.

Storage durability: `journal_mode=WAL`, `synchronous=FULL`, and `/var/log` on
tmpfs. The device is powered from an unswitched outlet and will lose power
mid-write.

## 7. REST API

Served by `QHttpServer` on `127.0.0.1:8080`. nginx proxies `/api/*` to
`/admin/*` and serves the frontend bundle from `/`.

```
GET    /admin/health
GET    /admin/version
GET    /admin/status
GET    /admin/zones
PUT    /admin/zones/{number}
POST   /admin/zones/{number}/run      { "seconds": N }
POST   /admin/zones/{number}/stop
GET    /admin/programs
POST   /admin/programs
PUT    /admin/programs/{id}
DELETE /admin/programs/{id}
POST   /admin/programs/{id}/run
POST   /admin/stop
GET    /admin/settings
PUT    /admin/settings
```

Run requests are decided on the thread that owns the valves and the reply waits
for the decision, bounded at five seconds: 202 when accepted, 409 with a
`reason` of `cap_reached`, `stop_held`, `master_disabled`, `zone_disabled` or
`already_queued`, 500 when the open failed, 503 when no decision arrived.

`POST /admin/zones/{number}/stop` closes that zone and also aborts the running
program and empties the queue, recording `dropped_stop`; other manual and panel
zones stay open.

Program bodies carry `steps: [{ zones: [zoneId…], durationSeconds }]`;
responses add each step's stored `id`.

`/admin/status` is the UI's polling endpoint. It returns `running` (one entry
per open zone: zone number, seconds remaining, `program` or `manual`),
`program` (id, name, step, step count, waiting zone numbers, or `null`),
`queue` (program id, name, queued-at instant), `maxConcurrentZones`, the next
scheduled occurrence, the rain delay, the master enable, whether STOP is held,
and the controller's timezone identifier alongside its UTC timestamps.

## 8. Web interface

React 19 + Vite + TypeScript in `web/`, built to `web/dist/`.

Three screens:

- **Now** — a Running card, shown only while something is running or queued
  or the status is unknown: one row per open zone with its countdown, a
  program/manual/panel tag and its own Stop, always enabled; the running
  program's step and waiting zones; the queued programs; and a Stop all only
  when no row offers a Stop (status unknown, or a program with no zone open).
  Below it, the next scheduled run and eight zone tiles: a closed tile's
  button reads Run, for a quick manual open, and is disabled at the cap with
  an "N of N running" hint; an open tile glows and reads Running with no
  button. A refused run shows the daemon's reason.
  Mobile-first with large touch targets and high contrast for outdoor
  readability.
- **Programs** — list, create, edit. Day rule, start times, ordered steps of
  zone chips with one duration each, a "runs in waves" warning on a step with
  more zones than the cap, computed total runtime assuming waves, next run.
- **Settings** — rain delay, master enable, maximum zone runtime, max zones at
  once, zone names.

The UI polls `/admin/status` every 2s while a zone is open,
a program runs, or a program waits; every 15s otherwise.

All times render in the **controller's** timezone as reported by
`/admin/status`, not the viewing browser's.

## 9. Yocto layer and deployment

`~/src/punak/rpi/meta-rpi4-irrigation`, sibling to `meta-rpi4-gateway`, with
`kas/rpi4-irrigation.yml`.

Recipes:

| Recipe | Purpose |
|---|---|
| `irrigationd_1.0.bb` | Cross-builds the daemon. `DEPENDS = "qtbase qthttpserver libgpiod"`, `gitsm://` `SRC_URI` with pinned `SRCREV` |
| `irrigation-web_1.0.bb` | Installs `web/dist` to `/var/www/irrigation/html`, fails the build if the bundle is absent |
| `nginx-irrigation-config_1.0.bb` | Static bundle plus `/api` reverse proxy |
| `irrigation-init_1.0.bb` | `config.txt` fragment, systemd unit, `/var/lib/irrigationd` |
| `rpi4-irrigation-image.bb` | `core-image-base` plus the above, avahi, sqlite3, tzdata, wpa-supplicant, SSH |

`PREFERRED_VERSION_libgpiod = "2.2.2"`. meta-oe carries 1.6.5 and 2.2.2 side by
side and their APIs are not compatible.

The `SRC_URI` uses `gitsm://` rather than `git://`. The Kanoop libraries are
consumed as submodules through `add_subdirectory`, so a plain git fetch produces
a source tree that configures and then fails to link.

```
PACKAGECONFIG:append:pn-qtbase = " sql-sqlite"
```

meta-qt6 enables the SQLite driver only through `PACKAGECONFIG_KDE`, which is
gated on the `kde` distro feature. A headless image does not set it, so qtbase
builds with no SQL drivers at all and `QSqlDatabase::addDatabase("QSQLITE")`
fails at runtime with a clean build and no warning.

`config.txt` fragment:

```
dtparam=i2c_arm=on
dtoverlay=i2c-rtc,ds3231
gpio=5,6,12,13,16,19,20,21=op,dh
```

The `gpio=` line holds the relay inputs de-asserted from the bootloader onward.
Removing it opens every valve at power-up.

Development loop: build the Qt6 SDK once with the existing `build-sdk.sh`,
cross-compile `irrigationd` on the development host against the SDK sysroot,
rsync the binary to the target, and restart the unit. Full image rebuilds are
needed only when the layer or image contents change.

## 10. Testing

Test-driven, following the project convention.

- `MockBackend` lets `ZoneController` tests assert exact line states for every
  transition.
- `Scheduler` takes an injected clock, so day rules, start times, DST
  transitions, and grace-window behaviour are exercised across simulated months
  without waiting.
- `ProgramRunner` is tested against a fake `ZoneController`.

The safety invariants get dedicated tests: duration clamping, the concurrency
cap, per-zone deadlines, the watchdog's set comparison and count check, and
`allOff()` aborting an active program.

## 11. Deferred

Weather-aware skip and runtime scaling; run history and reporting built on
`fired_instants`; flow sensing and leak detection; master valve or pump-start
output; read-only rootfs with a writable overlay; `sd_notify` watchdog
integration with systemd.
