# Gardener Panel Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the gardener run, advance and stop zones at the box with the RUN button and see what the controller is doing on the TM1637 display, with no phone and no network.

**Architecture:** A `PanelController` holds the RUN state machine and the frame it shows. It touches no GPIO and owns no timer: the daemon feeds it RUN line changes from a `RunButton` and calls `tick()` every 100 ms; it reads the controller through an `IPanelHost` snapshot, times everything on a monotonic clock from `IClock`, and emits each new frame once. `PanelHost` is the daemon-side `IPanelHost`: it decides refusals exactly as a manual run does, opens and swaps panel zones on the zone controller, and clears the controller with the same `clearController()` the STOP path now uses. `Tm1637Display` bit-bangs frames over two push-pull outputs through KanoopPiQt's `OutputBank`, `PanelFormat` builds the segment bytes, and the daemon executable gains a one-shot `--dashes` mode that the unit's stop-post hook runs.

**Tech Stack:** Qt 6.10 (Core, Network, HttpServer, Sql, Test), CMake, SQLite through `KanoopDatabaseQt`, libgpiod 2.x through `KanoopPiQt`; React 19, Vite 7, TypeScript 5.9 (strict), Vitest 3 + Testing Library; Yocto walnascar (kas) for the image layer.

**Spec:** docs/design/2026-10-03-gardener-panel-design.md — binding. It amends `docs/design/2026-09-05-irrigation-design.md` and builds on `docs/design/2026-10-01-concurrent-zones-design.md`; where they disagree the panel spec wins for the panel. Executors read the spec alongside this plan.

---

## Global Constraints

These apply to every task. A task's requirements implicitly include this section.

- **Repositories.** `~/src/punak/irrigation`, branch `feature/superproject`: daemon under `IrrigationD/`, web under `web/`, docs under `docs/`. `~/src/punak/rpi`: the Yocto layer `meta-rpi4-irrigation` and `kas/rpi4-irrigation.yml`. The two repositories are committed separately; no commit spans both. Do not touch the Kanoop submodules.
- **The rpi tree carries unrelated uncommitted work** (`gateway-admin/frontend/src/pages/Login.tsx`, `meta-rpi4-gateway/.../gateway-ssl-init.sh` at the time of writing). Stage only the files a task names, by path.
- **Never `git push`.** Commits stay local. The owner controls every remote push and every image flash. This plan never flashes a card.
- **Never discard uncommitted changes.** No `git checkout --`, `git restore`, `git clean -f`, `git reset --hard`. The owner edits these trees in Qt Creator and vim while you work: re-read a file immediately before editing it, and before staging a file run `git diff --stat <file>` — if the line count dwarfs your edit, stop and ask.
- **Tests come at the end.** Implementation tasks (1–10) write no tests. Dedicated test tasks (12–15) come after the whole-branch review task (11) and its fix wave. Implementation tasks must leave the tree building: `cmake --build build -j 32` exits zero and `npm --prefix web run typecheck` exits zero. Existing tests that go red because behaviour changed on purpose stay red until the test task that owns them; each task lists which. When a change would stop an existing test file compiling, the task applies the minimal mechanical edit that keeps it compiling. Assertion fixes belong to the test tasks.
- **Builds.** Host daemon build: `cmake --build build -j 32`; tests: `ctest --test-dir build --output-on-failure -R <name>`. If `build/` is not configured, configure it first with `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON`. Web: `npm --prefix web run typecheck`, `npm --prefix web test`, `npm --prefix web run build`.
- **C++ style** per `meta-qt-mains/.claude/docs/codestyle-cpp.md` and the surrounding code: `_underscorePrefixed` members, `camelCase` methods, `PascalCase` classes, `if(` with no space, `== false` instead of `!`, function brace on its own line, control brace on the same line, `catch`/`else` on their own line. Primitive, enum and raw-pointer members get an in-class initializer. Global namespace; enum values scoped inside their holder class. Every `.cpp` whose header declares `Q_OBJECT` ends with its moc include. `-Wextra -Wall -Werror`. Write valid C++17. The code model is `meta-qt-mains`; never model on `kanooptorrentd`.
- **Doxygen on every public member of every new class**, including fields of new value classes (`///<`) and the overrides of `IPanelHost`.
- **Qt connections.** Never pass `Qt::QueuedConnection` to `connect()` or `QMetaObject::invokeMethod()`. `Qt::AutoConnection` already queues across threads. No comment justifies a connection type.
- **Comments state traps only.** A comment stays when it states something the code cannot show and a plausible edit would silently break: a wire contract, an ordering constraint, a lifetime rule, a hardware contract. No design rationale, no pattern names, no "the old code did X", no pointers to other code as justification. History goes in commit messages. This applies to unit files, udev rules, INI files and kas YAML as much as to C++.
- **No "it's X, not Y" antithesis** in code, comments, commit messages or docs. Delete the negated clause; if the sentence still says everything, the construction was doing no work.
- **Schema changes** touch `schema.sql` and a new migration script together, register the script in `irrigation.qrc`, and bump `IRRIGATION_DB_VERSION` in `IrrigationD/CMakeLists.txt`.
- **Web:** every network call goes through `web/src/api/client.ts`; `npm run typecheck` is the `-Werror`.
- **Commit per task.** Conventional commits (`feat`, `fix`, `refactor`, `test`, `doc`, `style`), every message ending with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
  ```

---

## Review Focus

The five input classes most likely to bite the gardener that the spec implies without testing, and the test that pins each.

1. **An advance at the RUN button that is refused must leave the current zone running, and an advance at a cap of 1 must still swap zones.** A host that closes the panel zone before deciding the refusal leaves the gardener standing in a dry yard when watering was turned off or the cap is full; a host that opens before closing can never advance at a cap of 1. Pinned by `TestPanelHost::aRefusedAdvanceLeavesTheReplacedZoneOpen`, `TestPanelHost::anAdvanceSwapsZonesAtACapOfOne` (Task 14) and `TestPanelController::aRefusedAdvanceKeepsTheCurrentZoneRunning` (Task 13).
2. **The system clock steps while a selection is pending.** The RTC hands over to NTP after boot and either can step the wall clock. A commit timed on wall time either fires at once or waits an hour. Pinned by `TestPanelController::aWallClockStepDoesNotMoveTheCommit` (Task 13).
3. **A press lands after the panel zone's timer ended but before a tick saw it.** Without re-reading the controller on the press, the panel "advances" a run that already ended and opens the next zone by itself. Pinned by `TestPanelController::aPressJustAfterThePanelZoneClosedStartsASelection` (Task 13).
4. **A press that cannot start anything while a program or an app run is watering.** Master enable off or no zone enabled must show `OFF` or `nonE` and stop nothing; taking over first would kill a program the gardener cannot replace. Pinned by `TestPanelController::aRefusedPressTakesNothingOver` (Task 13).
5. **A zone disabled in the app between the selection and its commit.** The commit must open nothing, and the host must refuse a disabled zone on its own. Pinned by `TestPanelController::aZoneDisabledBeforeTheCommitOpensNothing` (Task 13) and `TestPanelHost::refusalsComeInTheManualRunOrderAndOpenNothing` (Task 14).

---

## File Structure

| Path | Change | Responsibility |
|---|---|---|
| `IrrigationD/src/iclock.h` | Modify | Adds a monotonic millisecond count to `IClock`, `SystemClock` and `TestClock`. |
| `IrrigationD/src/panelformat.{h,cpp}` | Create | Segment bytes: the clock, zone-and-minutes, the glyph words, `----`. |
| `IrrigationD/src/tm1637display.{h,cpp}` | Create | TM1637 framing over two push-pull outputs through `OutputBank`. |
| `IrrigationD/src/runbutton.{h,cpp}` | Create | Requests the RUN line and reports its level changes. |
| `IrrigationD/src/ipanelhost.h` | Create | `PanelSnapshot` and the `IPanelHost` interface the panel calls. |
| `IrrigationD/src/panelcontroller.{h,cpp}` | Create | The RUN state machine, the stuck-line rule and the frame. |
| `IrrigationD/src/panelhost.{h,cpp}` | Create | The daemon's `IPanelHost`: refusals, open, swap, close, and `clearController()`. |
| `IrrigationD/src/irrigationsettings.{h,cpp}` | Modify | `runButtonOffset`, `displayClockOffset`, `displayDataOffset`. |
| `IrrigationD/src/database/schema.sql` | Modify | `panel_run_minutes` default. |
| `IrrigationD/src/database/migrate/irrigation/1.2.0/01-panel-run-minutes.sql` | Create | Adds the default to an existing database. |
| `IrrigationD/src/database/irrigation.qrc`, `IrrigationD/CMakeLists.txt` | Modify | Register the script, bump to 1.2.0, new sources. |
| `IrrigationD/src/irrigationcontrolserver.{h,cpp}` | Modify | `panel_run_minutes` key and validation; `RunningZoneStatus::Source`. |
| `IrrigationD/src/json/statusjson.cpp` | Modify | `source: "panel"`. |
| `IrrigationD/src/irrigationdaemon.{h,cpp}` | Modify | Builds and wires the panel, routes STOP through `clearController()`, tags panel zones in the status. |
| `IrrigationD/src/main.cpp` | Modify | `--dashes` one-shot mode. |
| `IrrigationD/systemd/irrigationd.service` | Modify | The repository's own unit copy: no network wait, stop-post hook. |
| `IrrigationD/tests/*` | Modify / Create | Task 6 compile edit; Tasks 12–14 tests; new `tst_panelformat`, `tst_tm1637display`, `tst_runbutton`, `tst_panelcontroller`, `tst_panelhost`. |
| `web/src/api/{types,decode}.ts` | Modify | `RunSource` gains `'panel'`. |
| `web/src/screens/NowScreen.tsx`, `web/src/components/ZoneTile.tsx`, `web/src/styles/app.css` | Modify | "Panel" tag on rows and tiles. |
| `web/src/settings/settingsMap.ts`, `web/src/screens/SettingsScreen.tsx` | Modify | "Gardener panel run time (minutes)". |
| `web/src/test/fixtures.ts`, `web/src/**/*.test.ts(x)` | Modify | Task 15. |
| rpi `meta-rpi4-irrigation/recipes-core/irrigation-init/files/irrigationd.service` | Modify | Order after the RTC device, drop the network wait, stop-post hook. |
| rpi `meta-rpi4-irrigation/recipes-core/irrigation-init/files/99-irrigation-rtc.rules` | Create | Tags `rtc0` for systemd so `dev-rtc0.device` exists. |
| rpi `meta-rpi4-irrigation/recipes-core/irrigation-init/files/dev-rtc0-timeout.conf` | Create | Bounds the wait for an RTC that never appears. |
| rpi `meta-rpi4-irrigation/recipes-core/irrigation-init/irrigation-init_1.0.bb` | Modify | Installs the rule and the drop-in. |
| rpi `meta-rpi4-irrigation/recipes-core/irrigation-init/files/irrigationd.ini` | Modify | The three panel keys. |
| rpi `kas/rpi4-irrigation.yml` | Modify | `gpio=24=ip,pu`; `pu` on the zone line. |
| rpi `meta-rpi4-irrigation/recipes-images/images/rpi4-irrigation-image.bb` | Modify | Guard checks both lines. |

---

## Wire and configuration contract this plan builds

- `GET /admin/status`: `running[].source` is one of `"program"`, `"manual"`, `"panel"`. A zone the running program owns is `"program"` even when the panel opened it first. Nothing about the display is exposed.
- `GET`/`PUT /admin/settings`: new key `panel_run_minutes`, a string integer 1–60, default `"10"`.
- `irrigationd.ini` `[gpio]`: `runButtonOffset`, `displayClockOffset`, `displayDataOffset`, each a non-negative integer line offset. A missing or malformed key reads as absent: no `runButtonOffset` disables RUN; either display key absent runs the panel without its display. Each is logged at startup.
- `irrigationd --config <ini> --dashes`: requests only the two display lines, writes `----`, releases them, and exits 0. With no display keys it exits 0 having written nothing; a failed chip open, request or write exits 1.

---

## Rulings on spec gaps

Decisions the spec leaves open. Each task implements them.

1. **The panel logic is its own class, driven by snapshots plus presses with a test clock.** `PanelController` holds no timer and no GPIO. It pulls a `PanelSnapshot` from `IPanelHost` on every press and every tick, and asks the host synchronously to open, swap, close or take over; both live on the daemon thread, so every call returns its decision. `IClock` gains `monotonicMsecs()`: every interval (the 3 s commit, 2 s messages, 2 s alternation, 10 s stuck line, the selection blink) runs on it, and only the clock face reads wall time.
2. **STOP and the takeover share one sequence.** `PanelHost::clearController()` holds `dropAll(DroppedStop)` → `abort()` → `allOff()`. `IrrigationDaemon::onStopPressed()` (physical STOP and API STOP) and `PanelHost::takeOverForPanel()` both call it, so the takeover is exactly an API STOP.
3. **An advance decides every refusal before it closes anything,** counting the panel zone's slot as free. On a refusal the current zone keeps running and the display shows the refusal. When a slot is free the new zone opens first and the old one closes after; at a full cap the old one closes first. A waiting program zone that grabs the slot the close freed (ProgramRunner refills from `zoneClosed` synchronously) answers `CapReached`: the display shows `FuLL` and the panel run ends, because the panel zone already closed.
4. **A press that can start nothing stops nothing.** With master enable off the press shows `OFF`; with no zone enabled it shows `nonE`; in both cases no takeover happens. Takeover follows only once a selection can be offered.
5. **A press during a run with master enable off shows `OFF` and leaves the panel zone running.** A press on the last enabled zone closes it regardless, since closing is never refused.
6. **Refusals with no glyph in §3.5** (`ZoneDisabled`, `Failed`, `StopHeld`) show no message. `StOP` and `Err` already appear from the controller state; a zone disabled between the selection and its commit ends the selection silently.
7. **The selection's minutes are the run time as it will open:** `panel_run_minutes` clamped by the zone-duration ceiling, rounded up to whole minutes. The countdown shows minutes left rounded up, never below 1 while the zone is open, and never above 99.
8. **Three-letter words are left-aligned** with a blank right digit (`Err `, `OFF `). The colon is lit only on the clock face.
9. **Blink phases.** The clock colon is lit for the first half of each wall-clock second. The selection's zone digit is lit for the first half second after each press and every second after that, so a press always shows its zone at once.
10. **A STOP press cancels a pending selection and forgets the panel run** through `PanelController::cancel()`, called from `onStopPressed()` ahead of `clearController()`. A STOP tap shorter than a tick still cancels. A held STOP or a fault seen by a tick also cancels a pending selection.
11. **A low edge while the line already reads low is no press.** That is what makes "no further presses until it has been seen high again" concrete for a bouncing or stuck line. The ten-second stuck log fires once per low period, including a line low at startup.
12. **RUN is non-fatal.** A missing `runButtonOffset` or a failed RUN line request is logged and the daemon runs without RUN, the same as a missing display. STOP stays fatal.
13. **A failed display write is logged once and retried on every later frame;** the first success after a failure logs that the display is writing again. The clock colon changes every half second, so a recovered display repaints within one.
14. **The display uses KanoopPiQt's `OutputBank`.** `LibGpiodBackend::requestOutputs` sets no drive, and libgpiod's default drive is push-pull, which is what the bench proved. No backend change is needed. Each line change waits about 10 µs by spinning on `std::chrono::steady_clock`; `nanosleep`'s timer slack would stretch a frame from about 3 ms to about 20 ms on the valve thread.
15. **The panel ticks every 100 ms** and reads a full snapshot each time, including two small SQLite reads (`allZones`, `master_enabled`). Frames are emitted only when their bytes change.
16. **The tile tag appears for panel runs only.** The rows already carry Program/Manual tags; tiles carry none today, and the spec asks for the "panel" tag on tiles.
17. **`dev-rtc0.device` needs a udev rule.** No shipped udev rule tags rtc devices for systemd: on the controller (2026-10-03) `dev-rtc0.device` reads `ActiveState=inactive` with an empty `SysFSPath` while `/sys/class/rtc/rtc0/hctosys` is `1`. Without the tag, `After=` alone orders against a unit that never runs (a no-op), and `Wants=` waits out the 90 s device timeout on every boot. The layer adds `99-irrigation-rtc.rules` (`SUBSYSTEM=="rtc", KERNEL=="rtc0", TAG+="systemd"`) and the unit takes `Wants=` plus `After=` on `dev-rtc0.device`. The kernel sets the system clock while rtc0 registers, before udev sees the device, so the device unit going active means the clock is already right.
18. **A missing RTC delays the daemon by at most 20 s.** A `dev-rtc0.device.d` drop-in sets `JobRunningTimeoutSec=20s`. If the device never appears, its start job fails at the timeout, and because the dependency is `Wants=`, irrigationd starts anyway on the time-set clock. Released relay lines are safe meanwhile: 10k pull-ups are fitted and `pu` holds each pad high. `time-sync.target` stays in `After=`; it is reached at 5.5 s on this image and costs nothing.
19. **The stop-post hook is `ExecStopPost=-/usr/bin/irrigationd --config /etc/irrigationd.ini --dashes`.** systemd runs `ExecStopPost=` after every stop of the main process, a crash or `kill -9` included, and the leading `-` keeps a failed clear from marking the unit failed.
20. **The image keeps its `SRCREV` in this plan.** The rpi task does not bump `irrigationd_1.0.bb`'s `SRCREV`: that needs this branch pushed, which is the owner's call. An image built before the bump pairs the new unit with the old binary; its `--dashes` call fails on an unknown option and the `-` prefix ignores it, and the old binary ignores the new INI keys.
21. **Advancing onto a zone already open from the app** re-opens it for the panel run time (the controller resets its deadline) and it becomes the panel zone. If the running program owns that zone, the status still says `"program"`.

---
### Task 1: A monotonic clock and the display's segment bytes

Spec §3.1, §3.3, §3.5, §6.2. `IClock` gains a monotonic count for the panel's intervals; `PanelFormat` turns every view the panel shows into four segment bytes.

**Files:**
- Modify: `IrrigationD/src/iclock.h`
- Create: `IrrigationD/src/panelformat.h`, `IrrigationD/src/panelformat.cpp`
- Modify: `IrrigationD/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing new.
- Produces:
  - `virtual qint64 IClock::monotonicMsecs() const = 0;`
  - `void TestClock::advanceMsecs(qint64 msecs);` — moves wall time and the monotonic count together. `TestClock::advance(qint64 seconds)` keeps its meaning; `TestClock::setNowUtc()` moves wall time only, as a system clock step does.
  - `class PanelFormat` (static only): `SegA = 0x01` … `SegG = 0x40`, `Colon = 0x80`, `DigitCount = 4`; `static QByteArray clock(const QTime& time, bool colonOn);` `static QByteArray zoneMinutes(int zoneNumber, int minutes, bool zoneVisible);` `static int minutesLeft(int seconds);` `static QByteArray text(const QString& glyphs);` `static quint8 digit(int value);` and the words `stopHeld()` (`StOP`), `fault()` (`Err`), `masterOff()` (`OFF`), `full()` (`FuLL`), `noZones()` (`nonE`), `dashes()` (`----`).

- [ ] **Step 1: Replace `IrrigationD/src/iclock.h`**

```cpp
#ifndef ICLOCK_H
#define ICLOCK_H

#include <QDateTime>
#include <QElapsedTimer>

/** @brief Source of the current instant and of a millisecond count that never steps. */
class IClock
{
public:
    virtual ~IClock() {}

    /** @brief Returns the current instant in UTC. */
    virtual QDateTime nowUtc() const = 0;

    /**
     * @brief Returns milliseconds on a clock that never steps.
     *
     * @warning Measure intervals with this. nowUtc() steps whenever the RTC or NTP
     *          corrects the system clock.
     */
    virtual qint64 monotonicMsecs() const = 0;
};

/** @brief Clock backed by the system time. */
class SystemClock : public IClock
{
public:
    /** @brief Constructs a clock whose monotonic count starts at zero now. */
    SystemClock() { _elapsed.start(); }

    /** @brief Returns the system time in UTC. */
    virtual QDateTime nowUtc() const override { return QDateTime::currentDateTimeUtc(); }

    /** @brief Returns the milliseconds since construction on the system's monotonic clock. */
    virtual qint64 monotonicMsecs() const override { return _elapsed.elapsed(); }

private:
    QElapsedTimer _elapsed;
};

/** @brief Clock the test drives by hand. */
class TestClock : public IClock
{
public:
    /** @brief Constructs a clock reading @p start, with a monotonic count of zero. */
    explicit TestClock(const QDateTime& start) : _now(start.toUTC()) {}

    /** @brief Returns the instant this clock was set or advanced to. */
    virtual QDateTime nowUtc() const override { return _now; }

    /** @brief Returns the milliseconds this clock has been advanced by. */
    virtual qint64 monotonicMsecs() const override { return _monotonicMsecs; }

    /** @brief Sets the instant this clock reports and leaves the monotonic count alone, as a system clock step does. */
    void setNowUtc(const QDateTime& value) { _now = value.toUTC(); }

    /** @brief Moves the wall time and the monotonic count forward by @p seconds. */
    void advance(qint64 seconds) { advanceMsecs(seconds * 1000); }

    /** @brief Moves the wall time and the monotonic count forward by @p msecs. */
    void advanceMsecs(qint64 msecs)
    {
        _now = _now.addMSecs(msecs);
        _monotonicMsecs += msecs;
    }

private:
    QDateTime _now;
    qint64 _monotonicMsecs = 0;
};

#endif // ICLOCK_H
```

- [ ] **Step 2: Create `IrrigationD/src/panelformat.h`**

```cpp
#ifndef PANELFORMAT_H
#define PANELFORMAT_H

#include <QByteArray>
#include <QString>
#include <QTime>

/**
 * @brief Builds the four segment bytes the TM1637 shows, digit 0 leftmost.
 *
 * Bit 0 is segment a through bit 6 segment g. Bit 7 of digit 1 lights the centre colon.
 */
class PanelFormat
{
public:
    static constexpr quint8 SegA = 0x01;    ///< Top.
    static constexpr quint8 SegB = 0x02;    ///< Upper right.
    static constexpr quint8 SegC = 0x04;    ///< Lower right.
    static constexpr quint8 SegD = 0x08;    ///< Bottom.
    static constexpr quint8 SegE = 0x10;    ///< Lower left.
    static constexpr quint8 SegF = 0x20;    ///< Upper left.
    static constexpr quint8 SegG = 0x40;    ///< Middle.
    static constexpr quint8 Colon = 0x80;   ///< The centre colon, carried by digit 1.

    /** @brief The number of digits on the module. */
    static constexpr int DigitCount = 4;

    /**
     * @brief Returns @p time as a 12-hour h:mm.
     *
     * The leading digit is blank before 10 o'clock; midnight and noon read 12. The colon
     * is lit when @p colonOn is true.
     */
    static QByteArray clock(const QTime& time, bool colonOn);

    /**
     * @brief Returns @p zoneNumber in the left digit and @p minutes right-aligned in the right two.
     *
     * The second digit and the colon stay dark. The zone digit is blank when @p zoneVisible
     * is false. @p minutes is bounded to 0 through 99.
     */
    static QByteArray zoneMinutes(int zoneNumber, int minutes, bool zoneVisible);

    /** @brief Returns the whole minutes in @p seconds rounded up, and 1 for anything below one second. */
    static int minutesLeft(int seconds);

    /** @brief Returns @p glyphs left-aligned and padded with blanks to four digits. A character without a glyph shows blank. */
    static QByteArray text(const QString& glyphs);

    /** @brief Returns the segment byte for the decimal digit @p value, bounded to 0 through 9. */
    static quint8 digit(int value);

    /** @brief Returns StOP. */
    static QByteArray stopHeld() { return text("StOP"); }

    /** @brief Returns Err. */
    static QByteArray fault() { return text("Err"); }

    /** @brief Returns OFF. */
    static QByteArray masterOff() { return text("OFF"); }

    /** @brief Returns FuLL. */
    static QByteArray full() { return text("FuLL"); }

    /** @brief Returns nonE. */
    static QByteArray noZones() { return text("nonE"); }

    /** @brief Returns ----. */
    static QByteArray dashes() { return text("----"); }

private:
    static quint8 glyph(QChar character);
};

#endif // PANELFORMAT_H
```

- [ ] **Step 3: Create `IrrigationD/src/panelformat.cpp`**

```cpp
#include "panelformat.h"

QByteArray PanelFormat::clock(const QTime& time, bool colonOn)
{
    int hour = time.hour() % 12;
    if(hour == 0) {
        hour = 12;
    }

    QByteArray result(DigitCount, '\0');
    result[0] = static_cast<char>(hour >= 10 ? digit(hour / 10) : 0);
    result[1] = static_cast<char>(digit(hour % 10) | (colonOn == true ? Colon : 0));
    result[2] = static_cast<char>(digit(time.minute() / 10));
    result[3] = static_cast<char>(digit(time.minute() % 10));
    return result;
}

QByteArray PanelFormat::zoneMinutes(int zoneNumber, int minutes, bool zoneVisible)
{
    const int bounded = qBound(0, minutes, 99);

    QByteArray result(DigitCount, '\0');
    result[0] = static_cast<char>(zoneVisible == true ? digit(zoneNumber % 10) : 0);
    result[2] = static_cast<char>(bounded >= 10 ? digit(bounded / 10) : 0);
    result[3] = static_cast<char>(digit(bounded % 10));
    return result;
}

int PanelFormat::minutesLeft(int seconds)
{
    return seconds <= 0 ? 1 : (seconds + 59) / 60;
}

QByteArray PanelFormat::text(const QString& glyphs)
{
    QByteArray result(DigitCount, '\0');
    for(int i = 0; i < DigitCount && i < glyphs.length(); i++) {
        result[i] = static_cast<char>(glyph(glyphs.at(i)));
    }
    return result;
}

quint8 PanelFormat::digit(int value)
{
    static const quint8 digits[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    return digits[qBound(0, value, 9)];
}

quint8 PanelFormat::glyph(QChar character)
{
    if(character.isDigit() == true) {
        return digit(character.digitValue());
    }

    switch(character.unicode()) {
    case '-':
        return SegG;
    case 'E':
        return SegA | SegD | SegE | SegF | SegG;
    case 'F':
        return SegA | SegE | SegF | SegG;
    case 'L':
        return SegD | SegE | SegF;
    case 'O':
        return SegA | SegB | SegC | SegD | SegE | SegF;
    case 'P':
        return SegA | SegB | SegE | SegF | SegG;
    case 'S':
        return SegA | SegC | SegD | SegF | SegG;
    case 'n':
        return SegC | SegE | SegG;
    case 'o':
        return SegC | SegD | SegE | SegG;
    case 'r':
        return SegE | SegG;
    case 't':
        return SegD | SegE | SegF | SegG;
    case 'u':
        return SegC | SegD | SegE;
    default:
        return 0;
    }
}
```

- [ ] **Step 4: Add the sources to `IrrigationD/CMakeLists.txt`**

In `qt_add_executable(${PROJ} ...)`, after the line `src/stopbutton.h          src/stopbutton.cpp`, add:

```cmake
    src/panelformat.h         src/panelformat.cpp
```

- [ ] **Step 5: Build**

```bash
cmake --build build -j 32
```

Expected: exits zero. Every existing test still compiles: `TestClock`'s constructor and `advance()` are unchanged, and no test subclasses `IClock`. Run `ctest --test-dir build --output-on-failure -R 'tst_scheduler|tst_programqueue'` and confirm both pass.

- [ ] **Step 6: Commit**

```bash
git add IrrigationD/src/iclock.h IrrigationD/src/panelformat.h IrrigationD/src/panelformat.cpp IrrigationD/CMakeLists.txt
git commit -F - <<'EOF'
feat: format the gardener panel's display and add a monotonic clock

PanelFormat builds the TM1637's four segment bytes for the 12-hour
clock with its colon, the zone-and-minutes view, and the words StOP,
Err, OFF, FuLL, nonE and ----. IClock gains monotonicMsecs() so the
panel can time its intervals on a clock that never steps; TestClock
advances both clocks together and setNowUtc() models a clock step.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---

### Task 2: The TM1637 driver

Spec §2 (hardware contract and protocol) and §7. Two push-pull outputs, LSB first, about 10 µs per edge, DIO driven low through every acknowledge clock, brightness 4.

**Files:**
- Create: `IrrigationD/src/tm1637display.h`, `IrrigationD/src/tm1637display.cpp`
- Modify: `IrrigationD/CMakeLists.txt`

**Interfaces:**
- Consumes: `OutputBank(IGpioBackend*, const QString& consumer, const QList<quint32>& offsets, bool activeLow, QObject* parent)`, `OutputBank::request()`, `OutputBank::setValue(quint32, bool)`, `OutputBank::errorText()` from KanoopPiQt.
- Produces (on `Tm1637Display : QObject`):
  - `Tm1637Display(IGpioBackend* backend, quint32 clockOffset, quint32 dataOffset, QObject* parent = nullptr);`
  - `bool begin();` — requests `{clockOffset, dataOffset}` as outputs, consumer `irrigationd-display`, active-high, both low
  - `bool show(const QByteArray& segments);` — four bytes
  - `void setEdgeDelayMicroseconds(int value);` — zero in tests
  - `QString errorText() const;`
  - `static constexpr int Brightness = 4;`, `static constexpr int DefaultEdgeDelayMicroseconds = 10;`

- [ ] **Step 1: Create `IrrigationD/src/tm1637display.h`**

```cpp
#ifndef TM1637DISPLAY_H
#define TM1637DISPLAY_H

#include <QByteArray>
#include <QObject>
#include <QString>

#include <Kanoop/pi/outputbank.h>

/**
 * @brief Writes four segment bytes to a TM1637 over two GPIO outputs.
 *
 * Each update sends the data command, the address command with the four bytes, then
 * the display-control command with the fixed brightness, every byte least significant
 * bit first.
 *
 * @warning Both lines must be driven push-pull, and DIO must be driven low through
 *          every acknowledge clock. The module has no pull-up on CLK, and a DIO left
 *          to the module's own pull-up stayed blank on the bench.
 */
class Tm1637Display : public QObject
{
    Q_OBJECT
public:
    /** @brief The display-control brightness, 0 through 7. */
    static constexpr int Brightness = 4;

    /** @brief The pause after each line change unless setEdgeDelayMicroseconds() says otherwise. */
    static constexpr int DefaultEdgeDelayMicroseconds = 10;

    /**
     * @brief Constructs a display on @p clockOffset and @p dataOffset.
     * @param backend The GPIO backend. Must already have an open chip.
     * @param clockOffset The CLK line offset.
     * @param dataOffset The DIO line offset.
     * @param parent The Qt parent object.
     */
    Tm1637Display(IGpioBackend* backend, quint32 clockOffset, quint32 dataOffset, QObject* parent = nullptr);

    /** @brief Requests both lines as outputs, driven low. @return True on success. */
    bool begin();

    /**
     * @brief Shows @p segments, four bytes with digit 0 leftmost.
     * @return True when every line write succeeded. False for a byte count other than
     *         four, lines that are not requested, or a failed write; a failed write
     *         leaves the frame unfinished and the next show() starts a whole one.
     */
    bool show(const QByteArray& segments);

    /** @brief Sets the pause after each line change in microseconds. Zero removes it. */
    void setEdgeDelayMicroseconds(int value) { _edgeDelayMicroseconds = value; }

    /** @brief Returns the text of the most recent failure. */
    QString errorText() const { return _errorText; }

private:
    bool startCondition();
    bool stopCondition();
    bool writeByte(quint8 value);
    bool setLine(quint32 offset, bool high);
    void pause() const;

    static constexpr quint8 DataCommand = 0x40;
    static constexpr quint8 AddressCommand = 0xC0;
    static constexpr quint8 DisplayControlCommand = 0x88;

    IGpioBackend* _backend = nullptr;
    quint32 _clockOffset = 0;
    quint32 _dataOffset = 0;
    OutputBank* _bank = nullptr;
    int _edgeDelayMicroseconds = DefaultEdgeDelayMicroseconds;
    QString _errorText;
};

#endif // TM1637DISPLAY_H
```

- [ ] **Step 2: Create `IrrigationD/src/tm1637display.cpp`**

```cpp
#include "tm1637display.h"

#include <chrono>

Tm1637Display::Tm1637Display(IGpioBackend* backend, quint32 clockOffset, quint32 dataOffset, QObject* parent) :
    QObject(parent),
    _backend(backend),
    _clockOffset(clockOffset),
    _dataOffset(dataOffset)
{
}

bool Tm1637Display::begin()
{
    if(_bank != nullptr) {
        _errorText = QString("The display lines are already requested");
        return false;
    }

    _bank = new OutputBank(_backend, "irrigationd-display", { _clockOffset, _dataOffset }, false, this);
    if(_bank->request() == false) {
        _errorText = _bank->errorText();
        delete _bank;
        _bank = nullptr;
        return false;
    }

    return true;
}

bool Tm1637Display::show(const QByteArray& segments)
{
    if(_bank == nullptr) {
        _errorText = QString("The display lines are not requested");
        return false;
    }
    if(segments.size() != 4) {
        _errorText = QString("Expected 4 segment bytes, got %1").arg(segments.size());
        return false;
    }

    bool ok = startCondition() && writeByte(DataCommand) && stopCondition();
    ok = ok && startCondition() && writeByte(AddressCommand);
    for(int i = 0; ok == true && i < segments.size(); i++) {
        ok = writeByte(static_cast<quint8>(segments.at(i)));
    }
    ok = ok && stopCondition();
    ok = ok && startCondition() && writeByte(DisplayControlCommand | Brightness) && stopCondition();
    return ok;
}

bool Tm1637Display::startCondition()
{
    return setLine(_clockOffset, true) && setLine(_dataOffset, true)
        && setLine(_dataOffset, false) && setLine(_clockOffset, false);
}

bool Tm1637Display::stopCondition()
{
    return setLine(_clockOffset, false) && setLine(_dataOffset, false)
        && setLine(_clockOffset, true) && setLine(_dataOffset, true);
}

bool Tm1637Display::writeByte(quint8 value)
{
    for(int bit = 0; bit < 8; bit++) {
        if(setLine(_clockOffset, false) == false
           || setLine(_dataOffset, ((value >> bit) & 1) != 0) == false
           || setLine(_clockOffset, true) == false) {
            return false;
        }
    }

    // Acknowledge clock: DIO stays driven low so it never fights the chip's acknowledge pull-down.
    return setLine(_clockOffset, false) && setLine(_dataOffset, false)
        && setLine(_clockOffset, true) && setLine(_clockOffset, false);
}

bool Tm1637Display::setLine(quint32 offset, bool high)
{
    if(_bank->setValue(offset, high) == false) {
        _errorText = _bank->errorText();
        return false;
    }
    pause();
    return true;
}

void Tm1637Display::pause() const
{
    if(_edgeDelayMicroseconds <= 0) {
        return;
    }

    const std::chrono::steady_clock::time_point until =
        std::chrono::steady_clock::now() + std::chrono::microseconds(_edgeDelayMicroseconds);
    while(std::chrono::steady_clock::now() < until) {
    }
}

#include "moc_tm1637display.cpp"
```

- [ ] **Step 3: Add the sources to `IrrigationD/CMakeLists.txt`**

After the `src/panelformat.h` line Task 1 added:

```cmake
    src/tm1637display.h       src/tm1637display.cpp
```

- [ ] **Step 4: Build**

```bash
cmake --build build -j 32
```

Expected: exits zero.

- [ ] **Step 5: Commit**

```bash
git add IrrigationD/src/tm1637display.h IrrigationD/src/tm1637display.cpp IrrigationD/CMakeLists.txt
git commit -F - <<'EOF'
feat: drive the TM1637 display over two push-pull outputs

Tm1637Display requests CLK and DIO through KanoopPiQt's OutputBank,
which libgpiod drives push-pull, and sends each update as the data
command, the address command with four segment bytes, and the
display-control command at brightness 4, least significant bit first
with a 10 us pause per edge. DIO is driven low through every
acknowledge clock, the sequence the bench proved on this module.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---

### Task 3: The RUN button line

Spec §2 (RUN on BCM 24, active low, pull-up, 20 ms debounce) and §3.7. The class only reports the line; the press rules live in `PanelController` (Task 4).

**Files:**
- Create: `IrrigationD/src/runbutton.h`, `IrrigationD/src/runbutton.cpp`
- Modify: `IrrigationD/CMakeLists.txt`

**Interfaces:**
- Consumes: `InputPin` from KanoopPiQt, as `StopButton` uses it.
- Produces (on `RunButton : QObject`):
  - `RunButton(IGpioBackend* backend, quint32 offset, QObject* parent = nullptr);`
  - `bool begin();` — consumer `irrigationd-run`, active-low, pull-up, both edges, 20 ms debounce, reads the starting level
  - `bool isLow() const;`
  - `QString errorText() const;`
  - signal `void lineChanged(bool low);`

- [ ] **Step 1: Create `IrrigationD/src/runbutton.h`**

```cpp
#ifndef RUNBUTTON_H
#define RUNBUTTON_H

#include <QObject>
#include <QString>

#include <Kanoop/pi/inputpin.h>

/**
 * @brief Reports every level change of the gardener panel's RUN button line.
 *
 * The button is 1NO to ground, so a press drives the line low. Every edge is reported;
 * deciding which of them is a press is the caller's job.
 */
class RunButton : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief Constructs a RUN button over @p offset.
     * @param backend The GPIO backend.
     * @param offset The line offset.
     * @param parent The Qt parent object.
     */
    RunButton(IGpioBackend* backend, quint32 offset, QObject* parent = nullptr);

    /** @brief Requests the line and reads its starting level. @return True on success. */
    bool begin();

    /** @brief Returns whether the line reads low, tracked from its edges. */
    bool isLow() const { return _low; }

    /** @brief Returns the text of the most recent failure. */
    QString errorText() const { return _errorText; }

signals:
    /** @brief Emitted on every edge with the line's new level; @p low is true while the button is pressed. */
    void lineChanged(bool low);

private slots:
    void onAsserted();
    void onDeasserted();

private:
    IGpioBackend* _backend = nullptr;
    quint32 _offset = 0;
    InputPin* _pin = nullptr;
    bool _low = false;
    QString _errorText;
};

#endif // RUNBUTTON_H
```

- [ ] **Step 2: Create `IrrigationD/src/runbutton.cpp`**

```cpp
#include "runbutton.h"

RunButton::RunButton(IGpioBackend* backend, quint32 offset, QObject* parent) :
    QObject(parent),
    _backend(backend),
    _offset(offset)
{
}

bool RunButton::begin()
{
    if(_pin != nullptr) {
        _errorText = QString("The RUN button line is already requested");
        return false;
    }

    _pin = new InputPin(_backend, "irrigationd-run", _offset, this);
    // 1NO to ground: PullUp and activeLow together make a press assert.
    _pin->setActiveLow(true);
    _pin->setBias(Gpio::Bias::PullUp);
    _pin->setEdge(Gpio::Edge::Both);
    _pin->setDebounce(TimeSpan::fromMilliseconds(20));

    if(_pin->request() == false) {
        _errorText = _pin->errorText();
        delete _pin;
        _pin = nullptr;
        return false;
    }

    connect(_pin, &InputPin::asserted, this, &RunButton::onAsserted);
    connect(_pin, &InputPin::deasserted, this, &RunButton::onDeasserted);

    bool ok = false;
    _low = _pin->isAsserted(&ok);
    if(ok == false) {
        _errorText = _pin->errorText();
        delete _pin;
        _pin = nullptr;
        return false;
    }

    return true;
}

void RunButton::onAsserted()
{
    _low = true;
    emit lineChanged(true);
}

void RunButton::onDeasserted()
{
    _low = false;
    emit lineChanged(false);
}

#include "moc_runbutton.cpp"
```

- [ ] **Step 3: Add the sources to `IrrigationD/CMakeLists.txt`**

After the `src/tm1637display.h` line:

```cmake
    src/runbutton.h           src/runbutton.cpp
```

- [ ] **Step 4: Build**

```bash
cmake --build build -j 32
```

Expected: exits zero.

- [ ] **Step 5: Commit**

```bash
git add IrrigationD/src/runbutton.h IrrigationD/src/runbutton.cpp IrrigationD/CMakeLists.txt
git commit -F - <<'EOF'
feat: read the gardener panel's RUN button line

RunButton requests the RUN line active-low with a pull-up, both edges
and a 20 ms kernel debounce, the same configuration as STOP, reads its
starting level and reports every level change.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---
### Task 4: The panel state machine

Spec §3 entire. `PanelController` decides what every RUN press does and what the display shows. It reads the controller through `IPanelHost` and never touches GPIO, so Task 13 tests it with a fake host and a `TestClock`. Rulings 1, 3–11 and 15 land here.

**Files:**
- Create: `IrrigationD/src/ipanelhost.h`
- Create: `IrrigationD/src/panelcontroller.h`, `IrrigationD/src/panelcontroller.cpp`
- Modify: `IrrigationD/CMakeLists.txt`

**Interfaces:**
- Consumes: `IClock::monotonicMsecs()`, `IClock::nowUtc()` (Task 1); `PanelFormat` (Task 1); `RunRequest::Refusal` from `runrequest.h`.
- Produces:
  - `class PanelSnapshot` with `class OpenZone { int zone; int secondsRemaining; }`, fields `QList<OpenZone> openZones` (ascending), `QList<int> enabledZones` (ascending), `int runMinutes`, `bool stopHeld`, `bool faulted`, `bool masterEnabled`, and `int secondsRemaining(int zoneNumber) const` (−1 when closed)
  - `class IPanelHost` with `virtual PanelSnapshot panelSnapshot() = 0;`, `virtual RunRequest::Refusal openPanelZone(int zoneNumber, int replacingZone) = 0;`, `virtual void closePanelZone(int zoneNumber) = 0;`, `virtual void takeOverForPanel() = 0;`
  - `class PanelController : QObject, LoggingBaseClass`:
    - `enum class Mode { Idle, Selecting, PanelRun };` with `Q_ENUM`
    - constants `DefaultRunMinutes = 10`, `MinimumRunMinutes = 1`, `MaximumRunMinutes = 60`, `CommitDelayMsecs = 3000`, `MessageMsecs = 2000`, `AlternateMsecs = 2000`, `BlinkHalfPeriodMsecs = 500`, `StuckLineMsecs = 10000`
    - `PanelController(IPanelHost* host, IClock* clock, QObject* parent = nullptr);`
    - `void setTimeZone(const QTimeZone& value);`, `void setInitialRunLine(bool low);`, `void cancel();`
    - `Mode mode() const;`, `int selectedZone() const;`, `int panelZone() const;`, `bool isRunLineStuck() const;`, `QByteArray frame() const;`
    - slots `void onRunLineChanged(bool low);`, `void tick();`
    - signals `void frameChanged(const QByteArray& segments);`, `void stateChanged();`

- [ ] **Step 1: Create `IrrigationD/src/ipanelhost.h`**

```cpp
#ifndef IPANELHOST_H
#define IPANELHOST_H

#include <QList>

#include "runrequest.h"

/** @brief The controller state the gardener panel reads before each decision and each frame. */
class PanelSnapshot
{
public:
    /** @brief One open zone. */
    class OpenZone
    {
    public:
        int zone = 0;               ///< The zone number.
        int secondsRemaining = 0;   ///< Seconds until the zone's deadline.
    };

    QList<OpenZone> openZones;      ///< Every open zone, ascending by zone number.
    QList<int> enabledZones;        ///< Every enabled zone number, ascending.
    int runMinutes = 0;             ///< The panel run time as it will open, in whole minutes rounded up.
    bool stopHeld = false;          ///< The STOP button is held.
    bool faulted = false;           ///< The zone controller's watchdog latch is set.
    bool masterEnabled = true;      ///< The master enable is on.

    /** @brief Returns the seconds remaining on @p zoneNumber, or -1 when it is not open. */
    int secondsRemaining(int zoneNumber) const
    {
        for(const OpenZone& open : openZones) {
            if(open.zone == zoneNumber) {
                return open.secondsRemaining;
            }
        }
        return -1;
    }
};

/**
 * @brief What the gardener panel asks of the daemon.
 *
 * Every call runs on the thread that owns the zone controller and returns once the
 * work is done.
 */
class IPanelHost
{
public:
    virtual ~IPanelHost() {}

    /** @brief Returns the controller state now. */
    virtual PanelSnapshot panelSnapshot() = 0;

    /**
     * @brief Opens @p zoneNumber as a panel run, closing @p replacingZone when it is not zero.
     *
     * Every refusal is decided before anything closes, counting @p replacingZone's slot
     * as free, and a refused call leaves @p replacingZone open. The one exception is a
     * CapReached after the close: a waiting program zone took the freed slot.
     * @return None when @p zoneNumber opened, otherwise why it did not.
     */
    virtual RunRequest::Refusal openPanelZone(int zoneNumber, int replacingZone) = 0;

    /** @brief Closes @p zoneNumber. */
    virtual void closePanelZone(int zoneNumber) = 0;

    /** @brief Clears the controller as an API STOP does: the queue empties recording dropped_stop, the program aborts, every zone closes. */
    virtual void takeOverForPanel() = 0;
};

#endif // IPANELHOST_H
```

- [ ] **Step 2: Create `IrrigationD/src/panelcontroller.h`**

```cpp
#ifndef PANELCONTROLLER_H
#define PANELCONTROLLER_H

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QTimeZone>

#include <Kanoop/utility/loggingbaseclass.h>

#include "iclock.h"
#include "ipanelhost.h"

/**
 * @brief The gardener panel's RUN-button state machine and the frame the display shows.
 *
 * Holds no timer and touches no GPIO. The owner feeds it RUN line changes and calls
 * tick() on a short interval. It reads the controller through IPanelHost on every press
 * and every tick, times every interval on IClock::monotonicMsecs(), and emits each frame
 * once, when its bytes change.
 */
class PanelController : public QObject,
                        public LoggingBaseClass
{
    Q_OBJECT
public:
    /** @brief What the panel is doing. */
    enum class Mode
    {
        Idle,       ///< No selection and no panel run.
        Selecting,  ///< A zone is offered and commits three seconds after the last press.
        PanelRun    ///< The panel zone is open.
    };
    Q_ENUM(Mode)

    /** @brief The panel run time until panel_run_minutes says otherwise. */
    static constexpr int DefaultRunMinutes = 10;

    /** @brief The lowest panel_run_minutes accepted. */
    static constexpr int MinimumRunMinutes = 1;

    /** @brief The highest panel_run_minutes accepted. */
    static constexpr int MaximumRunMinutes = 60;

    /** @brief How long after the last press a selection commits. */
    static constexpr qint64 CommitDelayMsecs = 3000;

    /** @brief How long OFF, FuLL and nonE stay on the display. */
    static constexpr qint64 MessageMsecs = 2000;

    /** @brief How long each of several other running zones is shown. */
    static constexpr qint64 AlternateMsecs = 2000;

    /** @brief Each half of a blink: the clock colon and the selected zone digit. */
    static constexpr qint64 BlinkHalfPeriodMsecs = 500;

    /** @brief How long the RUN line may read low before it is logged as stuck. */
    static constexpr qint64 StuckLineMsecs = 10000;

    /**
     * @brief Constructs a panel reading the controller through @p host and time from @p clock.
     *
     * The clock face reads UTC until setTimeZone() is called.
     */
    PanelController(IPanelHost* host, IClock* clock, QObject* parent = nullptr);

    /** @brief Sets the zone the clock face shows. */
    void setTimeZone(const QTimeZone& value) { _timeZone = value; }

    /** @brief Records the RUN line's level at startup. A line already low produces no press until it has been seen high. */
    void setInitialRunLine(bool low);

    /** @brief Ends any selection, panel run and message without touching a zone. Called when STOP clears the controller. */
    void cancel();

    /** @brief Returns what the panel is doing. */
    Mode mode() const { return _mode; }

    /** @brief Returns the zone number on offer, or zero unless a selection is pending. */
    int selectedZone() const { return _selectedZone; }

    /** @brief Returns the panel zone's number, or zero unless a panel run is active. */
    int panelZone() const { return _panelZone; }

    /** @brief Returns whether the RUN line has read low for longer than StuckLineMsecs without being seen high. */
    bool isRunLineStuck() const { return _stuckLogged; }

    /** @brief Returns the frame most recently emitted, or an empty array before the first. */
    QByteArray frame() const { return _lastFrame; }

public slots:
    /**
     * @brief Takes the RUN line's new level.
     *
     * A change to low is a press. A low while the line already reads low is ignored: the
     * line must be seen high first.
     */
    void onRunLineChanged(bool low);

    /** @brief Commits a due selection, ends a panel run whose zone closed, checks the stuck line, and refreshes the frame. */
    void tick();

signals:
    /** @brief Emitted when the frame's bytes change. @p segments holds four segment bytes, digit 0 leftmost. */
    void frameChanged(const QByteArray& segments);

    /** @brief Emitted after a press is handled, and after a tick that committed, cancelled or ended something. */
    void stateChanged();

private:
    void press();
    void pressWhileIdle(const PanelSnapshot& snapshot);
    void pressWhileSelecting(const PanelSnapshot& snapshot);
    void pressDuringRun(const PanelSnapshot& snapshot);
    void commit(const PanelSnapshot& snapshot);
    bool endRunIfClosed(const PanelSnapshot& snapshot);
    void showRefusal(RunRequest::Refusal refusal);
    void showMessage(const QByteArray& segments);
    void publishFrame(const PanelSnapshot& snapshot);
    QByteArray render(const PanelSnapshot& snapshot);
    QByteArray otherZonesFrame(const PanelSnapshot& snapshot, qint64 now);
    static int nextEnabledZone(const QList<int>& enabledZones, int afterZone, bool wrap);

    IPanelHost* _host = nullptr;
    IClock* _clock = nullptr;
    QTimeZone _timeZone;
    Mode _mode = Mode::Idle;
    int _selectedZone = 0;
    qint64 _lastPressMsecs = 0;
    int _panelZone = 0;
    QByteArray _message;
    qint64 _messageUntilMsecs = 0;
    int _shownZone = 0;
    qint64 _shownSinceMsecs = 0;
    bool _lineLow = false;
    qint64 _lowSinceMsecs = 0;
    bool _stuckLogged = false;
    QByteArray _lastFrame;
};

#endif // PANELCONTROLLER_H
```

- [ ] **Step 3: Create `IrrigationD/src/panelcontroller.cpp`**

```cpp
#include "panelcontroller.h"

#include "panelformat.h"

PanelController::PanelController(IPanelHost* host, IClock* clock, QObject* parent) :
    QObject(parent),
    LoggingBaseClass("panel"),
    _host(host),
    _clock(clock),
    _timeZone(QTimeZone::UTC)
{
}

void PanelController::setInitialRunLine(bool low)
{
    _lineLow = low;
    _lowSinceMsecs = _clock->monotonicMsecs();
    _stuckLogged = false;
}

void PanelController::cancel()
{
    _mode = Mode::Idle;
    _selectedZone = 0;
    _panelZone = 0;
    _messageUntilMsecs = 0;
}

void PanelController::onRunLineChanged(bool low)
{
    if(low == false) {
        if(_stuckLogged == true) {
            logText(LVL_INFO, "The RUN line reads high again");
        }
        _lineLow = false;
        _stuckLogged = false;
        return;
    }

    if(_lineLow == true) {
        return;
    }

    _lineLow = true;
    _lowSinceMsecs = _clock->monotonicMsecs();
    press();
}

void PanelController::tick()
{
    const qint64 now = _clock->monotonicMsecs();
    PanelSnapshot snapshot = _host->panelSnapshot();

    if(_lineLow == true && _stuckLogged == false && now - _lowSinceMsecs > StuckLineMsecs) {
        logText(LVL_WARNING, QString("The RUN line has read low for more than %1 seconds and is stuck")
                                 .arg(StuckLineMsecs / 1000));
        _stuckLogged = true;
    }

    bool changed = endRunIfClosed(snapshot);
    if(_mode == Mode::Selecting) {
        if(snapshot.stopHeld == true || snapshot.faulted == true) {
            logText(LVL_INFO, "The selection was cancelled: STOP is held or the controller is faulted");
            _mode = Mode::Idle;
            _selectedZone = 0;
            changed = true;
        }
        else if(now - _lastPressMsecs >= CommitDelayMsecs) {
            commit(snapshot);
            changed = true;
        }
    }

    if(changed == true) {
        snapshot = _host->panelSnapshot();
        endRunIfClosed(snapshot);
        emit stateChanged();
    }
    publishFrame(snapshot);
}

void PanelController::press()
{
    PanelSnapshot snapshot = _host->panelSnapshot();
    if(snapshot.stopHeld == true || snapshot.faulted == true) {
        logText(LVL_INFO, "RUN ignored: STOP is held or the controller is faulted");
        publishFrame(snapshot);
        return;
    }

    endRunIfClosed(snapshot);
    _messageUntilMsecs = 0;

    switch(_mode) {
    case Mode::Idle:
        pressWhileIdle(snapshot);
        break;
    case Mode::Selecting:
        pressWhileSelecting(snapshot);
        break;
    case Mode::PanelRun:
        pressDuringRun(snapshot);
        break;
    }

    snapshot = _host->panelSnapshot();
    endRunIfClosed(snapshot);
    emit stateChanged();
    publishFrame(snapshot);
}

void PanelController::pressWhileIdle(const PanelSnapshot& snapshot)
{
    if(snapshot.masterEnabled == false) {
        logText(LVL_WARNING, "RUN refused: watering is turned off");
        showMessage(PanelFormat::masterOff());
        return;
    }
    if(snapshot.enabledZones.isEmpty() == true) {
        logText(LVL_WARNING, "RUN refused: no zone is enabled");
        showMessage(PanelFormat::noZones());
        return;
    }

    if(snapshot.openZones.isEmpty() == false) {
        logText(LVL_WARNING, "RUN takes over the running zones");
        _host->takeOverForPanel();
    }

    _mode = Mode::Selecting;
    _selectedZone = snapshot.enabledZones.first();
    _lastPressMsecs = _clock->monotonicMsecs();
}

void PanelController::pressWhileSelecting(const PanelSnapshot& snapshot)
{
    const int next = nextEnabledZone(snapshot.enabledZones, _selectedZone, true);
    if(next == 0) {
        logText(LVL_WARNING, "RUN refused: no zone is enabled");
        _mode = Mode::Idle;
        _selectedZone = 0;
        showMessage(PanelFormat::noZones());
        return;
    }

    _selectedZone = next;
    _lastPressMsecs = _clock->monotonicMsecs();
}

void PanelController::pressDuringRun(const PanelSnapshot& snapshot)
{
    const int next = nextEnabledZone(snapshot.enabledZones, _panelZone, false);
    if(next == 0) {
        const int closing = _panelZone;
        logText(LVL_INFO, QString("RUN closes zone %1, the last enabled zone; the panel run ends").arg(closing));
        _mode = Mode::Idle;
        _panelZone = 0;
        _host->closePanelZone(closing);
        return;
    }

    const RunRequest::Refusal refusal = _host->openPanelZone(next, _panelZone);
    if(refusal != RunRequest::Refusal::None) {
        showRefusal(refusal);
        return;
    }

    logText(LVL_INFO, QString("RUN moves the panel run from zone %1 to zone %2").arg(_panelZone).arg(next));
    _panelZone = next;
}

void PanelController::commit(const PanelSnapshot& snapshot)
{
    const int zoneNumber = _selectedZone;
    _mode = Mode::Idle;
    _selectedZone = 0;

    if(snapshot.enabledZones.contains(zoneNumber) == false) {
        logText(LVL_WARNING, QString("Zone %1 was disabled during the selection; nothing starts").arg(zoneNumber));
        return;
    }

    const RunRequest::Refusal refusal = _host->openPanelZone(zoneNumber, 0);
    if(refusal != RunRequest::Refusal::None) {
        showRefusal(refusal);
        return;
    }

    logText(LVL_INFO, QString("Zone %1 starts as a panel run").arg(zoneNumber));
    _mode = Mode::PanelRun;
    _panelZone = zoneNumber;
}

bool PanelController::endRunIfClosed(const PanelSnapshot& snapshot)
{
    if(_mode != Mode::PanelRun || snapshot.secondsRemaining(_panelZone) >= 0) {
        return false;
    }

    logText(LVL_INFO, QString("Zone %1 closed; the panel run ends").arg(_panelZone));
    _mode = Mode::Idle;
    _panelZone = 0;
    return true;
}

void PanelController::showRefusal(RunRequest::Refusal refusal)
{
    if(refusal == RunRequest::Refusal::CapReached) {
        showMessage(PanelFormat::full());
    }
    else if(refusal == RunRequest::Refusal::MasterDisabled) {
        showMessage(PanelFormat::masterOff());
    }
}

void PanelController::showMessage(const QByteArray& segments)
{
    _message = segments;
    _messageUntilMsecs = _clock->monotonicMsecs() + MessageMsecs;
}

void PanelController::publishFrame(const PanelSnapshot& snapshot)
{
    const QByteArray next = render(snapshot);
    if(next != _lastFrame) {
        _lastFrame = next;
        emit frameChanged(next);
    }
}

QByteArray PanelController::render(const PanelSnapshot& snapshot)
{
    const qint64 now = _clock->monotonicMsecs();
    QByteArray result;
    bool showingOthers = false;

    if(snapshot.stopHeld == true) {
        result = PanelFormat::stopHeld();
    }
    else if(snapshot.faulted == true) {
        result = PanelFormat::fault();
    }
    else if(now < _messageUntilMsecs) {
        result = _message;
    }
    else if(_mode == Mode::Selecting) {
        const bool zoneVisible = (now - _lastPressMsecs) % (2 * BlinkHalfPeriodMsecs) < BlinkHalfPeriodMsecs;
        result = PanelFormat::zoneMinutes(_selectedZone, snapshot.runMinutes, zoneVisible);
    }
    else if(_mode == Mode::PanelRun) {
        result = PanelFormat::zoneMinutes(_panelZone, PanelFormat::minutesLeft(snapshot.secondsRemaining(_panelZone)), true);
    }
    else if(snapshot.openZones.isEmpty() == false) {
        result = otherZonesFrame(snapshot, now);
        showingOthers = true;
    }
    else {
        const QTime local = _clock->nowUtc().toTimeZone(_timeZone).time();
        result = PanelFormat::clock(local, local.msec() < BlinkHalfPeriodMsecs);
    }

    if(showingOthers == false) {
        _shownZone = 0;
    }
    return result;
}

QByteArray PanelController::otherZonesFrame(const PanelSnapshot& snapshot, qint64 now)
{
    if(snapshot.secondsRemaining(_shownZone) < 0) {
        _shownZone = snapshot.openZones.first().zone;
        _shownSinceMsecs = now;
    }
    else if(now - _shownSinceMsecs >= AlternateMsecs) {
        int next = snapshot.openZones.first().zone;
        for(const PanelSnapshot::OpenZone& open : snapshot.openZones) {
            if(open.zone > _shownZone) {
                next = open.zone;
                break;
            }
        }
        _shownZone = next;
        _shownSinceMsecs = now;
    }

    return PanelFormat::zoneMinutes(_shownZone, PanelFormat::minutesLeft(snapshot.secondsRemaining(_shownZone)), true);
}

int PanelController::nextEnabledZone(const QList<int>& enabledZones, int afterZone, bool wrap)
{
    for(int zoneNumber : enabledZones) {
        if(zoneNumber > afterZone) {
            return zoneNumber;
        }
    }
    return wrap == true && enabledZones.isEmpty() == false ? enabledZones.first() : 0;
}

#include "moc_panelcontroller.cpp"
```

- [ ] **Step 4: Add the sources to `IrrigationD/CMakeLists.txt`**

After the `src/runbutton.h` line:

```cmake
    src/ipanelhost.h
    src/panelcontroller.h     src/panelcontroller.cpp
```

- [ ] **Step 5: Build**

```bash
cmake --build build -j 32
```

Expected: exits zero.

- [ ] **Step 6: Commit**

```bash
git add IrrigationD/src/ipanelhost.h IrrigationD/src/panelcontroller.h IrrigationD/src/panelcontroller.cpp IrrigationD/CMakeLists.txt
git commit -F - <<'EOF'
feat: add the gardener panel's RUN state machine

PanelController offers enabled zones in ascending order on each RUN
press and commits the shown zone three seconds after the last one. A
press during a panel run opens the next enabled zone at once, a press
on the last enabled zone ends the run, and the run ends with its
zone's timer. A press while other zones run takes over with STOP
semantics. StOP and Err win the display; OFF, FuLL and nonE show for
two seconds. Other running zones alternate every two seconds, the idle
display is a 12-hour clock with a blinking colon, and a RUN line low
for more than ten seconds is logged once as stuck and gives no press
until it reads high.

The controller reads everything through IPanelHost snapshots and times
its intervals on the monotonic clock, so it runs without GPIO.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---

### Task 5: Configuration — the panel run time and the panel's lines

Spec §2 (the three `[gpio]` keys, a missing key disables that part) and §4 (`panel_run_minutes`, 1–60, default 10, schema and migration together).

**Files:**
- Modify: `IrrigationD/src/irrigationsettings.h`, `IrrigationD/src/irrigationsettings.cpp`
- Modify: `IrrigationD/src/database/schema.sql`
- Create: `IrrigationD/src/database/migrate/irrigation/1.2.0/01-panel-run-minutes.sql`
- Modify: `IrrigationD/src/database/irrigation.qrc`
- Modify: `IrrigationD/CMakeLists.txt`
- Modify: `IrrigationD/src/irrigationcontrolserver.cpp`

**Interfaces:**
- Consumes: `PanelController::MinimumRunMinutes`, `PanelController::MaximumRunMinutes` (Task 4).
- Produces:
  - `int IrrigationSettings::runButtonOffset() const;`, `int IrrigationSettings::displayClockOffset() const;`, `int IrrigationSettings::displayDataOffset() const;` — each −1 when the key is missing or is not a non-negative integer
  - settings key `panel_run_minutes` in `GET`/`PUT /admin/settings`, validated 1–60; seeded `'10'` in a fresh database and by the 1.2.0 migration
  - `IRRIGATION_DB_VERSION 1.2.0`

- [ ] **Step 1: Add the offsets to `IrrigationD/src/irrigationsettings.h`**

After `stopButtonOffset()`:

```cpp
    /** @brief Returns the GPIO line offset of the RUN button, or -1 when runButtonOffset is missing or malformed. */
    int runButtonOffset() const { return lineOffset(KEY_RUN_BUTTON_OFFSET); }

    /** @brief Returns the GPIO line offset of the display's CLK, or -1 when displayClockOffset is missing or malformed. */
    int displayClockOffset() const { return lineOffset(KEY_DISPLAY_CLOCK_OFFSET); }

    /** @brief Returns the GPIO line offset of the display's DIO, or -1 when displayDataOffset is missing or malformed. */
    int displayDataOffset() const { return lineOffset(KEY_DISPLAY_DATA_OFFSET); }
```

In the `private:` section, before the first `static const QString`:

```cpp
    int lineOffset(const QString& key) const;

```

and after `static const QString KEY_STOP_BUTTON_OFFSET;`:

```cpp
    static const QString KEY_RUN_BUTTON_OFFSET;
    static const QString KEY_DISPLAY_CLOCK_OFFSET;
    static const QString KEY_DISPLAY_DATA_OFFSET;
```

- [ ] **Step 2: Implement them in `IrrigationD/src/irrigationsettings.cpp`**

After the `KEY_STOP_BUTTON_OFFSET` definition:

```cpp
const QString IrrigationSettings::KEY_RUN_BUTTON_OFFSET    = "gpio/runButtonOffset";
const QString IrrigationSettings::KEY_DISPLAY_CLOCK_OFFSET = "gpio/displayClockOffset";
const QString IrrigationSettings::KEY_DISPLAY_DATA_OFFSET  = "gpio/displayDataOffset";
```

Before the moc include at the end:

```cpp
int IrrigationSettings::lineOffset(const QString& key) const
{
    if(_settings.contains(key) == false) {
        return -1;
    }

    const QString raw = _settings.value(key).toString();
    bool ok = false;
    const int offset = raw.trimmed().toInt(&ok);
    if(ok == false || offset < 0) {
        Log::logText(LVL_WARNING, QString("Ignoring %1=\"%2\": expected a line offset").arg(key, raw));
        return -1;
    }

    return offset;
}

```

- [ ] **Step 3: Seed the default in `IrrigationD/src/database/schema.sql`**

Replace the last two lines of the settings insert:

```sql
    ('log_level',            'info'),
    ('max_concurrent_zones', '2');
```

with:

```sql
    ('log_level',            'info'),
    ('max_concurrent_zones', '2'),
    ('panel_run_minutes',    '10');
```

- [ ] **Step 4: Create `IrrigationD/src/database/migrate/irrigation/1.2.0/01-panel-run-minutes.sql`**

```sql
INSERT OR IGNORE INTO settings (key, value) VALUES ('panel_run_minutes', '10');
```

- [ ] **Step 5: Register the script and bump the version**

In `IrrigationD/src/database/irrigation.qrc`, after the 1.1.0 line:

```xml
        <file>migrate/irrigation/1.2.0/01-panel-run-minutes.sql</file>
```

In `IrrigationD/CMakeLists.txt`, replace `set(IRRIGATION_DB_VERSION 1.1.0)` with:

```cmake
set(IRRIGATION_DB_VERSION 1.2.0)
```

- [ ] **Step 6: Accept and validate the key in `IrrigationD/src/irrigationcontrolserver.cpp`**

Add `#include "panelcontroller.h"` beside the other project includes at the top of the file. Replace the `SettingsKeys` definition with:

```cpp
const QStringList IrrigationControlServer::SettingsKeys = {
    "rain_delay_until", "master_enabled", "max_zone_seconds", "log_level", "max_concurrent_zones",
    "panel_run_minutes"
};
```

In `isValidSettingValue()`, after the `max_concurrent_zones` block and before the final `return false;`:

```cpp
    if(key == "panel_run_minutes") {
        bool ok = false;
        const int minutes = value.toInt(&ok);
        return ok && minutes >= PanelController::MinimumRunMinutes && minutes <= PanelController::MaximumRunMinutes;
    }

```

- [ ] **Step 7: Build and run the touched suites**

```bash
cmake --build build -j 32
ctest --test-dir build --output-on-failure -R 'tst_settings|tst_irrigationdatasource|tst_repository|tst_controlserver'
```

Expected: the build exits zero. `TestControlServer::settingsGetReturnsExactlyTheAllowlistedKeys` now fails on purpose (a sixth key) and is updated in Task 14; every other test in these suites passes, including the 1.1.0 migration test, which now migrates through 1.2.0 and compares against the compiled version.

- [ ] **Step 8: Commit**

```bash
git add IrrigationD/src/irrigationsettings.h IrrigationD/src/irrigationsettings.cpp \
        IrrigationD/src/database/schema.sql \
        IrrigationD/src/database/migrate/irrigation/1.2.0/01-panel-run-minutes.sql \
        IrrigationD/src/database/irrigation.qrc IrrigationD/CMakeLists.txt \
        IrrigationD/src/irrigationcontrolserver.cpp
git commit -F - <<'EOF'
feat: configure the gardener panel's run time and lines

panel_run_minutes joins the settings API as a whole number from 1 to
60, seeded at 10 by the fresh schema and by the 1.2.0 migration. The
INI gains runButtonOffset, displayClockOffset and displayDataOffset
under [gpio]; a missing or malformed key reads as absent.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---
### Task 6: The daemon side — PanelHost, wiring, and `source: "panel"`

Spec §3.4 (takeover with STOP semantics), §3.6 (a panel run is a manual run with a different source), §5 (`running[].source` gains `"panel"`), §6.1 (first screen within a second), §7 (display failure). Rulings 2, 3, 10, 12, 13, 15 and 21 land here.

**Files:**
- Create: `IrrigationD/src/panelhost.h`, `IrrigationD/src/panelhost.cpp`
- Modify: `IrrigationD/src/irrigationdaemon.h`, `IrrigationD/src/irrigationdaemon.cpp`
- Modify: `IrrigationD/src/irrigationcontrolserver.h` (`RunningZoneStatus`)
- Modify: `IrrigationD/src/json/statusjson.h`, `IrrigationD/src/json/statusjson.cpp`
- Modify: `IrrigationD/CMakeLists.txt`
- Modify: `IrrigationD/tests/tst_controlserver.cpp` (mechanical edit only)

**Interfaces:**
- Consumes: `IPanelHost`, `PanelSnapshot`, `PanelController` (Task 4); `PanelFormat::minutesLeft()` (Task 1); `Tm1637Display` (Task 2); `RunButton` (Task 3); `IrrigationSettings::runButtonOffset()`, `displayClockOffset()`, `displayDataOffset()` and `panel_run_minutes` (Task 5); `ZoneController::openZone()`, `closeZone()`, `allOff()`, `isOpen()`, `hasSlotFor()`, `isFaulted()`, `openZoneNumbers()`, `secondsRemaining()`, `maxZoneSeconds()`; `ProgramQueue::dropAll()`; `ProgramRunner::abort()`; `StopButton::isHeld()`; `IrrigationDataSource::allZones()`, `isMasterEnabled()`.
- Produces:
  - `class PanelHost : IPanelHost, LoggingBaseClass` with `PanelHost(ZoneController*, ProgramRunner*, ProgramQueue*, IrrigationDataSource*, StopButton*)`, `void setRunMinutes(int value)` (bounded 1–60), `int runMinutes() const`, `int runSeconds() const` (minutes × 60 clamped by `maxZoneSeconds()`), `void clearController()`, and the four `IPanelHost` overrides
  - `RunningZoneStatus::Source { Manual, Program, Panel }` replacing `bool fromProgram`; `RunningZoneStatus{ zone, secondsRemaining, source }` aggregate order unchanged
  - `static QString StatusJson::sourceToJson(RunningZoneStatus::Source value);`
  - daemon log line `The panel is showing its first frame` (the bench reads it)

- [ ] **Step 1: Create `IrrigationD/src/panelhost.h`**

```cpp
#ifndef PANELHOST_H
#define PANELHOST_H

#include <QList>

#include <Kanoop/utility/loggingbaseclass.h>

#include "ipanelhost.h"

class IrrigationDataSource;
class ProgramQueue;
class ProgramRunner;
class StopButton;
class ZoneController;

/**
 * @brief The daemon's side of the gardener panel: reads the controller, and opens, swaps, closes and clears zones for it.
 *
 * A panel run is refused for the same reasons as a manual run from the app, in the same
 * order: STOP held, master enable off, zone disabled, cap reached.
 *
 * @warning Every method must be called on the thread that owns the zone controller.
 */
class PanelHost : public IPanelHost,
                  public LoggingBaseClass
{
public:
    /** @brief Constructs a host over the daemon's components. None is owned. */
    PanelHost(ZoneController* controller,
              ProgramRunner* runner,
              ProgramQueue* queue,
              IrrigationDataSource* source,
              StopButton* stopButton);

    /** @brief Sets the panel run time in minutes, bounded to PanelController::MinimumRunMinutes through MaximumRunMinutes. */
    void setRunMinutes(int value);

    /** @brief Returns the panel run time in minutes. */
    int runMinutes() const { return _runMinutes; }

    /** @brief Returns the seconds a panel run opens for: the run time clamped by the zone controller's ceiling. */
    int runSeconds() const;

    /** @brief Clears the controller as STOP does: the queue empties recording dropped_stop, the program aborts, every zone closes. */
    void clearController();

    /** @brief Reads the open zones, the enabled zones, the run time, STOP, the fault latch and the master enable. */
    virtual PanelSnapshot panelSnapshot() override;

    /** @brief Decides the refusals, then opens @p zoneNumber for runSeconds(), swapping out @p replacingZone when it is open. */
    virtual RunRequest::Refusal openPanelZone(int zoneNumber, int replacingZone) override;

    /** @brief Closes @p zoneNumber. */
    virtual void closePanelZone(int zoneNumber) override;

    /** @brief Logs the takeover and calls clearController(). */
    virtual void takeOverForPanel() override;

private:
    QList<int> enabledZoneNumbers();

    ZoneController* _controller = nullptr;
    ProgramRunner* _runner = nullptr;
    ProgramQueue* _queue = nullptr;
    IrrigationDataSource* _source = nullptr;
    StopButton* _stopButton = nullptr;
    int _runMinutes = 0;
};

#endif // PANELHOST_H
```

- [ ] **Step 2: Create `IrrigationD/src/panelhost.cpp`**

```cpp
#include "panelhost.h"

#include "database/irrigationdatasource.h"
#include "panelcontroller.h"
#include "panelformat.h"
#include "programqueue.h"
#include "programrunner.h"
#include "stopbutton.h"
#include "zonecontroller.h"

#include <algorithm>

PanelHost::PanelHost(ZoneController* controller,
                     ProgramRunner* runner,
                     ProgramQueue* queue,
                     IrrigationDataSource* source,
                     StopButton* stopButton) :
    LoggingBaseClass("panel"),
    _controller(controller),
    _runner(runner),
    _queue(queue),
    _source(source),
    _stopButton(stopButton),
    _runMinutes(PanelController::DefaultRunMinutes)
{
}

void PanelHost::setRunMinutes(int value)
{
    _runMinutes = qBound(PanelController::MinimumRunMinutes, value, PanelController::MaximumRunMinutes);
}

int PanelHost::runSeconds() const
{
    return qMin(_runMinutes * 60, _controller->maxZoneSeconds());
}

void PanelHost::clearController()
{
    // dropAll() precedes abort(): an aborted program starts the next queued one.
    _queue->dropAll(FiredInstant::Outcome::DroppedStop);
    _runner->abort();

    if(_controller->allOff() == false) {
        logText(LVL_ERROR, QString("Failed to close the zones: %1").arg(_controller->errorText()));
    }
}

PanelSnapshot PanelHost::panelSnapshot()
{
    PanelSnapshot snapshot;

    const QList<int> openZones = _controller->openZoneNumbers();
    for(int zoneNumber : openZones) {
        PanelSnapshot::OpenZone open;
        open.zone = zoneNumber;
        open.secondsRemaining = _controller->secondsRemaining(zoneNumber);
        snapshot.openZones.append(open);
    }

    snapshot.enabledZones = enabledZoneNumbers();
    snapshot.runMinutes = PanelFormat::minutesLeft(runSeconds());
    snapshot.stopHeld = _stopButton->isHeld();
    snapshot.faulted = _controller->isFaulted();
    snapshot.masterEnabled = _source->isMasterEnabled();
    return snapshot;
}

RunRequest::Refusal PanelHost::openPanelZone(int zoneNumber, int replacingZone)
{
    const bool replacing = replacingZone > 0 && replacingZone != zoneNumber && _controller->isOpen(replacingZone);

    if(_stopButton->isHeld() == true) {
        logText(LVL_WARNING, QString("Refused a panel run of zone %1: the stop button is held").arg(zoneNumber));
        return RunRequest::Refusal::StopHeld;
    }
    if(_source->isMasterEnabled() == false) {
        logText(LVL_WARNING, QString("Refused a panel run of zone %1: the master enable is off").arg(zoneNumber));
        return RunRequest::Refusal::MasterDisabled;
    }
    if(enabledZoneNumbers().contains(zoneNumber) == false) {
        logText(LVL_WARNING, QString("Refused a panel run of zone %1: the zone is disabled or has no database row").arg(zoneNumber));
        return RunRequest::Refusal::ZoneDisabled;
    }
    if(_controller->isFaulted() == true) {
        logText(LVL_ERROR, QString("Refused a panel run of zone %1: %2").arg(zoneNumber).arg(_controller->errorText()));
        return RunRequest::Refusal::Failed;
    }
    if(_controller->hasSlotFor(zoneNumber) == false && replacing == false) {
        logText(LVL_WARNING, QString("Refused a panel run of zone %1: %2 zones already running")
                                 .arg(zoneNumber).arg(static_cast<int>(_controller->openZoneNumbers().count())));
        return RunRequest::Refusal::CapReached;
    }

    const int seconds = runSeconds();

    if(_controller->hasSlotFor(zoneNumber) == false) {
        // closeZone() emits zoneClosed, and ProgramRunner refills the freed slot from its
        // waiting zones before closeZone() returns, so the slot can be gone again below.
        if(_controller->closeZone(replacingZone) == false) {
            logText(LVL_ERROR, QString("Failed to close zone %1: %2").arg(replacingZone).arg(_controller->errorText()));
            return RunRequest::Refusal::Failed;
        }
        if(_controller->hasSlotFor(zoneNumber) == false) {
            logText(LVL_WARNING, QString("Refused a panel run of zone %1: a waiting program zone took the slot zone %2 freed")
                                     .arg(zoneNumber).arg(replacingZone));
            return RunRequest::Refusal::CapReached;
        }
    }

    if(_controller->openZone(zoneNumber, seconds) == false) {
        logText(LVL_ERROR, QString("Failed to open zone %1 for %2 seconds: %3")
                               .arg(zoneNumber).arg(seconds).arg(_controller->errorText()));
        return RunRequest::Refusal::Failed;
    }

    if(replacing == true && _controller->isOpen(replacingZone) == true && _controller->closeZone(replacingZone) == false) {
        logText(LVL_ERROR, QString("Failed to close zone %1: %2").arg(replacingZone).arg(_controller->errorText()));
    }

    logText(LVL_INFO, QString("The panel opened zone %1 for %2 seconds").arg(zoneNumber).arg(seconds));
    return RunRequest::Refusal::None;
}

void PanelHost::closePanelZone(int zoneNumber)
{
    logText(LVL_INFO, QString("The panel closes zone %1").arg(zoneNumber));
    if(_controller->closeZone(zoneNumber) == false) {
        logText(LVL_ERROR, QString("Failed to close zone %1: %2").arg(zoneNumber).arg(_controller->errorText()));
    }
}

void PanelHost::takeOverForPanel()
{
    logText(LVL_WARNING, "RUN took over: closing every zone, aborting the program and emptying the queue");
    clearController();
}

QList<int> PanelHost::enabledZoneNumbers()
{
    QList<int> result;
    const ZoneList zones = _source->allZones();
    for(const Zone& zone : zones) {
        if(zone.enabled == true) {
            result.append(zone.number);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}
```

- [ ] **Step 3: Give `RunningZoneStatus` a source in `IrrigationD/src/irrigationcontrolserver.h`**

Replace the struct:

```cpp
/** @brief One open zone in a status snapshot. zone is the zone number. */
struct RunningZoneStatus
{
    int zone = 0;
    int secondsRemaining = 0;
    bool fromProgram = false;
};
```

with:

```cpp
/** @brief One open zone in a status snapshot. zone is the zone number. */
struct RunningZoneStatus
{
    /** @brief What opened the zone. */
    enum class Source
    {
        Manual,     ///< A manual run from the app or the API.
        Program,    ///< The running program owns the zone.
        Panel       ///< The gardener panel's current panel run.
    };

    int zone = 0;
    int secondsRemaining = 0;
    Source source = Source::Manual;
};
```

- [ ] **Step 4: Serialize the source in `IrrigationD/src/json/statusjson.h` and `.cpp`**

In `statusjson.h`, replace `struct ServerStatus;` with an include, so the nested enum is visible:

```cpp
#include "irrigationcontrolserver.h"
```

and add to the `public:` section of `StatusJson`:

```cpp
    /**
     * @brief Returns the wire name for @p value.
     *
     * The names are the running[].source contract with the web client's RunSource type.
     */
    static QString sourceToJson(RunningZoneStatus::Source value);
```

In `statusjson.cpp`, replace:

```cpp
        entry["source"] = zone.fromProgram == true ? "program" : "manual";
```

with:

```cpp
        entry["source"] = sourceToJson(zone.source);
```

and add before `StatusJson::instantToJson`:

```cpp
QString StatusJson::sourceToJson(RunningZoneStatus::Source value)
{
    switch(value) {
    case RunningZoneStatus::Source::Program:
        return "program";
    case RunningZoneStatus::Source::Panel:
        return "panel";
    case RunningZoneStatus::Source::Manual:
        break;
    }
    return "manual";
}

```

- [ ] **Step 5: Keep `tst_controlserver` compiling**

```bash
cd /home/spunak/src/punak/irrigation
sed -i \
  -e 's/RunningZoneStatus{ \([0-9]*\), \([0-9]*\), false }/RunningZoneStatus{ \1, \2, RunningZoneStatus::Source::Manual }/g' \
  -e 's/RunningZoneStatus{ \([0-9]*\), \([0-9]*\), true }/RunningZoneStatus{ \1, \2, RunningZoneStatus::Source::Program }/g' \
  IrrigationD/tests/tst_controlserver.cpp
grep -n 'RunningZoneStatus{' IrrigationD/tests/tst_controlserver.cpp
```

Expected: three initializers, at the lines that held `{ 4, 137, false }`, `{ 6, 1712, true }` and `{ 7, 42, false }`, now naming `Source::Manual`, `Source::Program` and `Source::Manual`. `git diff --stat IrrigationD/tests` shows only this file.

- [ ] **Step 6: Declare the panel members in `IrrigationD/src/irrigationdaemon.h`**

Add `#include <QByteArray>` beside `#include <QDateTime>`. Add to the forward declarations, keeping them sorted:

```cpp
class PanelController;
class PanelHost;
class RunButton;
class Tm1637Display;
```

Add to `private slots:` after `void publishStatus();`:

```cpp
    void onPanelFrame(const QByteArray& segments);
```

Add to `private:` after `void connectComponents();`:

```cpp
    /**
     * @brief Requests the display and RUN lines that are configured and builds the panel.
     *
     * A missing key or a failed request is logged and leaves that part out; the daemon
     * runs on without it.
     */
    void setUpPanel();
```

Add after `static constexpr int FiredInstantRetentionDays = 90;`:

```cpp
    static constexpr int PanelTickMilliseconds = 100;
```

Add after `QTimer* _statusTimer = nullptr;`:

```cpp
    Tm1637Display* _display = nullptr;
    RunButton* _runButton = nullptr;
    PanelHost* _panelHost = nullptr;
    PanelController* _panel = nullptr;
    QTimer* _panelTimer = nullptr;
    int _panelRunMinutes = 0;
    bool _displayFailing = false;
```

- [ ] **Step 7: Build and wire the panel in `IrrigationD/src/irrigationdaemon.cpp`**

Add to the project includes, sorted:

```cpp
#include "panelcontroller.h"
#include "panelhost.h"
#include "runbutton.h"
#include "tm1637display.h"
```

In `threadStarted()`, after `_scheduler = new Scheduler(_dataSource, &_clock);`:

```cpp
        setUpPanel();
```

In `threadStarted()`, replace:

```cpp
        connectComponents();

        if(_controlServer->start() == false) {
```

with:

```cpp
        connectComponents();

        _panel->tick();
        _panelTimer->start();
        logText(LVL_INFO, "The panel is showing its first frame");

        if(_controlServer->start() == false) {
```

In `threadAboutToFinish()`, replace:

```cpp
    delete _statusTimer;
    _statusTimer = nullptr;
```

with:

```cpp
    delete _statusTimer;
    _statusTimer = nullptr;

    delete _panelTimer;
    _panelTimer = nullptr;

    delete _panel;
    _panel = nullptr;

    delete _panelHost;
    _panelHost = nullptr;
```

and replace:

```cpp
    delete _stopButton;
    _stopButton = nullptr;
```

with:

```cpp
    delete _stopButton;
    _stopButton = nullptr;

    delete _runButton;
    _runButton = nullptr;

    // ~Tm1637Display releases its lines through the backend, which must still hold the chip.
    delete _display;
    _display = nullptr;
```

In `connectComponents()`, after `connect(_statusTimer, &QTimer::timeout, this, &IrrigationDaemon::publishStatus);`:

```cpp
    if(_runButton != nullptr) {
        connect(_runButton, &RunButton::lineChanged, _panel, &PanelController::onRunLineChanged);
    }
    connect(_panel, &PanelController::frameChanged, this, &IrrigationDaemon::onPanelFrame);
    connect(_panel, &PanelController::stateChanged, this, &IrrigationDaemon::publishStatus);
    connect(_panelTimer, &QTimer::timeout, _panel, &PanelController::tick);
```

Replace the body of `onStopPressed()`:

```cpp
void IrrigationDaemon::onStopPressed()
{
    if(isTornDown()) {
        return;
    }

    logText(LVL_WARNING, "Stop requested");

    _panel->cancel();
    _panelHost->clearController();

    publishStatus();
}
```

In `onSettingsChanged()`, after `applyRuntimeSettings();`:

```cpp
    _panelHost->setRunMinutes(_panelRunMinutes);
```

In `applyRuntimeSettings()`, after the `max_concurrent_zones` block and before `const QString levelName`:

```cpp
    const QString storedPanelMinutes = _dataSource->settingValue("panel_run_minutes");
    bool parsedPanelMinutes = false;
    const int panelMinutes = storedPanelMinutes.toInt(&parsedPanelMinutes);
    _panelRunMinutes = parsedPanelMinutes == true
                           && panelMinutes >= PanelController::MinimumRunMinutes
                           && panelMinutes <= PanelController::MaximumRunMinutes
                       ? panelMinutes
                       : PanelController::DefaultRunMinutes;
    logText(LVL_INFO, QString("Panel runs last %1 minutes (database '%2')").arg(_panelRunMinutes).arg(storedPanelMinutes));

```

In `publishStatus()`, replace:

```cpp
        running.fromProgram = _programRunner->ownsZone(zoneNumber);
```

with:

```cpp
        if(_programRunner->ownsZone(zoneNumber) == true) {
            running.source = RunningZoneStatus::Source::Program;
        }
        else if(_panel != nullptr && _panel->panelZone() == zoneNumber) {
            running.source = RunningZoneStatus::Source::Panel;
        }
```

Add the two new functions before `publishStatus()`:

```cpp
void IrrigationDaemon::setUpPanel()
{
    const int clockOffset = _settings->displayClockOffset();
    const int dataOffset = _settings->displayDataOffset();
    if(clockOffset < 0 || dataOffset < 0) {
        logText(LVL_WARNING, "displayClockOffset or displayDataOffset is not configured; the panel runs without its display");
    }
    else {
        _display = new Tm1637Display(_backend, static_cast<quint32>(clockOffset), static_cast<quint32>(dataOffset));
        if(_display->begin() == false) {
            logText(LVL_ERROR, QString("Failed to request the display lines %1 and %2: %3; the panel runs without its display")
                                   .arg(clockOffset).arg(dataOffset).arg(_display->errorText()));
            delete _display;
            _display = nullptr;
        }
    }

    const int runOffset = _settings->runButtonOffset();
    if(runOffset < 0) {
        logText(LVL_WARNING, "runButtonOffset is not configured; the RUN button is disabled");
    }
    else {
        _runButton = new RunButton(_backend, static_cast<quint32>(runOffset));
        if(_runButton->begin() == false) {
            logText(LVL_ERROR, QString("Failed to request the RUN button line %1: %2; the RUN button is disabled")
                                   .arg(runOffset).arg(_runButton->errorText()));
            delete _runButton;
            _runButton = nullptr;
        }
    }

    _panelHost = new PanelHost(_zoneController, _programRunner, _programQueue, _dataSource, _stopButton);
    _panelHost->setRunMinutes(_panelRunMinutes);

    _panel = new PanelController(_panelHost, &_clock);
    _panel->setTimeZone(QTimeZone::systemTimeZone());
    if(_runButton != nullptr) {
        _panel->setInitialRunLine(_runButton->isLow());
        if(_runButton->isLow() == true) {
            logText(LVL_WARNING, "The RUN button reads pressed at startup");
        }
    }

    _panelTimer = new QTimer();
    _panelTimer->setInterval(PanelTickMilliseconds);
}

void IrrigationDaemon::onPanelFrame(const QByteArray& segments)
{
    if(_display == nullptr) {
        return;
    }

    if(_display->show(segments) == false) {
        if(_displayFailing == false) {
            logText(LVL_ERROR, QString("Failed to write the display: %1").arg(_display->errorText()));
        }
        _displayFailing = true;
    }
    else if(_displayFailing == true) {
        logText(LVL_INFO, "The display is writing again");
        _displayFailing = false;
    }
}

```

- [ ] **Step 8: Add the sources to `IrrigationD/CMakeLists.txt`**

After the `src/panelcontroller.h` line:

```cmake
    src/panelhost.h           src/panelhost.cpp
```

- [ ] **Step 9: Build and grep**

```bash
cmake --build build -j 32
grep -rn 'fromProgram' IrrigationD/ || echo none
grep -rn 'QueuedConnection' IrrigationD/src || echo none
```

Expected: the build exits zero; both greps print `none`. `tst_controlserver` builds; `settingsGetReturnsExactlyTheAllowlistedKeys` still fails from Task 5 and nothing else in it does.

- [ ] **Step 10: Commit**

```bash
git add IrrigationD/src/panelhost.h IrrigationD/src/panelhost.cpp \
        IrrigationD/src/irrigationdaemon.h IrrigationD/src/irrigationdaemon.cpp \
        IrrigationD/src/irrigationcontrolserver.h \
        IrrigationD/src/json/statusjson.h IrrigationD/src/json/statusjson.cpp \
        IrrigationD/CMakeLists.txt IrrigationD/tests/tst_controlserver.cpp
git commit -F - <<'EOF'
feat: run the gardener panel in the daemon

The daemon requests the RUN and display lines named in the INI, builds
the panel, draws its first frame before the control server starts and
ticks it every 100 ms. A missing key or failed request leaves that part
out and is logged; a failed display write is logged once and retried.

PanelHost answers the panel's requests on the valve thread. A panel run
is refused for the same reasons as a manual run, every refusal decided
before anything closes, and opens for panel_run_minutes clamped by the
zone ceiling. STOP and the RUN takeover now share clearController(), so
the takeover clears the queue, the program and every zone exactly as
STOP does. A STOP press also cancels a pending selection.

The status names a panel run's zone "panel".

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---

### Task 7: The one-shot `--dashes` mode

Spec §6.2. The stop-post hook runs the daemon executable in a mode that writes `----` to the display, touches no other line, and exits. Task 10 wires it into the image's unit; this task also brings the repository's own unit copy in line.

**Files:**
- Modify: `IrrigationD/src/main.cpp`
- Modify: `IrrigationD/systemd/irrigationd.service`

**Interfaces:**
- Consumes: `IrrigationSettings::chipLabel()`, `displayClockOffset()`, `displayDataOffset()` (Task 5); `Tm1637Display` (Task 2); `PanelFormat::dashes()` (Task 1); `LibGpiodBackend` from KanoopPiQt.
- Produces: command-line option `--dashes`; exit 0 on success or with no display configured, exit 1 on a failed chip open, line request or write.

- [ ] **Step 1: Add the mode to `IrrigationD/src/main.cpp`**

Add includes after `#include <Kanoop/log.h>`:

```cpp
#include <Kanoop/pi/libgpiodbackend.h>
```

and after `#include "irrigationdaemon.h"`:

```cpp
#include "irrigationsettings.h"
#include "panelformat.h"
#include "tm1637display.h"
```

Inside the anonymous namespace, after `posixSignalHandler`:

```cpp
    bool showDashes(const QString& configPath)
    {
        IrrigationSettings settings(configPath);
        const int clockOffset = settings.displayClockOffset();
        const int dataOffset = settings.displayDataOffset();
        if(clockOffset < 0 || dataOffset < 0) {
            Log::logText(LVL_INFO, "No display lines are configured; there is nothing to clear");
            return true;
        }

        LibGpiodBackend backend;
        if(backend.openChipByLabel(settings.chipLabel()) == false) {
            Log::logText(LVL_ERROR, QString("Failed to open GPIO chip '%1': %2").arg(settings.chipLabel(), backend.errorText()));
            return false;
        }

        Tm1637Display display(&backend, static_cast<quint32>(clockOffset), static_cast<quint32>(dataOffset));
        if(display.begin() == false || display.show(PanelFormat::dashes()) == false) {
            Log::logText(LVL_ERROR, QString("Failed to write ---- to the display: %1").arg(display.errorText()));
            return false;
        }

        return true;
    }
```

After `const QString keyConfig =  "config";` add:

```cpp
const QString keyDashes =  "dashes";
```

In `parser.addOptions({ ... })`, after the `keyConfig` row:

```cpp
        {{ keyDashes },         "Write ---- to the panel display and exit",                             },
```

After `Log::systemLog()->openLog();` and before the `socketpair` call:

```cpp
    if(parser.isSet(keyDashes)) {
        return showDashes(parser.value(keyConfig)) == true ? 0 : 1;
    }

```

- [ ] **Step 2: Bring `IrrigationD/systemd/irrigationd.service` in line**

Replace the file with:

```ini
[Unit]
Description=Irrigation controller
After=time-sync.target

[Service]
Type=simple
ExecStart=/usr/bin/irrigationd --config /etc/irrigationd.ini
# Runs after every stop of the daemon, a crash included; the leading '-' keeps a
# failed clear from marking the unit failed.
ExecStopPost=-/usr/bin/irrigationd --config /etc/irrigationd.ini --dashes
Restart=always
RestartSec=5
User=root
StateDirectory=irrigationd
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

- [ ] **Step 3: Build and exercise the mode on the host**

```bash
cd /home/spunak/src/punak/irrigation
cmake --build build -j 32
d=$(mktemp -d)
printf '[gpio]\nchipLabel=no-such-chip\n' > "$d/none.ini"
build/IrrigationD/irrigationd --config "$d/none.ini" --dashes; echo "exit $?"
printf '[gpio]\nchipLabel=no-such-chip\ndisplayClockOffset=18\ndisplayDataOffset=27\n' > "$d/chip.ini"
build/IrrigationD/irrigationd --config "$d/chip.ini" --dashes; echo "exit $?"
build/IrrigationD/irrigationd --help | grep -- --dashes
rm -r "$d"
```

Expected: `exit 0` with no display keys; `exit 1` with the keys and a chip that does not exist; the help lists `--dashes`. The host has no TM1637, so nothing else is reachable here; the bench task exercises the real write.

- [ ] **Step 4: Commit**

```bash
git add IrrigationD/src/main.cpp IrrigationD/systemd/irrigationd.service
git commit -F - <<'EOF'
feat: clear the gardener panel's display from a one-shot mode

irrigationd --dashes requests only the two display lines named in the
INI, writes ---- and exits, so a unit's stop-post hook can leave a
stopped controller showing ---- in place of a stale countdown. With no
display configured it exits zero having written nothing.

The repository's own unit no longer waits for the network and runs the
mode after every stop.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---
### Task 8: Web — the "panel" tag

Spec §5. `running[].source` gains `"panel"`; the Now screen tags panel rows and tiles "panel" next to the existing "program" and "manual" tags. Ruling 16: the tile tag appears for panel runs only.

**Files:**
- Modify: `web/src/api/types.ts`, `web/src/api/decode.ts`
- Modify: `web/src/screens/NowScreen.tsx`
- Modify: `web/src/components/ZoneTile.tsx`
- Modify: `web/src/styles/app.css`

**Interfaces:**
- Consumes: Task 6 `source: "panel"` in `/api/status`.
- Produces: `RunSource = 'program' | 'manual' | 'panel'`; row tag text `Panel` with class `running-row__tag--panel`; `ZoneTileProps.panel: boolean`; tile tag `<span className="zone-tile__tag">Panel</span>`.

- [ ] **Step 1: Widen `RunSource` in `web/src/api/types.ts`**

Replace:

```ts
export type RunSource = 'program' | 'manual'
```

with:

```ts
export type RunSource = 'program' | 'manual' | 'panel'
```

- [ ] **Step 2: Accept it in `web/src/api/decode.ts`**

In `decodeRunningZone`, replace:

```ts
  if (runSource !== 'program' && runSource !== 'manual') {
```

with:

```ts
  if (runSource !== 'program' && runSource !== 'manual' && runSource !== 'panel') {
```

- [ ] **Step 3: Label rows by source in `web/src/screens/NowScreen.tsx`**

Change the types import to:

```ts
import type { RunSource, RunningProgram, RunningZone, Zone } from '../api/types'
```

After `const DEFAULT_QUICK_RUN = 600` add:

```ts
const SOURCE_LABELS: Record<RunSource, string> = {
  program: 'Program',
  manual: 'Manual',
  panel: 'Panel',
}
```

In `RunningRow`, replace:

```tsx
        {entry.source === 'program' ? 'Program' : 'Manual'}
```

with:

```tsx
        {SOURCE_LABELS[entry.source]}
```

In the zone grid, replace:

```tsx
        {zones.map((entry) => {
          const open = running.some((item) => item.zone === entry.number)
          return (
            <ZoneTile
              key={entry.number}
              zone={entry}
              running={open}
```

with:

```tsx
        {zones.map((entry) => {
          const source = running.find((item) => item.zone === entry.number)?.source
          const open = source !== undefined
          return (
            <ZoneTile
              key={entry.number}
              zone={entry}
              running={open}
              panel={source === 'panel'}
```

- [ ] **Step 4: Tag the tile in `web/src/components/ZoneTile.tsx`**

Replace the props interface and the component's opening:

```tsx
export interface ZoneTileProps {
  zone: Zone
  running: boolean
  disabled: boolean
  onRun: (zone: Zone) => void
  onStop: (zoneNumber: number) => void
}

export default function ZoneTile({ zone, running, disabled, onRun, onStop }: ZoneTileProps) {
```

with:

```tsx
export interface ZoneTileProps {
  zone: Zone
  running: boolean
  /** True while the gardener panel's run holds this zone open. */
  panel: boolean
  disabled: boolean
  onRun: (zone: Zone) => void
  onStop: (zoneNumber: number) => void
}

export default function ZoneTile({ zone, running, panel, disabled, onRun, onStop }: ZoneTileProps) {
```

After the line `{zone.enabled ? null : <span className="zone-tile__off">Disabled</span>}` add:

```tsx
      {panel ? <span className="zone-tile__tag">Panel</span> : null}
```

- [ ] **Step 5: Style both tags in `web/src/styles/app.css`**

After the `.running-row__tag--program` rule:

```css
.running-row__tag--panel {
  border-color: var(--accent);
  color: var(--accent);
}
```

After the `.zone-tile__off` rule:

```css
.zone-tile__tag {
  align-self: flex-start;
  padding: 2px 10px;
  border-radius: 999px;
  font-size: 0.8rem;
  font-weight: 700;
  letter-spacing: 0.08em;
  text-transform: uppercase;
  border: 1px solid var(--accent);
  color: var(--accent);
}
```

- [ ] **Step 6: Typecheck and run the suite**

```bash
npm --prefix web run typecheck
npm --prefix web test
```

Expected: typecheck exits zero; every existing test passes.

- [ ] **Step 7: Commit**

```bash
git add web/src/api/types.ts web/src/api/decode.ts web/src/screens/NowScreen.tsx \
        web/src/components/ZoneTile.tsx web/src/styles/app.css
git commit -F - <<'EOF'
feat: tag gardener panel runs on the Now screen

The status decoder accepts the "panel" source. A panel run's row is
tagged Panel beside the Program and Manual tags, and its tile carries
the same tag.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---

### Task 9: Web — "Gardener panel run time (minutes)" on Settings

Spec §4. A whole-number field from 1 to 60 writing `panel_run_minutes`.

**Files:**
- Modify: `web/src/settings/settingsMap.ts`
- Modify: `web/src/screens/SettingsScreen.tsx`

**Interfaces:**
- Consumes: Task 5 `panel_run_minutes` on `GET`/`PUT /api/settings`.
- Produces: `SETTING_KEYS.panelRunMinutes = 'panel_run_minutes'`, `DEFAULT_PANEL_RUN_MINUTES = 10`, `PANEL_RUN_MINUTES_LIMIT = 60`; label `Gardener panel run time (minutes)`, button `Save panel run time`.

- [ ] **Step 1: Add the key, default and limit to `web/src/settings/settingsMap.ts`**

Replace `SETTING_KEYS` with:

```ts
export const SETTING_KEYS = {
  rainDelayUntil: 'rain_delay_until',
  masterEnabled: 'master_enabled',
  maxZoneSeconds: 'max_zone_seconds',
  logLevel: 'log_level',
  maxConcurrentZones: 'max_concurrent_zones',
  panelRunMinutes: 'panel_run_minutes',
} as const
```

After `MAX_CONCURRENT_ZONES_LIMIT`:

```ts
/** The daemon's default for panel_run_minutes, used when the setting is absent. */
export const DEFAULT_PANEL_RUN_MINUTES = 10

/** The daemon rejects any panel_run_minutes outside 1 through this value with a 400. */
export const PANEL_RUN_MINUTES_LIMIT = 60
```

- [ ] **Step 2: Add the field to `web/src/screens/SettingsScreen.tsx`**

Add `DEFAULT_PANEL_RUN_MINUTES` and `PANEL_RUN_MINUTES_LIMIT` to the `settingsMap` import, keeping it sorted:

```ts
import {
  DEFAULT_MAX_ZONE_SECONDS,
  DEFAULT_MAX_CONCURRENT_ZONES,
  DEFAULT_PANEL_RUN_MINUTES,
  MAX_CONCURRENT_ZONES_LIMIT,
  PANEL_RUN_MINUTES_LIMIT,
  SETTING_KEYS,
  parseInstant,
  parseInteger,
  parseMasterEnabled,
  serializeMasterEnabled,
} from '../settings/settingsMap'
```

After `const [maxZones, setMaxZones] = useState(DEFAULT_MAX_CONCURRENT_ZONES)`:

```tsx
  const [panelMinutes, setPanelMinutes] = useState(DEFAULT_PANEL_RUN_MINUTES)
```

In `load`, after the `setMaxZones(...)` line:

```tsx
      setPanelMinutes(parseInteger(loadedSettings[SETTING_KEYS.panelRunMinutes], DEFAULT_PANEL_RUN_MINUTES))
```

After `onSaveMaxZones`:

```tsx
  const onSavePanelMinutes = useCallback(() => {
    if (Number.isInteger(panelMinutes) === false || panelMinutes < 1 || panelMinutes > PANEL_RUN_MINUTES_LIMIT) {
      setError(`The gardener panel run time must be a whole number of minutes from 1 to ${PANEL_RUN_MINUTES_LIMIT}.`)
      return
    }
    void write({ [SETTING_KEYS.panelRunMinutes]: String(panelMinutes) })
  }, [panelMinutes, write])
```

After the `Save max zones` button:

```tsx
          <label>
            Gardener panel run time (minutes)
            <input
              type="number"
              min={1}
              max={PANEL_RUN_MINUTES_LIMIT}
              value={panelMinutes}
              onChange={(event) => {
                setPanelMinutes(Number(event.target.value))
              }}
            />
          </label>
          <button type="button" onClick={onSavePanelMinutes}>
            Save panel run time
          </button>
```

- [ ] **Step 3: Typecheck and run the suite**

```bash
npm --prefix web run typecheck
npm --prefix web test
```

Expected: typecheck exits zero; `SettingsScreen.test.tsx` still passes.

- [ ] **Step 4: Commit**

```bash
git add web/src/settings/settingsMap.ts web/src/screens/SettingsScreen.tsx
git commit -F - <<'EOF'
feat: set the gardener panel's run time from Settings

A "Gardener panel run time (minutes)" field writes panel_run_minutes
as a whole number from 1 to 60.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---

### Task 10: The image layer (rpi repository)

Spec §2 (INI keys, `gpio=24=ip,pu`), §6.1 (start after the RTC with no network wait), §6.2 (stop-post hook), §9 (these ride the next reflash). Rulings 17–20. This task commits in `~/src/punak/rpi` only. The owner builds and flashes the image; this task builds one recipe to prove its install step and nothing more.

**Files** (all under `/home/spunak/src/punak/rpi`):
- Modify: `meta-rpi4-irrigation/recipes-core/irrigation-init/files/irrigationd.service`
- Create: `meta-rpi4-irrigation/recipes-core/irrigation-init/files/99-irrigation-rtc.rules`
- Create: `meta-rpi4-irrigation/recipes-core/irrigation-init/files/dev-rtc0-timeout.conf`
- Modify: `meta-rpi4-irrigation/recipes-core/irrigation-init/irrigation-init_1.0.bb`
- Modify: `meta-rpi4-irrigation/recipes-core/irrigation-init/files/irrigationd.ini`
- Modify: `kas/rpi4-irrigation.yml`
- Modify: `meta-rpi4-irrigation/recipes-images/images/rpi4-irrigation-image.bb`

**Interfaces:**
- Consumes: Task 5's INI keys; Task 7's `--dashes`.
- Produces: the unit, udev rule, device drop-in, INI and boot config the bench task (16) hand-applies and the next image carries.

- [ ] **Step 1: Check the tree**

```bash
cd /home/spunak/src/punak/rpi
git status --short
git diff --stat -- meta-rpi4-irrigation kas/rpi4-irrigation.yml
```

Expected: no changes under `meta-rpi4-irrigation` or in `kas/rpi4-irrigation.yml`. Unrelated gateway files may be modified; leave them alone. If any file this task edits already has changes, stop and ask.

- [ ] **Step 2: Replace `meta-rpi4-irrigation/recipes-core/irrigation-init/files/irrigationd.service`**

```ini
[Unit]
Description=Irrigation controller daemon
Documentation=https://github.com/StevePunak/irrigation
# dev-rtc0.device exists only while 99-irrigation-rtc.rules tags rtc0 for
# systemd. Without the tag, this Wants= waits out the device timeout
# (dev-rtc0.device.d/10-irrigation-timeout.conf) on every boot.
Wants=dev-rtc0.device
After=dev-rtc0.device time-sync.target
# A repeatable startup failure must never let the rate limiter park this
# unit failed: a parked unit waters nothing and never retries. This key
# only takes effect in [Unit]; systemd silently ignores it under [Service].
StartLimitIntervalSec=0

[Service]
Type=simple
ExecStart=/usr/bin/irrigationd --config /etc/irrigationd.ini
# Runs after every stop of the daemon, a crash included; the leading '-'
# keeps a failed clear from marking the unit failed.
ExecStopPost=-/usr/bin/irrigationd --config /etc/irrigationd.ini --dashes
Restart=always
RestartSec=5
TimeoutStopSec=30
WorkingDirectory=/var/lib/irrigationd

[Install]
WantedBy=multi-user.target
```

- [ ] **Step 3: Create `meta-rpi4-irrigation/recipes-core/irrigation-init/files/99-irrigation-rtc.rules`**

```
# irrigationd.service orders itself after dev-rtc0.device, which systemd
# tracks only for a device udev tags "systemd".
SUBSYSTEM=="rtc", KERNEL=="rtc0", TAG+="systemd"
```

- [ ] **Step 4: Create `meta-rpi4-irrigation/recipes-core/irrigation-init/files/dev-rtc0-timeout.conf`**

```ini
# Bounds how long irrigationd.service waits for an RTC that never
# appears; the daemon starts when this expires.
[Unit]
JobRunningTimeoutSec=20s
```

- [ ] **Step 5: Install both from `meta-rpi4-irrigation/recipes-core/irrigation-init/irrigation-init_1.0.bb`**

Replace `SRC_URI`:

```bitbake
SRC_URI = " \
    file://irrigationd.service \
    file://irrigationd.ini \
    file://99-irrigation-rtc.rules \
    file://dev-rtc0-timeout.conf \
"
```

In `do_install()`, after the `irrigationd.service` install line:

```bitbake

    install -d ${D}${systemd_system_unitdir}/dev-rtc0.device.d
    install -m 0644 ${UNPACKDIR}/dev-rtc0-timeout.conf \
        ${D}${systemd_system_unitdir}/dev-rtc0.device.d/10-irrigation-timeout.conf

    install -d ${D}${nonarch_base_libdir}/udev/rules.d
    install -m 0644 ${UNPACKDIR}/99-irrigation-rtc.rules \
        ${D}${nonarch_base_libdir}/udev/rules.d/99-irrigation-rtc.rules
```

Replace `FILES:${PN}`:

```bitbake
FILES:${PN} = " \
    ${sysconfdir}/irrigationd.ini \
    ${systemd_system_unitdir}/irrigationd.service \
    ${systemd_system_unitdir}/dev-rtc0.device.d/10-irrigation-timeout.conf \
    ${nonarch_base_libdir}/udev/rules.d/99-irrigation-rtc.rules \
    ${localstatedir}/lib/irrigationd \
    ${libdir}/systemd/system-preset/90-irrigationd.preset \
"
```

- [ ] **Step 6: Add the panel keys to `meta-rpi4-irrigation/recipes-core/irrigation-init/files/irrigationd.ini`**

Replace the line `stopButtonOffset=25` with:

```ini
stopButtonOffset=25
; The gardener panel. Without runButtonOffset the RUN button is disabled;
; without either display key the panel runs without its display.
runButtonOffset=24
displayClockOffset=18
displayDataOffset=27
```

- [ ] **Step 7: Update the boot config in `kas/rpi4-irrigation.yml`**

Replace the comment block and the assignment:

```yaml
    # The gpio= line drives the relay inputs high — the inactive level for a
    # LOW-trigger board — from firmware through kernel boot. It covers only
    # up to the point irrigationd requests the lines for itself: once a
    # process releases a GPIO line the kernel reverts it to input and the
    # pad's pull takes over, and six of the eight offsets (12, 13, 16, 19,
    # 20, 21) are in BCM 9-27, which pull down — asserted on this board.
    # gpio=25=ip,pu pulls the stop button's input high; it reads a ground
    # switch and needs the opposite bias from the eight outputs.
    RPI_EXTRA_CONFIG = 'dtparam=i2c_arm=on\ndtoverlay=i2c-rtc,ds3231\ngpio=5,6,12,13,16,19,20,21=op,dh\ngpio=25=ip,pu'
```

with:

```yaml
    # The zone gpio= line drives the relay inputs high, the inactive level
    # for a LOW-trigger board, from firmware through kernel boot. Its pu
    # keeps each pad pulled high once irrigationd releases the line; without
    # it six of the eight offsets (12, 13, 16, 19, 20, 21, all in BCM 9-27)
    # fall back to a pull-down, which asserts a relay. gpio=25=ip,pu and
    # gpio=24=ip,pu pull the STOP and RUN inputs high; each reads a switch
    # to ground.
    RPI_EXTRA_CONFIG = 'dtparam=i2c_arm=on\ndtoverlay=i2c-rtc,ds3231\ngpio=5,6,12,13,16,19,20,21=op,dh,pu\ngpio=25=ip,pu\ngpio=24=ip,pu'
```

- [ ] **Step 8: Extend the guard in `meta-rpi4-irrigation/recipes-images/images/rpi4-irrigation-image.bb`**

Replace the body of `assert_gpio_safety_line()`:

```bitbake
assert_gpio_safety_line() {
    generated="${DEPLOY_DIR_IMAGE}/bootfiles/config.txt"
    if ! grep -qF 'gpio=5,6,12,13,16,19,20,21=op,dh,pu' "$generated"; then
        bbfatal "gpio=5,6,12,13,16,19,20,21=op,dh,pu missing from $generated — RPI_EXTRA_CONFIG did not reach the boot partition, and the relay lines come up unheld"
    fi
    if ! grep -qF 'gpio=25=ip,pu' "$generated"; then
        bbfatal "gpio=25=ip,pu missing from $generated — the stop button input has no pull-up and reads a floating line"
    fi
    if ! grep -qF 'gpio=24=ip,pu' "$generated"; then
        bbfatal "gpio=24=ip,pu missing from $generated — the RUN button input has no pull-up until irrigationd requests the line"
    fi
}
```

- [ ] **Step 9: Build the recipe's install step in the container**

Packaging on this Fedora 44 host fails under pseudo (host `tar` calls `openat2`), so build inside `kas-container`, and stop at `do_install` so `rm_work` keeps the tree:

```bash
cd /home/spunak/src/punak/rpi
kas-container shell kas/rpi4-irrigation.yml:kas/irrigation-wifi.yml -c "bitbake -f -c install irrigation-init"
find build/tmp/work/*/irrigation-init/*/image -type f | sed 's|.*/image||' | sort
```

Expected: the bitbake run ends with no errors, and the listing is:

```
/etc/irrigationd.ini
/usr/lib/systemd/system-preset/90-irrigationd.preset
/usr/lib/systemd/system/dev-rtc0.device.d/10-irrigation-timeout.conf
/usr/lib/systemd/system/irrigationd.service
```

plus the udev rule at `/lib/udev/rules.d/99-irrigation-rtc.rules` or `/usr/lib/udev/rules.d/99-irrigation-rtc.rules`, whichever `nonarch_base_libdir` resolves to in this distro. The controller's own rules live in `/lib/udev/rules.d`; a different prefix here means the distro moved, and the bench task copies to the same path the image uses. If `kas-container` cannot run (no podman or docker), say so and skip this step; the owner's image build covers it.

- [ ] **Step 10: Commit in the rpi repository**

```bash
cd /home/spunak/src/punak/rpi
git add meta-rpi4-irrigation/recipes-core/irrigation-init/files/irrigationd.service \
        meta-rpi4-irrigation/recipes-core/irrigation-init/files/99-irrigation-rtc.rules \
        meta-rpi4-irrigation/recipes-core/irrigation-init/files/dev-rtc0-timeout.conf \
        meta-rpi4-irrigation/recipes-core/irrigation-init/irrigation-init_1.0.bb \
        meta-rpi4-irrigation/recipes-core/irrigation-init/files/irrigationd.ini \
        kas/rpi4-irrigation.yml \
        meta-rpi4-irrigation/recipes-images/images/rpi4-irrigation-image.bb
git status --short
git commit -F - <<'EOF'
feat: start irrigationd after the RTC and clear the gardener panel on stop

irrigationd.service drops the network-online wait and orders itself
after dev-rtc0.device, the point at which the RTC has set the system
clock. A udev rule tags rtc0 for systemd so the device unit exists, and
a drop-in bounds the wait at 20 s when the RTC never appears. Every
stop now runs irrigationd --dashes, so a stopped controller shows ----.

The INI names the RUN button and display lines, config.txt pulls the
RUN input up and keeps the relay pads pulled high after release, and
the image guard checks both lines.

The irrigationd SRCREV stays put until the panel branch is pushed.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

Expected after `git status --short`, before the commit: the seven paths staged and the unrelated gateway files still unstaged.

---
### Task 11: Final whole-branch review and fix wave

Runs after every implementation task and before any test task. No new tests.

**Files:** whatever the review finds.

**Interfaces:** none.

- [ ] **Step 1:** In the irrigation repository, find the base with `base=$(git log --format=%H -1 -- docs/plans/2026-10-03-gardener-panel.md)`, then run `git log --oneline $base..HEAD` and `git diff $base..HEAD --stat`. Confirm one commit per Task 1–9 and no file outside the File Structure table. In the rpi repository, confirm Task 10's single commit with `git show --stat HEAD` (or the commit whose subject starts "feat: start irrigationd after the RTC") and that no gateway file rode along.
- [ ] **Step 2:** Spec walk. For each spec section 2, 3.1–3.7, 4, 5, 6.1, 6.2 and 7, read the code that implements it and record the file and function in the review notes. Flag anything the spec says that no code does, and anything the code does that the Rulings section does not cover.
- [ ] **Step 3:** Ordering traps. Read and confirm each against its comment or ruling:
  - `PanelHost::clearController()`: `dropAll` → `abort` → `allOff`.
  - `PanelHost::openPanelZone()`: every refusal before the first `closeZone()`; the `hasSlotFor()` re-check after the close; open-then-close when a slot is free.
  - `IrrigationDaemon::onStopPressed()`: `_panel->cancel()` before `clearController()`.
  - `IrrigationDaemon::threadAboutToFinish()`: panel timer, panel and host deleted before the queue; `_display` deleted before `_backend`.
  - `IrrigationDaemon::threadStarted()`: `setUpPanel()` after the queue exists; the first `_panel->tick()` after `connectComponents()` and before `_controlServer->start()`.
  - `PanelController::press()` and `tick()`: state set before `stateChanged` is emitted, so `publishStatus()` reads the new `panelZone()`.
  - `Tm1637Display::writeByte()`: DIO written low before the ninth clock rises.
- [ ] **Step 4:** Threading and timing. `grep -rn QueuedConnection IrrigationD/src` prints nothing. Every panel call happens on the daemon thread: `RunButton`, `PanelController`, `PanelHost`, `Tm1637Display` and `_panelTimer` are all created in `threadStarted()`. No interval in `PanelController` reads `nowUtc()`; only the clock face does.
- [ ] **Step 5:** Comment and prose audit. `git diff $base..HEAD -- '*.cpp' '*.h' '*.ts' '*.tsx' '*.css' '*.service'` and the rpi commit's diff: read every added comment against the Global Constraints rule and delete narration. Scan every commit message in both repositories for "X, not Y" antithesis.
- [ ] **Step 6:** Wire contract. Compare `StatusJson::sourceToJson()` with `RunSource` and `decodeRunningZone`; `SettingsKeys` and `isValidSettingValue("panel_run_minutes")` with `SETTING_KEYS.panelRunMinutes` and `PANEL_RUN_MINUTES_LIMIT`; the INI key names in `IrrigationSettings` with the rpi `irrigationd.ini`; the `--dashes` option name in `main.cpp` with both unit files.
- [ ] **Step 7:** Doxygen. Every public member of `PanelFormat`, `Tm1637Display`, `RunButton`, `PanelSnapshot`, `IPanelHost`, `PanelController` and `PanelHost` carries a doc comment.
- [ ] **Step 8:** Build everything and the web bundle:

```bash
cd /home/spunak/src/punak/irrigation
cmake --build build -j 32
npm --prefix web run build
```

- [ ] **Step 9:** Fix every finding in its own commit in the repository it belongs to, typed `fix`, `refactor` or `doc`, with the standard trailer. Re-run Step 8 after the last fix.

---

### Task 12: Daemon tests — the display bytes, the TM1637 framing and the RUN line

Spec §8: display formatting (the clock, the zone-and-minutes format with rounding, the blink phases, every glyph used) and TM1637 framing against the in-memory backend, including DIO low through each acknowledge clock.

**Files:**
- Create: `IrrigationD/tests/tst_panelformat.cpp`
- Create: `IrrigationD/tests/tst_tm1637display.cpp`
- Create: `IrrigationD/tests/tst_runbutton.cpp`
- Modify: `IrrigationD/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 1–3 as produced; `MockBackend` from KanoopPiQt (`simulateEdge`, `setLineValue`, `lineValue`, `lastInputRequest`, `lastOutputRequest`, `setFailNextRequest`, overridable `setValues`).

- [ ] **Step 1: Register the three suites in `IrrigationD/tests/CMakeLists.txt`**

Append:

```cmake
irrigation_add_test(tst_panelformat
    tst_panelformat.cpp
    ../src/panelformat.cpp
)

irrigation_add_test(tst_tm1637display
    tst_tm1637display.cpp
    ../src/tm1637display.cpp
)

irrigation_add_test(tst_runbutton
    tst_runbutton.cpp
    ../src/runbutton.cpp
)
```

- [ ] **Step 2: Create `IrrigationD/tests/tst_panelformat.cpp`**

```cpp
#include <QTest>

#include "panelformat.h"

namespace
{
QByteArray bytes(quint8 first, quint8 second, quint8 third, quint8 fourth)
{
    QByteArray result;
    result.append(static_cast<char>(first));
    result.append(static_cast<char>(second));
    result.append(static_cast<char>(third));
    result.append(static_cast<char>(fourth));
    return result;
}
}

class TestPanelFormat : public QObject
{
    Q_OBJECT
private slots:
    void clockBlanksTheLeadingDigitBeforeTen();
    void clockShowsBothHourDigitsFromTenToTwelve();
    void clockReadsMidnightAndNoonAsTwelve();
    void clockLightsTheColonOnlyWhenAsked();
    void zoneMinutesRightAlignsTheMinutes();
    void zoneMinutesBlanksTheZoneDigitWhenHidden();
    void zoneMinutesBoundsTheMinutesToTwoDigits();
    void minutesLeftRoundsUp_data();
    void minutesLeftRoundsUp();
    void everyGlyphUsedHasItsSegments_data();
    void everyGlyphUsedHasItsSegments();
    void textShowsACharacterWithoutAGlyphBlank();
};

void TestPanelFormat::clockBlanksTheLeadingDigitBeforeTen()
{
    // " 6:42" with the colon on digit 1.
    QCOMPARE(PanelFormat::clock(QTime(6, 42), true), bytes(0x00, 0x7D | 0x80, 0x66, 0x5B));
    QCOMPARE(PanelFormat::clock(QTime(18, 42), true), bytes(0x00, 0x7D | 0x80, 0x66, 0x5B));
}

void TestPanelFormat::clockShowsBothHourDigitsFromTenToTwelve()
{
    QCOMPARE(PanelFormat::clock(QTime(22, 5), true), bytes(0x06, 0x3F | 0x80, 0x3F, 0x6D));
    QCOMPARE(PanelFormat::clock(QTime(11, 59), true), bytes(0x06, 0x06 | 0x80, 0x6D, 0x6F));
}

void TestPanelFormat::clockReadsMidnightAndNoonAsTwelve()
{
    QCOMPARE(PanelFormat::clock(QTime(0, 30), true), bytes(0x06, 0x5B | 0x80, 0x4F, 0x3F));
    QCOMPARE(PanelFormat::clock(QTime(12, 0), true), bytes(0x06, 0x5B | 0x80, 0x3F, 0x3F));
}

void TestPanelFormat::clockLightsTheColonOnlyWhenAsked()
{
    const QByteArray lit = PanelFormat::clock(QTime(6, 42), true);
    const QByteArray dark = PanelFormat::clock(QTime(6, 42), false);
    QCOMPARE(static_cast<int>(static_cast<quint8>(lit.at(1)) & PanelFormat::Colon), static_cast<int>(PanelFormat::Colon));
    QCOMPARE(static_cast<int>(static_cast<quint8>(dark.at(1)) & PanelFormat::Colon), 0);
    QCOMPARE(dark, bytes(0x00, 0x7D, 0x66, 0x5B));
}

void TestPanelFormat::zoneMinutesRightAlignsTheMinutes()
{
    // "3 12", "3  1" and "1 10": digit 1 and the colon stay dark.
    QCOMPARE(PanelFormat::zoneMinutes(3, 12, true), bytes(0x4F, 0x00, 0x06, 0x5B));
    QCOMPARE(PanelFormat::zoneMinutes(3, 1, true), bytes(0x4F, 0x00, 0x00, 0x06));
    QCOMPARE(PanelFormat::zoneMinutes(1, 10, true), bytes(0x06, 0x00, 0x06, 0x3F));
}

void TestPanelFormat::zoneMinutesBlanksTheZoneDigitWhenHidden()
{
    QCOMPARE(PanelFormat::zoneMinutes(1, 10, false), bytes(0x00, 0x00, 0x06, 0x3F));
}

void TestPanelFormat::zoneMinutesBoundsTheMinutesToTwoDigits()
{
    QCOMPARE(PanelFormat::zoneMinutes(2, 250, true), bytes(0x5B, 0x00, 0x6F, 0x6F));
    QCOMPARE(PanelFormat::zoneMinutes(2, -4, true), bytes(0x5B, 0x00, 0x00, 0x3F));
}

void TestPanelFormat::minutesLeftRoundsUp_data()
{
    QTest::addColumn<int>("seconds");
    QTest::addColumn<int>("minutes");

    QTest::newRow("a negative count still shows a minute") << -5 << 1;
    QTest::newRow("zero seconds on an open zone shows a minute") << 0 << 1;
    QTest::newRow("one second") << 1 << 1;
    QTest::newRow("just under a minute") << 59 << 1;
    QTest::newRow("exactly a minute") << 60 << 1;
    QTest::newRow("one second over a minute") << 61 << 2;
    QTest::newRow("twelve minutes exactly") << 720 << 12;
    QTest::newRow("eleven minutes and one second") << 661 << 12;
    QTest::newRow("an hour") << 3600 << 60;
}

void TestPanelFormat::minutesLeftRoundsUp()
{
    QFETCH(int, seconds);
    QFETCH(int, minutes);
    QCOMPARE(PanelFormat::minutesLeft(seconds), minutes);
}

void TestPanelFormat::everyGlyphUsedHasItsSegments_data()
{
    QTest::addColumn<QByteArray>("actual");
    QTest::addColumn<QByteArray>("expected");

    QTest::newRow("StOP") << PanelFormat::stopHeld() << bytes(0x6D, 0x78, 0x3F, 0x73);
    QTest::newRow("Err, left-aligned") << PanelFormat::fault() << bytes(0x79, 0x50, 0x50, 0x00);
    QTest::newRow("OFF, left-aligned") << PanelFormat::masterOff() << bytes(0x3F, 0x71, 0x71, 0x00);
    QTest::newRow("FuLL") << PanelFormat::full() << bytes(0x71, 0x1C, 0x38, 0x38);
    QTest::newRow("nonE") << PanelFormat::noZones() << bytes(0x54, 0x5C, 0x54, 0x79);
    QTest::newRow("----") << PanelFormat::dashes() << bytes(0x40, 0x40, 0x40, 0x40);
    QTest::newRow("digits 0-3") << PanelFormat::text("0123") << bytes(0x3F, 0x06, 0x5B, 0x4F);
    QTest::newRow("digits 4-7") << PanelFormat::text("4567") << bytes(0x66, 0x6D, 0x7D, 0x07);
    QTest::newRow("digits 8-9") << PanelFormat::text("89") << bytes(0x7F, 0x6F, 0x00, 0x00);
}

void TestPanelFormat::everyGlyphUsedHasItsSegments()
{
    QFETCH(QByteArray, actual);
    QFETCH(QByteArray, expected);
    QCOMPARE(actual, expected);
}

void TestPanelFormat::textShowsACharacterWithoutAGlyphBlank()
{
    QCOMPARE(PanelFormat::text("S?P"), bytes(0x6D, 0x00, 0x73, 0x00));
    QCOMPARE(PanelFormat::text("StOPPED"), PanelFormat::stopHeld());
}

QTEST_MAIN(TestPanelFormat)
#include "tst_panelformat.moc"
```

- [ ] **Step 3: Create `IrrigationD/tests/tst_tm1637display.cpp`**

```cpp
#include <QTest>

#include <Kanoop/pi/mockbackend.h>

#include "tm1637display.h"

namespace
{
const quint32 CLOCK_OFFSET = 18;
const quint32 DATA_OFFSET = 27;

QByteArray bytes(quint8 first, quint8 second, quint8 third, quint8 fourth)
{
    QByteArray result;
    result.append(static_cast<char>(first));
    result.append(static_cast<char>(second));
    result.append(static_cast<char>(third));
    result.append(static_cast<char>(fourth));
    return result;
}
}

/** Records every successful line write in order. failAtWrite fails the write with that index once. */
class RecordingBackend : public MockBackend
{
public:
    class Write
    {
    public:
        quint32 offset = 0;
        bool high = false;
    };

    virtual bool setValues(Gpio::RequestHandle handle,
                           const QList<quint32>& offsets,
                           const QList<Gpio::Value>& values) override
    {
        if(failAtWrite >= 0 && writes.count() == failAtWrite) {
            failAtWrite = -1;
            setErrorText("injected write failure");
            return false;
        }
        if(MockBackend::setValues(handle, offsets, values) == false) {
            return false;
        }
        for(int i = 0; i < offsets.count(); i++) {
            Write write;
            write.offset = offsets.at(i);
            write.high = values.at(i) == Gpio::Value::Active;
            writes.append(write);
        }
        return true;
    }

    QList<Write> writes;
    int failAtWrite = -1;
};

/**
 * Replays line writes the way a TM1637 sees them, starting from both lines low. A start
 * is DIO falling while CLK is high; a stop is DIO rising while CLK is high. Each CLK
 * rising edge inside a frame samples DIO, eight data bits least significant first; the
 * ninth clock is the acknowledge, which runs from its rising edge to its falling edge.
 */
class Tm1637Trace
{
public:
    explicit Tm1637Trace(const QList<RecordingBackend::Write>& writes)
    {
        bool clock = false;
        bool data = false;
        bool inFrame = false;
        bool inAcknowledge = false;
        bool dataHighDuringAcknowledge = false;
        int bit = 0;
        quint8 value = 0;
        QByteArray frame;

        for(const RecordingBackend::Write& write : writes) {
            if(write.offset == CLOCK_OFFSET) {
                const bool rising = clock == false && write.high == true;
                const bool falling = clock == true && write.high == false;
                clock = write.high;
                if(inFrame == true && rising == true) {
                    if(bit < 8) {
                        if(data == true) {
                            value |= static_cast<quint8>(1 << bit);
                        }
                        bit++;
                    }
                    else {
                        inAcknowledge = true;
                        dataHighDuringAcknowledge = data;
                    }
                }
                if(inFrame == true && falling == true && inAcknowledge == true) {
                    acknowledgeClocks++;
                    if(dataHighDuringAcknowledge == true) {
                        acknowledgeClocksWithDataHigh++;
                    }
                    frame.append(static_cast<char>(value));
                    value = 0;
                    bit = 0;
                    inAcknowledge = false;
                }
            }
            else if(write.offset == DATA_OFFSET) {
                if(clock == true && data == true && write.high == false) {
                    inFrame = true;
                    frame.clear();
                    bit = 0;
                    value = 0;
                }
                else if(clock == true && data == false && write.high == true && inFrame == true) {
                    frames.append(frame);
                    inFrame = false;
                }
                data = write.high;
                if(inAcknowledge == true && data == true) {
                    dataHighDuringAcknowledge = true;
                }
            }
        }
    }

    QList<QByteArray> frames;
    int acknowledgeClocks = 0;
    int acknowledgeClocksWithDataHigh = 0;
};

class TestTm1637Display : public QObject
{
    Q_OBJECT
private slots:
    void beginRequestsBothLinesAsOutputsDrivenLow();
    void oneUpdateSendsTheDataAddressAndControlCommands();
    void everyByteGoesOutLeastSignificantBitFirst();
    void dataIsLowThroughEveryAcknowledgeClock();
    void bothLinesRestHighAfterAnUpdate();
    void onlyTheTwoDisplayLinesAreWritten();
    void showRefusesAnythingButFourBytes();
    void showBeforeBeginFails();
    void aFailedWriteFailsTheUpdateAndTheNextIsWhole();
    void beginFailsWhenTheLinesCannotBeRequested();
    void aSecondBeginIsRefused();
};

void TestTm1637Display::beginRequestsBothLinesAsOutputsDrivenLow()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);
    QVERIFY(display.begin());

    const Gpio::OutputRequest request = backend.lastOutputRequest();
    QCOMPARE(request.consumer, QString("irrigationd-display"));
    QCOMPARE(request.offsets, QList<quint32>({ CLOCK_OFFSET, DATA_OFFSET }));
    QCOMPARE(request.activeLow, false);
    QCOMPARE(request.initialValue, Gpio::Value::Inactive);
}

void TestTm1637Display::oneUpdateSendsTheDataAddressAndControlCommands()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);
    display.setEdgeDelayMicroseconds(0);
    QVERIFY(display.begin());

    QVERIFY2(display.show(bytes(0x06, 0x5B | 0x80, 0x4F, 0x66)), qPrintable(display.errorText()));

    const Tm1637Trace trace(backend.writes);
    QCOMPARE(trace.frames.count(), 3);
    QCOMPARE(trace.frames.at(0), QByteArray(1, static_cast<char>(0x40)));
    QByteArray address(1, static_cast<char>(0xC0));
    address.append(bytes(0x06, 0x5B | 0x80, 0x4F, 0x66));
    QCOMPARE(trace.frames.at(1), address);
    QCOMPARE(trace.frames.at(2), QByteArray(1, static_cast<char>(0x88 | Tm1637Display::Brightness)));
    QCOMPARE(static_cast<quint8>(trace.frames.at(2).at(0)), quint8(0x8C));
}

void TestTm1637Display::everyByteGoesOutLeastSignificantBitFirst()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);
    display.setEdgeDelayMicroseconds(0);
    QVERIFY(display.begin());
    QVERIFY(display.show(bytes(0x01, 0x80, 0x00, 0xFF)));

    // The first data clock of the 0x01 byte must carry DIO high and the next seven low.
    const Tm1637Trace trace(backend.writes);
    QCOMPARE(trace.frames.at(1).mid(1), bytes(0x01, 0x80, 0x00, 0xFF));
}

void TestTm1637Display::dataIsLowThroughEveryAcknowledgeClock()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);
    display.setEdgeDelayMicroseconds(0);
    QVERIFY(display.begin());

    // Every segment byte ends on a 1 bit, so DIO is high when each acknowledge clock comes due.
    QVERIFY(display.show(bytes(0xFF, 0xFF, 0xFF, 0xFF)));

    const Tm1637Trace trace(backend.writes);
    // 0x40, then 0xC0 and four segment bytes, then 0x8C: seven bytes, seven acknowledge clocks.
    QCOMPARE(trace.acknowledgeClocks, 7);
    QCOMPARE(trace.acknowledgeClocksWithDataHigh, 0);
}

void TestTm1637Display::bothLinesRestHighAfterAnUpdate()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);
    display.setEdgeDelayMicroseconds(0);
    QVERIFY(display.begin());
    QCOMPARE(backend.lineValue(CLOCK_OFFSET), Gpio::Value::Inactive);
    QCOMPARE(backend.lineValue(DATA_OFFSET), Gpio::Value::Inactive);

    QVERIFY(display.show(bytes(0x40, 0x40, 0x40, 0x40)));
    QCOMPARE(backend.lineValue(CLOCK_OFFSET), Gpio::Value::Active);
    QCOMPARE(backend.lineValue(DATA_OFFSET), Gpio::Value::Active);
}

void TestTm1637Display::onlyTheTwoDisplayLinesAreWritten()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);
    display.setEdgeDelayMicroseconds(0);
    QVERIFY(display.begin());
    QVERIFY(display.show(bytes(0x40, 0x40, 0x40, 0x40)));

    QVERIFY(backend.writes.isEmpty() == false);
    for(const RecordingBackend::Write& write : backend.writes) {
        QVERIFY(write.offset == CLOCK_OFFSET || write.offset == DATA_OFFSET);
    }
}

void TestTm1637Display::showRefusesAnythingButFourBytes()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);
    display.setEdgeDelayMicroseconds(0);
    QVERIFY(display.begin());

    QVERIFY(display.show(QByteArray(3, '\0')) == false);
    QVERIFY(display.show(QByteArray(5, '\0')) == false);
    QVERIFY(display.errorText().contains("4 segment bytes"));
    QVERIFY(backend.writes.isEmpty());
}

void TestTm1637Display::showBeforeBeginFails()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);

    QVERIFY(display.show(bytes(0x40, 0x40, 0x40, 0x40)) == false);
    QVERIFY(display.errorText().isEmpty() == false);
    QVERIFY(backend.writes.isEmpty());
}

void TestTm1637Display::aFailedWriteFailsTheUpdateAndTheNextIsWhole()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);
    display.setEdgeDelayMicroseconds(0);
    QVERIFY(display.begin());

    // Write 40 lands inside the address command's first segment byte.
    backend.failAtWrite = 40;
    QVERIFY(display.show(bytes(0x06, 0x06, 0x06, 0x06)) == false);
    QVERIFY(display.errorText().contains("injected write failure"));
    QCOMPARE(backend.writes.count(), 40);

    backend.writes.clear();
    QVERIFY(display.show(bytes(0x5B, 0x5B, 0x5B, 0x5B)));
    const Tm1637Trace trace(backend.writes);
    QCOMPARE(trace.frames.count(), 3);
    QCOMPARE(trace.frames.at(1).mid(1), bytes(0x5B, 0x5B, 0x5B, 0x5B));
}

void TestTm1637Display::beginFailsWhenTheLinesCannotBeRequested()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);

    backend.setFailNextRequest(true);
    QVERIFY(display.begin() == false);
    QVERIFY(display.errorText().isEmpty() == false);
    QVERIFY(display.show(bytes(0x40, 0x40, 0x40, 0x40)) == false);

    QVERIFY(display.begin());
}

void TestTm1637Display::aSecondBeginIsRefused()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);
    QVERIFY(display.begin());
    QVERIFY(display.begin() == false);
    QVERIFY(display.errorText().contains("already"));
}

QTEST_MAIN(TestTm1637Display)
#include "tst_tm1637display.moc"
```

Write 40 sits in the address frame: the data-command frame takes 4 (start) + 28 (byte) + 4 (stop) = 36 writes, the address start condition 4 more, so index 40 is the first write of the 0xC0 byte. The test asserts the count it relies on.

- [ ] **Step 4: Create `IrrigationD/tests/tst_runbutton.cpp`**

```cpp
#include <QTest>
#include <QSignalSpy>

#include <Kanoop/pi/mockbackend.h>

#include "runbutton.h"

namespace
{
// Off the production RUN offset (24) and every other configured line, so a RunButton
// that ignores its configured offset fails loudly.
const quint32 RUN_OFFSET = 26;
}

class TestRunButton : public QObject
{
    Q_OBJECT
private slots:
    void requestUsesActiveLowPullUpBothEdgesAnd20msDebounce();
    void aPressReportsLowAndAReleaseReportsHigh();
    void aLineLowAtStartupIsReadWithoutAChange();
    void beginFailsWhenTheLineCannotBeRequested();
    void aSecondBeginIsRefused();
};

void TestRunButton::requestUsesActiveLowPullUpBothEdgesAnd20msDebounce()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    RunButton button(&backend, RUN_OFFSET);
    QVERIFY(button.begin());

    const Gpio::InputRequest request = backend.lastInputRequest();
    QCOMPARE(request.consumer, QString("irrigationd-run"));
    QCOMPARE(request.offsets, QList<quint32>({ RUN_OFFSET }));
    QCOMPARE(request.activeLow, true);
    QCOMPARE(request.bias, Gpio::Bias::PullUp);
    QCOMPARE(request.edge, Gpio::Edge::Both);
    QCOMPARE(request.debounceMicroseconds, 20000UL);
}

// A press pulls the line low; activeLow inversion reports it as a logical Rising edge.
void TestRunButton::aPressReportsLowAndAReleaseReportsHigh()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    RunButton button(&backend, RUN_OFFSET);
    QVERIFY(button.begin());
    QSignalSpy spy(&button, &RunButton::lineChanged);

    backend.simulateEdge(RUN_OFFSET, Gpio::Edge::Rising);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toBool(), true);
    QVERIFY(button.isLow());

    backend.simulateEdge(RUN_OFFSET, Gpio::Edge::Falling);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toBool(), false);
    QVERIFY(button.isLow() == false);
}

void TestRunButton::aLineLowAtStartupIsReadWithoutAChange()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    backend.setLineValue(RUN_OFFSET, Gpio::Value::Active);

    RunButton button(&backend, RUN_OFFSET);
    QSignalSpy spy(&button, &RunButton::lineChanged);
    QVERIFY(button.begin());

    QVERIFY(button.isLow());
    QCOMPARE(spy.count(), 0);
}

void TestRunButton::beginFailsWhenTheLineCannotBeRequested()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    RunButton button(&backend, RUN_OFFSET);

    backend.setFailNextRequest(true);
    QVERIFY(button.begin() == false);
    QVERIFY(button.errorText().isEmpty() == false);

    QVERIFY(button.begin());
}

void TestRunButton::aSecondBeginIsRefused()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    RunButton button(&backend, RUN_OFFSET);
    QVERIFY(button.begin());
    QVERIFY(button.begin() == false);
    QVERIFY(button.errorText().contains("already"));
}

QTEST_MAIN(TestRunButton)
#include "tst_runbutton.moc"
```

- [ ] **Step 5: Build and run**

```bash
cmake --build build -j 32
ctest --test-dir build --output-on-failure -R 'tst_panelformat|tst_tm1637display|tst_runbutton'
```

Expected: all three pass. A failure here is a defect in Tasks 1–3: fix the production code in a `fix` commit, never the expected bytes, unless the spec or this plan's segment table says the test is wrong.

- [ ] **Step 6: Commit**

```bash
git add IrrigationD/tests/CMakeLists.txt IrrigationD/tests/tst_panelformat.cpp \
        IrrigationD/tests/tst_tm1637display.cpp IrrigationD/tests/tst_runbutton.cpp
git commit -F - <<'EOF'
test: pin the panel's segment bytes, the TM1637 framing and the RUN line

The display tests cover the 12-hour clock and its colon, the
zone-and-minutes view with rounding and bounds, and every word the
panel shows. The TM1637 tests replay the line writes as the chip sees
them: three commands per update, least significant bit first, DIO low
through every acknowledge clock, and a whole frame after a failed
write. The RUN line tests pin its request and its level reports.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---
### Task 13: Daemon tests — the panel state machine

Spec §8 (the panel state machine with a test clock and fake presses: selection order and wrap over enabled zones only and the three-second commit; next zone during a run and the end at the last zone and at the timer; takeover; each refusal display and the priority of STOP held and a fault; alternation over other running zones; the stuck-line rule) and the display's blink phases. Review Focus 1–5 on the panel's side. Takeover's effect on a real program and queue is pinned in Task 14.

**Files:**
- Create: `IrrigationD/tests/tst_panelcontroller.cpp`
- Modify: `IrrigationD/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Tasks 1 and 4 as produced; `LogConsumer` and `Log::LogEntry::unformattedText()` from KanoopCommonQt, as `tst_settings` uses them.

- [ ] **Step 1: Register the suite in `IrrigationD/tests/CMakeLists.txt`**

Append:

```cmake
irrigation_add_test(tst_panelcontroller
    tst_panelcontroller.cpp
    ../src/panelcontroller.cpp
    ../src/panelformat.cpp
    ../src/runrequest.cpp
)
```

- [ ] **Step 2: Create `IrrigationD/tests/tst_panelcontroller.cpp`**

```cpp
#include <QTest>
#include <QPair>
#include <QSignalSpy>
#include <QStringList>
#include <QTimeZone>

#include <Kanoop/log.h>
#include <Kanoop/logconsumer.h>
#include <Kanoop/logentry.h>

#include "iclock.h"
#include "panelcontroller.h"
#include "panelformat.h"

/** An IPanelHost whose state the test sets by hand. Opens and closes change that state the way the daemon would. */
class FakeHost : public IPanelHost
{
public:
    FakeHost()
    {
        state.enabledZones = { 1, 2, 3, 4, 5, 6, 7, 8 };
        state.runMinutes = 10;
    }

    virtual PanelSnapshot panelSnapshot() override
    {
        return state;
    }

    virtual RunRequest::Refusal openPanelZone(int zoneNumber, int replacingZone) override
    {
        opens.append(QPair<int, int>(zoneNumber, replacingZone));
        if(nextRefusal != RunRequest::Refusal::None) {
            const RunRequest::Refusal refusal = nextRefusal;
            nextRefusal = RunRequest::Refusal::None;
            return refusal;
        }
        if(replacingZone > 0) {
            close(replacingZone);
        }
        open(zoneNumber, runSeconds);
        return RunRequest::Refusal::None;
    }

    virtual void closePanelZone(int zoneNumber) override
    {
        closes.append(zoneNumber);
        close(zoneNumber);
    }

    virtual void takeOverForPanel() override
    {
        takeOvers++;
        state.openZones.clear();
    }

    void open(int zoneNumber, int seconds)
    {
        close(zoneNumber);
        PanelSnapshot::OpenZone entry;
        entry.zone = zoneNumber;
        entry.secondsRemaining = seconds;
        int index = 0;
        while(index < state.openZones.count() && state.openZones.at(index).zone < zoneNumber) {
            index++;
        }
        state.openZones.insert(index, entry);
    }

    void close(int zoneNumber)
    {
        for(int i = 0; i < state.openZones.count(); i++) {
            if(state.openZones.at(i).zone == zoneNumber) {
                state.openZones.removeAt(i);
                return;
            }
        }
    }

    bool isOpen(int zoneNumber) const { return state.secondsRemaining(zoneNumber) >= 0; }

    /** Reads "2/0 3/2": each open request as zone/replacing, in order. */
    QString openCalls() const
    {
        QStringList calls;
        for(const QPair<int, int>& call : opens) {
            calls.append(QString("%1/%2").arg(call.first).arg(call.second));
        }
        return calls.join(' ');
    }

    PanelSnapshot state;
    RunRequest::Refusal nextRefusal = RunRequest::Refusal::None;
    QList<QPair<int, int>> opens;
    QList<int> closes;
    int takeOvers = 0;
    int runSeconds = 600;
};

class Rig
{
public:
    Rig() :
        clock(QDateTime(QDate(2026, 10, 3), QTime(18, 42, 0), QTimeZone::UTC)),
        panel(&host, &clock)
    {
        panel.setTimeZone(QTimeZone(QTimeZone::UTC));
    }

    /** One press and its release, as the RUN line reports them. */
    void press()
    {
        panel.onRunLineChanged(true);
        panel.onRunLineChanged(false);
    }

    /** Lets @p msecs pass in the daemon's 100 ms ticks. */
    void wait(qint64 msecs)
    {
        for(qint64 elapsed = 0; elapsed < msecs; elapsed += 100) {
            clock.advanceMsecs(qMin<qint64>(100, msecs - elapsed));
            panel.tick();
        }
    }

    /** Presses until @p zoneNumber is on offer, then waits out the commit. */
    void startRunOn(int zoneNumber)
    {
        press();
        for(int presses = 0; presses < 16 && panel.selectedZone() != zoneNumber; presses++) {
            press();
        }
        wait(PanelController::CommitDelayMsecs);
    }

    FakeHost host;
    TestClock clock;
    PanelController panel;
};

/** Collects every log line written while it lives. */
class LogCapture
{
public:
    LogCapture()
    {
        QObject::connect(&_consumer, &LogConsumer::logEntry, [this](const Log::LogEntry& entry) {
            _entries.append(entry);
        });
        Log::addConsumer(&_consumer);
    }

    ~LogCapture()
    {
        Log::removeConsumer(&_consumer);
    }

    int count(const QString& fragment) const
    {
        int result = 0;
        for(const Log::LogEntry& entry : _entries) {
            if(entry.unformattedText().contains(fragment) == true) {
                result++;
            }
        }
        return result;
    }

private:
    LogConsumer _consumer;
    QList<Log::LogEntry> _entries;
};

class TestPanelController : public QObject
{
    Q_OBJECT
private slots:
    void idleShowsTheClockWithABlinkingColon();
    void idleClockReadsTheControllerZone();
    void aFrameIsEmittedOnlyWhenItChanges();
    void firstPressShowsTheFirstEnabledZoneBlinkingWithTheRunMinutes();
    void eachPressMovesToTheNextEnabledZoneAndWraps();
    void disabledZonesAreNeverOffered();
    void theSelectionCommitsThreeSecondsAfterTheLastPress();
    void noEnabledZoneShowsNoneForTwoSeconds();
    void aPanelRunCountsDownInMinutesRoundedUp();
    void aPressDuringARunOpensTheNextEnabledZoneAtOnce();
    void aPressOnTheLastEnabledZoneClosesItAndEndsTheRun();
    void theRunEndsWithItsZoneAndNeverMovesOn();
    void aPressJustAfterThePanelZoneClosedStartsASelection();
    void duringARunOnlyThePanelZoneShows();
    void aPressWithOtherZonesOpenTakesOverAndShowsTheSelection();
    void aPressWithNothingOpenTakesNothingOver();
    void stopHeldShowsStopAndIgnoresPresses();
    void aFaultShowsErrAndIgnoresPresses();
    void stopHeldAndAFaultWinOverEverythingElse();
    void masterOffShowsOffForTwoSecondsAndStartsNothing();
    void masterOffAtTheCommitShowsOff();
    void aRefusalAtTheCapShowsFullAndStartsNothing();
    void aRefusedAdvanceKeepsTheCurrentZoneRunning();
    void aRefusedPressTakesNothingOver();
    void aZoneDisabledBeforeTheCommitOpensNothing();
    void stopHeldDuringASelectionCancelsIt();
    void cancelEndsASelectionAndARun();
    void otherZonesAlternateEveryTwoSecondsInZoneOrder();
    void aClosedShownZoneMovesOnAtOnce();
    void aWallClockStepDoesNotMoveTheCommit();
    void aLineHeldLowIsLoggedOnceAsStuck();
    void aLowWithoutASeenHighIsNoPress();
    void aLineLowAtStartupGivesNoPress();
    void aLineLowAtStartupIsReportedStuckAfterTenSeconds();
    void aSeenHighClearsTheStuckState();
};

void TestPanelController::idleShowsTheClockWithABlinkingColon()
{
    Rig rig;
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::clock(QTime(18, 42), true));
    rig.wait(500);
    QCOMPARE(rig.panel.frame(), PanelFormat::clock(QTime(18, 42), false));
    rig.wait(500);
    QCOMPARE(rig.panel.frame(), PanelFormat::clock(QTime(18, 42), true));
}

void TestPanelController::idleClockReadsTheControllerZone()
{
    Rig rig;
    rig.panel.setTimeZone(QTimeZone("America/Los_Angeles"));
    rig.clock.setNowUtc(QDateTime(QDate(2026, 10, 3), QTime(13, 5, 0), QTimeZone::UTC));
    rig.panel.tick();

    // 13:05 UTC is 06:05 PDT: " 6:05", leading digit blank.
    QCOMPARE(rig.panel.frame(), PanelFormat::clock(QTime(6, 5), true));
    QCOMPARE(static_cast<int>(rig.panel.frame().at(0)), 0);
}

void TestPanelController::aFrameIsEmittedOnlyWhenItChanges()
{
    Rig rig;
    QSignalSpy frames(&rig.panel, &PanelController::frameChanged);
    rig.panel.tick();
    rig.wait(400);
    QCOMPARE(frames.count(), 1);
    rig.wait(100);
    QCOMPARE(frames.count(), 2);
}

void TestPanelController::firstPressShowsTheFirstEnabledZoneBlinkingWithTheRunMinutes()
{
    Rig rig;
    rig.host.state.enabledZones = { 2, 3, 5 };
    rig.press();

    QCOMPARE(rig.panel.mode(), PanelController::Mode::Selecting);
    QCOMPARE(rig.panel.selectedZone(), 2);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 10, true));
    rig.wait(500);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 10, false));
    rig.wait(500);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 10, true));
    QVERIFY(rig.host.opens.isEmpty());
}

void TestPanelController::eachPressMovesToTheNextEnabledZoneAndWraps()
{
    Rig rig;
    rig.host.state.enabledZones = { 2, 3, 5 };
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 2);
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 3);
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 5);
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 2);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 10, true));
}

void TestPanelController::disabledZonesAreNeverOffered()
{
    Rig rig;
    rig.host.state.enabledZones = { 1, 4 };
    rig.host.state.runMinutes = 25;
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 1);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(1, 25, true));
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 4);
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 1);
}

void TestPanelController::theSelectionCommitsThreeSecondsAfterTheLastPress()
{
    Rig rig;
    rig.press();
    rig.wait(2000);
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 2);
    rig.wait(2900);
    QVERIFY(rig.host.opens.isEmpty());
    rig.wait(100);

    QCOMPARE(rig.host.openCalls(), QString("2/0"));
    QCOMPARE(rig.panel.mode(), PanelController::Mode::PanelRun);
    QCOMPARE(rig.panel.panelZone(), 2);
    QCOMPARE(rig.panel.selectedZone(), 0);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 10, true));
}

void TestPanelController::noEnabledZoneShowsNoneForTwoSeconds()
{
    Rig rig;
    rig.host.state.enabledZones.clear();
    rig.press();
    QCOMPARE(rig.panel.frame(), PanelFormat::noZones());
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    rig.wait(1900);
    QCOMPARE(rig.panel.frame(), PanelFormat::noZones());
    rig.wait(100);
    QCOMPARE(rig.panel.frame(), PanelFormat::clock(QTime(18, 42), true));
    QVERIFY(rig.host.opens.isEmpty());
}

void TestPanelController::aPanelRunCountsDownInMinutesRoundedUp()
{
    Rig rig;
    rig.startRunOn(3);
    QCOMPARE(rig.panel.panelZone(), 3);

    rig.host.open(3, 661);
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(3, 12, true));
    rig.host.open(3, 660);
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(3, 11, true));
    rig.host.open(3, 1);
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(3, 1, true));
}

void TestPanelController::aPressDuringARunOpensTheNextEnabledZoneAtOnce()
{
    Rig rig;
    rig.startRunOn(1);
    rig.press();

    QCOMPARE(rig.host.openCalls(), QString("1/0 2/1"));
    QCOMPARE(rig.panel.mode(), PanelController::Mode::PanelRun);
    QCOMPARE(rig.panel.panelZone(), 2);
    QVERIFY(rig.host.isOpen(1) == false);
    QVERIFY(rig.host.isOpen(2));
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 10, true));
}

void TestPanelController::aPressOnTheLastEnabledZoneClosesItAndEndsTheRun()
{
    Rig rig;
    rig.host.state.enabledZones = { 1, 2 };
    rig.startRunOn(1);
    rig.press();
    QCOMPARE(rig.panel.panelZone(), 2);
    rig.press();

    QCOMPARE(rig.host.closes, QList<int>({ 2 }));
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    QCOMPARE(rig.panel.panelZone(), 0);
    QVERIFY(rig.host.state.openZones.isEmpty());
    QCOMPARE(rig.host.openCalls(), QString("1/0 2/1"));
    QCOMPARE(rig.panel.frame(), PanelFormat::clock(QTime(18, 42), true));
}

void TestPanelController::theRunEndsWithItsZoneAndNeverMovesOn()
{
    Rig rig;
    rig.startRunOn(1);
    rig.host.close(1);
    rig.panel.tick();

    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    QCOMPARE(rig.panel.panelZone(), 0);
    rig.wait(5000);
    QCOMPARE(rig.host.openCalls(), QString("1/0"));
    QCOMPARE(rig.panel.frame(), PanelFormat::clock(QTime(18, 42), true));
}

// Review Focus 3.
void TestPanelController::aPressJustAfterThePanelZoneClosedStartsASelection()
{
    Rig rig;
    rig.startRunOn(1);
    rig.host.close(1);
    rig.press();

    QCOMPARE(rig.host.openCalls(), QString("1/0"));
    QVERIFY(rig.host.closes.isEmpty());
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Selecting);
    QCOMPARE(rig.panel.selectedZone(), 1);
}

void TestPanelController::duringARunOnlyThePanelZoneShows()
{
    Rig rig;
    rig.startRunOn(1);
    rig.host.open(5, 300);
    rig.wait(4000);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(1, 10, true));

    rig.host.close(1);
    rig.panel.tick();
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(5, 5, true));
}

void TestPanelController::aPressWithOtherZonesOpenTakesOverAndShowsTheSelection()
{
    Rig rig;
    rig.host.open(4, 300);
    rig.host.open(6, 120);
    rig.press();

    QCOMPARE(rig.host.takeOvers, 1);
    QVERIFY(rig.host.state.openZones.isEmpty());
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Selecting);
    QCOMPARE(rig.panel.selectedZone(), 1);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(1, 10, true));
    QVERIFY(rig.host.opens.isEmpty());
}

void TestPanelController::aPressWithNothingOpenTakesNothingOver()
{
    Rig rig;
    rig.press();
    QCOMPARE(rig.host.takeOvers, 0);
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Selecting);
}

void TestPanelController::stopHeldShowsStopAndIgnoresPresses()
{
    Rig rig;
    rig.host.state.stopHeld = true;
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::stopHeld());

    rig.press();
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    QCOMPARE(rig.host.takeOvers, 0);
    QVERIFY(rig.host.opens.isEmpty());
    QCOMPARE(rig.panel.frame(), PanelFormat::stopHeld());
}

void TestPanelController::aFaultShowsErrAndIgnoresPresses()
{
    Rig rig;
    rig.host.open(4, 300);
    rig.host.state.faulted = true;
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::fault());

    rig.press();
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    QCOMPARE(rig.host.takeOvers, 0);
    QVERIFY(rig.host.opens.isEmpty());
    QCOMPARE(rig.panel.frame(), PanelFormat::fault());
}

void TestPanelController::stopHeldAndAFaultWinOverEverythingElse()
{
    Rig rig;
    rig.startRunOn(1);
    rig.host.state.faulted = true;
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::fault());
    rig.host.state.stopHeld = true;
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::stopHeld());

    rig.host.state.stopHeld = false;
    rig.host.state.faulted = false;
    rig.host.close(1);
    rig.panel.tick();
    rig.host.state.enabledZones.clear();
    rig.press();
    QCOMPARE(rig.panel.frame(), PanelFormat::noZones());
    rig.host.state.stopHeld = true;
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::stopHeld());
}

void TestPanelController::masterOffShowsOffForTwoSecondsAndStartsNothing()
{
    Rig rig;
    rig.host.state.masterEnabled = false;
    rig.press();
    QCOMPARE(rig.panel.frame(), PanelFormat::masterOff());
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    QVERIFY(rig.host.opens.isEmpty());
    rig.wait(2000);
    QCOMPARE(rig.panel.frame(), PanelFormat::clock(QTime(18, 42), true));
}

void TestPanelController::masterOffAtTheCommitShowsOff()
{
    Rig rig;
    rig.press();
    rig.host.nextRefusal = RunRequest::Refusal::MasterDisabled;
    rig.wait(3000);
    QCOMPARE(rig.host.openCalls(), QString("1/0"));
    QCOMPARE(rig.panel.frame(), PanelFormat::masterOff());
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
}

void TestPanelController::aRefusalAtTheCapShowsFullAndStartsNothing()
{
    Rig rig;
    rig.press();
    rig.host.nextRefusal = RunRequest::Refusal::CapReached;
    rig.wait(3000);
    QCOMPARE(rig.panel.frame(), PanelFormat::full());
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    QCOMPARE(rig.panel.panelZone(), 0);
    rig.wait(2000);
    QCOMPARE(rig.panel.frame(), PanelFormat::clock(QTime(18, 42), true));
}

// Review Focus 1.
void TestPanelController::aRefusedAdvanceKeepsTheCurrentZoneRunning()
{
    Rig rig;
    rig.startRunOn(1);

    rig.host.nextRefusal = RunRequest::Refusal::CapReached;
    rig.press();
    QCOMPARE(rig.host.openCalls(), QString("1/0 2/1"));
    QCOMPARE(rig.panel.frame(), PanelFormat::full());
    QCOMPARE(rig.panel.mode(), PanelController::Mode::PanelRun);
    QCOMPARE(rig.panel.panelZone(), 1);
    QVERIFY(rig.host.isOpen(1));
    rig.wait(2000);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(1, 10, true));

    rig.host.nextRefusal = RunRequest::Refusal::MasterDisabled;
    rig.press();
    QCOMPARE(rig.panel.frame(), PanelFormat::masterOff());
    QCOMPARE(rig.panel.panelZone(), 1);
    QVERIFY(rig.host.isOpen(1));
}

// Review Focus 4.
void TestPanelController::aRefusedPressTakesNothingOver()
{
    Rig rig;
    rig.host.open(4, 300);

    rig.host.state.masterEnabled = false;
    rig.press();
    QCOMPARE(rig.panel.frame(), PanelFormat::masterOff());
    QCOMPARE(rig.host.takeOvers, 0);
    QVERIFY(rig.host.isOpen(4));

    rig.host.state.masterEnabled = true;
    rig.host.state.enabledZones.clear();
    rig.press();
    QCOMPARE(rig.panel.frame(), PanelFormat::noZones());
    QCOMPARE(rig.host.takeOvers, 0);
    QVERIFY(rig.host.isOpen(4));
}

// Review Focus 5.
void TestPanelController::aZoneDisabledBeforeTheCommitOpensNothing()
{
    Rig rig;
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 1);
    rig.host.state.enabledZones = { 2, 3 };
    rig.wait(3000);
    QVERIFY(rig.host.opens.isEmpty());
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);

    rig.host.state.enabledZones = { 1, 2, 3 };
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 1);
    rig.host.state.enabledZones = { 3 };
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 3);
}

void TestPanelController::stopHeldDuringASelectionCancelsIt()
{
    Rig rig;
    rig.press();
    rig.host.state.stopHeld = true;
    rig.panel.tick();
    rig.host.state.stopHeld = false;
    rig.wait(3000);
    QVERIFY(rig.host.opens.isEmpty());
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
}

void TestPanelController::cancelEndsASelectionAndARun()
{
    Rig rig;
    rig.press();
    rig.panel.cancel();
    rig.wait(3000);
    QVERIFY(rig.host.opens.isEmpty());
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);

    rig.startRunOn(2);
    QCOMPARE(rig.panel.panelZone(), 2);
    rig.panel.cancel();
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    QCOMPARE(rig.panel.panelZone(), 0);
}

void TestPanelController::otherZonesAlternateEveryTwoSecondsInZoneOrder()
{
    Rig rig;
    rig.host.open(6, 300);
    rig.host.open(2, 120);
    rig.host.open(4, 61);
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 2, true));
    rig.wait(1900);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 2, true));
    rig.wait(100);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(4, 2, true));
    rig.wait(2000);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(6, 5, true));
    rig.wait(2000);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 2, true));
}

void TestPanelController::aClosedShownZoneMovesOnAtOnce()
{
    Rig rig;
    rig.host.open(2, 120);
    rig.host.open(4, 61);
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 2, true));
    rig.host.close(2);
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(4, 2, true));
}

// Review Focus 2.
void TestPanelController::aWallClockStepDoesNotMoveTheCommit()
{
    Rig backward;
    backward.press();
    backward.clock.setNowUtc(backward.clock.nowUtc().addSecs(-3600));
    backward.wait(2900);
    QVERIFY(backward.host.opens.isEmpty());
    backward.wait(100);
    QCOMPARE(backward.host.openCalls(), QString("1/0"));

    Rig forward;
    forward.press();
    forward.clock.setNowUtc(forward.clock.nowUtc().addSecs(3600));
    forward.panel.tick();
    QVERIFY(forward.host.opens.isEmpty());
    forward.wait(3000);
    QCOMPARE(forward.host.openCalls(), QString("1/0"));
}

void TestPanelController::aLineHeldLowIsLoggedOnceAsStuck()
{
    LogCapture log;
    Rig rig;
    rig.panel.onRunLineChanged(true);
    rig.wait(10000);
    QVERIFY(rig.panel.isRunLineStuck() == false);
    rig.wait(100);
    QVERIFY(rig.panel.isRunLineStuck());
    QCOMPARE(log.count("stuck"), 1);
    rig.wait(20000);
    QCOMPARE(log.count("stuck"), 1);
}

void TestPanelController::aLowWithoutASeenHighIsNoPress()
{
    Rig rig;
    rig.panel.onRunLineChanged(true);
    QCOMPARE(rig.panel.selectedZone(), 1);
    rig.panel.onRunLineChanged(true);
    QCOMPARE(rig.panel.selectedZone(), 1);
    rig.panel.onRunLineChanged(false);
    rig.panel.onRunLineChanged(true);
    QCOMPARE(rig.panel.selectedZone(), 2);
}

void TestPanelController::aLineLowAtStartupGivesNoPress()
{
    Rig rig;
    rig.panel.setInitialRunLine(true);
    rig.panel.tick();
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    rig.panel.onRunLineChanged(true);
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    rig.panel.onRunLineChanged(false);
    rig.panel.onRunLineChanged(true);
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Selecting);
}

void TestPanelController::aLineLowAtStartupIsReportedStuckAfterTenSeconds()
{
    Rig rig;
    rig.panel.setInitialRunLine(true);
    rig.wait(10100);
    QVERIFY(rig.panel.isRunLineStuck());
    QVERIFY(rig.host.opens.isEmpty());
}

void TestPanelController::aSeenHighClearsTheStuckState()
{
    LogCapture log;
    Rig rig;
    rig.panel.onRunLineChanged(true);
    rig.wait(10100);
    QVERIFY(rig.panel.isRunLineStuck());

    rig.panel.onRunLineChanged(false);
    QVERIFY(rig.panel.isRunLineStuck() == false);
    rig.panel.onRunLineChanged(true);
    rig.wait(10100);
    QVERIFY(rig.panel.isRunLineStuck());
    QCOMPARE(log.count("stuck"), 2);
}

QTEST_MAIN(TestPanelController)
#include "tst_panelcontroller.moc"
```

- [ ] **Step 3: Build and run**

```bash
cmake --build build -j 32
ctest --test-dir build --output-on-failure -R tst_panelcontroller
```

Expected: every test passes. A failure is a defect in Task 4 unless the test contradicts the spec or a ruling; fix the production code in a `fix` commit.

- [ ] **Step 4: Commit**

```bash
git add IrrigationD/tests/CMakeLists.txt IrrigationD/tests/tst_panelcontroller.cpp
git commit -F - <<'EOF'
test: cover the gardener panel's state machine with a test clock

A fake host and a test clock drive the panel through selection order
and wrap over enabled zones, the three-second commit, advancing and
ending a panel run, takeover, every refusal display with STOP held and
a fault on top, alternation over other running zones, the blink
phases, clock steps during a selection and the stuck-line rule.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---
### Task 14: Daemon tests — the host, the configuration and the control server

Spec §8: takeover clearing a program, app zones and the queue with `dropped_stop` (against real components), `panel_run_minutes` validation and the migration default. Plus the host's refusal order, the swap rules (Review Focus 1 and 5), the INI keys, and `source: "panel"` on the wire. Restores `settingsGetReturnsExactlyTheAllowlistedKeys`, red since Task 5.

**Files:**
- Create: `IrrigationD/tests/tst_panelhost.cpp`
- Modify: `IrrigationD/tests/CMakeLists.txt`
- Modify: `IrrigationD/tests/tst_settings.cpp`
- Modify: `IrrigationD/tests/tst_datasource.cpp`
- Modify: `IrrigationD/tests/tst_controlserver.cpp`

**Interfaces:**
- Consumes: Tasks 4–6 as produced; `ZoneController`, `ProgramRunner`, `ProgramQueue`, `StopButton`, `IrrigationDataSource` on `MockBackend`, as `tst_programqueue` builds them.

- [ ] **Step 1: Register the host suite in `IrrigationD/tests/CMakeLists.txt`**

Append:

```cmake
irrigation_add_test(tst_panelhost
    tst_panelhost.cpp
    ../src/panelhost.cpp
    ../src/panelcontroller.cpp
    ../src/panelformat.cpp
    ../src/programqueue.cpp
    ../src/programrunner.cpp
    ../src/runrequest.cpp
    ../src/stopbutton.cpp
    ../src/zonecontroller.cpp
    ../src/database/irrigationdatasource.cpp
    ../src/database/irrigation.qrc
    ../src/model/zone.cpp
    ../src/model/program.cpp
    ../src/model/programstarttime.cpp
    ../src/model/programstep.cpp
    ../src/model/firedinstant.cpp
)
```

- [ ] **Step 2: Create `IrrigationD/tests/tst_panelhost.cpp`**

```cpp
#include <QTest>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimeZone>

#include <Kanoop/pi/mockbackend.h>

#include "database/irrigationdatasource.h"
#include "iclock.h"
#include "model/program.h"
#include "model/programstep.h"
#include "model/zone.h"
#include "panelcontroller.h"
#include "panelhost.h"
#include "programqueue.h"
#include "programrunner.h"
#include "stopbutton.h"
#include "zonecontroller.h"

namespace
{
const quint32 STOP_OFFSET = 25;
const QDateTime DueAt = QDateTime(QDate(2026, 10, 3), QTime(13, 0), QTimeZone::UTC);

QMap<int, quint32> eightZones()
{
    return { {1,5}, {2,6}, {3,12}, {4,13}, {5,16}, {6,19}, {7,20}, {8,21} };
}
}

/** The daemon's components on the in-memory backend, wired as threadStarted() wires them. */
class Rig
{
public:
    Rig() :
        source(dir.filePath("irrigation.db")),
        clock(DueAt),
        controller(&backend, eightZones(), true, 3600),
        stopButton(&backend, STOP_OFFSET),
        runner(&controller, &source),
        queue(&runner, &source, &clock),
        host(&controller, &runner, &queue, &source, &stopButton)
    {
    }

    bool begin()
    {
        return source.open() && backend.openChipByLabel("mock") && controller.begin() && stopButton.begin();
    }

    int buildProgram(const QString& name, const QList<int>& zoneNumbers, int seconds)
    {
        Program program;
        program.name = name;
        if(source.insertProgram(program) == false) {
            return 0;
        }

        ProgramStep step;
        step.programId = program.id;
        step.sequence = 1;
        step.durationSeconds = seconds;
        for(const Zone& zone : source.allZones()) {
            if(zoneNumbers.contains(zone.number) == true) {
                step.zoneIds.append(zone.id);
            }
        }
        return source.insertProgramStep(step) == true ? program.id : 0;
    }

    bool insertQueuedFiring(int programId, int startTimeId, const QDateTime& scheduledAtUtc)
    {
        FiredInstant instant;
        instant.programId = programId;
        instant.startTimeId = startTimeId;
        instant.scheduledAtUtc = scheduledAtUtc;
        instant.outcome = FiredInstant::Outcome::Queued;
        return source.recordFiring(instant);
    }

    QString outcomeFor(int programId, int startTimeId)
    {
        bool ok = false;
        QSqlQuery query = source.rawQuery(
            QString("SELECT outcome FROM fired_instants WHERE program_id = %1 AND start_time_id = %2")
                .arg(programId).arg(startTimeId), &ok);
        return ok == true && query.next() == true ? query.value(0).toString() : QString();
    }

    bool setZoneEnabled(int zoneNumber, bool enabled)
    {
        for(Zone zone : source.allZones()) {
            if(zone.number == zoneNumber) {
                zone.enabled = enabled;
                return source.updateZone(zone);
            }
        }
        return false;
    }

    // A press pulls the line low; activeLow inversion reports it as a logical Rising edge.
    void holdStop() { backend.simulateEdge(STOP_OFFSET, Gpio::Edge::Rising); }
    void releaseStop() { backend.simulateEdge(STOP_OFFSET, Gpio::Edge::Falling); }

    QTemporaryDir dir;
    IrrigationDataSource source;
    MockBackend backend;
    TestClock clock;
    ZoneController controller;
    StopButton stopButton;
    ProgramRunner runner;
    ProgramQueue queue;
    PanelHost host;
};

class TestPanelHost : public QObject
{
    Q_OBJECT
private slots:
    void theSnapshotListsOpenAndEnabledZonesAscending();
    void theRunTimeIsBoundedAndClampedByTheZoneCeiling();
    void aPanelZoneOpensForTheRunTime();
    void refusalsComeInTheManualRunOrderAndOpenNothing();
    void aRefusedAdvanceLeavesTheReplacedZoneOpen();
    void anAdvanceSwapsZonesAtACapOfOne();
    void anAdvanceBesideAnotherZoneKeepsThatZone();
    void aWaitingProgramZoneTakesTheFreedSlot();
    void closePanelZoneClosesOnlyThatZone();
    void takeOverClearsAProgramAppZonesAndTheQueue();
};

void TestPanelHost::theSnapshotListsOpenAndEnabledZonesAscending()
{
    Rig rig;
    QVERIFY(rig.begin());
    QVERIFY(rig.setZoneEnabled(3, false));
    QVERIFY(rig.controller.openZone(5, 300));
    QVERIFY(rig.controller.openZone(2, 120));

    const PanelSnapshot snapshot = rig.host.panelSnapshot();
    QCOMPARE(snapshot.openZones.count(), 2);
    QCOMPARE(snapshot.openZones.at(0).zone, 2);
    QVERIFY(snapshot.openZones.at(0).secondsRemaining > 115 && snapshot.openZones.at(0).secondsRemaining <= 120);
    QCOMPARE(snapshot.openZones.at(1).zone, 5);
    QCOMPARE(snapshot.enabledZones, QList<int>({ 1, 2, 4, 5, 6, 7, 8 }));
    QCOMPARE(snapshot.runMinutes, PanelController::DefaultRunMinutes);
    QVERIFY(snapshot.stopHeld == false);
    QVERIFY(snapshot.faulted == false);
    QVERIFY(snapshot.masterEnabled);

    rig.holdStop();
    QVERIFY(rig.source.setSettingValue("master_enabled", "0"));
    const PanelSnapshot later = rig.host.panelSnapshot();
    QVERIFY(later.stopHeld);
    QVERIFY(later.masterEnabled == false);
}

void TestPanelHost::theRunTimeIsBoundedAndClampedByTheZoneCeiling()
{
    Rig rig;
    QVERIFY(rig.begin());

    rig.host.setRunMinutes(0);
    QCOMPARE(rig.host.runMinutes(), 1);
    rig.host.setRunMinutes(99);
    QCOMPARE(rig.host.runMinutes(), 60);

    rig.host.setRunMinutes(10);
    QCOMPARE(rig.host.runSeconds(), 600);
    rig.controller.setMaxZoneSeconds(290);
    QCOMPARE(rig.host.runSeconds(), 290);
    QCOMPARE(rig.host.panelSnapshot().runMinutes, 5);
}

void TestPanelHost::aPanelZoneOpensForTheRunTime()
{
    Rig rig;
    QVERIFY(rig.begin());
    rig.host.setRunMinutes(7);

    QCOMPARE(rig.host.openPanelZone(3, 0), RunRequest::Refusal::None);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 3 }));
    QVERIFY(rig.controller.secondsRemaining(3) > 415 && rig.controller.secondsRemaining(3) <= 420);
}

// Review Focus 5 on the host's side.
void TestPanelHost::refusalsComeInTheManualRunOrderAndOpenNothing()
{
    Rig rig;
    QVERIFY(rig.begin());
    QVERIFY(rig.controller.openZone(7, 300));
    QVERIFY(rig.controller.openZone(8, 300));
    QVERIFY(rig.setZoneEnabled(3, false));
    QVERIFY(rig.source.setSettingValue("master_enabled", "0"));
    rig.holdStop();

    QCOMPARE(rig.host.openPanelZone(3, 0), RunRequest::Refusal::StopHeld);
    rig.releaseStop();
    QCOMPARE(rig.host.openPanelZone(3, 0), RunRequest::Refusal::MasterDisabled);
    QVERIFY(rig.source.setSettingValue("master_enabled", "1"));
    QCOMPARE(rig.host.openPanelZone(3, 0), RunRequest::Refusal::ZoneDisabled);
    QVERIFY(rig.setZoneEnabled(3, true));
    QCOMPARE(rig.host.openPanelZone(3, 0), RunRequest::Refusal::CapReached);

    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 7, 8 }));
}

// Review Focus 1.
void TestPanelHost::aRefusedAdvanceLeavesTheReplacedZoneOpen()
{
    Rig rig;
    QVERIFY(rig.begin());
    QCOMPARE(rig.host.openPanelZone(1, 0), RunRequest::Refusal::None);

    QVERIFY(rig.source.setSettingValue("master_enabled", "0"));
    QCOMPARE(rig.host.openPanelZone(2, 1), RunRequest::Refusal::MasterDisabled);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 1 }));

    QVERIFY(rig.source.setSettingValue("master_enabled", "1"));
    QVERIFY(rig.setZoneEnabled(2, false));
    QCOMPARE(rig.host.openPanelZone(2, 1), RunRequest::Refusal::ZoneDisabled);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 1 }));

    QVERIFY(rig.setZoneEnabled(2, true));
    rig.holdStop();
    QCOMPARE(rig.host.openPanelZone(2, 1), RunRequest::Refusal::StopHeld);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 1 }));
}

// Review Focus 1.
void TestPanelHost::anAdvanceSwapsZonesAtACapOfOne()
{
    Rig rig;
    QVERIFY(rig.begin());
    rig.controller.setMaxConcurrentZones(1);

    QCOMPARE(rig.host.openPanelZone(1, 0), RunRequest::Refusal::None);
    QCOMPARE(rig.host.openPanelZone(2, 1), RunRequest::Refusal::None);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 2 }));
    QVERIFY(rig.controller.secondsRemaining(2) > 595);
}

void TestPanelHost::anAdvanceBesideAnotherZoneKeepsThatZone()
{
    Rig rig;
    QVERIFY(rig.begin());
    QVERIFY(rig.controller.openZone(5, 300));

    // Cap 2, full with the app's zone 5 and the panel's zone 1.
    QCOMPARE(rig.host.openPanelZone(1, 0), RunRequest::Refusal::None);
    QCOMPARE(rig.host.openPanelZone(2, 1), RunRequest::Refusal::None);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 2, 5 }));

    // Cap 3 leaves a slot free, so the new zone opens before the old one closes.
    rig.controller.setMaxConcurrentZones(3);
    QCOMPARE(rig.host.openPanelZone(3, 2), RunRequest::Refusal::None);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 3, 5 }));
}

void TestPanelHost::aWaitingProgramZoneTakesTheFreedSlot()
{
    Rig rig;
    QVERIFY(rig.begin());
    QCOMPARE(rig.host.openPanelZone(1, 0), RunRequest::Refusal::None);

    const int program = rig.buildProgram("Pair", { 5, 6 }, 600);
    QVERIFY(program > 0);
    QCOMPARE(rig.queue.enqueueManual(program), RunRequest::Refusal::None);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 1, 5 }));
    QCOMPARE(rig.runner.waitingZones(), QList<int>({ 6 }));

    QCOMPARE(rig.host.openPanelZone(2, 1), RunRequest::Refusal::CapReached);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 5, 6 }));
}

void TestPanelHost::closePanelZoneClosesOnlyThatZone()
{
    Rig rig;
    QVERIFY(rig.begin());
    QVERIFY(rig.controller.openZone(5, 300));
    QCOMPARE(rig.host.openPanelZone(1, 0), RunRequest::Refusal::None);

    rig.host.closePanelZone(1);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 5 }));
}

void TestPanelHost::takeOverClearsAProgramAppZonesAndTheQueue()
{
    Rig rig;
    QVERIFY(rig.begin());
    PanelController panel(&rig.host, &rig.clock);

    const int running = rig.buildProgram("Running", { 1 }, 600);
    const int waiting = rig.buildProgram("Waiting", { 2 }, 600);
    QVERIFY(rig.insertQueuedFiring(running, 31, DueAt));
    QVERIFY(rig.insertQueuedFiring(waiting, 32, DueAt));
    rig.queue.enqueueScheduled(running, 31, DueAt);
    rig.queue.enqueueScheduled(waiting, 32, DueAt);
    QVERIFY(rig.controller.openZone(7, 300));
    QVERIFY(rig.runner.isRunning());
    QCOMPARE(rig.queue.entries().count(), 1);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 1, 7 }));

    panel.onRunLineChanged(true);
    panel.onRunLineChanged(false);

    QVERIFY(rig.runner.isRunning() == false);
    QVERIFY(rig.queue.entries().isEmpty());
    QVERIFY(rig.controller.openZoneNumbers().isEmpty());
    QCOMPARE(rig.outcomeFor(waiting, 32), QString("dropped_stop"));
    QCOMPARE(rig.outcomeFor(running, 31), QString("ran"));
    QCOMPARE(panel.mode(), PanelController::Mode::Selecting);
    QCOMPARE(panel.selectedZone(), 1);

    for(int tick = 0; tick < 30; tick++) {
        rig.clock.advanceMsecs(100);
        panel.tick();
    }

    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 1 }));
    QCOMPARE(panel.panelZone(), 1);
    QVERIFY(rig.runner.isRunning() == false);
    QVERIFY(rig.queue.entries().isEmpty());
}

QTEST_MAIN(TestPanelHost)
#include "tst_panelhost.moc"
```

- [ ] **Step 3: Add the INI key tests to `IrrigationD/tests/tst_settings.cpp`**

Declare in `private slots:` after `zoneMapCleanRawIniProducesNoWarnings();`:

```cpp
    void panelOffsetsReadFromTheGpioGroup();
    void missingPanelOffsetsReadAsAbsent();
    void malformedPanelOffsetsReadAsAbsent();
```

Add before `QTEST_MAIN`:

```cpp
void TestSettings::panelOffsetsReadFromTheGpioGroup()
{
    QTemporaryDir dir;
    QString path = dir.filePath("test.ini");
    QVERIFY(writeRawIni(path, "[gpio]\nrunButtonOffset=24\ndisplayClockOffset=18\ndisplayDataOffset=27\n"));

    IrrigationSettings settings(path);
    QCOMPARE(settings.runButtonOffset(), 24);
    QCOMPARE(settings.displayClockOffset(), 18);
    QCOMPARE(settings.displayDataOffset(), 27);
}

void TestSettings::missingPanelOffsetsReadAsAbsent()
{
    QTemporaryDir dir;
    QString path = dir.filePath("test.ini");
    QVERIFY(writeRawIni(path, "[gpio]\nstopButtonOffset=25\n"));

    IrrigationSettings settings(path);
    QCOMPARE(settings.runButtonOffset(), -1);
    QCOMPARE(settings.displayClockOffset(), -1);
    QCOMPARE(settings.displayDataOffset(), -1);
}

void TestSettings::malformedPanelOffsetsReadAsAbsent()
{
    QTemporaryDir dir;
    QString path = dir.filePath("test.ini");
    QVERIFY(writeRawIni(path, "[gpio]\nrunButtonOffset=abc\ndisplayClockOffset=-3\ndisplayDataOffset=\n"));

    IrrigationSettings settings(path);
    QCOMPARE(settings.runButtonOffset(), -1);
    QCOMPARE(settings.displayClockOffset(), -1);
    QCOMPARE(settings.displayDataOffset(), -1);
}
```

- [ ] **Step 4: Add the default and migration tests to `IrrigationD/tests/tst_datasource.cpp`**

Declare in `private slots:` after `migratesProgramZonesIntoOneZoneSteps();`:

```cpp
    void seedsThePanelRunTime();
    void migratesThePanelRunTimeDefault();
    void keepsAPanelRunTimeAlreadyStored();
```

Add a helper after the includes, before `class TestDataSource`:

```cpp
// Builds a 1.1.0 database at @p path from the shipped scripts, then runs @p extra on it.
static bool seedVersion110(const QString& path, const QStringList& extra)
{
    bool ok = true;
    {
        QSqlDatabase seed = QSqlDatabase::addDatabase("QSQLITE", "seed-110-connection");
        seed.setDatabaseName(path);
        ok = seed.open();
        QSqlQuery query(seed);

        const QStringList scripts = {
            ":/database/migrate/irrigation/1.0.0/01-initial.sql",
            ":/database/migrate/irrigation/1.1.0/01-program-steps.sql"
        };
        for(const QString& resource : scripts) {
            QFile script(resource);
            ok = ok && script.open(QIODevice::ReadOnly);
            SqlParser parser(QString::fromUtf8(script.readAll()));
            ok = ok && parser.isValid();
            for(const QString& statement : parser.statements()) {
                ok = ok && query.exec(statement);
            }
        }

        ok = ok && query.exec("CREATE TABLE info (id INTEGER PRIMARY KEY, sw_version TEXT NOT NULL)");
        ok = ok && query.exec("INSERT INTO info (id, sw_version) VALUES (1, '1.1.0')");
        for(const QString& statement : extra) {
            ok = ok && query.exec(statement);
        }
        seed.close();
    }
    QSqlDatabase::removeDatabase("seed-110-connection");
    return ok;
}
```

Add before `QTEST_MAIN`:

```cpp
void TestDataSource::seedsThePanelRunTime()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QCOMPARE(source.settingValue("panel_run_minutes"), QString("10"));
}

void TestDataSource::migratesThePanelRunTimeDefault()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("irrigation.db");
    QVERIFY(seedVersion110(path, { "UPDATE settings SET value = '3' WHERE key = 'max_concurrent_zones'" }));

    IrrigationDataSource source(path);
    QVERIFY2(source.open(), qPrintable(source.errorText()));

    const QFileInfo dbInfo(path);
    QVERIFY(QDir(dbInfo.absolutePath())
                .entryList(QStringList() << dbInfo.fileName() + ".*.backup", QDir::Files).isEmpty());
    QCOMPARE(source.settingValue("panel_run_minutes"), QString("10"));
    QCOMPARE(source.settingValue("max_concurrent_zones"), QString("3"));

    QSqlQuery query(QSqlDatabase::database(source.connectionName()));
    QVERIFY(query.exec("SELECT sw_version FROM info WHERE id = 1"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), source.compiledDatabaseVersion());
    QCOMPARE(source.compiledDatabaseVersion(), QString("1.2.0"));
}

void TestDataSource::keepsAPanelRunTimeAlreadyStored()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("irrigation.db");
    QVERIFY(seedVersion110(path, { "INSERT INTO settings (key, value) VALUES ('panel_run_minutes', '25')" }));

    IrrigationDataSource source(path);
    QVERIFY2(source.open(), qPrintable(source.errorText()));
    QCOMPARE(source.settingValue("panel_run_minutes"), QString("25"));
}
```

- [ ] **Step 5: Update `IrrigationD/tests/tst_controlserver.cpp`**

In `settingsGetReturnsExactlyTheAllowlistedKeys`, replace:

```cpp
    const QStringList expected = { "log_level", "master_enabled", "max_concurrent_zones", "max_zone_seconds", "rain_delay_until" };
```

with:

```cpp
    const QStringList expected = { "log_level", "master_enabled", "max_concurrent_zones", "max_zone_seconds",
                                   "panel_run_minutes", "rain_delay_until" };
```

and after `QCOMPARE(body.value("max_concurrent_zones").toString(), QString("2"));` add:

```cpp
    QCOMPARE(body.value("panel_run_minutes").toString(), QString("10"));
```

In `settingsPutRejectsInvalidValue_data`, after the `max_concurrent_zones non-numeric` row:

```cpp
    QTest::newRow("panel_run_minutes at the zero boundary") << QString("panel_run_minutes") << QString("0");
    QTest::newRow("panel_run_minutes above the ceiling") << QString("panel_run_minutes") << QString("61");
    QTest::newRow("panel_run_minutes non-numeric") << QString("panel_run_minutes") << QString("ten");
```

In `settingsPutRejectsInvalidValue`, before the final `else {`:

```cpp
    else if(key == QString("panel_run_minutes")) {
        QCOMPARE(verify.settingValue("panel_run_minutes"), QString("10"));
    }
```

In `settingsPutAcceptsValidValueAtBothEdges_data`, after the `max_concurrent_zones at the ceiling` row:

```cpp
    QTest::newRow("panel_run_minutes at the minimum") << QString("panel_run_minutes") << QString("1");
    QTest::newRow("panel_run_minutes at the ceiling") << QString("panel_run_minutes") << QString("60");
```

Declare in `private slots:` after `void updateStatusFromTheTestThreadAppearsInTheNextStatusGet();`:

```cpp
    void statusNamesAPanelRunPanel();
```

and add the definition after `updateStatusFromTheTestThreadAppearsInTheNextStatusGet`'s:

```cpp
void TestControlServer::statusNamesAPanelRunPanel()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;

    ServerStatus status;
    status.running = { RunningZoneStatus{ 3, 600, RunningZoneStatus::Source::Panel },
                       RunningZoneStatus{ 5, 300, RunningZoneStatus::Source::Program } };
    status.maxConcurrentZones = 2;
    status.timezone = "America/Los_Angeles";
    status.masterEnabled = true;
    server.updateStatus(status);

    QJsonArray running;
    for(int attempt = 0; attempt < 20 && running.count() != 2; attempt++) {
        QNetworkReply* reply = getJson(manager, server.boundPort(), "/admin/status");
        running = QJsonDocument::fromJson(reply->readAll()).object().value("running").toArray();
    }

    QCOMPARE(running.count(), 2);
    QCOMPARE(running.at(0).toObject().value("zone").toInt(), 3);
    QCOMPARE(running.at(0).toObject().value("source").toString(), QString("panel"));
    QCOMPARE(running.at(1).toObject().value("source").toString(), QString("program"));

    server.stop(TimeSpan::fromSeconds(5));
}
```

- [ ] **Step 6: Build and run every daemon suite**

```bash
cmake --build build -j 32
ctest --test-dir build --output-on-failure
```

Expected: every suite passes, including `tst_controlserver` whole. A failure in `tst_panelhost` is a defect in Task 6 unless the test contradicts the spec or a ruling; fix the production code in a `fix` commit.

- [ ] **Step 7: Commit**

```bash
git add IrrigationD/tests/CMakeLists.txt IrrigationD/tests/tst_panelhost.cpp \
        IrrigationD/tests/tst_settings.cpp IrrigationD/tests/tst_datasource.cpp \
        IrrigationD/tests/tst_controlserver.cpp
git commit -F - <<'EOF'
test: pin the panel host, the panel's configuration and its wire name

Against the real zone controller, runner and queue on the in-memory
backend: a panel run's refusals in the manual-run order, a refused
advance leaving the current zone open, swaps at a cap of one and beside
an app zone, a waiting program zone taking a freed slot, and a RUN
takeover clearing a program, an app zone and the queue with
dropped_stop before the selection commits.

Also the INI's panel offsets, panel_run_minutes validation, its fresh
and migrated default, and source "panel" in the status.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---
### Task 15: Web tests

Spec §8: the "panel" tag and the Settings field. Plus the decoder accepting the new source.

**Files:**
- Modify: `web/src/test/fixtures.ts`
- Modify: `web/src/api/decode.test.ts`
- Modify: `web/src/screens/NowScreen.test.tsx`
- Modify: `web/src/screens/SettingsScreen.test.tsx`

**Interfaces:**
- Consumes: Tasks 8 and 9 as produced.

- [ ] **Step 1: Add a panel fixture to `web/src/test/fixtures.ts`**

After `cappedStatus`:

```ts
/** Zone 3 runs from the gardener panel beside program zone 5. */
export const panelStatus: Status = {
  ...idleStatus,
  running: [
    { zone: 3, secondsRemaining: 540, source: 'panel' },
    { zone: 5, secondsRemaining: 1712, source: 'program' },
  ],
}
```

- [ ] **Step 2: Accept the source in `web/src/api/decode.test.ts`**

In the `decodeStatus` describe block, after `it('names the running entry with an unknown source', ...)`:

```ts
  it('reads a panel source', () => {
    const running = [{ zone: 3, secondsRemaining: 600, source: 'panel' }]
    expect(decodeStatus({ ...goodStatus, running }).running).toEqual(running)
  })
```

- [ ] **Step 3: Tag rows and tiles in `web/src/screens/NowScreen.test.tsx`**

Add `panelStatus` to the fixtures import:

```ts
import { cappedStatus, idleStatus, panelStatus, runningStatus, zoneFixtures } from '../test/fixtures'
```

Append a describe block at the end of the file:

```tsx
describe('NowScreen with a gardener panel run', () => {
  it('tags the panel row "panel" beside the program row', async () => {
    render(<NowScreen status={panelStatus} polls={1} refresh={refresh} />)

    const row = await screen.findByTestId('running-zone-3')
    expect(row).toHaveTextContent('Roses')
    expect(row).toHaveTextContent('9:00')
    expect(within(row).getByText('Panel')).toBeInTheDocument()
    expect(within(screen.getByTestId('running-zone-5')).getByText('Program')).toBeInTheDocument()
  })

  it('tags only the panel run tile "panel"', async () => {
    render(<NowScreen status={panelStatus} polls={1} refresh={refresh} />)

    const tile = await screen.findByTestId('zone-tile-3')
    expect(tile).toHaveAttribute('data-running', 'true')
    expect(within(tile).getByText('Panel')).toBeInTheDocument()
    expect(within(screen.getByTestId('zone-tile-5')).queryByText('Panel')).toBeNull()
    expect(within(screen.getByTestId('zone-tile-1')).queryByText('Panel')).toBeNull()
  })

  it('keeps the panel run tile stoppable', async () => {
    const user = userEvent.setup()
    const stopZone = vi.spyOn(client, 'stopZone').mockResolvedValue(undefined)
    render(<NowScreen status={panelStatus} polls={1} refresh={refresh} />)

    const tile = await screen.findByTestId('zone-tile-3')
    await user.click(within(tile).getByRole('button', { name: 'Stop zone 3' }))
    expect(stopZone).toHaveBeenCalledWith(3)
  })
})
```

- [ ] **Step 4: Cover the field in `web/src/screens/SettingsScreen.test.tsx`**

Inside `describe('SettingsScreen', ...)`, after `it('refuses max zones outside 1 to 8', ...)`:

```tsx
  it('shows a gardener panel run time of 10 minutes when the setting is absent', async () => {
    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    expect(await screen.findByLabelText(/gardener panel run time/i)).toHaveValue(10)
  })

  it('reads the stored gardener panel run time', async () => {
    vi.spyOn(client, 'getSettings').mockResolvedValue({ master_enabled: '1', panel_run_minutes: '25' })
    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)
    expect(await screen.findByLabelText(/gardener panel run time/i)).toHaveValue(25)
  })

  it('saves the gardener panel run time as a string', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)
    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)

    const field = await screen.findByLabelText(/gardener panel run time/i)
    await user.clear(field)
    await user.type(field, '15')
    await user.click(screen.getByRole('button', { name: 'Save panel run time' }))

    await waitFor(() => {
      expect(putSettings).toHaveBeenCalledWith({ panel_run_minutes: '15' })
    })
  })

  it.each(['0', '61'])('refuses a gardener panel run time of %s', async (value) => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)
    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)

    const field = await screen.findByLabelText(/gardener panel run time/i)
    await user.clear(field)
    await user.type(field, value)
    await user.click(screen.getByRole('button', { name: 'Save panel run time' }))

    expect(await screen.findByRole('alert')).toHaveTextContent(/1 to 60/)
    expect(putSettings).not.toHaveBeenCalled()
  })
```

- [ ] **Step 5: Typecheck and run**

```bash
npm --prefix web run typecheck
npm --prefix web test
npm --prefix web run build
```

Expected: all three exit zero.

- [ ] **Step 6: Commit**

```bash
git add web/src/test/fixtures.ts web/src/api/decode.test.ts \
        web/src/screens/NowScreen.test.tsx web/src/screens/SettingsScreen.test.tsx
git commit -F - <<'EOF'
test: cover the panel tag and the gardener panel run time field

The decoder reads the panel source, the Now screen tags a panel run's
row and tile Panel and keeps its Stop live, and Settings loads, saves
and bounds the gardener panel run time.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---

### Task 16: Bench verification on the controller

Spec §8's bench list: power-on to clock time, a walk through the enabled zones, picking zone 3, takeover of a running program, STOP held showing `StOP`, `kill -9` leaving `----`, and a reboot showing `----` and then the clock. Runs on the real Pi with relays wired to real solenoids, after Task 15 is green.

This task deploys **without reflashing**: a cross-built binary and the web bundle copied over the installed ones, and Task 10's unit file, udev rule, device drop-in and INI keys applied by hand. Those also ride the next image, which the owner builds and flashes. `gpio=24=ip,pu` and `pu` on the zone line are already in the controller's `/boot/config.txt` (added by hand on 2026-10-03); Step 1 confirms it.

> **Water.** Each panel run clicks a real relay and, if the supply valve is open, waters a real zone. Ask the owner whether the water is on before Step 7. Step 6 drops the panel run time to one minute for the bench and Step 13 restores it.

> **Released lines.** 10k pull-ups are fitted on the relay inputs and the zone pads carry `pu`, so a stopped daemon leaves every relay off. `kill -9` and `systemctl stop` are safe on this controller; the steps below still keep the daemon down only as long as each check needs.

**Files:** none in either repository. Backups on the Pi, all suffixed `.pre-panel`: `/usr/bin/irrigationd`, `/var/lib/irrigationd/irrigation.db`, `/var/www/irrigation/html`, `/usr/lib/systemd/system/irrigationd.service`, `/etc/irrigationd.ini`.

**Interfaces:**
- Consumes: the whole branch; the rpi commit from Task 10; the Yocto SDK at `/home/spunak/opt/poky/5.2.4` (a symlink to `/opt/poky/5.2.4`, the path the Qt Creator RPi kit uses); the controller at `root@irrigation.local`, empty password.

Every command block defines its own helpers, because the Bash tool keeps no shell functions between calls:

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
pcp() { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w scp -o PubkeyAuthentication=no "$1" "root@irrigation.local:$2" </dev/null; }
api() { pi "curl -s -w '\n%{http_code}\n' $1"; }
```

- [ ] **Step 1: Confirm the controller is reachable, idle and configured**

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
pi 'systemctl is-active irrigationd; curl -s http://127.0.0.1/api/status; echo;
    sqlite3 /var/lib/irrigationd/irrigation.db "SELECT sw_version FROM info;";
    grep -n "^gpio=" /boot/config.txt; ls /lib/udev/rules.d | grep -c rules;
    ls /usr/lib/libQt6HttpServer.so.6* /usr/lib/libgpiod.so.3*'
ls /home/spunak/opt/poky/5.2.4/sysroots/cortexa72-poky-linux/usr/lib/libQt6HttpServer.so.6* \
   /home/spunak/opt/poky/5.2.4/sysroots/cortexa72-poky-linux/usr/lib/libgpiod.so.3*
```

Expected: `active`; a status with `"running":[]`; `1.1.0`; the three lines `gpio=5,6,12,13,16,19,20,21=op,dh,pu`, `gpio=25=ip,pu`, `gpio=24=ip,pu`; a non-zero rule count (the directory exists); matching `libQt6HttpServer` and `libgpiod` sonames on both sides.

If ssh reports `REMOTE HOST IDENTIFICATION HAS CHANGED`, stop and ask the owner to refresh the `irrigation.local` entry in `~/.ssh/known_hosts` (the 2026-10-01 reflash changed the host key). Never edit `known_hosts` or turn host-key checking off yourself. If `irrigation.local` does not resolve, ask the owner for the controller's address. If a `gpio=` line is missing, stop: the bench needs the boot config the image will carry.

- [ ] **Step 2: Cross-build `irrigationd`**

```bash
cd /home/spunak/src/punak/irrigation
(
  source /home/spunak/opt/poky/5.2.4/environment-setup-cortexa72-poky-linux
  cmake -S . -B build/bench-arm64 -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$OECORE_NATIVE_SYSROOT/usr/share/cmake/OEToolchainConfig.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DIRRIGATION_USE_MOLD=OFF -DBUILD_TESTING=OFF
  cmake --build build/bench-arm64 --target irrigationd -j 32
)
file build/bench-arm64/IrrigationD/irrigationd
```

Expected: `ELF 64-bit LSB pie executable, ARM aarch64`.

- [ ] **Step 3: Build the web bundle**

```bash
cd /home/spunak/src/punak/irrigation
npm --prefix web run build
tar -C web/dist -czf build/bench-web.tgz .
```

- [ ] **Step 4: Back up on the Pi**

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
pi 'ls -d /usr/bin/irrigationd.pre-panel /var/lib/irrigationd/irrigation.db.pre-panel /var/www/irrigation/html.pre-panel /usr/lib/systemd/system/irrigationd.service.pre-panel /etc/irrigationd.ini.pre-panel 2>/dev/null'
```

Expected: no output. If any backup already exists, stop and ask the owner; never overwrite one.

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
pi 'cp -a /usr/bin/irrigationd /usr/bin/irrigationd.pre-panel &&
    sqlite3 /var/lib/irrigationd/irrigation.db ".backup /var/lib/irrigationd/irrigation.db.pre-panel" &&
    cp -a /var/www/irrigation/html /var/www/irrigation/html.pre-panel &&
    cp -a /usr/lib/systemd/system/irrigationd.service /usr/lib/systemd/system/irrigationd.service.pre-panel &&
    cp -a /etc/irrigationd.ini /etc/irrigationd.ini.pre-panel &&
    sqlite3 /var/lib/irrigationd/irrigation.db.pre-panel "SELECT sw_version FROM info; SELECT COUNT(*) FROM programs;"'
```

Record the program count; expect `1.1.0`. `.backup` takes a consistent copy of a live WAL database; a plain `cp` of the `.db` file does not.

- [ ] **Step 5: Deploy the binary, the bundle and the image files**

First confirm the Pi's INI is the layer's pre-Task-10 copy, so replacing it loses nothing:

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
cd /home/spunak/src/punak/rpi
ini=meta-rpi4-irrigation/recipes-core/irrigation-init/files/irrigationd.ini
c=$(git log -1 --format=%H -- "$ini")
diff <(pi 'cat /etc/irrigationd.ini') <(git show "$c~1:$ini") && echo SAME
```

Expected: `SAME`. Any difference means someone edited the Pi's copy: stop and ask the owner which keys to keep.

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
pcp() { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w scp -o PubkeyAuthentication=no "$1" "root@irrigation.local:$2" </dev/null; }
files=/home/spunak/src/punak/rpi/meta-rpi4-irrigation/recipes-core/irrigation-init/files
cd /home/spunak/src/punak/irrigation
pcp build/bench-arm64/IrrigationD/irrigationd /usr/bin/irrigationd.new
pcp build/bench-web.tgz /tmp/bench-web.tgz
pcp "$files/irrigationd.service" /usr/lib/systemd/system/irrigationd.service
pcp "$files/irrigationd.ini" /etc/irrigationd.ini
pi 'mkdir -p /usr/lib/systemd/system/dev-rtc0.device.d'
pcp "$files/dev-rtc0-timeout.conf" /usr/lib/systemd/system/dev-rtc0.device.d/10-irrigation-timeout.conf
pcp "$files/99-irrigation-rtc.rules" /lib/udev/rules.d/99-irrigation-rtc.rules
pi 'chmod 0755 /usr/bin/irrigationd.new && mv /usr/bin/irrigationd.new /usr/bin/irrigationd &&
    udevadm control --reload-rules && udevadm trigger --subsystem-match=rtc --action=add &&
    systemctl daemon-reload && sleep 1 &&
    systemctl show -p ActiveState -p SysFSPath dev-rtc0.device &&
    systemctl restart irrigationd'
pi 'sleep 3; systemctl is-active irrigationd; journalctl -u irrigationd -n 40 --no-pager'
pi 'sqlite3 /var/lib/irrigationd/irrigation.db "SELECT sw_version FROM info; SELECT value FROM settings WHERE key = '"'"'panel_run_minutes'"'"'; SELECT COUNT(*) FROM programs;"'
pi 'ls /var/lib/irrigationd/*.backup 2>/dev/null'
pi 'rm -rf /var/www/irrigation/html/* && tar -C /var/www/irrigation/html -xzf /tmp/bench-web.tgz && rm /tmp/bench-web.tgz && ls /var/www/irrigation/html'
pi 'curl -s http://127.0.0.1/api/status'
```

If Task 10 Step 9 showed the rule under `/usr/lib/udev/rules.d`, copy it there instead. The binary goes in through `irrigationd.new` and `mv`: writing over a running executable fails with "Text file busy".

Expected: `ActiveState=active` with a `SysFSPath` under `/sys/devices`; `active`; the journal shows the migration to 1.2.0 with no "recreating the database" line, `Panel runs last 10 minutes`, and `The panel is showing its first frame`, and no warning about missing display or RUN keys; `1.2.0`, `10`, the program count from Step 4; no `*.backup` file; `index.html` in the bundle; a status with `"running":[]`. Ask the owner to confirm the display shows the clock with a blinking colon. If anything here fails, roll back (Step 14) before investigating.

- [ ] **Step 6: Shorten panel runs for the bench**

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
api() { pi "curl -s -w '\n%{http_code}\n' $1"; }
api "-X PUT -H 'content-type: application/json' -d '{\"panel_run_minutes\":\"1\"}' http://127.0.0.1/api/settings"
api "http://127.0.0.1/api/zones"
```

Expected: `200` with `"panel_run_minutes":"1"`. Note which zones are enabled; the walk in Step 8 visits those only.

- [ ] **Step 7: Power-on to clock time**

Ask the owner whether the water is on. Then ask the owner to remove mains power, wait ten seconds, restore it, and time power-on to the first clock on the display. After the controller is back:

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
pi 'journalctl -b -o short-monotonic -u irrigationd --no-pager | head -20;
    systemctl show -p ActiveEnterTimestampMonotonic dev-rtc0.device time-sync.target irrigationd.service;
    journalctl -b -o short-monotonic --no-pager | grep -i -m3 rtc'
```

Expected: the owner reports roughly ten seconds from power-on to the clock, and a blank display before it. `Starting irrigationd` at about 8 s monotonic (it was 15.9 s before this branch); `The panel is showing its first frame` within one second of it; `dev-rtc0.device` active before `irrigationd.service`. Record the measured figures.

- [ ] **Step 8: Walk through the enabled zones**

Start a status watch, then ask the owner to press RUN once, wait for the zone to start, look at the display, and keep pressing RUN once per zone until the last enabled zone closes:

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
pi 'for i in $(seq 120); do curl -s http://127.0.0.1/api/status | grep -o "\"running\":\[[^]]*\]"; sleep 1; done'
```

Expected, confirmed with the owner at each point: the first press shows the first enabled zone's digit blinking with ` 1` steady on the right; three seconds later the digit stops blinking, its relay clicks on, and the status shows that zone alone with `"source":"panel"`. Each further press closes the current zone and opens the next enabled zone at once, with no blinking selection in between, one relay at a time. The press on the last enabled zone closes it, the status goes empty, and the display returns to the clock.

- [ ] **Step 9: Pick zone 3 and let it run out**

Ask the owner to press RUN until zone 3 blinks (pressing past the end wraps to the first enabled zone), then leave it. Repeat the status watch from Step 8 for 90 seconds.

Expected: three seconds after the last press zone 3 opens as `"source":"panel"`; the display reads `3  1`; when its minute runs out the zone closes, the panel run ends without moving on, and the display returns to the clock.

- [ ] **Step 10: Take over a running program**

Create a disabled one-step program on zone 1 for five minutes and run it from the API:

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
api() { pi "curl -s -w '\n%{http_code}\n' $1"; }
pi 'timedatectl show -p Timezone --value; curl -s http://127.0.0.1/api/zones'
```

From the output take the controller's zone and zone 1's `id`. Then, substituting `ID1` and `TZ`:

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
api() { pi "curl -s -w '\n%{http_code}\n' $1"; }
api "-X POST -H 'content-type: application/json' -d '{\"name\":\"Bench Panel\",\"enabled\":false,\"dayMode\":\"DaysOfWeek\",\"dowMask\":127,\"intervalDays\":0,\"anchorDate\":\"\",\"startTimes\":[{\"minutesAfterMidnight\":180,\"timezone\":\"TZ\"}],\"steps\":[{\"zones\":[ID1],\"durationSeconds\":300}]}' http://127.0.0.1/api/programs"
```

Record the program id as `P`, then:

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
api() { pi "curl -s -w '\n%{http_code}\n' $1"; }
api "-X POST http://127.0.0.1/api/programs/P/run"
api "http://127.0.0.1/api/status"
```

Expected: `202`; `running` holds zone 1 as `"program"` and `program` names `Bench Panel`. The display shows `1  5`. Now ask the owner to press RUN once, and immediately:

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
api() { pi "curl -s -w '\n%{http_code}\n' $1"; }
api "http://127.0.0.1/api/status"
pi 'journalctl -u irrigationd -n 15 --no-pager'
```

Expected: `running` empty, `program` `null`, `queue` empty, and the journal shows `RUN took over`. The display shows the selection, zone 1 blinking; three seconds later zone 1 opens as `"panel"`. Ask the owner to press STOP to end it.

- [ ] **Step 11: STOP held shows `StOP`**

Ask the owner to press RUN to start a selection, then hold STOP, press RUN while holding it, wait five seconds, and release STOP.

Expected: `StOP` appears while STOP is held and the selection is gone; the RUN press does nothing; after release the display returns to the clock and the status shows nothing running and `"stopHeld":false`.

- [ ] **Step 12: `kill -9` leaves `----`; a reboot shows `----` and then the clock**

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
pi 'kill -9 $(pidof irrigationd); sleep 2; systemctl status irrigationd --no-pager | head -12'
```

Expected: the owner sees `----` within a second of the kill and the clock again about five seconds later, when `Restart=` brings the daemon back. The status output shows the stop-post command having run.

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
pi 'systemctl reboot'
```

Expected: the owner sees `----` while the controller reboots, then the clock once the daemon starts. Afterwards:

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
pi 'systemctl is-active irrigationd; journalctl -b -1 -u irrigationd --no-pager | tail -5; curl -s http://127.0.0.1/api/status'
```

Expected: `active`; the previous boot's journal ends with the daemon stopping cleanly; an idle status.

- [ ] **Step 13: Clean up**

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
api() { pi "curl -s -w '\n%{http_code}\n' $1"; }
api "-X DELETE http://127.0.0.1/api/programs/P"
api "-X PUT -H 'content-type: application/json' -d '{\"panel_run_minutes\":\"10\"}' http://127.0.0.1/api/settings"
api "http://127.0.0.1/api/status"
```

Expected: `204`, `200` with `"panel_run_minutes":"10"`, and an idle status. Ask the owner to open the app on a phone and check the Settings field reads 10, and that a panel run started from the box shows a Panel tag on its row and tile. Leave the backups in place; the owner removes them.

- [ ] **Step 14: Rollback (only if something above failed)**

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
pi 'systemctl stop irrigationd;
    cp -a /usr/bin/irrigationd.pre-panel /usr/bin/irrigationd;
    rm -f /var/lib/irrigationd/irrigation.db-wal /var/lib/irrigationd/irrigation.db-shm;
    cp -a /var/lib/irrigationd/irrigation.db.pre-panel /var/lib/irrigationd/irrigation.db;
    cp -a /usr/lib/systemd/system/irrigationd.service.pre-panel /usr/lib/systemd/system/irrigationd.service;
    cp -a /etc/irrigationd.ini.pre-panel /etc/irrigationd.ini;
    rm -f /lib/udev/rules.d/99-irrigation-rtc.rules /usr/lib/systemd/system/dev-rtc0.device.d/10-irrigation-timeout.conf;
    rmdir /usr/lib/systemd/system/dev-rtc0.device.d 2>/dev/null;
    udevadm control --reload-rules; systemctl daemon-reload;
    systemctl start irrigationd;
    rm -rf /var/www/irrigation/html/* && cp -a /var/www/irrigation/html.pre-panel/. /var/www/irrigation/html/;
    sleep 3; systemctl is-active irrigationd; curl -s http://127.0.0.1/api/status'
```

The `-wal` and `-shm` files belong to the migrated database; restoring the main file next to them corrupts it. The 1.1.0 binary will not run against a 1.2.0 database it did not create, so the database and the binary always roll back together. The stop at the top of the rollback still runs the new unit's stop-post hook, so the display shows `----`; the 1.1.0 binary never writes the display, and `----` stays until the next image.

Expected: `active` and an idle status.

---

## Spec coverage

| Spec | Requirement | Task (code) | Task (test) |
|---|---|---|---|
| §2 | RUN on BCM 24, active low, pull-up from config.txt and the bias request, 20 ms debounce | 3, 10 | 12, 16 |
| §2 | CLK BCM 18 and DIO BCM 27 push-pull; DIO low through each acknowledge clock | 2 | 12 |
| §2 | `runButtonOffset`, `displayClockOffset`, `displayDataOffset`; a missing key disables that part and logs it | 5, 6, 10 | 14, 16 |
| §2 | 0x40, 0xC0 + four bytes, 0x88\|brightness; LSB first; about 10 µs per edge; brightness 4 | 2 | 12 |
| §3.1 | 12-hour `h:mm`, leading digit blank before 10, colon blinking half a second, no AM/PM | 1, 4 | 12, 13 |
| §3.2 | First enabled zone blinking with the run minutes; next enabled zone per press, wrapping; disabled never offered | 4 | 13 |
| §3.2 | Commit three seconds after the last press; digit steady, countdown | 4, 6 | 13, 16 |
| §3.2 | `nonE` for two seconds with no zone enabled | 4 | 13 |
| §3.3 | Minutes left rounded up; next enabled zone at once on a press; the last zone ends the run; the timer ends it without moving on; the display returns to the clock or the running view | 1, 4 | 12, 13, 16 |
| §3.4 | Other zones shown one at a time, every two seconds in ascending order | 4 | 13 |
| §3.4 | Takeover clears exactly as API STOP, `dropped_stop`, then the selection | 4, 6 | 13, 14, 16 |
| §3.4 | During a panel run only the panel zone shows | 4 | 13 |
| §3.5 | `StOP`, `Err`, `OFF`, `FuLL` with RUN ignored or refused; STOP held and a fault win | 1, 4 | 12, 13, 16 |
| §3.5 | Refusals apply at the commit and on each press during a run | 4, 6 | 13, 14 |
| §3.6 | Duration clamped by the ceiling; counts against the cap; same refusals as a manual run; STOP ends it; `source: "panel"` | 6 | 14 |
| §3.6 | A program due during a panel run starts or queues by its normal rules | 6 (unchanged queue path) | 14 (`aWaitingProgramZoneTakesTheFreedSlot`) |
| §3.7 | Falling edges are presses; stuck over ten seconds logged once; no press until seen high; low at startup is no press | 3, 4 | 12, 13 |
| §4 | `panel_run_minutes` 1–60, default 10; Settings field; schema and migration together | 5, 9 | 14, 15 |
| §5 | `running[].source` gains `"panel"`; Now tags rows and tiles; display state not exposed | 6, 8 | 14, 15 |
| §6.1 | No network-online ordering; ordered after the RTC device; first screen within a second | 6, 10 | 16 |
| §6.2 | Stop-post hook runs the one-shot mode writing `----`, touching no other line; a starting daemon overwrites the display | 7, 10 | 12 (`onlyTheTwoDisplayLinesAreWritten`), 16 |
| §7 | A failed display request or write is logged; RUN and watering carry on; no display lines runs RUN alone | 6 | 12 (`aFailedWriteFailsTheUpdateAndTheNextIsWhole`), 16 |
| §8 | Daemon, web and bench tests | 12–16 | — |
| §9 | Deploy onto the running controller; unit, hook and `gpio=24=ip,pu` in the image | 10 | 16 |

---

## Known gaps

- **`IrrigationDaemon`'s panel wiring has no unit test.** `setUpPanel()`, `onPanelFrame()`, STOP calling `cancel()`, and `publishStatus()` naming panel zones run only on the bench (Task 16); Task 11 reads each against this plan. The daemon class still has no unit-test harness.
- **The `--dashes` mode is exercised on the host only for its exit codes** (Task 7) and for real on the bench (Task 16). The host has no TM1637 or GPIO chip.
- **Boot timing has no automated test.** The 8 s start and the one-second first frame are read from the journal in Task 16.
- **The image's `irrigationd` `SRCREV` is not bumped** (ruling 20). The owner pushes this branch and bumps it before the next image build; until then an image pairs the new unit with the old binary.
- **Display dimming, sound and showing programs or the queue are out of scope** (spec non-goals).

---
