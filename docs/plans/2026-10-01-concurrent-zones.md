# Concurrent Zones Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let `irrigationd` open more than one zone at a time under a configurable cap, run program steps whose zones water together, queue programs that come due while another runs, and show all of it in the web interface.

**Architecture:** `ZoneController` keeps sole ownership of the valve lines and swaps mutual exclusion for a cap: a set of open zones, each with its own deadline and close timer, a per-zone close reason, and a watchdog that compares the read-back against the expected *set*. `ProgramRunner` now walks steps: it opens every zone of a step that fits, parks the rest as waiting, and fills freed slots in ascending zone number. A new `ProgramQueue` sits between the scheduler and the runner, owns the FIFO and every `fired_instants` outcome after `queued`, and starts the head entry whenever the runner falls idle. HTTP run routes hand a shared `RunRequest` to the valve thread and wait, bounded, for its decision, so a refusal reaches the browser as a 409 with a reason.

**Tech Stack:** Qt 6.10 (Core, Network, HttpServer, Sql, Test), CMake, SQLite through `KanoopDatabaseQt`, libgpiod 2.x through `KanoopPiQt`; React 19, Vite 7, TypeScript 5.9 (strict), Vitest 3 + Testing Library.

**Spec:** docs/design/2026-10-01-concurrent-zones-design.md — binding. It amends `docs/design/2026-09-05-irrigation-design.md` §5.1, §5.5, §6, §7 and §8; where the two disagree the concurrent-zones spec wins. The plan argues from both; executors read both.

---

## Global Constraints

These apply to every task. A task's requirements implicitly include this section.

- **Repository:** `~/src/punak/irrigation`, branch `feature/superproject`. Daemon work is under `IrrigationD/`, web work under `web/`, docs under `docs/`. Do not touch the Kanoop submodules.
- **Never `git push`.** Commits stay local. The owner controls every remote push.
- **Never discard uncommitted changes.** No `git checkout --`, `git restore`, `git clean -f`, `git reset --hard`. The owner edits this tree in Qt Creator and vim while you work: re-read a file immediately before editing it, and before staging a file run `git diff --stat <file>` — if the line count dwarfs your edit, stop and ask.
- **Tests come at the end.** Implementation tasks (1–10) write no new tests. They must leave the tree building: `cmake --build build -j 32` exits zero and `npm --prefix web run typecheck` exits zero. Existing tests that go red because behaviour changed on purpose stay red until the test tasks (12–15); each task lists which ones. When a change would stop an existing test file compiling, the task applies the minimal edit that keeps it compiling — a mechanical rename, or deleting the tests whose subject the task removed — and where that edit would mean rewriting the file's subject, the task **parks** the file instead (below). Assertion fixes belong to the test tasks.
- **Parking.** A parked C++ test target has its `irrigation_add_test(...)` block removed from `IrrigationD/tests/CMakeLists.txt`; the source file stays on disk untouched. A parked web test file is listed in the `exclude` arrays of `web/tsconfig.json` and `web/vitest.config.ts`. The test task that owns the file restores it with the exact block or list given in that task. Parked by this plan: `tst_controlserver` (Task 5, restored in Task 14); `web/src/api/decode.test.ts` (Task 7) and `web/src/api/client.test.ts`, `web/src/programs/dayRule.test.ts`, `web/src/screens/ProgramEditor.test.tsx`, `web/src/screens/ProgramsScreen.test.tsx` (Task 8), all restored in Task 15.
- **Builds.** Host daemon build: `cmake --build build -j 32`; tests: `ctest --test-dir build --output-on-failure -R <name>`. If `build/` is not configured, configure it first with `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON`. Web: `npm --prefix web run typecheck`, `npm --prefix web test`, `npm --prefix web run build`.
- **C++ style** per `meta-qt-mains/.claude/docs/codestyle-cpp.md` and the surrounding code: `_underscorePrefixed` members, `camelCase` methods, `PascalCase` classes, `if(` with no space, `== false` instead of `!`, function brace on its own line, control brace on the same line, `catch`/`else` on their own line. Primitive, enum and raw-pointer members get an in-class initializer. Doxygen on every public member. Global namespace; enum values scoped inside their holder class. Every `.cpp` whose header declares `Q_OBJECT` ends with its moc include. `-Wextra -Wall -Werror`. Write valid C++17. The code model is `meta-qt-mains`; never model on `kanooptorrentd`.
- **Qt connections.** Never pass `Qt::QueuedConnection` to `connect()` or `QMetaObject::invokeMethod()`. `Qt::AutoConnection` already queues across threads. No comment justifies a connection type.
- **Comments state traps only.** A comment stays when it states something the code cannot show and a plausible edit would silently break: a wire contract, an ordering constraint, a lifetime rule. No design rationale, no pattern names, no "the old code did X", no pointers to other code as justification. History goes in commit messages.
- **No "it's X, not Y" antithesis** in code, comments, commit messages or docs. Delete the negated clause; if the sentence still says everything, the construction was doing no work.
- **Schema changes** touch `schema.sql` and a new migration script together, register the script in `irrigation.qrc`, and bump `IRRIGATION_DB_VERSION` in `IrrigationD/CMakeLists.txt`.
- **Two zone identifiers** (web README): `zone.number` addresses a valve on run and stop routes and in `/admin/status`; `zone.id` is what a program stores. Every new field in this plan follows that rule and the README says which is which.
- **Web:** every network call goes through `web/src/api/client.ts`; every rendered instant goes through `web/src/time/zonedformat.ts` with the controller's zone id; `npm run typecheck` is the `-Werror`.
- Commit messages are conventional commits (`feat`, `fix`, `refactor`, `test`, `doc`, `style`) and end with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
  ```

---

## Review Focus

The five failure modes most likely to survive implementation unnoticed, and the test that catches each.

1. **A slot opening a zone from inside `zoneClosed` re-energises a zone that just closed.** `ProgramRunner` answers a close by opening waiting zones, and `openZone()` builds the bank write from the controller's open set. If a closing zone is still in that set when `zoneClosed` is emitted, the runner's write turns it back on with no deadline. Caught by `TestZoneController::aZoneOpenedFromAClosedSlotNeverReenergisesTheClosedZone` and `aZoneOpenedFromAnAllOffSlotLeavesEveryClosedZoneOff` (Task 12).
2. **STOP or daemon teardown starts the next queued program.** `ProgramQueue` starts its head entry on `programAborted`. A STOP that aborts the runner before emptying the queue, or a teardown that aborts the runner while the queue still exists, opens valves while everything is supposed to be going off. Caught by `TestProgramQueue::dropAllBeforeAbortLeavesEveryValveClosed`, `abortingWithAQueuedEntryStartsIt` and `aDeletedQueueStartsNothingWhenTheRunnerAborts` (Task 13).
3. **A close timer firing late trips the watchdog.** A coarse `QTimer` can fire after the zone's `QDeadlineTimer` has expired, and a watchdog tick in that gap sees a zone past its deadline, trips, latches a fault and aborts the program. More zones open means more deadlines in play. Caught by `TestZoneController::aZoneClosingOnItsOwnTimerNeverTripsAFastWatchdog` (Task 12).
4. **An unanswered run request wedges the HTTP thread or the daemon's shutdown.** The run routes block for the valve thread's decision. During teardown the daemon thread is inside `IrrigationControlServer::stop()` and never answers; an unbounded wait would deadlock that `stop()`. Caught by `TestControlServer::zoneRunUnansweredAnswers503WithinTheDecisionTimeout` and `stopReturnsWhileAZoneRunDecisionIsPending` (Task 14).
5. **The 1.1.0 migration fails on a real database and the recreate path silently wipes every program.** `migrate()` renames a database whose script throws and recreates it empty. The step migration copies rows across three tables with foreign keys on; a column or id slip passes against an empty database and destroys a populated one. Caught by `TestDataSource::migratesProgramZonesIntoOneZoneSteps`, which seeds a 1.0.0 database with diverging ids and asserts no backup file appeared (Task 13), and by the bench task's row-count check (Task 16).

---

## File Structure

| Path | Change | Responsibility |
|---|---|---|
| `IrrigationD/src/zonecontroller.{h,cpp}` | Modify | Cap, per-zone deadlines and close timers, close reasons, set read-back, count check. |
| `IrrigationD/src/programrunner.{h,cpp}` | Modify | Steps, waves, waiting zones, takeover, close-reason handling, status accessors. |
| `IrrigationD/src/runrequest.{h,cpp}` | Create | A run request shared between the HTTP thread and the valve thread, with its refusal. |
| `IrrigationD/src/programqueue.{h,cpp}` | Create | FIFO of waiting programs and every firing outcome after `queued`. |
| `IrrigationD/src/scheduler.{h,cpp}` | Modify | Records a due firing as `queued`. |
| `IrrigationD/src/irrigationdaemon.{h,cpp}` | Modify | Wires the queue, the decisions, per-zone stop, the cap setting and the new status. |
| `IrrigationD/src/irrigationcontrolserver.{h,cpp}` | Modify | New `ServerStatus`, decision waits, per-zone stop route, steps in program routes, cap setting. |
| `IrrigationD/src/json/statusjson.cpp` | Modify | New `/admin/status` shape. |
| `IrrigationD/src/json/programjson.{h,cpp}` | Modify | `steps` in program bodies. |
| `IrrigationD/src/model/programstep.{h,cpp}` | Create | `ProgramStep` value type. |
| `IrrigationD/src/model/programzone.{h,cpp}` | Delete | Replaced by `ProgramStep`. |
| `IrrigationD/src/model/firedinstant.h` | Modify | New outcomes. |
| `IrrigationD/src/database/irrigationdatasource.{h,cpp}` | Modify | Step queries, outcome replacement. |
| `IrrigationD/src/database/schema.sql` | Modify | `program_steps`, `program_step_zones`, cap setting. |
| `IrrigationD/src/database/migrate/irrigation/1.1.0/01-program-steps.sql` | Create | Converts `program_zones` into one-zone steps. |
| `IrrigationD/src/database/irrigation.qrc`, `IrrigationD/CMakeLists.txt` | Modify | Register the script, bump to 1.1.0, new sources. |
| `IrrigationD/tests/*` | Modify / Create | Task 1, 2, 5, 6 compile edits; Tasks 12–14 tests; new `tst_programqueue.cpp`. |
| `web/src/api/{types,decode,client}.ts` | Modify | New status and program shapes, refusal reason, per-zone stop. |
| `web/src/hooks/useStatus.ts` | Modify | Poll fast while anything runs, runs a program, or waits. |
| `web/src/screens/NowScreen.tsx` | Modify | Running list with per-zone Stop, program and queue lines, cap disabling. |
| `web/src/screens/ProgramEditor.tsx`, `ProgramsScreen.tsx`, `web/src/programs/dayRule.ts` | Modify | Steps with zone chips, wave warning, wave-aware total. |
| `web/src/screens/SettingsScreen.tsx`, `web/src/settings/settingsMap.ts` | Modify | "Max zones at once". |
| `web/src/styles/app.css` | Modify | Running rows, tags, chips, step blocks, hints. |
| `web/src/test/fixtures.ts` | Modify | New shapes. |
| `docs/design/2026-09-05-irrigation-design.md`, `web/README.md` | Modify | Bring the base design and README in line with the spec. |

---

## Wire contract this plan builds

`GET /admin/status`:

```json
{
  "running":  [ { "zone": 5, "secondsRemaining": 1712, "source": "program" },
                { "zone": 1, "secondsRemaining": 240,  "source": "manual" } ],
  "program":  { "id": 2, "name": "Morning Drip", "step": 1, "stepCount": 2, "waitingZones": [7] },
  "queue":    [ { "programId": 1, "name": "Summer", "queuedAtUtc": "2026-10-01T13:00:04Z" } ],
  "maxConcurrentZones": 2,
  "nextRunUtc": "…", "rainDelayUntilUtc": "…", "masterEnabled": true,
  "stopHeld": false, "timezone": "America/Los_Angeles"
}
```

`running[].zone` and `program.waitingZones[]` are zone **numbers**. `running` is ordered by zone number. `program` is `null` when nothing runs. `program.step` is 1-based.

Program bodies: `"steps": [ { "zones": [zoneId, …], "durationSeconds": N } ]` in place of `"zones"`. Zone entries are zone **ids**. Array order is run order; there is no `sequence` on the wire. Responses add the stored step `"id"` to each step; requests ignore it. A step needs at least one zone, no zone twice, and `durationSeconds ≥ 1`.

Run routes (`POST /admin/zones/{n}/run`, `POST /admin/programs/{id}/run`):

| Outcome | Status | Body |
|---|---|---|
| Accepted (zone opened, program started or queued) | 202 | `{"accepted": true}` |
| Refused | 409 | `{"error": "<sentence for a person>", "reason": "cap_reached" \| "stop_held" \| "master_disabled" \| "zone_disabled" \| "already_queued"}` |
| The valve thread failed the request | 500 | `{"error": "<text>", "reason": "failed"}` |
| The valve thread never answered | 503 | `{"error": "the controller did not answer", "reason": "timeout"}` |

`POST /admin/zones/{n}/stop` → 202 `{"accepted": true}`, 404 for an unknown zone number. `PUT /admin/settings` accepts `max_concurrent_zones` as a string integer 1–8.

`fired_instants.outcome` values written from now on: `queued`, `ran`, `skipped_rain`, `skipped_stop`, `skipped_duplicate`, `skipped_disabled`, `dropped_stop`, `dropped_restart`, `missed`, `failed`. `skipped_busy` stays readable and is never written.

---

## Rulings on spec gaps

These are decisions the spec leaves open. Each task implements them; the doc task records them in the base design.

1. **Master enable off at dequeue** drops the entry and records a scheduled entry `skipped_disabled`, a new outcome. The spec names `skipped_rain` for the rain case only.
2. **The rain re-check at dequeue applies to scheduled entries only.** A manual "run program now" ignores the rain delay today and keeps doing so after waiting in the queue.
3. **A firing that starts immediately is inserted `queued` by the scheduler and updated to `ran` in the same event-loop pass.** The row is never observable as `queued` except across a crash, which the restart sweep then records `dropped_restart`.
4. **Per-zone stop of a waiting step zone does nothing.** Only an open zone has anything to close; the runner still opens it when a slot frees.
5. **Raising the cap fills waiting step zones immediately** (`ProgramRunner::fillSlots()` after a settings change).
6. **A queued program dequeued while the watchdog latch is set fails to open and is recorded `failed`.** The queue keeps draining; each entry fails the same way until the latch clears.
7. **Accepted runs stay 202**; a decision that never arrives answers 503 after a bounded wait (5 s by default).
8. **The cap hint appears once, above the tile grid**, with every non-open tile's Run disabled.
9. **Close timers are `Qt::PreciseTimer`.** Review Focus 3.

---

### Task 1: ZoneController — the cap, per-zone deadlines and close reasons

Spec §2 and §4. The controller stops closing the open zone on every `openZone()` and instead holds a set of open zones under `max_concurrent_zones`. Each zone gets its own deadline and its own single-shot close timer. Every close reports why it happened, and the watchdog compares the read-back with the expected set and checks the asserted-line count against the cap the open zones were opened under.

**Files:**
- Modify: `IrrigationD/src/zonecontroller.h`, `IrrigationD/src/zonecontroller.cpp`
- Modify: `IrrigationD/src/programrunner.h`, `IrrigationD/src/programrunner.cpp` (watchdog slot signature only)
- Modify: `IrrigationD/src/irrigationdaemon.cpp` (compile-keeping edits and the cap setting)
- Modify: `IrrigationD/tests/tst_zonecontroller.cpp`, `IrrigationD/tests/tst_programrunner.cpp` (mechanical rename only)

**Interfaces:**
- Consumes: `OutputBank::setValues(const QMap<quint32, bool>&)`, `OutputBank::readValues(QMap<quint32, bool>&)`, `OutputBank::isRequested()` from KanoopPiQt.
- Produces (all on `ZoneController`):
  - `enum class CloseReason { Deadline, Stopped, AllOff, Watchdog };` declared with `Q_ENUM`
  - `static constexpr int DefaultMaxConcurrentZones = 2;`, `static constexpr int MaxConcurrentZonesCeiling = 8;`
  - `bool openZone(int zoneNumber, int seconds);` — re-open of an open zone takes no slot and resets its deadline
  - `bool closeZone(int zoneNumber);` — reason `Stopped`; true when closed or already closed
  - `bool allOff();` — reason `AllOff`
  - `QList<int> openZoneNumbers() const;` — ascending
  - `bool isOpen(int zoneNumber) const;`
  - `bool hasSlotFor(int zoneNumber) const;` — true when open already or fewer than the cap are open
  - `int secondsRemaining(int zoneNumber) const;` — zero for a closed zone
  - `int maxConcurrentZones() const;`, `void setMaxConcurrentZones(int value);` — bounded to 1..8
  - signals `void zoneOpened(int zoneNumber, int seconds);`, `void zoneClosed(int zoneNumber, ZoneController::CloseReason reason);`, `void watchdogTripped(const QList<int>& zoneNumbers);`
  - test seams `void disableCloseTimerForTest(int zoneNumber);`, `void expireCloseTimerForTest(int zoneNumber);`, `bool closeTimerActiveForTest(int zoneNumber) const;`, `void triggerWatchdogForTest();`
  - removed: `int openZoneNumber() const`, `int secondsRemaining() const`, zero-argument test seams
- Produces on `ProgramRunner`: `void onWatchdogTripped();` (was `onWatchdogTripped(int)`)

- [ ] **Step 1: Replace `IrrigationD/src/zonecontroller.h`**

```cpp
#ifndef ZONECONTROLLER_H
#define ZONECONTROLLER_H

#include <QDeadlineTimer>
#include <QList>
#include <QMap>
#include <QObject>
#include <QTimer>

#include <Kanoop/timespan.h>
#include <Kanoop/utility/loggingbaseclass.h>
#include <Kanoop/pi/outputbank.h>

/**
 * @brief Sole owner of the valve outputs.
 *
 * Enforces five invariants regardless of caller:
 *  1. Concurrency cap — an open that would put more than maxConcurrentZones() zones
 *     open together fails. Every line change for one request goes out in one bank
 *     write. Re-opening an open zone takes no slot and resets its deadline.
 *  2. No open without a deadline — every open zone has its own single-shot close.
 *  3. Duration clamp — each request is clamped to the configured ceiling.
 *  4. Watchdog — a periodic tick reads the lines back and closes the bank when a
 *     zone is open past its deadline, the lines differ from the expected open set,
 *     or the read-back fails. A trip latches a fault that refuses every openZone()
 *     until a later tick finds no zone open and reads back exactly the expected set.
 *     A zone whose close failed inside the trip holds the latch until a retried
 *     close lands.
 *  5. Count check — the watchdog also trips when more lines read back asserted than
 *     the highest cap in force when the open zones were opened.
 *
 * @warning Every method must be called on the thread that owns this object.
 *          Other threads emit a request signal instead. Two threads writing the
 *          bank is the one race this class exists to prevent.
 */
class ZoneController : public QObject,
                       public LoggingBaseClass
{
    Q_OBJECT
public:
    /** @brief Why a zone closed. */
    enum class CloseReason
    {
        Deadline,
        Stopped,
        AllOff,
        Watchdog
    };
    Q_ENUM(CloseReason)

    /** @brief The cap in force until setMaxConcurrentZones() is called. */
    static constexpr int DefaultMaxConcurrentZones = 2;

    /** @brief The highest cap setMaxConcurrentZones() accepts. */
    static constexpr int MaxConcurrentZonesCeiling = 8;

    /**
     * @brief Constructs a controller over @p zoneGpioMap.
     * @param backend The GPIO backend. Must already have an open chip.
     * @param zoneGpioMap Zone number to line offset.
     * @param activeLow Whether the valve lines are active-low.
     * @param maxZoneSeconds The hard ceiling applied to every requested duration.
     *        setMaxZoneSeconds() can lower the ceiling and never raise it past this value.
     */
    ZoneController(IGpioBackend* backend,
                   const QMap<int, quint32>& zoneGpioMap,
                   bool activeLow,
                   int maxZoneSeconds,
                   QObject* parent = nullptr);

    /** @brief Destructor. Closes every zone. */
    virtual ~ZoneController();

    /** @brief Requests the lines and drives all of them inactive. @return True on success. */
    bool begin();

    /**
     * @brief Opens @p zoneNumber for @p seconds alongside the zones already open.
     *
     * Re-opening an open zone writes nothing, takes no slot, and sets its deadline to
     * now plus the clamped duration.
     * @return True on success. False for an unknown zone, a non-positive duration, no
     *         free slot under the cap, a failed write, or while the watchdog fault latch is set.
     */
    bool openZone(int zoneNumber, int seconds);

    /**
     * @brief Closes @p zoneNumber, reporting CloseReason::Stopped.
     * @return True when the zone closed or was not open. False when the write failed;
     *         the close is then retried on the zone's own timer.
     */
    bool closeZone(int zoneNumber);

    /**
     * @brief Closes every zone, reporting CloseReason::AllOff. Callable from any component; always takes precedence.
     * @return True when every line was driven inactive, or when the lines are not
     *         requested, in which case nothing is written and the open zones are forgotten.
     */
    bool allOff();

    /** @brief Returns the open zone numbers in ascending order. */
    QList<int> openZoneNumbers() const { return _open.keys(); }

    /** @brief Returns whether @p zoneNumber is open. */
    bool isOpen(int zoneNumber) const { return _open.contains(zoneNumber); }

    /** @brief Returns whether openZone(@p zoneNumber) would fit under the cap: the zone is open already or fewer than the cap are open. */
    bool hasSlotFor(int zoneNumber) const { return _open.contains(zoneNumber) || _open.count() < _maxConcurrentZones; }

    /** @brief Returns the seconds remaining on @p zoneNumber, or zero when it is not open. */
    int secondsRemaining(int zoneNumber) const;

    /** @brief Returns the ceiling the next openZone() clamps to, in seconds. */
    int maxZoneSeconds() const { return _maxZoneSeconds; }

    /**
     * @brief Sets the ceiling the next openZone() clamps to, bounded to 1 through the constructor's hard ceiling.
     *
     * A zone already open keeps the deadline it opened with.
     */
    void setMaxZoneSeconds(int value);

    /** @brief Returns how many zones may be open together. */
    int maxConcurrentZones() const { return _maxConcurrentZones; }

    /**
     * @brief Sets how many zones may be open together, bounded to 1 through MaxConcurrentZonesCeiling.
     *
     * Lowering the cap closes nothing. It refuses new opens until the open count falls below it.
     */
    void setMaxConcurrentZones(int value);

    /** @brief Returns whether the watchdog fault latch is refusing openZone(). */
    bool isFaulted() const { return _faulted; }

    /** @brief Sets how often the watchdog verifies line state against the deadlines. */
    void setWatchdogInterval(const TimeSpan& value);

    /** @brief Stops @p zoneNumber's close timer without closing the zone. Test seam for the watchdog. */
    void disableCloseTimerForTest(int zoneNumber);

    /** @brief Runs @p zoneNumber's close path immediately. Test seam. */
    void expireCloseTimerForTest(int zoneNumber);

    /** @brief Returns whether @p zoneNumber's close timer is armed. Test seam. */
    bool closeTimerActiveForTest(int zoneNumber) const;

    /** @brief Runs one watchdog check immediately. Test seam. */
    void triggerWatchdogForTest() { onWatchdogTimer(); }

    /** @brief Returns the text of the most recent failure. */
    QString errorText() const { return _errorText; }

signals:
    /** @brief Emitted after @p zoneNumber has been driven active, or re-opened, for @p seconds. */
    void zoneOpened(int zoneNumber, int seconds);

    /** @brief Emitted after @p zoneNumber has been driven inactive, with the reason it closed. */
    void zoneClosed(int zoneNumber, ZoneController::CloseReason reason);

    /**
     * @brief Emitted after the watchdog closed the bank.
     *
     * The watchdog trips when a zone is open past its deadline, when the lines read
     * back different from the expected open set, when more lines are asserted than the
     * cap allows, or when the read-back fails. @p zoneNumbers holds the zones that were
     * open, possibly none. Never emitted when the close inside the trip failed.
     */
    void watchdogTripped(const QList<int>& zoneNumbers);

private slots:
    void onWatchdogTimer();

private:
    class OpenZone
    {
    public:
        QDeadlineTimer deadline;
        int capAtOpen = 0;
        CloseReason pendingReason = CloseReason::Deadline;
    };

    bool writeOpenSet(const QList<int>& zoneNumbers);
    bool closeOne(int zoneNumber, CloseReason reason);
    bool closeAll(CloseReason reason);
    void startCloseTimer(int zoneNumber, const TimeSpan& delay);
    void onCloseTimer(int zoneNumber);
    void tripWatchdog();

    static const TimeSpan RetryInterval;
    static const TimeSpan DefaultWatchdogInterval;
    static const TimeSpan MinimumWatchdogInterval;

    QMap<int, quint32> _zoneGpioMap;
    int _hardMaxZoneSeconds = 3600;
    int _maxZoneSeconds = 3600;
    int _maxConcurrentZones = DefaultMaxConcurrentZones;
    OutputBank* _bank = nullptr;
    QMap<int, QTimer*> _closeTimers;
    QTimer _watchdogTimer;
    QMap<int, OpenZone> _open;
    bool _faulted = false;
    QString _errorText;
};

#endif // ZONECONTROLLER_H
```

- [ ] **Step 2: Replace `IrrigationD/src/zonecontroller.cpp`**

```cpp
#include "zonecontroller.h"

#include <QStringList>

#include <chrono>
#include <limits>

namespace
{
    QString zoneListText(const QList<int>& zoneNumbers)
    {
        QStringList parts;
        for(int zoneNumber : zoneNumbers) {
            parts.append(QString::number(zoneNumber));
        }
        return parts.join(", ");
    }
}

const TimeSpan ZoneController::RetryInterval           = TimeSpan::fromSeconds(1);
const TimeSpan ZoneController::DefaultWatchdogInterval = TimeSpan::fromSeconds(1);
const TimeSpan ZoneController::MinimumWatchdogInterval = TimeSpan::fromMilliseconds(50);

ZoneController::ZoneController(IGpioBackend* backend,
                               const QMap<int, quint32>& zoneGpioMap,
                               bool activeLow,
                               int maxZoneSeconds,
                               QObject* parent) :
    QObject(parent),
    LoggingBaseClass("zone"),
    _zoneGpioMap(zoneGpioMap),
    _hardMaxZoneSeconds(maxZoneSeconds),
    _maxZoneSeconds(maxZoneSeconds),
    _bank(new OutputBank(backend, QStringLiteral("irrigationd-zones"), zoneGpioMap.values(), activeLow, this))
{
    for(auto it = _zoneGpioMap.constBegin(); it != _zoneGpioMap.constEnd(); ++it) {
        const int zoneNumber = it.key();
        QTimer* timer = new QTimer(this);
        timer->setSingleShot(true);
        // A coarse timer may fire after the zone's deadline, and the watchdog trips on
        // any zone it finds past its deadline.
        timer->setTimerType(Qt::PreciseTimer);
        connect(timer, &QTimer::timeout, this, [this, zoneNumber]()
        {
            onCloseTimer(zoneNumber);
        });
        _closeTimers.insert(zoneNumber, timer);
    }

    setWatchdogInterval(DefaultWatchdogInterval);
    connect(&_watchdogTimer, &QTimer::timeout, this, &ZoneController::onWatchdogTimer);
}

ZoneController::~ZoneController()
{
    allOff();
}

bool ZoneController::begin()
{
    if(_bank->request() == false) {
        _errorText = _bank->errorText();
        return false;
    }

    _watchdogTimer.start();

    if(writeOpenSet(QList<int>()) == false) {
        return false;
    }

    return true;
}

bool ZoneController::writeOpenSet(const QList<int>& zoneNumbers)
{
    QMap<quint32, bool> values;
    for(auto it = _zoneGpioMap.constBegin(); it != _zoneGpioMap.constEnd(); ++it) {
        values.insert(it.value(), zoneNumbers.contains(it.key()));
    }

    if(_bank->setValues(values) == false) {
        _errorText = _bank->errorText();
        return false;
    }

    return true;
}

bool ZoneController::openZone(int zoneNumber, int seconds)
{
    if(_faulted == true) {
        _errorText = QString("Refused to open zone %1: the watchdog fault latch is set until a read-back matches the expected line state")
                         .arg(zoneNumber);
        return false;
    }

    if(_zoneGpioMap.contains(zoneNumber) == false) {
        _errorText = QString("Unknown zone %1").arg(zoneNumber);
        return false;
    }

    if(seconds < 1) {
        _errorText = QString("Duration %1 is not positive").arg(seconds);
        return false;
    }

    const bool reopen = _open.contains(zoneNumber);
    if(reopen == false && _open.count() >= _maxConcurrentZones) {
        _errorText = QString("Refused to open zone %1: %2 zones already running")
                         .arg(zoneNumber).arg(_open.count());
        return false;
    }

    if(reopen == false) {
        QList<int> next = _open.keys();
        next.append(zoneNumber);
        if(writeOpenSet(next) == false) {
            return false;
        }
    }

    const int clamped = qMin(seconds, _maxZoneSeconds);
    OpenZone& zone = _open[zoneNumber];
    zone.deadline = QDeadlineTimer(std::chrono::seconds(clamped));
    zone.capAtOpen = qMax(zone.capAtOpen, _maxConcurrentZones);
    zone.pendingReason = CloseReason::Deadline;
    startCloseTimer(zoneNumber, TimeSpan::fromSeconds(clamped));

    emit zoneOpened(zoneNumber, clamped);

    return true;
}

bool ZoneController::closeZone(int zoneNumber)
{
    return closeOne(zoneNumber, CloseReason::Stopped);
}

bool ZoneController::allOff()
{
    return closeAll(CloseReason::AllOff);
}

bool ZoneController::closeOne(int zoneNumber, CloseReason reason)
{
    if(_open.contains(zoneNumber) == false) {
        return true;
    }

    QList<int> remaining = _open.keys();
    remaining.removeAll(zoneNumber);
    if(_bank->isRequested() == true && writeOpenSet(remaining) == false) {
        logText(LVL_ERROR, QString("Failed to close zone %1: %2").arg(zoneNumber).arg(_errorText));
        _open[zoneNumber].pendingReason = reason;
        startCloseTimer(zoneNumber, RetryInterval);
        return false;
    }

    // The zone leaves _open before zoneClosed: a slot that opens a zone writes the bank
    // from _open, and a zone still listed there is re-energised.
    _closeTimers.value(zoneNumber)->stop();
    _open.remove(zoneNumber);

    emit zoneClosed(zoneNumber, reason);
    return true;
}

bool ZoneController::closeAll(CloseReason reason)
{
    const QList<int> closing = _open.keys();

    if(_bank->isRequested() == true && writeOpenSet(QList<int>()) == false) {
        logText(LVL_ERROR, QString("allOff failed: %1").arg(_errorText));
        for(int zoneNumber : closing) {
            _open[zoneNumber].pendingReason = reason;
            startCloseTimer(zoneNumber, RetryInterval);
        }
        return false;
    }

    // Every closed zone leaves _open before the first zoneClosed: a slot that opens a
    // zone writes the bank from _open, and a zone still listed there is re-energised.
    for(int zoneNumber : closing) {
        _closeTimers.value(zoneNumber)->stop();
        _open.remove(zoneNumber);
    }

    for(int zoneNumber : closing) {
        emit zoneClosed(zoneNumber, reason);
    }

    return true;
}

void ZoneController::startCloseTimer(int zoneNumber, const TimeSpan& delay)
{
    const qint64 milliseconds = qMin<qint64>(static_cast<qint64>(delay.totalMilliseconds()),
                                             std::numeric_limits<int>::max());
    _closeTimers.value(zoneNumber)->start(static_cast<int>(milliseconds));
}

int ZoneController::secondsRemaining(int zoneNumber) const
{
    int result = 0;
    if(_open.contains(zoneNumber)) {
        const qint64 secondsLeft = std::chrono::duration_cast<std::chrono::seconds>(
                                       _open.value(zoneNumber).deadline.remainingTimeAsDuration()).count();
        result = secondsLeft > 0 ? static_cast<int>(secondsLeft) : 0;
    }
    return result;
}

void ZoneController::setMaxZoneSeconds(int value)
{
    _maxZoneSeconds = qMin(qMax(value, 1), _hardMaxZoneSeconds);
}

void ZoneController::setMaxConcurrentZones(int value)
{
    _maxConcurrentZones = qMin(qMax(value, 1), MaxConcurrentZonesCeiling);
}

void ZoneController::setWatchdogInterval(const TimeSpan& value)
{
    const TimeSpan interval = TimeSpan::max(MinimumWatchdogInterval, value);
    _watchdogTimer.setInterval(static_cast<int>(interval.totalMilliseconds()));
}

void ZoneController::disableCloseTimerForTest(int zoneNumber)
{
    QTimer* timer = _closeTimers.value(zoneNumber, nullptr);
    if(timer != nullptr) {
        timer->stop();
    }
}

void ZoneController::expireCloseTimerForTest(int zoneNumber)
{
    disableCloseTimerForTest(zoneNumber);
    onCloseTimer(zoneNumber);
}

bool ZoneController::closeTimerActiveForTest(int zoneNumber) const
{
    QTimer* timer = _closeTimers.value(zoneNumber, nullptr);
    return timer != nullptr && timer->isActive();
}

void ZoneController::onCloseTimer(int zoneNumber)
{
    if(_open.contains(zoneNumber) == false) {
        return;
    }

    closeOne(zoneNumber, _open.value(zoneNumber).pendingReason);
}

void ZoneController::onWatchdogTimer()
{
    QMap<quint32, bool> actual;
    if(_bank->readValues(actual) == false) {
        logText(LVL_ERROR, QString("Watchdog could not read the bank: %1").arg(_bank->errorText()));
        tripWatchdog();
        return;
    }

    QMap<quint32, bool> expected;
    for(auto it = _zoneGpioMap.constBegin(); it != _zoneGpioMap.constEnd(); ++it) {
        expected.insert(it.value(), _open.contains(it.key()));
    }

    int asserted = 0;
    for(bool value : actual) {
        if(value == true) {
            asserted++;
        }
    }

    int allowed = 0;
    QList<int> late;
    for(auto it = _open.constBegin(); it != _open.constEnd(); ++it) {
        allowed = qMax(allowed, it.value().capAtOpen);
        if(it.value().deadline.hasExpired()) {
            late.append(it.key());
        }
    }

    const bool mismatch = actual != expected;
    const bool overCount = asserted > allowed;

    if(mismatch || overCount || late.isEmpty() == false) {
        logText(LVL_ERROR, QString("Watchdog tripped: open zones [%1], past deadline [%2], %3 lines asserted, %4 allowed")
                               .arg(zoneListText(_open.keys()), zoneListText(late))
                               .arg(asserted).arg(allowed));
        tripWatchdog();
    }
    else if(_faulted == true && _open.isEmpty()) {
        _faulted = false;
        logText(LVL_WARNING, "Watchdog read-back matches the expected line state; fault latch cleared");
    }
}

void ZoneController::tripWatchdog()
{
    const QList<int> trippedOn = _open.keys();

    // _faulted must be set before closeAll(): closeAll() emits zoneClosed synchronously,
    // and a slot answering it may call openZone().
    _faulted = true;

    if(closeAll(CloseReason::Watchdog) == true) {
        emit watchdogTripped(trippedOn);
    }
    else {
        logText(LVL_ERROR, QString("Watchdog could not close the bank: %1").arg(_errorText));
    }
}

#include "moc_zonecontroller.cpp"
```

- [ ] **Step 3: Change `ProgramRunner`'s watchdog slot to take no argument**

`watchdogTripped` now carries a `QList<int>`, which an `int` slot cannot receive. In `IrrigationD/src/programrunner.h` replace:

```cpp
    /** @brief Stops the running program when the controller's watchdog trips. */
    void onWatchdogTripped(int zoneNumber);
```

with:

```cpp
    /** @brief Stops the running program when the controller's watchdog trips. */
    void onWatchdogTripped();
```

In `IrrigationD/src/programrunner.cpp` replace the function head and drop the `Q_UNUSED` line:

```cpp
void ProgramRunner::onWatchdogTripped()
{
    if(_running == false) {
        return;
    }
```

`connect(_controller, &ZoneController::zoneClosed, this, &ProgramRunner::onZoneClosed)` keeps compiling: a slot may take fewer arguments than its signal. Task 3 rewrites the runner.

- [ ] **Step 4: Keep `IrrigationDaemon` compiling and apply the cap setting**

In `IrrigationD/src/irrigationdaemon.cpp`:

`publishStatus()` — replace the two `runningZone`/`secondsRemaining` lines (Task 5 replaces this function):

```cpp
    status.runningZone = _zoneController->openZoneNumbers().value(0);
    status.secondsRemaining = _zoneController->secondsRemaining(status.runningZone);
```

`onProgramDue()` — replace the busy branch head and log line (Task 4 replaces this function):

```cpp
    else if(_programRunner->isRunning() || _zoneController->openZoneNumbers().isEmpty() == false) {
        logText(LVL_WARNING, QString("Program %1 came due while zones are open").arg(programId));
```

`onManualZoneRunRequested()` — delete the two-line comment above `_programRunner->abort();`. The call stays until Task 6.

`applyRuntimeSettings()` — insert after the zone-ceiling `logText`:

```cpp
    const QString storedCap = _dataSource->settingValue("max_concurrent_zones");
    bool parsedCap = false;
    const int cap = storedCap.toInt(&parsedCap);
    _zoneController->setMaxConcurrentZones(parsedCap == true ? cap : ZoneController::DefaultMaxConcurrentZones);
    logText(LVL_INFO, QString("At most %1 zones open at once (database '%2')")
                          .arg(_zoneController->maxConcurrentZones()).arg(storedCap));
```

- [ ] **Step 5: Keep the two test files compiling with a mechanical rename**

```bash
cd /home/spunak/src/punak/irrigation
sed -i \
  -e 's/openZoneNumber()/openZoneNumbers().value(0)/g' \
  -e 's/controller\.secondsRemaining()/controller.secondsRemaining(controller.openZoneNumbers().value(0))/g' \
  -e 's/\(disableCloseTimerForTest\|expireCloseTimerForTest\|closeTimerActiveForTest\)()/\1(controller.openZoneNumbers().value(0))/g' \
  -e 's/watchdogTripped(4)/watchdogTripped({ 4 })/g' \
  IrrigationD/tests/tst_zonecontroller.cpp IrrigationD/tests/tst_programrunner.cpp
git diff --stat IrrigationD/tests
```

Expected: both files change, nothing else. Every `ZoneController` in both files is named `controller`.

- [ ] **Step 6: Build**

```bash
cmake --build build -j 32
```

Expected: exits zero.

These tests now fail on purpose and are rewritten in Task 12: `tst_zonecontroller` — `openingAZoneClosesTheOpenOneAtomically` (two zones now open together) and every assertion reading `watchdogTripped`'s first argument as an `int`; `tst_programrunner` — `manuallyOpenedZoneDoesNotCascadeWhenDisplaced`. Run `ctest --test-dir build --output-on-failure -R 'tst_zonecontroller|tst_programrunner'` and confirm no other test in those two suites fails.

- [ ] **Step 7: Commit**

```bash
git add IrrigationD/src/zonecontroller.h IrrigationD/src/zonecontroller.cpp \
        IrrigationD/src/programrunner.h IrrigationD/src/programrunner.cpp \
        IrrigationD/src/irrigationdaemon.cpp \
        IrrigationD/tests/tst_zonecontroller.cpp IrrigationD/tests/tst_programrunner.cpp
git commit -F - <<'EOF'
feat: let the zone controller open zones together under a cap

ZoneController now holds a set of open zones bounded by
max_concurrent_zones (default 2). Each zone has its own deadline and
precise single-shot close timer, re-opening an open zone resets its
deadline without taking a slot, and every close reports a reason:
deadline, per-zone stop, all-off or watchdog. The watchdog compares the
read-back with the expected open set and trips when more lines are
asserted than the cap the open zones were opened under.

Mutual exclusion is gone, so a second openZone() no longer closes the
first zone. The daemon reads the cap from the settings table.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---

### Task 2: Program steps and the new outcomes in the database

Spec §5. Steps replace the flat zone list. A migration turns every `program_zones` row into a one-zone step with the same id, sequence and duration, then drops `program_zones`. `fired_instants` gains five outcomes, and `max_concurrent_zones` is seeded.

`ProgramZone`, `zonesFor()`, `insertProgramZone()` and `deleteProgramZones()` stay in the tree until Task 6 removes them with their last callers. Between this task and Task 6 the program routes read and write a table that no longer exists; nothing ships in between.

**Files:**
- Create: `IrrigationD/src/model/programstep.h`, `IrrigationD/src/model/programstep.cpp`
- Create: `IrrigationD/src/database/migrate/irrigation/1.1.0/01-program-steps.sql`
- Modify: `IrrigationD/src/database/schema.sql`, `IrrigationD/src/database/irrigation.qrc`
- Modify: `IrrigationD/src/database/irrigationdatasource.h`, `IrrigationD/src/database/irrigationdatasource.cpp`
- Modify: `IrrigationD/src/model/firedinstant.h`
- Modify: `IrrigationD/CMakeLists.txt`, `IrrigationD/tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `class ProgramStep { int id = 0; int programId = 0; int sequence = 0; int durationSeconds = 0; QList<int> zoneIds; bool isValid() const; };` and `typedef QList<ProgramStep> ProgramStepList;` — `zoneIds` holds `zones.id` values
  - `ProgramStepList IrrigationDataSource::stepsFor(int programId, bool* ok = nullptr);` — ordered by `sequence`, then `id`; each step's `zoneIds` in insertion order
  - `bool IrrigationDataSource::insertProgramStep(ProgramStep& step);` — writes `step.id` back; inserts one `program_step_zones` row per zone id
  - `bool IrrigationDataSource::deleteProgramSteps(int programId);`
  - `bool IrrigationDataSource::replaceFiringOutcomes(FiredInstant::Outcome from, FiredInstant::Outcome to, int* changed = nullptr);`
  - `FiredInstant::Outcome::{Queued, DroppedStop, DroppedRestart, SkippedDuplicate, SkippedDisabled}` with storage strings `queued`, `dropped_stop`, `dropped_restart`, `skipped_duplicate`, `skipped_disabled`
  - compiled schema version `1.1.0`; settings key `max_concurrent_zones` seeded `'2'`

- [ ] **Step 1: Create `IrrigationD/src/model/programstep.h`**

```cpp
#ifndef PROGRAMSTEP_H
#define PROGRAMSTEP_H

#include <QList>

/**
 * @brief One step of a program: a set of zones that water together for one duration.
 *
 * sequence orders steps lowest first. zoneIds holds zones.id values.
 */
class ProgramStep
{
public:
    int id = 0;
    int programId = 0;
    int sequence = 0;
    int durationSeconds = 0;
    QList<int> zoneIds;

    /** @brief Returns true when this step came from the database. */
    bool isValid() const { return id > 0; }
};

typedef QList<ProgramStep> ProgramStepList;

#endif // PROGRAMSTEP_H
```

- [ ] **Step 2: Create `IrrigationD/src/model/programstep.cpp`**

```cpp
#include "model/programstep.h"
```

- [ ] **Step 3: Update `IrrigationD/src/database/schema.sql`**

Replace the `program_zones` table with the two step tables:

```sql
CREATE TABLE program_steps (
    id                      INTEGER PRIMARY KEY AUTOINCREMENT,
    program_id              INTEGER NOT NULL,
    sequence                INTEGER NOT NULL,
    duration_seconds        INTEGER NOT NULL,
    FOREIGN KEY (program_id) REFERENCES programs(id) ON DELETE CASCADE
);

CREATE TABLE program_step_zones (
    id                      INTEGER PRIMARY KEY AUTOINCREMENT,
    step_id                 INTEGER NOT NULL,
    zone_id                 INTEGER NOT NULL,
    FOREIGN KEY (step_id) REFERENCES program_steps(id) ON DELETE CASCADE,
    FOREIGN KEY (zone_id) REFERENCES zones(id)         ON DELETE CASCADE
);
```

Replace the index line `CREATE INDEX idx_program_zones_program  ON program_zones(program_id, sequence);` with:

```sql
CREATE INDEX idx_program_steps_program  ON program_steps(program_id, sequence);
CREATE INDEX idx_step_zones_step        ON program_step_zones(step_id);
```

Replace the settings seed with:

```sql
INSERT INTO settings (key, value) VALUES
    ('rain_delay_until',     ''),
    ('master_enabled',       '1'),
    ('max_zone_seconds',     '3600'),
    ('log_level',            'info'),
    ('max_concurrent_zones', '2');
```

- [ ] **Step 4: Create `IrrigationD/src/database/migrate/irrigation/1.1.0/01-program-steps.sql`**

The step ids reuse the `program_zones` ids, so a step and the zone row it came from share an id and `sqlite_sequence` moves past them on its own.

```sql
CREATE TABLE program_steps (
    id                      INTEGER PRIMARY KEY AUTOINCREMENT,
    program_id              INTEGER NOT NULL,
    sequence                INTEGER NOT NULL,
    duration_seconds        INTEGER NOT NULL,
    FOREIGN KEY (program_id) REFERENCES programs(id) ON DELETE CASCADE
);

CREATE TABLE program_step_zones (
    id                      INTEGER PRIMARY KEY AUTOINCREMENT,
    step_id                 INTEGER NOT NULL,
    zone_id                 INTEGER NOT NULL,
    FOREIGN KEY (step_id) REFERENCES program_steps(id) ON DELETE CASCADE,
    FOREIGN KEY (zone_id) REFERENCES zones(id)         ON DELETE CASCADE
);

INSERT INTO program_steps (id, program_id, sequence, duration_seconds)
    SELECT id, program_id, sequence, duration_seconds FROM program_zones;

INSERT INTO program_step_zones (step_id, zone_id)
    SELECT id, zone_id FROM program_zones ORDER BY id;

DROP TABLE program_zones;

CREATE INDEX idx_program_steps_program  ON program_steps(program_id, sequence);
CREATE INDEX idx_step_zones_step        ON program_step_zones(step_id);

INSERT OR IGNORE INTO settings (key, value) VALUES ('max_concurrent_zones', '2');
```

`DROP TABLE` drops `idx_program_zones_program` with it.

- [ ] **Step 5: Register the script and bump the schema version**

`IrrigationD/src/database/irrigation.qrc`:

```xml
<RCC>
    <qresource prefix="/database">
        <file>schema.sql</file>
        <file>migrate/irrigation/1.0.0/01-initial.sql</file>
        <file>migrate/irrigation/1.1.0/01-program-steps.sql</file>
    </qresource>
</RCC>
```

`IrrigationD/CMakeLists.txt`: change `set(IRRIGATION_DB_VERSION 1.0.0)` to `set(IRRIGATION_DB_VERSION 1.1.0)`, and add the model after `programzone`:

```cmake
    src/model/programzone.h       src/model/programzone.cpp
    src/model/programstep.h       src/model/programstep.cpp
```

`IrrigationD/tests/CMakeLists.txt`: in every target that lists `../src/model/programzone.cpp` (`tst_irrigationdatasource`, `tst_repository`, `tst_programrunner`, `tst_scheduler`, `tst_controlserver`), add the line `    ../src/model/programstep.cpp` directly after it.

- [ ] **Step 6: Add the outcomes to `IrrigationD/src/model/firedinstant.h`**

Replace the enum and the map:

```cpp
    /**
     * @brief What happened when a scheduled start time came due.
     *
     * SkippedBusy is read from rows written before programs queued and is never written.
     */
    enum class Outcome
    {
        Ran,
        SkippedBusy,
        SkippedRain,
        Missed,
        SkippedStop,
        Failed,
        Queued,
        DroppedStop,
        DroppedRestart,
        SkippedDuplicate,
        SkippedDisabled
    };
```

```cpp
    class OutcomeToStringMap : public KANOOP::EnumToStringMap<Outcome>
    {
    public:
        OutcomeToStringMap()
        {
            insert(Outcome::Ran,              "ran");
            insert(Outcome::SkippedBusy,      "skipped_busy");
            insert(Outcome::SkippedRain,      "skipped_rain");
            insert(Outcome::Missed,           "missed");
            insert(Outcome::SkippedStop,      "skipped_stop");
            insert(Outcome::Failed,           "failed");
            insert(Outcome::Queued,           "queued");
            insert(Outcome::DroppedStop,      "dropped_stop");
            insert(Outcome::DroppedRestart,   "dropped_restart");
            insert(Outcome::SkippedDuplicate, "skipped_duplicate");
            insert(Outcome::SkippedDisabled,  "skipped_disabled");
        }
    };
```

The strings are stored in `fired_instants.outcome`; renaming one orphans every row already written with it.

- [ ] **Step 7: Declare the repository methods in `IrrigationD/src/database/irrigationdatasource.h`**

Add `#include "model/programstep.h"` after `#include "model/programstarttime.h"`, and add after `deleteProgramZones`:

```cpp
    /**
     * @brief Returns the steps belonging to @p programId, ordered by sequence then id, each with its zone ids.
     * @param ok When non-null, set to false when a query failed, in which case the list is empty.
     */
    ProgramStepList stepsFor(int programId, bool* ok = nullptr);

    /**
     * @brief Inserts @p step and one program_step_zones row per zone id, and writes the generated id back.
     *
     * Not atomic on its own; call inside a transaction when a partial step must not survive.
     * @return True on success.
     */
    bool insertProgramStep(ProgramStep& step);

    /** @brief Deletes every step belonging to @p programId together with its zone rows. @return True on success. */
    bool deleteProgramSteps(int programId);

    /**
     * @brief Changes every firing whose outcome is @p from to @p to.
     * @param changed When non-null, set to the number of rows changed.
     * @return True on success.
     */
    bool replaceFiringOutcomes(FiredInstant::Outcome from, FiredInstant::Outcome to, int* changed = nullptr);
```

- [ ] **Step 8: Implement them in `IrrigationD/src/database/irrigationdatasource.cpp`**

Add after `deleteProgramZones()`:

```cpp
ProgramStepList IrrigationDataSource::stepsFor(int programId, bool* ok)
{
    ProgramStepList result;
    if(ok != nullptr) {
        *ok = false;
    }

    bool success = false;
    QSqlQuery steps = prepareQuery(
        "SELECT id, program_id, sequence, duration_seconds "
        "FROM program_steps WHERE program_id = :programId ORDER BY sequence, id",
        &success);
    if(success == false) {
        return result;
    }

    steps.bindValue(":programId", programId);
    if(executeQuery(steps) == false) {
        return result;
    }

    while(steps.next()) {
        ProgramStep step;
        step.id = steps.value("id").toInt();
        step.programId = steps.value("program_id").toInt();
        step.sequence = steps.value("sequence").toInt();
        step.durationSeconds = steps.value("duration_seconds").toInt();
        result.append(step);
    }

    QSqlQuery zones = prepareQuery(
        "SELECT z.step_id, z.zone_id FROM program_step_zones z "
        "JOIN program_steps s ON s.id = z.step_id "
        "WHERE s.program_id = :programId ORDER BY z.id",
        &success);
    if(success == false) {
        return ProgramStepList();
    }

    zones.bindValue(":programId", programId);
    if(executeQuery(zones) == false) {
        return ProgramStepList();
    }

    while(zones.next()) {
        const int stepId = zones.value("step_id").toInt();
        for(ProgramStep& step : result) {
            if(step.id == stepId) {
                step.zoneIds.append(zones.value("zone_id").toInt());
                break;
            }
        }
    }

    if(ok != nullptr) {
        *ok = true;
    }
    return result;
}

bool IrrigationDataSource::insertProgramStep(ProgramStep& step)
{
    bool success = false;
    QSqlQuery query = prepareQuery(
        "INSERT INTO program_steps (program_id, sequence, duration_seconds) "
        "VALUES (:programId, :sequence, :durationSeconds)",
        &success);
    if(success == false) {
        return false;
    }

    query.bindValue(":programId",       step.programId);
    query.bindValue(":sequence",        step.sequence);
    query.bindValue(":durationSeconds", step.durationSeconds);

    if(executeQuery(query) == false) {
        return false;
    }
    step.id = query.lastInsertId().toInt();

    for(int zoneId : step.zoneIds) {
        QSqlQuery zone = prepareQuery(
            "INSERT INTO program_step_zones (step_id, zone_id) VALUES (:stepId, :zoneId)",
            &success);
        if(success == false) {
            return false;
        }

        zone.bindValue(":stepId", step.id);
        zone.bindValue(":zoneId", zoneId);
        if(executeQuery(zone) == false) {
            return false;
        }
    }

    return true;
}

bool IrrigationDataSource::deleteProgramSteps(int programId)
{
    bool success = false;
    QSqlQuery zones = prepareQuery(
        "DELETE FROM program_step_zones WHERE step_id IN "
        "(SELECT id FROM program_steps WHERE program_id = :programId)",
        &success);
    if(success == false) {
        return false;
    }

    zones.bindValue(":programId", programId);
    if(executeQuery(zones) == false) {
        return false;
    }

    QSqlQuery steps = prepareQuery("DELETE FROM program_steps WHERE program_id = :programId", &success);
    if(success == false) {
        return false;
    }

    steps.bindValue(":programId", programId);
    return executeQuery(steps);
}

bool IrrigationDataSource::replaceFiringOutcomes(FiredInstant::Outcome from, FiredInstant::Outcome to, int* changed)
{
    if(changed != nullptr) {
        *changed = 0;
    }

    bool success = false;
    QSqlQuery query = prepareQuery("UPDATE fired_instants SET outcome = :to WHERE outcome = :from", &success);
    if(success == false) {
        return false;
    }

    query.bindValue(":to",   FiredInstant::outcomeToString(to));
    query.bindValue(":from", FiredInstant::outcomeToString(from));

    if(executeQuery(query) == false) {
        return false;
    }

    if(changed != nullptr) {
        *changed = query.numRowsAffected();
    }
    return true;
}
```

- [ ] **Step 9: Build**

```bash
cmake --build build -j 32
```

Expected: exits zero, and the `rcc` step lists `01-program-steps.sql`.

These tests now fail on purpose and are rewritten in Task 13: `tst_irrigationdatasource::createsSchemaOnFirstOpen` (it expects `program_zones`), and in `tst_repository` `programZonesRoundTripInSequenceOrder` and `deleteProgramZonesLeavesOtherProgramsIntact`. `tst_programrunner` goes red wherever a program is built, because its helper still writes `program_zones`; Task 6 points the helper at steps.

- [ ] **Step 10: Commit**

```bash
git add IrrigationD/src/model/programstep.h IrrigationD/src/model/programstep.cpp \
        IrrigationD/src/database/schema.sql IrrigationD/src/database/irrigation.qrc \
        IrrigationD/src/database/migrate/irrigation/1.1.0/01-program-steps.sql \
        IrrigationD/src/database/irrigationdatasource.h IrrigationD/src/database/irrigationdatasource.cpp \
        IrrigationD/src/model/firedinstant.h IrrigationD/CMakeLists.txt IrrigationD/tests/CMakeLists.txt
git commit -F - <<'EOF'
feat: store programs as steps of zones that water together

Schema 1.1.0 replaces program_zones with program_steps and
program_step_zones. The migration turns each program_zones row into a
one-zone step with the same id, sequence and duration, so every
existing program runs exactly as before, and seeds
max_concurrent_zones at 2.

fired_instants gains queued, dropped_stop, dropped_restart,
skipped_duplicate and skipped_disabled. skipped_busy stays readable.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---

### Task 3: ProgramRunner walks steps

Spec §3.2 and §3.4. The runner opens every zone of a step that fits under the cap, holds the rest as waiting, and opens waiting zones in ascending zone number whenever any zone closes for a deadline or a per-zone stop. A step completes when every one of its zones has opened and closed. A zone already open when its step starts is taken over: its deadline becomes the step duration. A close for `AllOff` or `Watchdog` aborts the program and opens nothing.

**Files:**
- Modify: `IrrigationD/src/programrunner.h`, `IrrigationD/src/programrunner.cpp`

**Interfaces:**
- Consumes: Task 1 `ZoneController::{openZone, closeZone, isOpen, hasSlotFor, zoneClosed(int, CloseReason), watchdogTripped}`; Task 2 `IrrigationDataSource::stepsFor`, `ProgramStep`; `IrrigationDataSource::allZones()`, `allPrograms()`.
- Produces (on `ProgramRunner`; constructor unchanged):
  - `bool startProgram(int programId);` — false when already running or the first step's open failed
  - `void abort();` — closes only the zones this program opened
  - `bool isRunning() const;`, `int runningProgramId() const;`
  - `QString runningProgramName() const;`
  - `int stepNumber() const;` — 1-based, zero when idle
  - `int stepCount() const;` — zero when idle
  - `QList<int> waitingZones() const;` — zone numbers, ascending
  - `bool ownsZone(int zoneNumber) const;` — true for an open zone of the current step
  - `void fillSlots();` — opens waiting zones that now fit
  - slots `void onZoneClosed(int zoneNumber, ZoneController::CloseReason reason);`, `void onWatchdogTripped();`
  - signals `programStarted(int)`, `programFinished(int)`, `programAborted(int)` unchanged

- [ ] **Step 1: Replace `IrrigationD/src/programrunner.h`**

```cpp
#ifndef PROGRAMRUNNER_H
#define PROGRAMRUNNER_H

#include <QList>
#include <QObject>
#include <QString>

#include <Kanoop/utility/loggingbaseclass.h>

#include "model/programstep.h"
#include "zonecontroller.h"

class IrrigationDataSource;

/**
 * @brief Walks one program's ordered steps, opening each step's zones together under the zone cap.
 *
 * Advances only on ZoneController's zoneClosed and watchdogTripped signals; this class
 * holds no timer of its own. Wires its own connections to @p controller's zoneClosed
 * and watchdogTripped signals at construction.
 *
 * @warning programFinished and programAborted are emitted after every member is reset.
 *          A slot may call startProgram() from inside either signal.
 */
class ProgramRunner : public QObject,
                      public LoggingBaseClass
{
    Q_OBJECT
public:
    /** @brief Constructs a runner driving @p controller from steps read through @p source. */
    ProgramRunner(ZoneController* controller, IrrigationDataSource* source, QObject* parent = nullptr);

    /** @brief Starts @p programId at its first step. @return False when a program is already running or an open in the first step failed. */
    bool startProgram(int programId);

    /** @brief Stops the running program and closes the zones it opened. Zones opened by anything else stay open. */
    void abort();

    /** @brief Returns whether a program is currently running. */
    bool isRunning() const { return _running; }

    /** @brief Returns the id of the running program, or zero when none is running. */
    int runningProgramId() const { return _programId; }

    /** @brief Returns the name of the running program, or an empty string when none is running. */
    QString runningProgramName() const { return _programName; }

    /** @brief Returns the 1-based number of the current step, or zero when none is running. */
    int stepNumber() const { return _running == true ? _stepIndex + 1 : 0; }

    /** @brief Returns how many steps the running program has, or zero when none is running. */
    int stepCount() const { return _running == true ? static_cast<int>(_steps.count()) : 0; }

    /** @brief Returns the zone numbers of the current step still waiting for a slot, ascending. */
    QList<int> waitingZones() const { return _waiting; }

    /** @brief Returns whether @p zoneNumber is an open zone of the current step. */
    bool ownsZone(int zoneNumber) const { return _openZones.contains(zoneNumber); }

    /** @brief Opens every waiting zone of the current step that now fits under the cap. */
    void fillSlots();

public slots:
    /** @brief Fills freed slots after a deadline or a per-zone stop; aborts the program on an all-off or watchdog close of one of its zones. */
    void onZoneClosed(int zoneNumber, ZoneController::CloseReason reason);

    /** @brief Stops the running program when the controller's watchdog trips. */
    void onWatchdogTripped();

signals:
    /** @brief Emitted when @p programId starts. */
    void programStarted(int programId);

    /** @brief Emitted after @p programId's last step has completed. */
    void programFinished(int programId);

    /** @brief Emitted when @p programId is stopped before finishing, by abort(), a failed zone open, an all-off, or a watchdog trip. */
    void programAborted(int programId);

private:
    bool advance();
    void loadStep();
    bool openWaiting();
    void finish();
    void stopRunning();

    ZoneController* _controller = nullptr;
    IrrigationDataSource* _source = nullptr;
    ProgramStepList _steps;
    QString _programName;
    int _programId = 0;
    int _stepIndex = -1;
    QList<int> _waiting;
    QList<int> _openZones;
    bool _running = false;
};

#endif // PROGRAMRUNNER_H
```

- [ ] **Step 2: Replace `IrrigationD/src/programrunner.cpp`**

```cpp
#include "programrunner.h"

#include "database/irrigationdatasource.h"
#include "model/program.h"
#include "model/zone.h"

#include <algorithm>

ProgramRunner::ProgramRunner(ZoneController* controller, IrrigationDataSource* source, QObject* parent) :
    QObject(parent),
    LoggingBaseClass("program"),
    _controller(controller),
    _source(source)
{
    connect(_controller, &ZoneController::zoneClosed, this, &ProgramRunner::onZoneClosed);
    connect(_controller, &ZoneController::watchdogTripped, this, &ProgramRunner::onWatchdogTripped);
}

bool ProgramRunner::startProgram(int programId)
{
    if(_running) {
        return false;
    }

    _steps = _source->stepsFor(programId);
    _programName.clear();
    const ProgramList programs = _source->allPrograms();
    for(const Program& program : programs) {
        if(program.id == programId) {
            _programName = program.name;
            break;
        }
    }

    _programId = programId;
    _stepIndex = -1;
    _waiting.clear();
    _openZones.clear();
    _running = true;

    logText(LVL_INFO, QString("Program %1 '%2' starts with %3 steps").arg(programId).arg(_programName).arg(_steps.count()));
    emit programStarted(programId);

    return advance();
}

void ProgramRunner::abort()
{
    if(_running == false) {
        return;
    }

    stopRunning();
}

void ProgramRunner::fillSlots()
{
    if(_running == false || _waiting.isEmpty()) {
        return;
    }

    openWaiting();
}

bool ProgramRunner::advance()
{
    while(_running == true) {
        _stepIndex++;
        if(_stepIndex >= _steps.count()) {
            finish();
            return true;
        }

        loadStep();
        if(_waiting.isEmpty()) {
            logText(LVL_WARNING, QString("Program %1 step %2 has no enabled zone; moving on")
                                     .arg(_programId).arg(_stepIndex + 1));
            continue;
        }

        logText(LVL_INFO, QString("Program %1 starts step %2 of %3")
                              .arg(_programId).arg(_stepIndex + 1).arg(_steps.count()));
        return openWaiting();
    }

    return false;
}

void ProgramRunner::loadStep()
{
    _waiting.clear();
    _openZones.clear();

    const ProgramStep& step = _steps.at(_stepIndex);
    const ZoneList zones = _source->allZones();

    for(int zoneId : step.zoneIds) {
        int zoneNumber = 0;
        bool zoneEnabled = false;
        for(const Zone& zone : zones) {
            if(zone.id == zoneId) {
                zoneNumber = zone.number;
                zoneEnabled = zone.enabled;
                break;
            }
        }

        if(zoneNumber == 0) {
            logText(LVL_ERROR, QString("Program %1 references unknown zone id %2").arg(_programId).arg(zoneId));
        }
        else if(zoneEnabled == false) {
            logText(LVL_WARNING, QString("Program %1 skips disabled zone %2").arg(_programId).arg(zoneNumber));
        }
        else if(_waiting.contains(zoneNumber) == false) {
            _waiting.append(zoneNumber);
        }
    }

    std::sort(_waiting.begin(), _waiting.end());
}

bool ProgramRunner::openWaiting()
{
    const int duration = _steps.at(_stepIndex).durationSeconds;
    const QList<int> candidates = _waiting;
    QList<int> stillWaiting;

    for(int zoneNumber : candidates) {
        if(_controller->hasSlotFor(zoneNumber) == false) {
            stillWaiting.append(zoneNumber);
            continue;
        }

        if(_controller->openZone(zoneNumber, duration) == false) {
            logText(LVL_ERROR, QString("Program %1 failed to open zone %2: %3")
                                   .arg(_programId).arg(zoneNumber).arg(_controller->errorText()));
            stopRunning();
            return false;
        }

        _openZones.append(zoneNumber);
    }

    _waiting = stillWaiting;
    return true;
}

void ProgramRunner::finish()
{
    const int finished = _programId;

    _running = false;
    _programId = 0;
    _programName.clear();
    _stepIndex = -1;
    _steps.clear();
    _waiting.clear();
    _openZones.clear();

    logText(LVL_INFO, QString("Program %1 finished").arg(finished));
    emit programFinished(finished);
}

void ProgramRunner::stopRunning()
{
    const int stopped = _programId;
    const QList<int> owned = _openZones;

    // State is cleared before any close: closeZone() emits zoneClosed synchronously,
    // and onZoneClosed() would otherwise open waiting zones of the program being stopped.
    _running = false;
    _programId = 0;
    _programName.clear();
    _stepIndex = -1;
    _steps.clear();
    _waiting.clear();
    _openZones.clear();

    for(int zoneNumber : owned) {
        if(_controller->closeZone(zoneNumber) == false) {
            logText(LVL_ERROR, QString("Failed to close zone %1 stopping program %2: %3")
                                   .arg(zoneNumber).arg(stopped).arg(_controller->errorText()));
        }
    }

    logText(LVL_WARNING, QString("Program %1 aborted").arg(stopped));
    emit programAborted(stopped);
}

void ProgramRunner::onZoneClosed(int zoneNumber, ZoneController::CloseReason reason)
{
    if(_running == false) {
        return;
    }

    if(reason == ZoneController::CloseReason::AllOff || reason == ZoneController::CloseReason::Watchdog) {
        if(_openZones.contains(zoneNumber)) {
            logText(LVL_WARNING, QString("Zone %1 of program %2 was closed by an all-off; aborting")
                                     .arg(zoneNumber).arg(_programId));
            stopRunning();
        }
        return;
    }

    _openZones.removeAll(zoneNumber);

    if(openWaiting() == false) {
        return;
    }

    if(_waiting.isEmpty() && _openZones.isEmpty()) {
        advance();
    }
}

void ProgramRunner::onWatchdogTripped()
{
    if(_running == false) {
        return;
    }

    logText(LVL_ERROR, QString("Watchdog tripped while running program %1; aborting").arg(_programId));
    stopRunning();
}

#include "moc_programrunner.cpp"
```

- [ ] **Step 3: Build**

```bash
cmake --build build -j 32
```

Expected: exits zero. `tst_programrunner` compiles against the new header (its helper still uses `ProgramZone`, which exists until Task 6) and stays red; Task 12 replaces it.

- [ ] **Step 4: Commit**

```bash
git add IrrigationD/src/programrunner.h IrrigationD/src/programrunner.cpp
git commit -F - <<'EOF'
feat: run program steps with their zones together

ProgramRunner now walks steps. It opens every zone of a step that fits
under the cap and holds the rest as waiting; any deadline or per-zone
close opens waiting zones in ascending zone number, each for the full
step duration from its own open. A step completes once every zone has
opened and closed. A zone already open when its step starts is taken
over with the step duration. An all-off or watchdog close of a program
zone aborts the program and opens nothing, and abort() closes only the
zones the program opened.

The runner reports its step, step count, waiting zones and the zones it
owns for /admin/status.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---

### Task 4: The program queue, run decisions and queued firings

Spec §3.3, §3.5 and §5. A new `ProgramQueue` takes every program that comes due or is asked for by hand. It starts one at once when the runner is idle and nothing waits, and otherwise queues it FIFO, one entry per program. Each time the runner falls idle it re-checks master enable and rain delay and starts the head entry. The scheduler records a due firing `queued`; the queue writes its final outcome. STOP empties the queue with `dropped_stop`, and a restart sweeps leftover `queued` rows to `dropped_restart`.

`RunRequest` arrives here because `ProgramQueue::enqueueManual()` answers with its refusal enum; Task 6 carries it across the HTTP thread.

**Files:**
- Create: `IrrigationD/src/runrequest.h`, `IrrigationD/src/runrequest.cpp`
- Create: `IrrigationD/src/programqueue.h`, `IrrigationD/src/programqueue.cpp`
- Modify: `IrrigationD/src/scheduler.h`, `IrrigationD/src/scheduler.cpp`
- Modify: `IrrigationD/src/irrigationdaemon.h`, `IrrigationD/src/irrigationdaemon.cpp`
- Modify: `IrrigationD/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2 `FiredInstant::Outcome::{Queued, Ran, Failed, SkippedRain, SkippedDisabled, SkippedDuplicate, DroppedStop, DroppedRestart}`, `IrrigationDataSource::{replaceFiringOutcomes, setFiringOutcome, recordFiring, isMasterEnabled, settingValue}`; Task 3 `ProgramRunner::{startProgram, isRunning, runningProgramId, abort, programFinished, programAborted}`; `IClock` from `iclock.h`.
- Produces:
  - `class RunRequest` with `enum class Refusal { None, CapReached, StopHeld, MasterDisabled, ZoneDisabled, AlreadyQueued, Failed };`, `void complete(Refusal refusal, const QString& message = QString());`, `bool wait(const TimeSpan& timeout);`, `Refusal refusal() const;`, `QString message() const;`, `static QString refusalToString(Refusal value);` — strings `none`, `cap_reached`, `stop_held`, `master_disabled`, `zone_disabled`, `already_queued`, `failed`
  - `typedef QSharedPointer<RunRequest> RunRequestPtr;` with `Q_DECLARE_METATYPE(RunRequestPtr)`
  - `class ProgramQueue` with nested `class Entry { int programId; int startTimeId; QDateTime scheduledAtUtc; QDateTime queuedAtUtc; bool isScheduled() const; }`, constructor `ProgramQueue(ProgramRunner* runner, IrrigationDataSource* source, IClock* clock, QObject* parent = nullptr)`, `void enqueueScheduled(int programId, int startTimeId, const QDateTime& scheduledAtUtc);`, `RunRequest::Refusal enqueueManual(int programId);`, `void dropAll(FiredInstant::Outcome outcome);`, `QList<Entry> entries() const;`, `bool recordRestartDrops();`
  - `Scheduler::programDue` now follows a `queued` insert
  - `IrrigationDaemon` member `ProgramQueue* _programQueue`

- [ ] **Step 1: Create `IrrigationD/src/runrequest.h`**

```cpp
#ifndef RUNREQUEST_H
#define RUNREQUEST_H

#include <QMetaType>
#include <QMutex>
#include <QSharedPointer>
#include <QString>
#include <QWaitCondition>

#include <Kanoop/kanoopcommon.h>
#include <Kanoop/timespan.h>

/**
 * @brief A run request handed from the HTTP thread to the valve thread, and the decision the HTTP thread waits for.
 *
 * The valve thread calls complete() once. The HTTP thread blocks in wait() until then or
 * until its timeout. Shared through RunRequestPtr so either side may outlive the other.
 */
class RunRequest
{
public:
    /** @brief Why a run was refused, or None when it was accepted. */
    enum class Refusal
    {
        None,
        CapReached,
        StopHeld,
        MasterDisabled,
        ZoneDisabled,
        AlreadyQueued,
        Failed
    };

    /** @brief Records the decision and wakes the waiting thread. @p message is a sentence for a person. */
    void complete(Refusal refusal, const QString& message = QString());

    /** @brief Blocks until complete() has been called or @p timeout elapses. @return True when a decision arrived. */
    bool wait(const TimeSpan& timeout);

    /** @brief Returns the recorded refusal; None until complete() is called. */
    Refusal refusal() const;

    /** @brief Returns the recorded message. */
    QString message() const;

    /**
     * @brief Returns the wire name for @p value.
     *
     * The names are the 409 reason contract with the web client.
     */
    static QString refusalToString(Refusal value);

private:
    class RefusalToStringMap : public KANOOP::EnumToStringMap<Refusal>
    {
    public:
        RefusalToStringMap()
        {
            insert(Refusal::None,           "none");
            insert(Refusal::CapReached,     "cap_reached");
            insert(Refusal::StopHeld,       "stop_held");
            insert(Refusal::MasterDisabled, "master_disabled");
            insert(Refusal::ZoneDisabled,   "zone_disabled");
            insert(Refusal::AlreadyQueued,  "already_queued");
            insert(Refusal::Failed,         "failed");
        }
    };

    static const RefusalToStringMap _RefusalToStringMap;

    mutable QMutex _mutex;
    QWaitCondition _completed;
    bool _done = false;
    Refusal _refusal = Refusal::None;
    QString _message;
};

typedef QSharedPointer<RunRequest> RunRequestPtr;

Q_DECLARE_METATYPE(RunRequestPtr)

#endif // RUNREQUEST_H
```

- [ ] **Step 2: Create `IrrigationD/src/runrequest.cpp`**

```cpp
#include "runrequest.h"

#include <QDeadlineTimer>
#include <QMutexLocker>

const RunRequest::RefusalToStringMap RunRequest::_RefusalToStringMap;

void RunRequest::complete(Refusal refusal, const QString& message)
{
    QMutexLocker locker(&_mutex);
    _refusal = refusal;
    _message = message;
    _done = true;
    _completed.wakeAll();
}

bool RunRequest::wait(const TimeSpan& timeout)
{
    QMutexLocker locker(&_mutex);
    const QDeadlineTimer deadline(static_cast<qint64>(timeout.totalMilliseconds()));

    bool waiting = true;
    while(_done == false && waiting == true) {
        waiting = _completed.wait(&_mutex, deadline);
    }

    return _done;
}

RunRequest::Refusal RunRequest::refusal() const
{
    QMutexLocker locker(&_mutex);
    return _refusal;
}

QString RunRequest::message() const
{
    QMutexLocker locker(&_mutex);
    return _message;
}

QString RunRequest::refusalToString(Refusal value)
{
    return _RefusalToStringMap.getString(value, "failed");
}
```

- [ ] **Step 3: Create `IrrigationD/src/programqueue.h`**

```cpp
#ifndef PROGRAMQUEUE_H
#define PROGRAMQUEUE_H

#include <QDateTime>
#include <QList>
#include <QObject>

#include <Kanoop/utility/loggingbaseclass.h>

#include "iclock.h"
#include "model/firedinstant.h"
#include "runrequest.h"

class IrrigationDataSource;
class ProgramRunner;

/**
 * @brief First-in, first-out queue of programs waiting for the one runner.
 *
 * A scheduled entry's fired_instants row is inserted queued by the Scheduler and
 * given its final outcome here. A manual entry has no row. Wires its own connections
 * to @p runner's programFinished and programAborted at construction and starts the head
 * entry each time the runner falls idle.
 *
 * @warning An aborted program hands the runner to the next queued entry, which opens
 *          valves. Call dropAll() before aborting the runner on a stop, and delete this
 *          object before aborting the runner during teardown.
 */
class ProgramQueue : public QObject,
                     public LoggingBaseClass
{
    Q_OBJECT
public:
    /** @brief One program waiting to run. */
    class Entry
    {
    public:
        int programId = 0;
        int startTimeId = 0;
        QDateTime scheduledAtUtc;
        QDateTime queuedAtUtc;

        /** @brief Returns true for an entry a start time produced, false for a manual run. */
        bool isScheduled() const { return startTimeId > 0; }
    };

    /** @brief Constructs a queue feeding @p runner, recording outcomes through @p source and timing entries by @p clock. */
    ProgramQueue(ProgramRunner* runner, IrrigationDataSource* source, IClock* clock, QObject* parent = nullptr);

    /**
     * @brief Runs or queues the firing a start time produced.
     *
     * The firing's row must already exist with outcome queued. Starts it now when the
     * runner is idle and nothing waits, recording ran or failed. A program that already
     * holds a queue entry is recorded skipped_duplicate. A running program may hold one entry.
     */
    void enqueueScheduled(int programId, int startTimeId, const QDateTime& scheduledAtUtc);

    /**
     * @brief Runs or queues a manual run of @p programId.
     * @return AlreadyQueued when the program is running or queued, Failed when it could not
     *         start, otherwise None.
     */
    RunRequest::Refusal enqueueManual(int programId);

    /** @brief Empties the queue, recording each scheduled entry with @p outcome. */
    void dropAll(FiredInstant::Outcome outcome);

    /** @brief Returns the waiting entries, head first. */
    QList<Entry> entries() const { return _entries; }

    /**
     * @brief Records every firing still marked queued as dropped_restart and logs how many.
     *
     * @warning Call once at startup, before the Scheduler's first tick: that tick inserts
     *          queued rows this would sweep.
     * @return False when the update failed.
     */
    bool recordRestartDrops();

private slots:
    void onProgramEnded(int programId);

private:
    bool start(const Entry& entry);
    void startNext();
    bool contains(int programId) const;
    bool isRainDelayed();
    void setOutcome(const Entry& entry, FiredInstant::Outcome outcome);

    ProgramRunner* _runner = nullptr;
    IrrigationDataSource* _source = nullptr;
    IClock* _clock = nullptr;
    QList<Entry> _entries;
};

#endif // PROGRAMQUEUE_H
```

- [ ] **Step 4: Create `IrrigationD/src/programqueue.cpp`**

```cpp
#include "programqueue.h"

#include "database/irrigationdatasource.h"
#include "programrunner.h"

ProgramQueue::ProgramQueue(ProgramRunner* runner, IrrigationDataSource* source, IClock* clock, QObject* parent) :
    QObject(parent),
    LoggingBaseClass("queue"),
    _runner(runner),
    _source(source),
    _clock(clock)
{
    connect(_runner, &ProgramRunner::programFinished, this, &ProgramQueue::onProgramEnded);
    connect(_runner, &ProgramRunner::programAborted, this, &ProgramQueue::onProgramEnded);
}

void ProgramQueue::enqueueScheduled(int programId, int startTimeId, const QDateTime& scheduledAtUtc)
{
    Entry entry;
    entry.programId = programId;
    entry.startTimeId = startTimeId;
    entry.scheduledAtUtc = scheduledAtUtc;
    entry.queuedAtUtc = _clock->nowUtc();

    if(contains(programId)) {
        logText(LVL_WARNING, QString("Program %1 came due while already queued").arg(programId));
        setOutcome(entry, FiredInstant::Outcome::SkippedDuplicate);
    }
    else if(_runner->isRunning() == false && _entries.isEmpty()) {
        start(entry);
    }
    else {
        logText(LVL_INFO, QString("Program %1 queued behind program %2")
                              .arg(programId).arg(_runner->runningProgramId()));
        _entries.append(entry);
    }
}

RunRequest::Refusal ProgramQueue::enqueueManual(int programId)
{
    if(_runner->runningProgramId() == programId || contains(programId)) {
        return RunRequest::Refusal::AlreadyQueued;
    }

    Entry entry;
    entry.programId = programId;
    entry.queuedAtUtc = _clock->nowUtc();

    if(_runner->isRunning() == false && _entries.isEmpty()) {
        return start(entry) == true ? RunRequest::Refusal::None : RunRequest::Refusal::Failed;
    }

    logText(LVL_INFO, QString("Manual run of program %1 queued behind program %2")
                          .arg(programId).arg(_runner->runningProgramId()));
    _entries.append(entry);
    return RunRequest::Refusal::None;
}

void ProgramQueue::dropAll(FiredInstant::Outcome outcome)
{
    const QList<Entry> dropped = _entries;
    _entries.clear();

    for(const Entry& entry : dropped) {
        if(entry.isScheduled()) {
            setOutcome(entry, outcome);
        }
    }

    if(dropped.isEmpty() == false) {
        logText(LVL_WARNING, QString("Dropped %1 queued programs as %2")
                                 .arg(dropped.count()).arg(FiredInstant::outcomeToString(outcome)));
    }
}

bool ProgramQueue::recordRestartDrops()
{
    int dropped = 0;
    if(_source->replaceFiringOutcomes(FiredInstant::Outcome::Queued, FiredInstant::Outcome::DroppedRestart, &dropped) == false) {
        logText(LVL_ERROR, "Failed to record the queued programs a restart dropped");
        return false;
    }

    if(dropped > 0) {
        logText(LVL_WARNING, QString("%1 queued programs were lost to a restart and recorded dropped_restart").arg(dropped));
    }
    return true;
}

void ProgramQueue::onProgramEnded(int programId)
{
    Q_UNUSED(programId)
    startNext();
}

bool ProgramQueue::start(const Entry& entry)
{
    const bool started = _runner->startProgram(entry.programId);
    if(started == false) {
        logText(LVL_ERROR, QString("Program %1 failed to start").arg(entry.programId));
    }

    if(entry.isScheduled()) {
        setOutcome(entry, started == true ? FiredInstant::Outcome::Ran : FiredInstant::Outcome::Failed);
    }

    return started;
}

void ProgramQueue::startNext()
{
    while(_runner->isRunning() == false && _entries.isEmpty() == false) {
        const Entry entry = _entries.takeFirst();

        if(_source->isMasterEnabled() == false) {
            logText(LVL_WARNING, QString("Dropping queued program %1: the master enable is off").arg(entry.programId));
            if(entry.isScheduled()) {
                setOutcome(entry, FiredInstant::Outcome::SkippedDisabled);
            }
        }
        else if(entry.isScheduled() && isRainDelayed()) {
            logText(LVL_WARNING, QString("Dropping queued program %1: a rain delay is in force").arg(entry.programId));
            setOutcome(entry, FiredInstant::Outcome::SkippedRain);
        }
        else {
            start(entry);
        }
    }
}

bool ProgramQueue::contains(int programId) const
{
    for(const Entry& entry : _entries) {
        if(entry.programId == programId) {
            return true;
        }
    }
    return false;
}

bool ProgramQueue::isRainDelayed()
{
    const QDateTime until = QDateTime::fromString(_source->settingValue("rain_delay_until"), Qt::ISODate);
    return until.isValid() && _clock->nowUtc() < until.toUTC();
}

void ProgramQueue::setOutcome(const Entry& entry, FiredInstant::Outcome outcome)
{
    if(_source->setFiringOutcome(entry.programId, entry.startTimeId, entry.scheduledAtUtc, outcome) == false) {
        logText(LVL_ERROR, QString("Failed to record program %1 at %2 as %3")
                               .arg(entry.programId)
                               .arg(entry.scheduledAtUtc.toUTC().toString(Qt::ISODate))
                               .arg(FiredInstant::outcomeToString(outcome)));
    }
}

#include "moc_programqueue.cpp"
```

- [ ] **Step 5: Make the scheduler record a due firing `queued`**

In `IrrigationD/src/scheduler.cpp`, `tick()`, replace:

```cpp
                if(recordOnce(program.id, startTime.id, scheduledUtc, FiredInstant::Outcome::Ran)) {
                    emit programDue(program.id, startTime.id, scheduledUtc);
                }
```

with:

```cpp
                if(recordOnce(program.id, startTime.id, scheduledUtc, FiredInstant::Outcome::Queued)) {
                    emit programDue(program.id, startTime.id, scheduledUtc);
                }
```

In `IrrigationD/src/scheduler.h` replace the signal's comment:

```cpp
    /** @brief Emitted once a scheduled start time has come due and been recorded queued. Whoever handles it writes the final outcome. */
    void programDue(int programId, int startTimeId, const QDateTime& scheduledAtUtc);
```

- [ ] **Step 6: Wire the queue into `IrrigationDaemon`**

`IrrigationD/src/irrigationdaemon.h`: add `class ProgramQueue;` to the forward declarations and the member `ProgramQueue* _programQueue = nullptr;` after `_programRunner`.

`IrrigationD/src/irrigationdaemon.cpp`: add `#include "programqueue.h"`.

In `threadStarted()`, after `_programRunner = new ProgramRunner(_zoneController, _dataSource);`:

```cpp
        _programQueue = new ProgramQueue(_programRunner, _dataSource, &_clock);
        _programQueue->recordRestartDrops();
```

`_scheduler->start()` already runs later in `threadStarted()`, which satisfies `recordRestartDrops()`'s ordering.

In `threadAboutToFinish()`, replace the runner abort block:

```cpp
    // The queue goes before the runner aborts: an aborted program starts the next queued
    // one, and nothing may open a valve during teardown.
    delete _programQueue;
    _programQueue = nullptr;

    if(_programRunner != nullptr) {
        _programRunner->abort();
    }
```

Replace `onStopPressed()`:

```cpp
void IrrigationDaemon::onStopPressed()
{
    logText(LVL_WARNING, "Stop requested");

    // dropAll() precedes abort(): an aborted program starts the next queued one.
    _programQueue->dropAll(FiredInstant::Outcome::DroppedStop);
    _programRunner->abort();

    if(_zoneController->allOff() == false) {
        logText(LVL_ERROR, QString("Failed to close the zones on a stop request: %1")
                               .arg(_zoneController->errorText()));
    }

    publishStatus();
}
```

Replace `onProgramDue()`:

```cpp
void IrrigationDaemon::onProgramDue(int programId, int startTimeId, const QDateTime& scheduledAtUtc)
{
    if(_stopButton->isHeld()) {
        logText(LVL_WARNING, QString("Program %1 came due while the stop button is held").arg(programId));
        if(_dataSource->setFiringOutcome(programId, startTimeId, scheduledAtUtc, FiredInstant::Outcome::SkippedStop) == false) {
            logText(LVL_ERROR, QString("Failed to record program %1 as skipped").arg(programId));
        }
    }
    else {
        _programQueue->enqueueScheduled(programId, startTimeId, scheduledAtUtc);
    }

    publishStatus();
}
```

Replace the `else` branch of `onProgramRunRequested()` (Task 6 replaces the whole function):

```cpp
    else {
        const RunRequest::Refusal refusal = _programQueue->enqueueManual(programId);
        if(refusal != RunRequest::Refusal::None) {
            logText(LVL_WARNING, QString("Refused a manual run of program %1: %2")
                                     .arg(programId).arg(RunRequest::refusalToString(refusal)));
        }

        publishStatus();
    }
```

- [ ] **Step 7: Add the sources to `IrrigationD/CMakeLists.txt`**

After `src/programrunner.h       src/programrunner.cpp`:

```cmake
    src/programqueue.h        src/programqueue.cpp
    src/runrequest.h          src/runrequest.cpp
```

- [ ] **Step 8: Build**

```bash
cmake --build build -j 32
```

Expected: exits zero. `tst_scheduler` goes red wherever it expects `ran` from a due firing; Task 13 flips those to `queued`.

- [ ] **Step 9: Commit**

```bash
git add IrrigationD/src/runrequest.h IrrigationD/src/runrequest.cpp \
        IrrigationD/src/programqueue.h IrrigationD/src/programqueue.cpp \
        IrrigationD/src/scheduler.h IrrigationD/src/scheduler.cpp \
        IrrigationD/src/irrigationdaemon.h IrrigationD/src/irrigationdaemon.cpp \
        IrrigationD/CMakeLists.txt
git commit -F - <<'EOF'
feat: queue programs that come due while another runs

A program that comes due while another is running is queued and runs
in full when its turn comes, first in first out, one entry per
program. The scheduler records a due firing queued and ProgramQueue
writes its final outcome: ran or failed when it starts, skipped_rain
or skipped_disabled when the rain delay or the master enable stops it
at the head of the queue, skipped_duplicate when the program already
waits. STOP empties the queue as dropped_stop, and startup records any
firing still queued as dropped_restart. A manual program run joins the
same queue and is refused while that program runs or waits.

skipped_busy is no longer written.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---

### Task 5: The new `/admin/status` shape

Spec §6. `runningZone` and `secondsRemaining` go; `running[]`, `program`, `queue[]` and `maxConcurrentZones` arrive. The daemon builds the snapshot from the controller, the runner and the queue; the control server serialises it.

`tst_controlserver` stops compiling here (it sets `ServerStatus::runningZone`) and the next task removes `ProgramZone` from under it as well, so it is **parked** until Task 14.

**Files:**
- Modify: `IrrigationD/src/irrigationcontrolserver.h` (status structs only)
- Modify: `IrrigationD/src/json/statusjson.cpp`
- Modify: `IrrigationD/src/irrigationdaemon.cpp` (`publishStatus()`)
- Modify: `IrrigationD/tests/CMakeLists.txt` (park `tst_controlserver`)

**Interfaces:**
- Consumes: Task 1 `ZoneController::{openZoneNumbers, secondsRemaining(int), maxConcurrentZones}`; Task 3 `ProgramRunner::{isRunning, runningProgramId, runningProgramName, stepNumber, stepCount, waitingZones, ownsZone}`; Task 4 `ProgramQueue::entries()`.
- Produces:
  - `struct RunningZoneStatus { int zone = 0; int secondsRemaining = 0; bool fromProgram = false; };`
  - `struct QueuedProgramStatus { int programId = 0; QString name; QDateTime queuedAtUtc; };`
  - `struct ServerStatus { QList<RunningZoneStatus> running; int programId = 0; QString programName; int programStep = 0; int programStepCount = 0; QList<int> waitingZones; QList<QueuedProgramStatus> queue; int maxConcurrentZones = 0; QDateTime nextRunUtc; QString timezone; bool masterEnabled = false; bool stopHeld = false; QDateTime rainDelayUntilUtc; };` — `programId == 0` means no program
  - JSON exactly as in **Wire contract** above

- [ ] **Step 1: Replace the status struct in `IrrigationD/src/irrigationcontrolserver.h`**

Replace the `ServerStatus` struct (keep `Q_DECLARE_METATYPE(ServerStatus)` after it):

```cpp
/** @brief One open zone in a status snapshot. zone is the zone number. */
struct RunningZoneStatus
{
    int zone = 0;
    int secondsRemaining = 0;
    bool fromProgram = false;
};

/** @brief One waiting program in a status snapshot. */
struct QueuedProgramStatus
{
    int programId = 0;
    QString name;
    QDateTime queuedAtUtc;
};

/**
 * @brief Snapshot of the daemon's runtime state, published through updateStatus() and served by GET /admin/status.
 *
 * programId is zero when no program runs. running is ordered by zone number. waitingZones
 * holds zone numbers.
 */
struct ServerStatus
{
    QList<RunningZoneStatus> running;
    int programId = 0;
    QString programName;
    int programStep = 0;
    int programStepCount = 0;
    QList<int> waitingZones;
    QList<QueuedProgramStatus> queue;
    int maxConcurrentZones = 0;
    QDateTime nextRunUtc;
    QString timezone;
    bool masterEnabled = false;
    bool stopHeld = false;
    QDateTime rainDelayUntilUtc;
};
```

Add `#include <QList>` to the header's Qt includes.

- [ ] **Step 2: Replace `StatusJson::toJson()` in `IrrigationD/src/json/statusjson.cpp`**

Add `#include <QJsonArray>` and replace the function:

```cpp
QJsonObject StatusJson::toJson(const ServerStatus& status)
{
    QJsonArray running;
    for(const RunningZoneStatus& zone : status.running) {
        QJsonObject entry;
        entry["zone"] = zone.zone;
        entry["secondsRemaining"] = zone.secondsRemaining;
        entry["source"] = zone.fromProgram == true ? "program" : "manual";
        running.append(entry);
    }

    QJsonValue program = QJsonValue(QJsonValue::Null);
    if(status.programId != 0) {
        QJsonArray waitingZones;
        for(int zoneNumber : status.waitingZones) {
            waitingZones.append(zoneNumber);
        }

        QJsonObject entry;
        entry["id"] = status.programId;
        entry["name"] = status.programName;
        entry["step"] = status.programStep;
        entry["stepCount"] = status.programStepCount;
        entry["waitingZones"] = waitingZones;
        program = entry;
    }

    QJsonArray queue;
    for(const QueuedProgramStatus& waiting : status.queue) {
        QJsonObject entry;
        entry["programId"] = waiting.programId;
        entry["name"] = waiting.name;
        entry["queuedAtUtc"] = instantToJson(waiting.queuedAtUtc);
        queue.append(entry);
    }

    QJsonObject object;
    object["running"] = running;
    object["program"] = program;
    object["queue"] = queue;
    object["maxConcurrentZones"] = status.maxConcurrentZones;
    object["nextRunUtc"] = instantToJson(status.nextRunUtc);
    object["timezone"] = status.timezone;
    object["masterEnabled"] = status.masterEnabled;
    object["stopHeld"] = status.stopHeld;
    object["rainDelayUntilUtc"] = instantToJson(status.rainDelayUntilUtc);
    return object;
}
```

- [ ] **Step 3: Replace `IrrigationDaemon::publishStatus()`**

```cpp
void IrrigationDaemon::publishStatus()
{
    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();

    ServerStatus status;

    const QList<int> openZones = _zoneController->openZoneNumbers();
    for(int zoneNumber : openZones) {
        RunningZoneStatus running;
        running.zone = zoneNumber;
        running.secondsRemaining = _zoneController->secondsRemaining(zoneNumber);
        running.fromProgram = _programRunner->ownsZone(zoneNumber);
        status.running.append(running);
    }

    if(_programRunner->isRunning()) {
        status.programId = _programRunner->runningProgramId();
        status.programName = _programRunner->runningProgramName();
        status.programStep = _programRunner->stepNumber();
        status.programStepCount = _programRunner->stepCount();
        status.waitingZones = _programRunner->waitingZones();
    }

    const QList<ProgramQueue::Entry> waiting = _programQueue->entries();
    if(waiting.isEmpty() == false) {
        const ProgramList programs = _dataSource->allPrograms();
        for(const ProgramQueue::Entry& entry : waiting) {
            QueuedProgramStatus queued;
            queued.programId = entry.programId;
            queued.queuedAtUtc = entry.queuedAtUtc;
            for(const Program& program : programs) {
                if(program.id == entry.programId) {
                    queued.name = program.name;
                    break;
                }
            }
            status.queue.append(queued);
        }
    }

    status.maxConcurrentZones = _zoneController->maxConcurrentZones();
    status.timezone = QString::fromUtf8(QTimeZone::systemTimeZoneId());
    status.masterEnabled = _dataSource->isMasterEnabled();
    status.stopHeld = _stopButton->isHeld();
    status.rainDelayUntilUtc =
        QDateTime::fromString(_dataSource->settingValue("rain_delay_until"), Qt::ISODate).toUTC();

    if(status.masterEnabled == true) {
        const bool rainDelayed = status.rainDelayUntilUtc.isValid() && nowUtc < status.rainDelayUntilUtc;
        status.nextRunUtc = nextScheduledRunUtc(rainDelayed == true ? status.rainDelayUntilUtc : nowUtc);
    }

    _controlServer->updateStatus(status);
}
```

- [ ] **Step 4: Park `tst_controlserver`**

In `IrrigationD/tests/CMakeLists.txt` delete the whole `irrigation_add_test(tst_controlserver ... )` block. Task 14 restores it with its final source list. Leave `tst_controlserver.cpp` untouched.

- [ ] **Step 5: Build**

```bash
cmake --build build -j 32
```

Expected: exits zero, and `ctest --test-dir build -N` no longer lists `tst_controlserver`.

- [ ] **Step 6: Commit**

```bash
git add IrrigationD/src/irrigationcontrolserver.h IrrigationD/src/json/statusjson.cpp \
        IrrigationD/src/irrigationdaemon.cpp IrrigationD/tests/CMakeLists.txt
git commit -F - <<'EOF'
feat: report every open zone, the running step and the queue in status

/admin/status replaces runningZone and secondsRemaining with a
running list (zone number, seconds remaining, program or manual), the
running program's step, step count and waiting zones, the program
queue, and the zone cap.

tst_controlserver is parked until its rewrite.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---

### Task 6: Run decisions over HTTP, per-zone stop, steps in program bodies

Spec §3.1, §3.2 and §6. The run routes hand a `RunRequest` to the valve thread and wait for its decision, answering 202, 409 with a reason, 500, or 503 after a bounded wait. A new route closes one zone. Program bodies carry `steps`. The settings allowlist gains `max_concurrent_zones`. The daemon makes every decision on the valve thread: a manual zone run opens alongside whatever runs or is refused at the cap, and no longer aborts the program. `ProgramZone` leaves the tree.

**Files:**
- Modify: `IrrigationD/src/irrigationcontrolserver.h`, `IrrigationD/src/irrigationcontrolserver.cpp`
- Modify: `IrrigationD/src/json/programjson.h`, `IrrigationD/src/json/programjson.cpp`
- Modify: `IrrigationD/src/irrigationdaemon.h`, `IrrigationD/src/irrigationdaemon.cpp`
- Modify: `IrrigationD/src/database/irrigationdatasource.h`, `IrrigationD/src/database/irrigationdatasource.cpp`
- Delete: `IrrigationD/src/model/programzone.h`, `IrrigationD/src/model/programzone.cpp`
- Modify: `IrrigationD/CMakeLists.txt`, `IrrigationD/tests/CMakeLists.txt`
- Modify: `IrrigationD/tests/tst_repository.cpp`, `IrrigationD/tests/tst_programrunner.cpp` (compile-keeping edits)

**Interfaces:**
- Consumes: Task 1 `ZoneController::{hasSlotFor, openZone, closeZone, openZoneNumbers, errorText, MaxConcurrentZonesCeiling}`; Task 2 step repository methods; Task 3 `ProgramRunner::fillSlots()`; Task 4 `RunRequest`, `RunRequestPtr`, `ProgramQueue::enqueueManual`.
- Produces on `IrrigationControlServer`:
  - signals `void manualZoneRunRequested(int zoneNumber, int seconds, const RunRequestPtr& decision);`, `void programRunRequested(int programId, const RunRequestPtr& decision);`, `void zoneStopRequested(int zoneNumber);` (`stopRequested()`, `settingsChanged()` unchanged)
  - `void setDecisionTimeout(const TimeSpan& value);` — call before `start()`; default 5 s
  - route `POST /admin/zones/<number>/stop`
- Produces on `ProgramJson`: `toJson(const Program&, const ProgramStartTimeList&, const ProgramStepList&, const QDateTime&)`, `fromJson(const QJsonObject&, Program&, ProgramStartTimeList&, ProgramStepList&, QString&)`
- Produces on `IrrigationDaemon`: slots `onManualZoneRunRequested(int, int, const RunRequestPtr&)`, `onProgramRunRequested(int, const RunRequestPtr&)`, `onZoneStopRequested(int)`
- Removed: `ProgramZone`, `ProgramZoneList`, `IrrigationDataSource::{zonesFor, insertProgramZone, deleteProgramZones}`

- [ ] **Step 1: Switch `ProgramJson` to steps**

`IrrigationD/src/json/programjson.h`: replace `#include "model/programzone.h"` with `#include "model/programstep.h"`, and replace the public and private declarations that mention zones:

```cpp
    /** @brief Serializes @p program, its start times, steps and computed next run into a full program object. Each step carries its stored id. */
    static QJsonObject toJson(const Program& program,
                              const ProgramStartTimeList& startTimes,
                              const ProgramStepList& steps,
                              const QDateTime& nextRunUtc);

    /**
     * @brief Parses a program draft payload into its program, start-time and step parts.
     *
     * Rejects a dayMode that is not one of the four registered names, a
     * minutesAfterMidnight outside 0..1439, an EveryNDays rule without an
     * intervalDays of at least 1 and a valid anchorDate, a DaysOfWeek dowMask that
     * is zero or sets a bit outside the seven days, an enabled that is present
     * but not a boolean, and a step with no zones, a zone listed twice, a non-numeric
     * zone id, or a durationSeconds below 1. An absent enabled means true. Each step's
     * sequence is its array position plus one.
     * @param errorMessage Set to a human-readable reason when parsing fails.
     * @return True when @p object matched the contract and every value validated.
     */
    static bool fromJson(const QJsonObject& object,
                         Program& program,
                         ProgramStartTimeList& startTimes,
                         ProgramStepList& steps,
                         QString& errorMessage);

private:
    static QJsonObject toJson(const ProgramStartTime& startTime);
    static QJsonObject toJson(const ProgramStep& step);

    static bool startTimeFromJson(const QJsonObject& object, ProgramStartTime& startTime, QString& errorMessage);
    static bool stepFromJson(const QJsonObject& object, ProgramStep& step, QString& errorMessage);
```

`IrrigationD/src/json/programjson.cpp`:

In `toJson(const Program&, …)` change the parameter to `const ProgramStepList& steps` and replace the zones array and key:

```cpp
    QJsonArray stepsArray;
    for(const ProgramStep& step : steps) {
        stepsArray.append(toJson(step));
    }
```

```cpp
    object["steps"] = stepsArray;
```

Replace `toJson(const ProgramZone&)` with:

```cpp
QJsonObject ProgramJson::toJson(const ProgramStep& step)
{
    QJsonArray zones;
    for(int zoneId : step.zoneIds) {
        zones.append(zoneId);
    }

    QJsonObject object;
    object["id"] = step.id;
    object["zones"] = zones;
    object["durationSeconds"] = step.durationSeconds;
    return object;
}
```

In `fromJson()` change the parameter to `ProgramStepList& steps`, replace the `zones must be an array` check with:

```cpp
    if(object.value("steps").isArray() == false) {
        errorMessage = "steps must be an array";
        return false;
    }
```

and replace the zone parsing loop and its assignment with:

```cpp
    ProgramStepList parsedSteps;
    const QJsonArray stepsArray = object.value("steps").toArray();
    for(const QJsonValue& value : stepsArray) {
        if(value.isObject() == false) {
            errorMessage = "each step must be an object";
            return false;
        }

        ProgramStep step;
        if(stepFromJson(value.toObject(), step, errorMessage) == false) {
            return false;
        }
        step.sequence = static_cast<int>(parsedSteps.count()) + 1;
        parsedSteps.append(step);
    }

    program = parsedProgram;
    startTimes = parsedStartTimes;
    steps = parsedSteps;
    return true;
```

Replace `zoneFromJson()` with:

```cpp
bool ProgramJson::stepFromJson(const QJsonObject& object, ProgramStep& step, QString& errorMessage)
{
    if(object.value("zones").isArray() == false) {
        errorMessage = "steps require a zones array";
        return false;
    }

    const QJsonArray zones = object.value("zones").toArray();
    if(zones.isEmpty()) {
        errorMessage = "each step needs at least one zone";
        return false;
    }

    QList<int> zoneIds;
    for(const QJsonValue& zone : zones) {
        if(zone.isDouble() == false) {
            errorMessage = "step zones must be numeric zone ids";
            return false;
        }
        const int zoneId = zone.toInt();
        if(zoneIds.contains(zoneId)) {
            errorMessage = QString("a step lists zone id %1 twice").arg(zoneId);
            return false;
        }
        zoneIds.append(zoneId);
    }

    if(object.value("durationSeconds").isDouble() == false) {
        errorMessage = "steps require a numeric durationSeconds";
        return false;
    }

    const int durationSeconds = object.value("durationSeconds").toInt();
    if(durationSeconds < 1) {
        errorMessage = "durationSeconds must be positive";
        return false;
    }

    step.zoneIds = zoneIds;
    step.durationSeconds = durationSeconds;
    return true;
}
```

- [ ] **Step 2: Update the control server header**

In `IrrigationD/src/irrigationcontrolserver.h`:

Replace `#include "model/programzone.h"` with `#include "model/programstep.h"` and add `#include "runrequest.h"`.

Add after `setListenPort()`:

```cpp
    /** @brief Sets how long a run route waits for the valve thread's decision before answering 503. Call before start(). */
    void setDecisionTimeout(const TimeSpan& value) { _decisionTimeout = value; }
```

Replace the three request signals:

```cpp
    /**
     * @brief Emitted when POST /admin/zones/{number}/run passes validation.
     *
     * The route blocks until a slot calls @p decision's complete(), or until the decision timeout.
     */
    void manualZoneRunRequested(int zoneNumber, int seconds, const RunRequestPtr& decision);

    /**
     * @brief Emitted when POST /admin/programs/{id}/run names a known program.
     *
     * The route blocks until a slot calls @p decision's complete(), or until the decision timeout.
     */
    void programRunRequested(int programId, const RunRequestPtr& decision);

    /** @brief Emitted when POST /admin/zones/{number}/stop names a known zone. */
    void zoneStopRequested(int zoneNumber);
```

Add `QHttpServerResponse handleZoneStop(int zoneNumber, const QHttpServerRequest& request);` after `handleZoneRun`, and `QHttpServerResponse decisionResponse(const RunRequestPtr& decision);` after `handleSettingsPut`.

Replace the `zoneIdsAreKnown` declaration:

```cpp
    /** @brief Returns whether every zone id in every step of @p steps names a row present in @p knownZones. */
    static bool zoneIdsAreKnown(const ProgramStepList& steps, const ZoneList& knownZones);
```

Add `static const TimeSpan DefaultDecisionTimeout;` next to `SettingsKeys`, and the member `TimeSpan _decisionTimeout;` after `_listenPort`.

- [ ] **Step 3: Update the control server implementation**

In `IrrigationD/src/irrigationcontrolserver.cpp`:

Add `#include "zonecontroller.h"` after `#include "scheduler.h"`.

Replace `SettingsKeys` and add the timeout:

```cpp
const QStringList IrrigationControlServer::SettingsKeys = {
    "rain_delay_until", "master_enabled", "max_zone_seconds", "log_level", "max_concurrent_zones"
};

// Bounded: the daemon stops this server from its own thread during teardown, and a
// route still waiting on that thread for a decision would block the stop() forever.
const TimeSpan IrrigationControlServer::DefaultDecisionTimeout = TimeSpan::fromSeconds(5);
```

Constructor — initialise `_decisionTimeout(DefaultDecisionTimeout)` after `_listenPort(8080)` and register the metatype next to `ServerStatus`:

```cpp
    qRegisterMetaType<ServerStatus>();
    qRegisterMetaType<RunRequestPtr>();
```

In `threadStarted()` add the stop route after the run route:

```cpp
    _httpServer->route("/admin/zones/<arg>/stop", QHttpServerRequest::Method::Post,
                       [this](int zoneNumber, const QHttpServerRequest& request)
    {
        return this->handleZoneStop(zoneNumber, request);
    });
```

In `handleZoneRun()` replace the disabled-zone reply and the emit/202 tail:

```cpp
    if(enabled == false) {
        return QHttpServerResponse(QJsonObject{{"error", QString("zone %1 is disabled").arg(zoneNumber)},
                                               {"reason", RunRequest::refusalToString(RunRequest::Refusal::ZoneDisabled)}},
                                   QHttpServerResponder::StatusCode::Conflict);
    }

    const RunRequestPtr decision(new RunRequest);
    emit manualZoneRunRequested(zoneNumber, seconds, decision);
    return decisionResponse(decision);
}
```

Add after `handleZoneRun()`:

```cpp
QHttpServerResponse IrrigationControlServer::handleZoneStop(int zoneNumber, const QHttpServerRequest& request)
{
    Q_UNUSED(request)
    const ZoneList zones = _source->allZones();
    bool known = false;
    for(const Zone& zone : zones) {
        if(zone.number == zoneNumber) {
            known = true;
            break;
        }
    }

    if(known == false) {
        return QHttpServerResponse(QJsonObject{{"error", "unknown zone"}},
                                   QHttpServerResponder::StatusCode::NotFound);
    }

    emit zoneStopRequested(zoneNumber);
    return QHttpServerResponse(QJsonObject{{"accepted", true}},
                               QHttpServerResponder::StatusCode::Accepted);
}
```

In `handleProgramsGet()` replace the zones line and argument:

```cpp
        const ProgramStepList steps = _source->stepsFor(program.id);
        const QDateTime nextRunUtc = Scheduler::nextRunUtc(program, startTimes, nowUtc);
        array.append(ProgramJson::toJson(program, startTimes, steps, nextRunUtc));
```

Replace `zoneIdsAreKnown()`:

```cpp
bool IrrigationControlServer::zoneIdsAreKnown(const ProgramStepList& steps, const ZoneList& knownZones)
{
    for(const ProgramStep& step : steps) {
        for(int zoneId : step.zoneIds) {
            bool found = false;
            for(const Zone& candidate : knownZones) {
                if(candidate.id == zoneId) {
                    found = true;
                    break;
                }
            }
            if(found == false) {
                return false;
            }
        }
    }
    return true;
}
```

In `handleProgramPost()`: rename `ProgramZoneList zones;` to `ProgramStepList steps;`, pass `steps` to `fromJson`, `zoneIdsAreKnown` and `toJson`, change the unknown-id message to `"unknown zoneId in steps"`, and replace the zone insert loop:

```cpp
    for(ProgramStep& step : steps) {
        if(ok == false) {
            break;
        }
        step.programId = program.id;
        ok = _source->insertProgramStep(step);
    }
```

In `handleProgramPut()`: the same renames and message, and replace the delete/insert block:

```cpp
    if(ok) {
        ok = _source->deleteProgramSteps(programId);
    }

    for(ProgramStep& step : steps) {
        if(ok == false) {
            break;
        }
        step.programId = programId;
        ok = _source->insertProgramStep(step);
    }
```

In `handleProgramRun()` replace the emit/202 tail:

```cpp
    const RunRequestPtr decision(new RunRequest);
    emit programRunRequested(programId, decision);
    return decisionResponse(decision);
}
```

In `isValidSettingValue()` add before the final `return false;`:

```cpp
    if(key == "max_concurrent_zones") {
        bool ok = false;
        const int zones = value.toInt(&ok);
        return ok && zones >= 1 && zones <= ZoneController::MaxConcurrentZonesCeiling;
    }
```

Add after `handleSettingsPut()`:

```cpp
QHttpServerResponse IrrigationControlServer::decisionResponse(const RunRequestPtr& decision)
{
    if(decision->wait(_decisionTimeout) == false) {
        logText(LVL_ERROR, "A run request went unanswered by the valve thread");
        return QHttpServerResponse(QJsonObject{{"error", "the controller did not answer"}, {"reason", "timeout"}},
                                   QHttpServerResponder::StatusCode::ServiceUnavailable);
    }

    const RunRequest::Refusal refusal = decision->refusal();
    if(refusal == RunRequest::Refusal::None) {
        return QHttpServerResponse(QJsonObject{{"accepted", true}},
                                   QHttpServerResponder::StatusCode::Accepted);
    }

    const QJsonObject body{{"error", decision->message()}, {"reason", RunRequest::refusalToString(refusal)}};
    if(refusal == RunRequest::Refusal::Failed) {
        return QHttpServerResponse(body, QHttpServerResponder::StatusCode::InternalServerError);
    }
    return QHttpServerResponse(body, QHttpServerResponder::StatusCode::Conflict);
}
```

Update the class comment's second paragraph to: `No route touches GPIO directly -- state-changing routes emit a request signal and let the daemon's owner of ZoneController and ProgramRunner act on it. Run routes wait for that owner's decision.`

- [ ] **Step 4: Make the daemon decide**

`IrrigationD/src/irrigationdaemon.h`: add `#include "runrequest.h"` and replace the two request slots, adding the stop slot:

```cpp
    void onManualZoneRunRequested(int zoneNumber, int seconds, const RunRequestPtr& decision);
    void onProgramRunRequested(int programId, const RunRequestPtr& decision);
    void onZoneStopRequested(int zoneNumber);
```

`IrrigationD/src/irrigationdaemon.cpp`, `connectComponents()` — add:

```cpp
    connect(_controlServer, &IrrigationControlServer::zoneStopRequested,
            this, &IrrigationDaemon::onZoneStopRequested);
```

Replace `onManualZoneRunRequested()` and `onProgramRunRequested()`, and add `onZoneStopRequested()`:

```cpp
void IrrigationDaemon::onManualZoneRunRequested(int zoneNumber, int seconds, const RunRequestPtr& decision)
{
    RunRequest::Refusal refusal = RunRequest::Refusal::None;
    QString message;

    if(_stopButton->isHeld()) {
        logText(LVL_WARNING, QString("Refused a manual run of zone %1: the stop button is held").arg(zoneNumber));
        refusal = RunRequest::Refusal::StopHeld;
        message = "the stop button is held";
    }
    else if(_dataSource->isMasterEnabled() == false) {
        logText(LVL_WARNING, QString("Refused a manual run of zone %1: the master enable is off").arg(zoneNumber));
        refusal = RunRequest::Refusal::MasterDisabled;
        message = "watering is turned off";
    }
    else if(isZoneEnabled(zoneNumber) == false) {
        logText(LVL_WARNING, QString("Refused a manual run of zone %1: the zone is disabled or has no database row").arg(zoneNumber));
        refusal = RunRequest::Refusal::ZoneDisabled;
        message = QString("zone %1 is disabled").arg(zoneNumber);
    }
    else if(_zoneController->hasSlotFor(zoneNumber) == false) {
        const int open = static_cast<int>(_zoneController->openZoneNumbers().count());
        logText(LVL_WARNING, QString("Refused a manual run of zone %1: %2 zones already running").arg(zoneNumber).arg(open));
        refusal = RunRequest::Refusal::CapReached;
        message = QString("%1 zones already running").arg(open);
    }
    else if(_zoneController->openZone(zoneNumber, seconds) == false) {
        logText(LVL_ERROR, QString("Failed to open zone %1 for %2 seconds: %3")
                               .arg(zoneNumber).arg(seconds).arg(_zoneController->errorText()));
        refusal = RunRequest::Refusal::Failed;
        message = _zoneController->errorText();
    }

    // publishStatus() precedes complete(): the snapshot then reaches the server thread
    // ahead of the reply, so the poll a client fires on the reply already sees the change.
    publishStatus();
    decision->complete(refusal, message);
}

void IrrigationDaemon::onProgramRunRequested(int programId, const RunRequestPtr& decision)
{
    RunRequest::Refusal refusal = RunRequest::Refusal::None;
    QString message;

    if(_stopButton->isHeld()) {
        logText(LVL_WARNING, QString("Refused a manual run of program %1: the stop button is held").arg(programId));
        refusal = RunRequest::Refusal::StopHeld;
        message = "the stop button is held";
    }
    else if(_dataSource->isMasterEnabled() == false) {
        logText(LVL_WARNING, QString("Refused a manual run of program %1: the master enable is off").arg(programId));
        refusal = RunRequest::Refusal::MasterDisabled;
        message = "watering is turned off";
    }
    else {
        refusal = _programQueue->enqueueManual(programId);
        if(refusal == RunRequest::Refusal::AlreadyQueued) {
            logText(LVL_WARNING, QString("Refused a manual run of program %1: it is already running or queued").arg(programId));
            message = "that program is already running or queued";
        }
        else if(refusal == RunRequest::Refusal::Failed) {
            message = QString("program %1 failed to start: %2").arg(programId).arg(_zoneController->errorText());
        }
    }

    // publishStatus() precedes complete(): the snapshot then reaches the server thread
    // ahead of the reply, so the poll a client fires on the reply already sees the change.
    publishStatus();
    decision->complete(refusal, message);
}

void IrrigationDaemon::onZoneStopRequested(int zoneNumber)
{
    logText(LVL_INFO, QString("Stop requested for zone %1").arg(zoneNumber));
    if(_zoneController->closeZone(zoneNumber) == false) {
        logText(LVL_ERROR, QString("Failed to close zone %1: %2").arg(zoneNumber).arg(_zoneController->errorText()));
    }

    publishStatus();
}
```

Replace `onSettingsChanged()`:

```cpp
void IrrigationDaemon::onSettingsChanged()
{
    applyRuntimeSettings();
    _programRunner->fillSlots();
    publishStatus();
}
```

- [ ] **Step 5: Remove `ProgramZone`**

```bash
cd /home/spunak/src/punak/irrigation
git rm IrrigationD/src/model/programzone.h IrrigationD/src/model/programzone.cpp
grep -rn "programzone\|ProgramZone\|zonesFor\|insertProgramZone\|deleteProgramZones" IrrigationD/src
```

In `IrrigationD/src/database/irrigationdatasource.h` delete `#include "model/programzone.h"` and the declarations of `zonesFor`, `insertProgramZone` and `deleteProgramZones`; in `irrigationdatasource.cpp` delete their definitions. In `IrrigationD/CMakeLists.txt` delete the line `src/model/programzone.h       src/model/programzone.cpp`. In `IrrigationD/tests/CMakeLists.txt` delete every `../src/model/programzone.cpp` line. Re-run the `grep`: expected no output.

- [ ] **Step 6: Keep `tst_repository` and `tst_programrunner` compiling**

`IrrigationD/tests/tst_repository.cpp`: delete the slot declarations and the definitions of `programZonesRoundTripInSequenceOrder` and `deleteProgramZonesLeavesOtherProgramsIntact`. Their subject is gone; Task 13 adds the step equivalents.

`IrrigationD/tests/tst_programrunner.cpp`: in `buildProgram(IrrigationDataSource&, const QList<ZoneStep>&)` replace

```cpp
        ProgramZone entry;
        entry.programId = program.id;
        entry.zoneId = zoneId;
        entry.sequence = step.sequence;
        entry.durationSeconds = step.durationSeconds;
        if(source.insertProgramZone(entry) == false) {
            return 0;
        }
```

with

```cpp
        ProgramStep entry;
        entry.programId = program.id;
        entry.zoneIds = { zoneId };
        entry.sequence = step.sequence;
        entry.durationSeconds = step.durationSeconds;
        if(source.insertProgramStep(entry) == false) {
            return 0;
        }
```

- [ ] **Step 7: Build**

```bash
cmake --build build -j 32
ctest --test-dir build --output-on-failure -R 'tst_programrunner|tst_repository|tst_irrigationdatasource'
```

Expected: the build exits zero. `tst_programrunner` builds one-zone-step programs again, so its plain sequencing tests (`walksZonesInSequenceOrder`, `finishesAfterTheLastZone`, `disabledZoneIsSkippedByARunningProgram`) pass; tests that encode mutual exclusion or the old watchdog semantics (`manuallyOpenedZoneDoesNotCascadeWhenDisplaced`, `watchdogTripOnTheFinalZoneEmitsProgramFinished`) stay red, and Task 12 replaces the file. `tst_irrigationdatasource::createsSchemaOnFirstOpen` stays red until Task 13.

- [ ] **Step 8: Commit**

```bash
git add IrrigationD/src/irrigationcontrolserver.h IrrigationD/src/irrigationcontrolserver.cpp \
        IrrigationD/src/json/programjson.h IrrigationD/src/json/programjson.cpp \
        IrrigationD/src/irrigationdaemon.h IrrigationD/src/irrigationdaemon.cpp \
        IrrigationD/src/database/irrigationdatasource.h IrrigationD/src/database/irrigationdatasource.cpp \
        IrrigationD/CMakeLists.txt IrrigationD/tests/CMakeLists.txt \
        IrrigationD/tests/tst_repository.cpp IrrigationD/tests/tst_programrunner.cpp
git commit -F - <<'EOF'
feat: answer run requests with the valve thread's decision

POST /admin/zones/{n}/run and POST /admin/programs/{id}/run now wait
for the daemon's decision on the valve thread and answer 202, 409 with
a reason (cap_reached, stop_held, master_disabled, zone_disabled,
already_queued), 500 when the open failed, or 503 when no decision
arrives within five seconds. POST /admin/zones/{n}/stop closes one
zone. A manual zone run opens alongside whatever runs and no longer
aborts the running program. Program bodies carry steps of zone ids
with one duration each, and max_concurrent_zones joins the settings
allowlist at 1-8. Raising the cap opens waiting step zones at once.

ProgramZone and its repository methods are removed.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---

### Task 7: Web — the status shape and the Now screen

Spec §6 and §7 (Now, zone tiles, polling). The client decodes the new `/admin/status`, carries a 409's `reason`, and gains `stopZone()`. The Now screen lists every open zone with its countdown, a program/manual tag and its own Stop button, shows the running program's step line and the queue line, glows every open tile, and disables Run on the other tiles at the cap with an "N of N running" hint. A refused run shows the daemon's sentence from the 409.

`decode.test.ts` reads the old status fields and is **parked** until Task 15. `useStatus.test.ts` takes a two-line compile edit.

**Files:**
- Modify: `web/src/api/types.ts`, `web/src/api/decode.ts`, `web/src/api/client.ts`
- Modify: `web/src/hooks/useStatus.ts`
- Modify: `web/src/screens/NowScreen.tsx`
- Modify: `web/src/styles/app.css`
- Modify: `web/src/test/fixtures.ts`, `web/src/hooks/useStatus.test.ts` (compile edit)
- Modify: `web/tsconfig.json`, `web/vitest.config.ts` (park)

**Interfaces:**
- Consumes: the **Wire contract** status JSON and 409 body.
- Produces:
  - `type RunSource = 'program' | 'manual'`
  - `interface RunningZone { zone: number; secondsRemaining: number; source: RunSource }` — `zone` is a zone number
  - `interface RunningProgram { id: number; name: string; step: number; stepCount: number; waitingZones: number[] }` — zone numbers
  - `interface QueuedProgram { programId: number; name: string; queuedAtUtc: string | null }`
  - `interface Status { running: RunningZone[]; program: RunningProgram | null; queue: QueuedProgram[]; maxConcurrentZones: number; nextRunUtc: string | null; timezone: string; masterEnabled: boolean; rainDelayUntilUtc: string | null }`
  - `class ApiError { status: number; reason: string | null; message }` — `new ApiError(status, message, reason = null)`
  - `stopZone(zoneNumber: number): Promise<void>` in `client.ts`
  - `programLine(program: RunningProgram): string` exported from `NowScreen.tsx`
  - test ids: `running-banner`, `running-zone-<number>`, `program-line`, `queue-line`, `cap-hint`, `zone-tile-<number>`

- [ ] **Step 1: Replace `Status` and `ApiError` in `web/src/api/types.ts`**

Replace the `Status` interface with:

```ts
export type RunSource = 'program' | 'manual'

/** One open valve. `zone` is the zone number. */
export interface RunningZone {
  zone: number
  secondsRemaining: number
  source: RunSource
}

/** The program the controller is running. `waitingZones` are zone numbers. */
export interface RunningProgram {
  id: number
  name: string
  step: number
  stepCount: number
  waitingZones: number[]
}

export interface QueuedProgram {
  programId: number
  name: string
  queuedAtUtc: string | null
}

export interface Status {
  running: RunningZone[]
  program: RunningProgram | null
  queue: QueuedProgram[]
  maxConcurrentZones: number
  nextRunUtc: string | null
  timezone: string
  masterEnabled: boolean
  rainDelayUntilUtc: string | null
}
```

Replace `ApiError` with:

```ts
/** Thrown for any non-2xx response. `status` is the HTTP code; `reason` is the daemon's refusal name, such as `cap_reached`, or null. */
export class ApiError extends Error {
  readonly status: number
  readonly reason: string | null

  constructor(status: number, message: string, reason: string | null = null) {
    super(message)
    this.name = 'ApiError'
    this.status = status
    this.reason = reason
  }
}
```

- [ ] **Step 2: Decode the new status in `web/src/api/decode.ts`**

Extend the type import with `type QueuedProgram, type RunningProgram, type RunningZone`. Add after `dayMode()`:

```ts
function numbers(value: unknown, field: string): number[] {
  return asArray(value, field).map((element, index) => {
    if (typeof element !== 'number' || Number.isFinite(element) === false) {
      throw new DecodeError(`${field}[${index}]`, `expected a number, received ${typeof element}`)
    }
    return element
  })
}

function decodeRunningZone(element: unknown, field: string): RunningZone {
  const source = asRecord(element, field)
  const runSource = str(source, 'source', field)
  if (runSource !== 'program' && runSource !== 'manual') {
    throw new DecodeError(`${field}.source`, `unknown source "${runSource}"`)
  }
  return {
    zone: num(source, 'zone', field),
    secondsRemaining: num(source, 'secondsRemaining', field),
    source: runSource,
  }
}

/** The daemon sends null when no program runs; an absent key is a contract break. */
function decodeRunningProgram(value: unknown, field: string): RunningProgram | null {
  if (value === null) {
    return null
  }
  const source = asRecord(value, field)
  return {
    id: num(source, 'id', field),
    name: str(source, 'name', field),
    step: num(source, 'step', field),
    stepCount: num(source, 'stepCount', field),
    waitingZones: numbers(source['waitingZones'], `${field}.waitingZones`),
  }
}

function decodeQueuedProgram(element: unknown, field: string): QueuedProgram {
  const source = asRecord(element, field)
  return {
    programId: num(source, 'programId', field),
    name: str(source, 'name', field),
    queuedAtUtc: instant(source, 'queuedAtUtc', field),
  }
}
```

Replace `decodeStatus()`:

```ts
export function decodeStatus(payload: unknown): Status {
  const source = asRecord(payload, 'status')
  return {
    running: asArray(source['running'], 'status.running').map((element, index) =>
      decodeRunningZone(element, `status.running[${index}]`),
    ),
    program: decodeRunningProgram(source['program'], 'status.program'),
    queue: asArray(source['queue'], 'status.queue').map((element, index) =>
      decodeQueuedProgram(element, `status.queue[${index}]`),
    ),
    maxConcurrentZones: num(source, 'maxConcurrentZones', 'status'),
    nextRunUtc: instant(source, 'nextRunUtc', 'status'),
    timezone: str(source, 'timezone', 'status'),
    masterEnabled: bool(source, 'masterEnabled', 'status'),
    rainDelayUntilUtc: instant(source, 'rainDelayUntilUtc', 'status'),
  }
}
```

- [ ] **Step 3: Carry the reason and add `stopZone()` in `web/src/api/client.ts`**

Replace the `response.ok === false` block in `request()`:

```ts
  if (response.ok === false) {
    const record =
      typeof payload === 'object' && payload !== null ? (payload as Record<string, unknown>) : null
    const detail =
      record !== null && typeof record['error'] === 'string'
        ? (record['error'] as string)
        : `${response.status} ${response.statusText}`
    const reason = record !== null && typeof record['reason'] === 'string' ? (record['reason'] as string) : null
    throw new ApiError(response.status, detail, reason)
  }
```

Add after `runZone()`:

```ts
/** Closes one valve. Addressed by zone number, the identifier `/api/status` reports. */
export async function stopZone(zoneNumber: number): Promise<void> {
  await request(`/zones/${zoneNumber}/stop`, { method: 'POST' })
}
```

- [ ] **Step 4: Poll fast while anything runs or waits in `web/src/hooks/useStatus.ts`**

```ts
function intervalFor(status: Status | null): number {
  const active =
    status !== null && (status.running.length > 0 || status.program !== null || status.queue.length > 0)
  return active ? RUNNING_POLL_MS : IDLE_POLL_MS
}
```

- [ ] **Step 5: Replace `web/src/screens/NowScreen.tsx`**

```tsx
import { useCallback, useEffect, useRef, useState } from 'react'
import { getZones, runZone, stopAll, stopZone } from '../api/client'
import type { RunningProgram, RunningZone, Zone } from '../api/types'
import StopButton from '../components/StopButton'
import ZoneTile from '../components/ZoneTile'
import { useCountdown } from '../hooks/useCountdown'
import { formatCountdown, formatDayAndClock, formatDuration } from '../time/zonedformat'
import type { ScreenProps } from './screenProps'

export const QUICK_RUN_CHOICES = [60, 300, 600, 900, 1200, 1800]
const DEFAULT_QUICK_RUN = 600

/** Reads "Morning Drip — step 1 of 2, zone 7 waiting". */
export function programLine(program: RunningProgram): string {
  const base = `${program.name} — step ${program.step} of ${program.stepCount}`
  const waiting = program.waitingZones
  if (waiting.length === 0) {
    return base
  }
  return `${base}, ${waiting.length === 1 ? 'zone' : 'zones'} ${waiting.join(', ')} waiting`
}

interface RunningRowProps {
  entry: RunningZone
  name: string
  polls: number
  onStop: (zoneNumber: number) => void
}

/** The row's Stop carries no disabled state: whoever is standing in the spray must be able to close this valve whatever else is in flight. */
function RunningRow({ entry, name, polls, onStop }: RunningRowProps) {
  const remaining = useCountdown(entry.secondsRemaining, polls)
  return (
    <li className="running-row" data-testid={`running-zone-${entry.zone}`}>
      <span className="running-row__zone">
        Zone {entry.zone} — {name}
      </span>
      <span className={`running-row__tag running-row__tag--${entry.source}`}>
        {entry.source === 'program' ? 'Program' : 'Manual'}
      </span>
      <span className="running__clock">{formatCountdown(remaining)}</span>
      <button
        type="button"
        className="running-row__stop"
        aria-label={`Stop zone ${entry.zone}`}
        onClick={() => {
          onStop(entry.zone)
        }}
      >
        Stop
      </button>
    </li>
  )
}

export default function NowScreen({ status, polls, refresh }: ScreenProps) {
  const [zones, setZones] = useState<Zone[]>([])
  const [seconds, setSeconds] = useState(DEFAULT_QUICK_RUN)
  const [error, setError] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)
  const actionSeq = useRef(0)
  const inFlight = useRef(0)

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

  const act = useCallback(
    (action: () => Promise<void>) => {
      const seq = (actionSeq.current += 1)
      inFlight.current += 1
      setError(null)
      setBusy(true)
      action()
        .then(() => {
          refresh()
        })
        .catch((caught: unknown) => {
          if (actionSeq.current === seq) {
            setError(caught instanceof Error ? caught.message : String(caught))
          }
        })
        .finally(() => {
          inFlight.current -= 1
          setBusy(inFlight.current > 0)
        })
    },
    [refresh],
  )

  const onRun = useCallback(
    (zone: Zone) => {
      act(() => runZone(zone, seconds))
    },
    [act, seconds],
  )

  const onStop = useCallback(() => {
    act(stopAll)
  }, [act])

  const onStopZone = useCallback(
    (zoneNumber: number) => {
      act(() => stopZone(zoneNumber))
    },
    [act],
  )

  const running = status?.running ?? []
  const cap = status?.maxConcurrentZones ?? 0
  const atCap = status !== null && running.length >= cap
  const nameOf = (zoneNumber: number) => zones.find((zone) => zone.number === zoneNumber)?.name ?? ''
  const zoneId = status?.timezone ?? ''

  return (
    <section className="screen">
      <h1>Now</h1>

      {error === null ? null : (
        <div className="alert" role="alert">
          {error}
        </div>
      )}

      <div className="running" data-testid="running-banner">
        {status === null ? (
          <span className="running__unknown">Zone state unknown</span>
        ) : running.length === 0 ? (
          <span className="running__idle">No zone running</span>
        ) : (
          <ul className="running-list">
            {running.map((entry) => (
              <RunningRow
                key={entry.zone}
                entry={entry}
                name={nameOf(entry.zone)}
                polls={polls}
                onStop={onStopZone}
              />
            ))}
          </ul>
        )}
        {status !== null && status.program !== null ? (
          <div className="program-line" data-testid="program-line">
            {programLine(status.program)}
          </div>
        ) : null}
        {status !== null && status.queue.length > 0 ? (
          <div className="queue-line" data-testid="queue-line">
            Queued: {status.queue.map((entry) => entry.name).join(', ')}
          </div>
        ) : null}
      </div>

      <StopButton onStop={onStop} busy={busy} />

      <div className="next-run" data-testid="next-run">
        Next run:{' '}
        {status === null
          ? 'unknown'
          : status.nextRunUtc !== null
            ? formatDayAndClock(status.nextRunUtc, zoneId)
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

      {atCap ? (
        <p className="cap-hint" data-testid="cap-hint">
          {`${running.length} of ${cap} running`}
        </p>
      ) : null}

      <div className="zone-grid">
        {zones.map((entry) => {
          const open = running.some((item) => item.zone === entry.number)
          return (
            <ZoneTile
              key={entry.number}
              zone={entry}
              running={open}
              disabled={busy || (atCap && open === false)}
              onRun={onRun}
            />
          )
        })}
      </div>
    </section>
  )
}
```

- [ ] **Step 6: Style the rows and lines in `web/src/styles/app.css`**

Replace the `.running` rule and append the new rules after `.running__unknown`:

```css
.running {
  display: flex;
  flex-direction: column;
  gap: var(--gap);
  padding: var(--gap);
  border: 1px solid var(--glass-edge);
  border-radius: var(--radius);
  background: var(--glass-raised);
  font-size: 1.3rem;
}

.running-list {
  display: flex;
  flex-direction: column;
  gap: var(--gap);
  margin: 0;
  padding: 0;
  list-style: none;
}

.running-row {
  display: grid;
  grid-template-columns: 1fr auto;
  grid-template-areas:
    'zone tag'
    'clock stop';
  align-items: center;
  gap: 6px var(--gap);
}

.running-row__zone {
  grid-area: zone;
}

.running-row__tag {
  grid-area: tag;
  padding: 2px 10px;
  border-radius: 999px;
  font-size: 0.8rem;
  font-weight: 700;
  letter-spacing: 0.08em;
  text-transform: uppercase;
  border: 1px solid var(--glass-edge);
  color: var(--ink-dim);
}

.running-row__tag--program {
  border-color: var(--running);
  color: var(--running);
}

.running-row .running__clock {
  grid-area: clock;
}

.running-row__stop {
  grid-area: stop;
  min-height: var(--touch);
  background: var(--danger);
  color: var(--danger-ink);
  font-weight: 800;
}

.program-line,
.queue-line {
  font-size: 1rem;
  color: var(--ink-dim);
}

.cap-hint {
  margin: 0;
  color: var(--warn);
  font-weight: 700;
}
```

`.zone-tile[data-running='true']` already glows every tile marked running; it needs no change.

- [ ] **Step 7: Update the status fixtures in `web/src/test/fixtures.ts`**

```ts
export const idleStatus: Status = {
  running: [],
  program: null,
  queue: [],
  maxConcurrentZones: 2,
  nextRunUtc: '2026-09-14T13:00:00Z',
  timezone: 'America/Los_Angeles',
  masterEnabled: true,
  rainDelayUntilUtc: null,
}

export const runningStatus: Status = {
  ...idleStatus,
  running: [{ zone: 3, secondsRemaining: 120, source: 'manual' }],
}
```

- [ ] **Step 8: Keep `useStatus.test.ts` compiling**

In `web/src/hooks/useStatus.test.ts` replace `expect(result.current.status?.runningZone).toBe(3)` with `expect(result.current.status?.running[0]?.zone).toBe(3)`, and `expect(result.current.status?.runningZone).toBe(0)` with `expect(result.current.status?.running).toEqual([])`.

- [ ] **Step 9: Park `decode.test.ts`**

`web/tsconfig.json` — add after `"include"`:

```json
  "exclude": ["src/api/decode.test.ts"]
```

`web/vitest.config.ts` — extend `exclude`:

```ts
    exclude: ['**/node_modules/**', '**/dist/**', 'src/**/*.tz.test.ts', 'src/api/decode.test.ts'],
```

- [ ] **Step 10: Typecheck and run the suite**

```bash
npm --prefix web run typecheck
npm --prefix web test
```

Expected: typecheck exits zero. In the suite, the `NowScreen` tests that click `getByRole('button', { name: /stop/i })` with a zone running now match both STOP and "Stop zone 3" and fail; Task 15 narrows them. Everything else passes.

- [ ] **Step 11: Commit**

```bash
git add web/src/api/types.ts web/src/api/decode.ts web/src/api/client.ts web/src/hooks/useStatus.ts \
        web/src/screens/NowScreen.tsx web/src/styles/app.css web/src/test/fixtures.ts \
        web/src/hooks/useStatus.test.ts web/tsconfig.json web/vitest.config.ts
git commit -F - <<'EOF'
feat: show every running zone with its own stop

The Now screen lists each open zone with its countdown, a program or
manual tag and a Stop button, then the running program's step and
waiting zones and the queued programs. Every open tile glows; at the
zone cap the other tiles' Run buttons are disabled under an "N of N
running" hint, and a refused run shows the daemon's own sentence from
the 409. Status polls every 2 s while anything runs, a program runs,
or a program waits.

decode.test.ts is parked until its rewrite.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---

### Task 8: Web — programs as steps

Spec §5 and §7 (Program editor). A program is a list of steps; each step holds zone chips (zone **ids**), a "+ zone" picker, one duration, reorder and delete. A step with more zones than the cap shows "Runs in waves" under it. The editor and the Programs list both compute the total runtime assuming waves: a step of *n* zones under a cap of *c* takes ⌈n/c⌉ × its duration.

`client.test.ts`, `dayRule.test.ts`, `ProgramEditor.test.tsx` and `ProgramsScreen.test.tsx` build the old program shape and are **parked** until Task 15.

**Files:**
- Modify: `web/src/api/types.ts`, `web/src/api/decode.ts`
- Modify: `web/src/programs/dayRule.ts`
- Modify: `web/src/screens/ProgramEditor.tsx`, `web/src/screens/ProgramsScreen.tsx`
- Modify: `web/src/settings/settingsMap.ts`
- Modify: `web/src/styles/app.css`, `web/src/test/fixtures.ts`
- Modify: `web/tsconfig.json`, `web/vitest.config.ts` (park)

**Interfaces:**
- Consumes: Task 7 `Status.maxConcurrentZones`.
- Produces:
  - `interface ProgramStep { zones: number[]; durationSeconds: number }` — `zones` are zone **ids**
  - `Program.steps: ProgramStep[]`, `ProgramDraft.steps: ProgramStep[]`; `ProgramZone` and `ProgramZoneDraft` removed
  - `totalRuntimeSeconds(steps: { zones: readonly unknown[]; durationSeconds: number }[], maxConcurrentZones: number): number`
  - `ProgramEditorProps.maxConcurrentZones: number`
  - `DEFAULT_MAX_CONCURRENT_ZONES = 2` in `settingsMap.ts`
  - editor labels: `Step N minutes`, `Add a zone to step N`, `Remove <number> · <name> from step N`, `Move step N up`, `Move step N down`, `Remove step N`, `Add step`; test ids `step-N`, `wave-warning-N`, `editor-total`; list test id `step-sequence`

- [ ] **Step 1: Replace the program zone types in `web/src/api/types.ts`**

Delete `ProgramZone` and `ProgramZoneDraft`. Add:

```ts
/** One step of a program. `zones` holds zone ids (`zone.id`). */
export interface ProgramStep {
  zones: number[]
  durationSeconds: number
}
```

In `Program` and in `ProgramDraft` replace the `zones` field with `steps: ProgramStep[]`.

- [ ] **Step 2: Decode steps in `web/src/api/decode.ts`**

Replace `type ProgramZone` in the import with `type ProgramStep`, replace `decodeProgramZone()` with:

```ts
function decodeProgramStep(element: unknown, field: string): ProgramStep {
  const source = asRecord(element, field)
  return {
    zones: numbers(source['zones'], `${field}.zones`),
    durationSeconds: num(source, 'durationSeconds', field),
  }
}
```

and in `decodePrograms()` replace the `zones:` entry with:

```ts
      steps: asArray(source['steps'], `${field}.steps`).map((step, i) =>
        decodeProgramStep(step, `${field}.steps[${i}]`),
      ),
```

Array order is run order; nothing sorts the steps.

- [ ] **Step 3: Add the default cap to `web/src/settings/settingsMap.ts`**

```ts
/** The daemon's default for max_concurrent_zones, used until /api/status reports the real cap. */
export const DEFAULT_MAX_CONCURRENT_ZONES = 2
```

- [ ] **Step 4: Wave-aware totals in `web/src/programs/dayRule.ts`**

Replace `totalRuntimeSeconds()` and the `zones` part of `toDraft()`:

```ts
/** Assumes nothing else is running: a step of n zones under a cap of c runs in ceil(n / c) waves of its duration. */
export function totalRuntimeSeconds(
  steps: { zones: readonly unknown[]; durationSeconds: number }[],
  maxConcurrentZones: number,
): number {
  const cap = Math.max(1, maxConcurrentZones)
  return steps.reduce((total, step) => total + Math.ceil(step.zones.length / cap) * step.durationSeconds, 0)
}
```

```ts
    steps: program.steps.map((step) => ({
      zones: [...step.zones],
      durationSeconds: step.durationSeconds,
    })),
```

- [ ] **Step 5: Replace `web/src/screens/ProgramEditor.tsx`**

```tsx
import { useCallback, useState } from 'react'
import { createProgram, deleteProgram, updateProgram } from '../api/client'
import { DAY_MODES, type DayMode, type Program, type ProgramDraft, type ProgramStep, type Zone } from '../api/types'
import { WEEKDAY_LABELS, toDraft, toggleWeekday, totalRuntimeSeconds } from '../programs/dayRule'
import { formatDuration, inputValueToMinutes, minutesToInputValue } from '../time/zonedformat'

const DAY_MODE_LABELS: Record<DayMode, string> = {
  DaysOfWeek: 'Days of week',
  Odd: 'Odd days',
  Even: 'Even days',
  EveryNDays: 'Every N days',
}

const DEFAULT_STEP_SECONDS = 600

export function emptyDraft(controllerZone: string): ProgramDraft {
  return {
    name: '',
    enabled: true,
    dayMode: 'DaysOfWeek',
    dowMask: 0,
    intervalDays: 0,
    anchorDate: null,
    startTimes: [{ minutesAfterMidnight: 360, timezone: controllerZone }],
    steps: [],
  }
}

export function validationError(draft: ProgramDraft): string | null {
  if (draft.name.trim().length === 0) {
    return 'Give the program a name.'
  }
  if (draft.startTimes.length === 0) {
    return 'Add at least one start time.'
  }
  if (draft.startTimes.some((start) => start.timezone.length === 0)) {
    return 'The controller timezone is not known yet. Save again once the controller answers.'
  }
  if (draft.steps.length === 0) {
    return 'Add at least one step.'
  }
  const empty = draft.steps.findIndex((step) => step.zones.length === 0)
  if (empty >= 0) {
    return `Step ${empty + 1} needs at least one zone.`
  }
  if (draft.steps.some((step) => step.durationSeconds < 1)) {
    return 'Every step needs a duration of at least one minute.'
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

export interface ProgramEditorProps {
  program: Program | null
  zones: Zone[]
  controllerZone: string
  maxConcurrentZones: number
  onDone: () => void
  onCancel: () => void
}

export default function ProgramEditor({
  program,
  zones,
  controllerZone,
  maxConcurrentZones,
  onDone,
  onCancel,
}: ProgramEditorProps) {
  const [draft, setDraft] = useState<ProgramDraft>(() =>
    program === null ? emptyDraft(controllerZone) : toDraft(program),
  )
  const [startTimeText, setStartTimeText] = useState<string[]>(() =>
    (program === null ? emptyDraft(controllerZone) : toDraft(program)).startTimes.map((start) =>
      minutesToInputValue(start.minutesAfterMidnight),
    ),
  )
  const [error, setError] = useState<string | null>(null)
  const [saving, setSaving] = useState(false)

  const patch = useCallback((changes: Partial<ProgramDraft>) => {
    setDraft((current) => ({ ...current, ...changes }))
  }, [])

  const patchStep = useCallback((index: number, change: (step: ProgramStep) => ProgramStep) => {
    setDraft((current) => ({
      ...current,
      steps: current.steps.map((step, i) => (i === index ? change(step) : step)),
    }))
  }, [])

  const onSave = useCallback(async () => {
    const unparsed = startTimeText.findIndex((text) => inputValueToMinutes(text) < 0)
    if (unparsed >= 0) {
      setError(`Start time ${unparsed + 1} needs a valid time.`)
      return
    }
    const normalised: ProgramDraft = {
      ...draft,
      name: draft.name.trim(),
      startTimes: draft.startTimes.map((start) =>
        start.timezone.length === 0 ? { ...start, timezone: controllerZone } : start,
      ),
    }
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
  }, [draft, startTimeText, program, controllerZone, onDone])

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

  const moveStep = useCallback((index: number, delta: number) => {
    setDraft((current) => {
      const target = index + delta
      if (target < 0 || target >= current.steps.length) {
        return current
      }
      const reordered = [...current.steps]
      const [moved] = reordered.splice(index, 1)
      reordered.splice(target, 0, moved!)
      return { ...current, steps: reordered }
    })
  }, [])

  const zoneLabel = (zoneId: number) => {
    const zone = zones.find((candidate) => candidate.id === zoneId)
    return zone === undefined ? `Zone id ${zoneId}` : `${zone.number} · ${zone.name}`
  }

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
              value={startTimeText[index] ?? minutesToInputValue(start.minutesAfterMidnight)}
              onChange={(event) => {
                const text = event.target.value
                setStartTimeText((current) => current.map((entry, i) => (i === index ? text : entry)))
                const minutes = inputValueToMinutes(text)
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
              setStartTimeText((current) => current.filter((_, i) => i !== index))
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
          setStartTimeText((current) => [...current, minutesToInputValue(360)])
          patch({
            startTimes: [...draft.startTimes, { minutesAfterMidnight: 360, timezone: controllerZone }],
          })
        }}
      >
        Add start time
      </button>

      <h3>Steps in run order</h3>
      {draft.steps.map((step, index) => {
        const number = index + 1
        const available = zones.filter((zone) => step.zones.includes(zone.id) === false)
        return (
          <div key={index} className="step" data-testid={`step-${number}`}>
            <div className="row">
              <span className="step__title">{`Step ${number}`}</span>
              {step.zones.map((zoneId) => (
                <span key={zoneId} className="chip">
                  {zoneLabel(zoneId)}
                  <button
                    type="button"
                    className="chip__remove"
                    aria-label={`Remove ${zoneLabel(zoneId)} from step ${number}`}
                    onClick={() => {
                      patchStep(index, (current) => ({
                        ...current,
                        zones: current.zones.filter((id) => id !== zoneId),
                      }))
                    }}
                  >
                    ×
                  </button>
                </span>
              ))}
              <select
                aria-label={`Add a zone to step ${number}`}
                value=""
                onChange={(event) => {
                  if (event.target.value === '') {
                    return
                  }
                  const zoneId = Number(event.target.value)
                  patchStep(index, (current) => ({ ...current, zones: [...current.zones, zoneId] }))
                }}
              >
                <option value="">+ zone</option>
                {available.map((candidate) => (
                  <option key={candidate.id} value={String(candidate.id)}>
                    {`${candidate.number} · ${candidate.name}`}
                  </option>
                ))}
              </select>
            </div>
            <div className="row">
              <label>
                {`Step ${number} minutes`}
                <input
                  type="number"
                  min={1}
                  value={Math.round(step.durationSeconds / 60)}
                  onChange={(event) => {
                    const minutes = Number(event.target.value)
                    patchStep(index, (current) => ({ ...current, durationSeconds: minutes * 60 }))
                  }}
                />
              </label>
              <button
                type="button"
                onClick={() => {
                  moveStep(index, -1)
                }}
              >
                {`Move step ${number} up`}
              </button>
              <button
                type="button"
                onClick={() => {
                  moveStep(index, 1)
                }}
              >
                {`Move step ${number} down`}
              </button>
              <button
                type="button"
                onClick={() => {
                  patch({ steps: draft.steps.filter((_, i) => i !== index) })
                }}
              >
                {`Remove step ${number}`}
              </button>
            </div>
            {step.zones.length > maxConcurrentZones ? (
              <p className="step__warning" data-testid={`wave-warning-${number}`}>
                {`Runs in waves: ${maxConcurrentZones} zones at a time`}
              </p>
            ) : null}
          </div>
        )
      })}
      <button
        type="button"
        onClick={() => {
          patch({ steps: [...draft.steps, { zones: [], durationSeconds: DEFAULT_STEP_SECONDS }] })
        }}
      >
        Add step
      </button>

      <div data-testid="editor-total">
        Total {formatDuration(totalRuntimeSeconds(draft.steps, maxConcurrentZones))}
      </div>

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

- [ ] **Step 6: Show steps and the wave-aware total in `web/src/screens/ProgramsScreen.tsx`**

Add `import { DEFAULT_MAX_CONCURRENT_ZONES } from '../settings/settingsMap'`. After `const controllerZone = status?.timezone ?? ''` add:

```tsx
  const maxConcurrentZones = status?.maxConcurrentZones ?? DEFAULT_MAX_CONCURRENT_ZONES
```

Pass it to the editor: add `maxConcurrentZones={maxConcurrentZones}` to `<ProgramEditor …>`.

Replace the zone sequence list and the total:

```tsx
          <ol data-testid="step-sequence">
            {program.steps.map((step, index) => (
              <li key={index}>
                {index + 1}. {step.zones.map(zoneName).join(' + ')} {formatDuration(step.durationSeconds)}
              </li>
            ))}
          </ol>

          <div data-testid="total-runtime">
            Total {formatDuration(totalRuntimeSeconds(program.steps, maxConcurrentZones))}
          </div>
```

- [ ] **Step 7: Style steps and chips in `web/src/styles/app.css`**

Append:

```css
.step {
  display: flex;
  flex-direction: column;
  gap: 6px;
  padding: var(--gap);
  border: 1px solid var(--glass-edge);
  border-radius: var(--radius);
  background: var(--glass-raised);
}

.step__title {
  font-weight: 700;
}

.chip {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 2px 4px 2px 12px;
  border: 1px solid var(--accent);
  border-radius: 999px;
}

.chip__remove {
  min-height: 32px;
  min-width: 32px;
  padding: 0;
  border: 0;
  background: transparent;
  color: var(--ink);
  font-size: 1.1rem;
}

.step__warning {
  margin: 0;
  color: var(--warn);
  font-size: 0.9rem;
}
```

- [ ] **Step 8: Update the program fixture in `web/src/test/fixtures.ts`**

Replace `morningProgram.zones` with:

```ts
  steps: [
    { zones: [7], durationSeconds: 600 },
    { zones: [9], durationSeconds: 300 },
  ],
```

- [ ] **Step 9: Park the four program test files**

`web/tsconfig.json`:

```json
  "exclude": [
    "src/api/decode.test.ts",
    "src/api/client.test.ts",
    "src/programs/dayRule.test.ts",
    "src/screens/ProgramEditor.test.tsx",
    "src/screens/ProgramsScreen.test.tsx"
  ]
```

`web/vitest.config.ts`:

```ts
    exclude: [
      '**/node_modules/**',
      '**/dist/**',
      'src/**/*.tz.test.ts',
      'src/api/decode.test.ts',
      'src/api/client.test.ts',
      'src/programs/dayRule.test.ts',
      'src/screens/ProgramEditor.test.tsx',
      'src/screens/ProgramsScreen.test.tsx',
    ],
```

- [ ] **Step 10: Typecheck and run the suite**

```bash
npm --prefix web run typecheck
npm --prefix web test
```

Expected: typecheck exits zero; the suite results match Task 7's.

- [ ] **Step 11: Commit**

```bash
git add web/src/api/types.ts web/src/api/decode.ts web/src/programs/dayRule.ts \
        web/src/screens/ProgramEditor.tsx web/src/screens/ProgramsScreen.tsx \
        web/src/settings/settingsMap.ts web/src/styles/app.css web/src/test/fixtures.ts \
        web/tsconfig.json web/vitest.config.ts
git commit -F - <<'EOF'
feat: edit programs as steps of zones that water together

Each program row is now a step: zone chips with a "+ zone" picker, one
duration, reorder and delete. A step with more zones than the cap shows
"Runs in waves" under it, and the editor and the Programs list both
total the runtime assuming waves. Programs store zone ids, as before.

The program client, day-rule, editor and list tests are parked until
their rewrite.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---

### Task 9: Web — "Max zones at once" on Settings

Spec §7 (Settings). A whole-number field from 1 to 8 writing `max_concurrent_zones`.

**Files:**
- Modify: `web/src/settings/settingsMap.ts`
- Modify: `web/src/screens/SettingsScreen.tsx`

**Interfaces:**
- Consumes: Task 6 `max_concurrent_zones` on `GET`/`PUT /api/settings`; Task 8 `DEFAULT_MAX_CONCURRENT_ZONES`.
- Produces: `SETTING_KEYS.maxConcurrentZones = 'max_concurrent_zones'`, `MAX_CONCURRENT_ZONES_LIMIT = 8`; Settings labels `Max zones at once` and button `Save max zones`.

- [ ] **Step 1: Add the key and limit to `web/src/settings/settingsMap.ts`**

```ts
export const SETTING_KEYS = {
  rainDelayUntil: 'rain_delay_until',
  masterEnabled: 'master_enabled',
  maxZoneSeconds: 'max_zone_seconds',
  logLevel: 'log_level',
  maxConcurrentZones: 'max_concurrent_zones',
} as const
```

```ts
/** The daemon rejects any max_concurrent_zones outside 1 through this value with a 400. */
export const MAX_CONCURRENT_ZONES_LIMIT = 8
```

- [ ] **Step 2: Add the field to `web/src/screens/SettingsScreen.tsx`**

Import `DEFAULT_MAX_CONCURRENT_ZONES` and `MAX_CONCURRENT_ZONES_LIMIT` alongside the existing `settingsMap` imports. Add state after `ceilingMinutes`:

```tsx
  const [maxZones, setMaxZones] = useState(DEFAULT_MAX_CONCURRENT_ZONES)
```

In `load`, after `setCeilingMinutes(...)`:

```tsx
      setMaxZones(parseInteger(loadedSettings[SETTING_KEYS.maxConcurrentZones], DEFAULT_MAX_CONCURRENT_ZONES))
```

After `onSaveCeiling`:

```tsx
  const onSaveMaxZones = useCallback(() => {
    if (Number.isInteger(maxZones) === false || maxZones < 1 || maxZones > MAX_CONCURRENT_ZONES_LIMIT) {
      setError(`Max zones at once must be a whole number from 1 to ${MAX_CONCURRENT_ZONES_LIMIT}.`)
      return
    }
    void write({ [SETTING_KEYS.maxConcurrentZones]: String(maxZones) })
  }, [maxZones, write])
```

After the `Save ceiling` button:

```tsx
          <label>
            Max zones at once
            <input
              type="number"
              min={1}
              max={MAX_CONCURRENT_ZONES_LIMIT}
              value={maxZones}
              onChange={(event) => {
                setMaxZones(Number(event.target.value))
              }}
            />
          </label>
          <button type="button" onClick={onSaveMaxZones}>
            Save max zones
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
feat: set how many zones may run at once from Settings

A "Max zones at once" field writes max_concurrent_zones as a whole
number from 1 to 8.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---

### Task 10: Bring the base design and the web README in line

Spec preamble: §5.1, §5.5, §6, §7 and §8 of the base design are updated when this lands. §10's safety-test list names mutual exclusion and is updated with them so the document does not contradict itself. The web README's identifier table gains the new fields.

**Files:**
- Modify: `docs/design/2026-09-05-irrigation-design.md`
- Modify: `web/README.md`

**Interfaces:** none.

- [ ] **Step 1: Replace §5.1 "ZoneController safety contract"**

Replace from `### 5.1 ZoneController safety contract` up to, not including, `### 5.2 Threading` with:

```markdown
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
   expected *set* of open zones. A zone past its deadline, a line that differs
   from the expected set, or a failed read-back closes the bank and latches a
   fault that refuses every open until a later tick reads back exactly the
   expected set with no zone open.
5. **Count check.** The watchdog also trips if more lines read back asserted
   than the cap in force when the open zones were opened.

The read-back uses `IGpioBackend::getValues()` through `OutputBank::readValues()`;
`InputPin::isAsserted()` gives the STOP button its level at startup.

Every close reports its reason — deadline, per-zone stop, all-off, or watchdog —
so the runner can tell a finished step zone from a STOP. `allOff()` is callable
from any component and always takes precedence.

Construction order is a hard requirement: `ZoneController` is constructed and
drives all eight lines de-asserted before the scheduler or HTTP server exist.
The systemd unit uses `Restart=always`.
```

- [ ] **Step 2: Replace §5.5 "ProgramRunner"**

Replace from `### 5.5 ProgramRunner` up to, not including, `### 5.6 Stop button` with:

```markdown
### 5.5 ProgramRunner and the program queue

A program is an ordered list of steps; a step is a set of zones and one
duration. `ProgramRunner` walks the steps, advanced only by `ZoneController`'s
close signal, so timing has a single authority.

- The runner opens every zone of the step that fits under the cap. A zone that
  does not fit waits and opens when a slot frees, then runs the step's full
  duration from its own open. Waiting zones open in ascending zone number.
- A step completes when every one of its zones has opened and closed. A
  disabled zone is skipped; a step of only disabled zones completes at once.
- A zone already open when its step starts (a manual run) is taken over: its
  deadline becomes now plus the step duration.
- A per-zone stop of a step zone counts as that zone finishing its share of the
  step. An all-off or watchdog close aborts the program.

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
```

- [ ] **Step 3: Update §5.6 "Stop button"**

Replace its last sentence `A press calls ZoneController::allOff() and aborts any active runner.` with:

```markdown
A press empties the program queue (each scheduled entry recorded
`dropped_stop`), aborts the running program, and calls
`ZoneController::allOff()`, in that order. While the button is held every open
request is refused and every program that comes due is recorded `skipped_stop`.
```

- [ ] **Step 4: Update §6 "Data model"**

Replace the `program_zones` line in the table block with:

```
program_steps         id, program_id, sequence, duration_seconds

program_step_zones    id, step_id, zone_id
```

Replace the paragraph starting `` `outcome` is one of `` with:

```markdown
`outcome` is one of `queued`, `ran`, `skipped_rain`, `skipped_stop`,
`skipped_duplicate`, `skipped_disabled`, `dropped_stop`, `dropped_restart`,
`missed`, `failed`. A due firing is inserted `queued` and updated to its final
outcome when it starts or leaves the queue. `skipped_busy` appears only in rows
written before schema 1.1.0.
```

Replace the settings-keys sentence with:

```markdown
Settings keys: `rain_delay_until` (UTC), `master_enabled`, `max_zone_seconds`,
`log_level`, `max_concurrent_zones` (1–8, default 2).
```

Add after the migration paragraph:

```markdown
Schema 1.1.0 replaced `program_zones` with `program_steps` and
`program_step_zones`; its migration turned each `program_zones` row into a
one-zone step with the same id, sequence and duration.
```

- [ ] **Step 5: Update §7 "REST API"**

Replace the route block and the `/admin/status` paragraph with:

````markdown
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

Program bodies carry `steps: [{ zones: [zoneId…], durationSeconds }]`;
responses add each step's stored `id`.

`/admin/status` is the UI's polling endpoint. It returns `running` (one entry
per open zone: zone number, seconds remaining, `program` or `manual`),
`program` (id, name, step, step count, waiting zone numbers, or `null`),
`queue` (program id, name, queued-at instant), `maxConcurrentZones`, the next
scheduled occurrence, the rain delay, the master enable, whether STOP is held,
and the controller's timezone identifier alongside its UTC timestamps.
````

- [ ] **Step 6: Update §8 "Web interface"**

Replace the three screen bullets and the polling paragraph with:

```markdown
- **Now** — one row per open zone with its countdown, a program/manual tag and
  its own Stop; the running program's step and waiting zones; the queued
  programs; the next scheduled run; eight zone tiles with a quick manual run,
  every open tile glowing and Run disabled on the others at the cap with an
  "N of N running" hint; a refused run shows the daemon's reason; and a
  prominent stop control. Mobile-first with large touch targets and high
  contrast for outdoor readability.
- **Programs** — list, create, edit. Day rule, start times, ordered steps of
  zone chips with one duration each, a "runs in waves" warning on a step with
  more zones than the cap, computed total runtime assuming waves, next run.
- **Settings** — rain delay, master enable, maximum zone runtime, max zones at
  once, zone names.

The UI polls `/admin/status` every 2s while a zone is open,
a program runs, or a program waits; every 15s otherwise.
```

- [ ] **Step 7: Update §10 "Testing"**

Replace its last paragraph with:

```markdown
The safety invariants get dedicated tests: duration clamping, the concurrency
cap, per-zone deadlines, the watchdog's set comparison and count check, and
`allOff()` aborting an active program.
```

- [ ] **Step 8: Update `web/README.md`**

Replace the "Two zone identifiers" table with:

```markdown
| Use | Identifier |
|---|---|
| `POST /api/zones/{n}/run`, `POST /api/zones/{n}/stop`, `PUT /api/zones/{n}` | `zone.number` |
| `status.running[].zone`, `status.program.waitingZones[]` | `zone.number` |
| `program.steps[].zones[]` | `zone.id` |
```

and add after the "Time" section:

```markdown
## Refused runs

`POST /api/zones/{n}/run` and `POST /api/programs/{id}/run` answer 409 with
`{ "error", "reason" }` when the controller refuses. `ApiError.message` carries
`error`, a sentence the screens show as-is, and `ApiError.reason` carries the
machine name (`cap_reached`, `stop_held`, `master_disabled`, `zone_disabled`,
`already_queued`).
```

- [ ] **Step 9: Commit**

```bash
git add docs/design/2026-09-05-irrigation-design.md web/README.md
git commit -F - <<'EOF'
doc: bring the base design and web README up to concurrent zones

Sections 5.1, 5.5, 5.6, 6, 7, 8 and 10 of the base design now describe
the zone cap, per-zone deadlines and close reasons, program steps, the
program queue and its outcomes, the run-decision replies and the new
status shape. The web README's identifier table covers the stop route
and the new status and program fields.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---

### Task 11: Final whole-branch review and fix wave

Runs after every implementation task and before any test task. No new tests.

**Files:** whatever the review finds.

**Interfaces:** none.

- [ ] **Step 1:** Run `git log --oneline 61b45f1..HEAD` and `git diff 61b45f1..HEAD --stat`. Confirm one commit per Task 1–10 and no file outside the plan's File Structure table.
- [ ] **Step 2:** Spec walk. For each spec section 2, 3.1–3.5, 4, 5, 6, 7 read the code that implements it and record the file and function in the review notes. Flag anything the spec says that no code does.
- [ ] **Step 3:** Ordering traps. Read `ZoneController::closeOne()`/`closeAll()` (open set cleared before `zoneClosed`), `ZoneController::tripWatchdog()` (`_faulted` before `closeAll()`), `ProgramRunner::stopRunning()` (state cleared before closes), `IrrigationDaemon::onStopPressed()` (`dropAll` → `abort` → `allOff`), `IrrigationDaemon::threadAboutToFinish()` (queue deleted before `abort`), `IrrigationDaemon::threadStarted()` (`recordRestartDrops()` before `_scheduler->start()`), `IrrigationDaemon::onManualZoneRunRequested()`/`onProgramRunRequested()` (`publishStatus()` before `complete()`). Each must match its trap comment.
- [ ] **Step 4:** Threading. Confirm no `Qt::QueuedConnection` anywhere (`grep -rn QueuedConnection IrrigationD/src`), that `RunRequestPtr` is registered before the first `connect()` that carries it, and that every daemon slot taking a `RunRequestPtr` calls `complete()` on every path.
- [ ] **Step 5:** Comment and prose audit. `git diff 61b45f1..HEAD -- '*.cpp' '*.h' '*.ts' '*.tsx' '*.css'` and read every added comment against the Global Constraints rule; delete narration. Scan every commit message and doc change for "X, not Y" antithesis.
- [ ] **Step 6:** Wire contract. Compare `StatusJson::toJson`, `ProgramJson::toJson`/`fromJson`, `IrrigationControlServer::decisionResponse` against `decodeStatus`, `decodePrograms` and `client.ts` field by field, including null handling of `program` and the zone-number/zone-id split.
- [ ] **Step 7:** Build everything and the web bundle:

```bash
cmake --build build -j 32
npm --prefix web run build
```

- [ ] **Step 8:** Fix every finding in its own commit, typed `fix`, `refactor` or `doc`, with the standard trailer. Re-run Step 7 after the last fix.

---

### Task 12: Daemon tests — the zone controller and the program runner

Spec §8: the cap at the controller, deadline bookkeeping per zone, the watchdog's set comparison and count check, step waves and the zone-number order of waiting zones, step takeover of a manually open zone, per-zone stop inside a step. Review Focus 1 and 3.

**Files:**
- Modify: `IrrigationD/tests/tst_zonecontroller.cpp`
- Replace: `IrrigationD/tests/tst_programrunner.cpp`

**Interfaces:**
- Consumes: Task 1 and Task 3 interfaces as produced.

- [ ] **Step 1: Repair the existing `tst_zonecontroller` assertions**

Task 1's rename left `controller.openZoneNumbers().value(0)` where a test named a zone by position. Replace each with the literal zone the test opened:

| Test | Literal |
|---|---|
| `durationIsClampedToTheCeiling` (`secondsRemaining`) | `1` |
| `watchdogClosesAZonePastItsDeadline` (`disableCloseTimerForTest`) | `3` |
| `allOffClosesEverything` (`closeTimerActiveForTest`) | `7` |
| `allOffFailingInsideTripLeavesLatchUntilRetriedCloseLands` (`disableCloseTimerForTest`, `closeTimerActiveForTest`, `expireCloseTimerForTest`) | `5` |
| `closeRetriesAfterTwoConsecutiveWriteFailuresThenSucceeds` (`expireCloseTimerForTest`, `closeTimerActiveForTest`) | `3` |
| `closeRetryLandsWithinTwoRetryIntervals` (`expireCloseTimerForTest`) | `3` |

Leave `QCOMPARE(controller.openZoneNumbers().value(0), N)` assertions as they are; they read "the only open zone is N" and still hold.

`watchdogTripped` now carries a `QList<int>`:
- In `watchdogTripsOnWrongLineEnergised` replace `QCOMPARE(spy.first().at(0).toInt(), 7);` with `QCOMPARE(spy.first().at(0).value<QList<int>>(), QList<int>({ 7 }));`. The identical line in `allOffClosesEverything` reads a `zoneClosed` spy and stays.
- Replace `QCOMPARE(trippedSpy.at(0).at(0).toInt(), 8);` with `QCOMPARE(trippedSpy.at(0).at(0).value<QList<int>>(), QList<int>({ 8 }));`.
- Replace both occurrences of `QCOMPARE(trippedSpy.first().at(0).toInt(), 4);` with `QCOMPARE(trippedSpy.first().at(0).value<QList<int>>(), QList<int>({ 4 }));`.

Delete `openingAZoneClosesTheOpenOneAtomically` (declaration and definition); `aSecondZoneOpensAlongsideTheFirstInOneWrite` below replaces it.

- [ ] **Step 2: Add the new `tst_zonecontroller` tests**

Declare each in the `private slots:` list and add the definitions before `QTEST_MAIN`. Offsets from `eightZones()`: zone 1→5, 2→6, 3→12, 4→13, 5→16, 6→19, 7→20, 8→21.

```cpp
void TestZoneController::aSecondZoneOpensAlongsideTheFirstInOneWrite()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(3, 60));
    backend.resetSetValuesCallCount();
    QVERIFY(controller.openZone(5, 60));

    QCOMPARE(backend.lineValue(12), Gpio::Value::Active);
    QCOMPARE(backend.lineValue(16), Gpio::Value::Active);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 3, 5 }));
    QCOMPARE(backend.setValuesCallCount(), 1);
}

void TestZoneController::openPastTheCapIsRefusedAndClosesNothing()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QCOMPARE(controller.maxConcurrentZones(), 2);

    QVERIFY(controller.openZone(1, 60));
    QVERIFY(controller.openZone(2, 60));

    QSignalSpy closed(&controller, &ZoneController::zoneClosed);
    QVERIFY(controller.hasSlotFor(3) == false);
    QVERIFY(controller.openZone(3, 60) == false);

    QVERIFY(controller.errorText().contains("2 zones already running"));
    QCOMPARE(backend.lineValue(12), Gpio::Value::Inactive);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1, 2 }));
    QCOMPARE(closed.count(), 0);
}

void TestZoneController::reopeningAnOpenZoneResetsItsDeadlineWithoutTakingASlot()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(1, 60));
    QVERIFY(controller.openZone(2, 60));

    QSignalSpy opened(&controller, &ZoneController::zoneOpened);
    backend.resetSetValuesCallCount();

    QVERIFY(controller.hasSlotFor(2));
    QVERIFY(controller.openZone(2, 900));

    QCOMPARE(opened.count(), 1);
    QCOMPARE(opened.first().at(0).toInt(), 2);
    QCOMPARE(opened.first().at(1).toInt(), 900);
    QVERIFY(controller.secondsRemaining(2) > 60);
    QVERIFY(controller.secondsRemaining(1) <= 60);
    QCOMPARE(backend.setValuesCallCount(), 0);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1, 2 }));
}

void TestZoneController::loweringTheCapClosesNothingAndRefusesNewOpens()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    controller.setWatchdogInterval(TimeSpan::fromSeconds(60));
    QVERIFY(controller.begin());
    controller.setMaxConcurrentZones(3);
    QVERIFY(controller.openZone(1, 600));
    QVERIFY(controller.openZone(2, 600));
    QVERIFY(controller.openZone(3, 600));

    QSignalSpy closed(&controller, &ZoneController::zoneClosed);
    QSignalSpy tripped(&controller, &ZoneController::watchdogTripped);

    controller.setMaxConcurrentZones(1);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1, 2, 3 }));
    QVERIFY(controller.openZone(4, 60) == false);

    // Three lines asserted against a cap of 1, but the zones opened under a cap of 3.
    controller.triggerWatchdogForTest();
    QCOMPARE(tripped.count(), 0);
    QVERIFY(controller.isFaulted() == false);
    QCOMPARE(closed.count(), 0);

    QVERIFY(controller.closeZone(1));
    QVERIFY(controller.closeZone(2));
    QVERIFY(controller.openZone(4, 60) == false);
    QVERIFY(controller.closeZone(3));
    QVERIFY(controller.openZone(4, 60));
}

void TestZoneController::maxConcurrentZonesIsBoundedOneToEight()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QCOMPARE(controller.maxConcurrentZones(), ZoneController::DefaultMaxConcurrentZones);

    controller.setMaxConcurrentZones(0);
    QCOMPARE(controller.maxConcurrentZones(), 1);

    controller.setMaxConcurrentZones(9);
    QCOMPARE(controller.maxConcurrentZones(), 8);
}

void TestZoneController::eachZoneClosesOnItsOwnDeadline()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    controller.setWatchdogInterval(TimeSpan::fromSeconds(60));
    QVERIFY(controller.begin());

    QSignalSpy closed(&controller, &ZoneController::zoneClosed);
    QVERIFY(controller.openZone(6, 1));
    QVERIFY(controller.openZone(2, 3));

    QVERIFY(closed.wait(2500));
    QCOMPARE(closed.count(), 1);
    QCOMPARE(closed.at(0).at(0).toInt(), 6);
    QCOMPARE(closed.at(0).at(1).value<ZoneController::CloseReason>(), ZoneController::CloseReason::Deadline);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 2 }));
    QCOMPARE(backend.lineValue(19), Gpio::Value::Inactive);
    QCOMPARE(backend.lineValue(6), Gpio::Value::Active);

    QVERIFY(closed.wait(3500));
    QCOMPARE(closed.at(1).at(0).toInt(), 2);
    QVERIFY(controller.openZoneNumbers().isEmpty());
}

void TestZoneController::secondsRemainingIsPerZone()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(1, 100));
    QVERIFY(controller.openZone(2, 500));

    QVERIFY(controller.secondsRemaining(1) >= 98 && controller.secondsRemaining(1) <= 100);
    QVERIFY(controller.secondsRemaining(2) >= 498 && controller.secondsRemaining(2) <= 500);
    QCOMPARE(controller.secondsRemaining(3), 0);
}

void TestZoneController::closeZoneClosesOnlyThatZoneWithReasonStopped()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(3, 600));
    QVERIFY(controller.openZone(5, 600));

    QSignalSpy closed(&controller, &ZoneController::zoneClosed);
    backend.resetSetValuesCallCount();

    QVERIFY(controller.closeZone(3));

    QCOMPARE(closed.count(), 1);
    QCOMPARE(closed.first().at(0).toInt(), 3);
    QCOMPARE(closed.first().at(1).value<ZoneController::CloseReason>(), ZoneController::CloseReason::Stopped);
    QCOMPARE(backend.lineValue(12), Gpio::Value::Inactive);
    QCOMPARE(backend.lineValue(16), Gpio::Value::Active);
    QCOMPARE(backend.setValuesCallCount(), 1);
    QVERIFY(controller.closeTimerActiveForTest(3) == false);
    QVERIFY(controller.closeTimerActiveForTest(5));

    QVERIFY(controller.closeZone(3));
    QCOMPARE(closed.count(), 1);
}

void TestZoneController::aRetriedCloseKeepsItsReason()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(3, 600));

    QSignalSpy closed(&controller, &ZoneController::zoneClosed);
    backend.setFailNextSetValues(true);
    QVERIFY(controller.closeZone(3) == false);
    QVERIFY(controller.closeTimerActiveForTest(3));
    QCOMPARE(closed.count(), 0);

    controller.expireCloseTimerForTest(3);

    QCOMPARE(closed.count(), 1);
    QCOMPARE(closed.first().at(1).value<ZoneController::CloseReason>(), ZoneController::CloseReason::Stopped);
    QCOMPARE(backend.lineValue(12), Gpio::Value::Inactive);
}

void TestZoneController::allOffReportsEveryOpenZoneWithReasonAllOff()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(4, 600));
    QVERIFY(controller.openZone(7, 600));

    QSignalSpy closed(&controller, &ZoneController::zoneClosed);
    backend.resetSetValuesCallCount();
    QVERIFY(controller.allOff());

    QCOMPARE(closed.count(), 2);
    QCOMPARE(closed.at(0).at(0).toInt(), 4);
    QCOMPARE(closed.at(1).at(0).toInt(), 7);
    QCOMPARE(closed.at(0).at(1).value<ZoneController::CloseReason>(), ZoneController::CloseReason::AllOff);
    QCOMPARE(closed.at(1).at(1).value<ZoneController::CloseReason>(), ZoneController::CloseReason::AllOff);
    QCOMPARE(backend.setValuesCallCount(), 1);
    for(quint32 offset : eightZones().values()) {
        QCOMPARE(backend.lineValue(offset), Gpio::Value::Inactive);
    }
}

void TestZoneController::watchdogTripReportsEveryOpenZoneWithReasonWatchdog()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(2, 600));
    QVERIFY(controller.openZone(8, 600));

    backend.setLineValue(13, Gpio::Value::Active);

    QSignalSpy closed(&controller, &ZoneController::zoneClosed);
    QSignalSpy tripped(&controller, &ZoneController::watchdogTripped);
    controller.triggerWatchdogForTest();

    QCOMPARE(tripped.count(), 1);
    QCOMPARE(tripped.first().at(0).value<QList<int>>(), QList<int>({ 2, 8 }));
    QCOMPARE(closed.count(), 2);
    QCOMPARE(closed.at(0).at(1).value<ZoneController::CloseReason>(), ZoneController::CloseReason::Watchdog);
    QCOMPARE(closed.at(1).at(1).value<ZoneController::CloseReason>(), ZoneController::CloseReason::Watchdog);
    QVERIFY(controller.isFaulted());
}

void TestZoneController::watchdogComparesTheWholeExpectedSet()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(2, 600));
    QVERIFY(controller.openZone(8, 600));

    QSignalSpy tripped(&controller, &ZoneController::watchdogTripped);
    controller.triggerWatchdogForTest();
    QCOMPARE(tripped.count(), 0);

    // Zone 2's line still reads active; only zone 8's has dropped out.
    backend.setLineValue(21, Gpio::Value::Inactive);
    controller.triggerWatchdogForTest();

    QCOMPARE(tripped.count(), 1);
    QCOMPARE(backend.lineValue(6), Gpio::Value::Inactive);
    QVERIFY(controller.openZoneNumbers().isEmpty());
}

void TestZoneController::aZoneOpenedFromAClosedSlotNeverReenergisesTheClosedZone()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(1, 600));
    QVERIFY(controller.openZone(2, 600));

    connect(&controller, &ZoneController::zoneClosed, &controller,
            [&controller](int zoneNumber, ZoneController::CloseReason reason)
    {
        if(zoneNumber == 1 && reason == ZoneController::CloseReason::Deadline) {
            QVERIFY(controller.openZone(3, 600));
        }
    });

    controller.expireCloseTimerForTest(1);

    QCOMPARE(backend.lineValue(5), Gpio::Value::Inactive);
    QCOMPARE(backend.lineValue(6), Gpio::Value::Active);
    QCOMPARE(backend.lineValue(12), Gpio::Value::Active);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 2, 3 }));
}

void TestZoneController::aZoneOpenedFromAnAllOffSlotLeavesEveryClosedZoneOff()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(1, 600));
    QVERIFY(controller.openZone(2, 600));

    bool reopened = false;
    connect(&controller, &ZoneController::zoneClosed, &controller,
            [&controller, &reopened](int zoneNumber, ZoneController::CloseReason reason)
    {
        if(zoneNumber == 1 && reason == ZoneController::CloseReason::AllOff) {
            reopened = controller.openZone(5, 600);
        }
    });

    QVERIFY(controller.allOff());

    QVERIFY(reopened);
    QCOMPARE(backend.lineValue(5), Gpio::Value::Inactive);
    QCOMPARE(backend.lineValue(6), Gpio::Value::Inactive);
    QCOMPARE(backend.lineValue(16), Gpio::Value::Active);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 5 }));
}

void TestZoneController::aZoneClosingOnItsOwnTimerNeverTripsAFastWatchdog()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    controller.setWatchdogInterval(TimeSpan::fromMilliseconds(50));
    QVERIFY(controller.begin());

    QSignalSpy closed(&controller, &ZoneController::zoneClosed);
    QSignalSpy tripped(&controller, &ZoneController::watchdogTripped);
    QVERIFY(controller.openZone(1, 1));
    QVERIFY(controller.openZone(2, 2));

    QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 2, 4000);
    QCOMPARE(tripped.count(), 0);
    QVERIFY(controller.isFaulted() == false);
    QCOMPARE(closed.at(0).at(1).value<ZoneController::CloseReason>(), ZoneController::CloseReason::Deadline);
    QCOMPARE(closed.at(1).at(1).value<ZoneController::CloseReason>(), ZoneController::CloseReason::Deadline);
}
```

- [ ] **Step 3: Replace `IrrigationD/tests/tst_programrunner.cpp`**

```cpp
#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <Kanoop/pi/mockbackend.h>

#include "database/irrigationdatasource.h"
#include "model/zone.h"
#include "programrunner.h"
#include "zonecontroller.h"

class RefusingBackend : public MockBackend
{
public:
    quint32 refusedOffset = 0;
    bool refuse = false;

    virtual bool setValues(Gpio::RequestHandle handle, const QList<quint32>& offsets, const QList<Gpio::Value>& values) override
    {
        for(int i = 0; refuse == true && i < offsets.count(); i++) {
            if(offsets.at(i) == refusedOffset && values.at(i) == Gpio::Value::Active) {
                setErrorText("injected refusal");
                return false;
            }
        }
        return MockBackend::setValues(handle, offsets, values);
    }
};

static QMap<int, quint32> eightZones()
{
    return { {1,5}, {2,6}, {3,12}, {4,13}, {5,16}, {6,19}, {7,20}, {8,21} };
}

struct StepSpec
{
    QList<int> zoneNumbers;
    int durationSeconds = 0;
    int sequence = 0;
};

class Bench
{
public:
    Bench() : source(dir.filePath("irrigation.db")) {}

    // Renumbers every zone id past its manifold number so a step that stored a zone
    // number where it should store an id names no zone at all.
    bool begin()
    {
        if(source.open() == false || backend.openChipByLabel("mock") == false) {
            return false;
        }
        bool ok = false;
        source.rawQuery("UPDATE zones SET id = id + 1000", &ok);
        return ok;
    }

    int zoneIdFor(int zoneNumber)
    {
        for(const Zone& zone : source.allZones()) {
            if(zone.number == zoneNumber) {
                return zone.id;
            }
        }
        return 0;
    }

    bool setZoneEnabled(int zoneNumber, bool enabled)
    {
        bool ok = false;
        source.rawQuery(QString("UPDATE zones SET enabled = %1 WHERE number = %2").arg(enabled ? 1 : 0).arg(zoneNumber), &ok);
        return ok;
    }

    // A zero sequence means the step's list position plus one.
    int buildProgram(const QList<StepSpec>& steps, const QString& name = "Test")
    {
        Program program;
        program.name = name;
        if(source.insertProgram(program) == false) {
            return 0;
        }

        for(int i = 0; i < steps.count(); i++) {
            ProgramStep step;
            step.programId = program.id;
            step.sequence = steps.at(i).sequence != 0 ? steps.at(i).sequence : i + 1;
            step.durationSeconds = steps.at(i).durationSeconds;
            for(int zoneNumber : steps.at(i).zoneNumbers) {
                step.zoneIds.append(zoneIdFor(zoneNumber));
            }
            if(source.insertProgramStep(step) == false) {
                return 0;
            }
        }
        return program.id;
    }

    QTemporaryDir dir;
    IrrigationDataSource source;
    RefusingBackend backend;
};

class TestProgramRunner : public QObject
{
    Q_OBJECT
private slots:
    void walksStepsInSequenceOrder();
    void opensEveryZoneOfAStepTogether();
    void aStepLargerThanTheCapRunsInWaves();
    void waitingZonesOpenInAscendingZoneNumber();
    void stepCompletesOnlyWhenEveryZoneHasOpenedAndClosed();
    void takesOverAManuallyOpenZoneAndResetsItsDeadline();
    void perZoneStopCountsAsFinishingTheZonesShare();
    void aManualZoneClosingFreesASlotForAWaitingZone();
    void allOffAbortsTheProgramAndOpensNothing();
    void watchdogTripAbortsTheProgram();
    void disabledZoneIsSkipped();
    void aStepOfOnlyDisabledZonesCompletesImmediately();
    void abortClosesOnlyTheProgramsZones();
    void firstOpenFailureReturnsFalseAndAborts();
    void openFailureMidProgramAborts();
    void fillSlotsOpensWaitingZonesAfterTheCapRises();
    void startingWhileRunningIsRejected();
    void aProgramWithNoStepsFinishesImmediately();
    void aZoneInTwoStepsWatersInEach();
    void reportsItsNameStepAndWaitingZones();
    void abortWhileIdleIsANoOp();
};

void TestProgramRunner::walksStepsInSequenceOrder()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    // Insertion order (2, 7, 5), sequence order (7, 5, 2) and zone-number order (2, 5, 7)
    // are pairwise different.
    const int programId = bench.buildProgram({
        StepSpec{ { 2 }, 613, 3 },
        StepSpec{ { 7 }, 617, 1 },
        StepSpec{ { 5 }, 619, 2 },
    });
    QVERIFY(programId > 0);

    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy opened(&controller, &ZoneController::zoneOpened);
    QSignalSpy finished(&runner, &ProgramRunner::programFinished);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 7 }));
    QCOMPARE(opened.at(0).at(1).toInt(), 617);

    controller.expireCloseTimerForTest(7);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 5 }));
    QCOMPARE(opened.at(1).at(1).toInt(), 619);

    controller.expireCloseTimerForTest(5);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 2 }));
    QCOMPARE(opened.at(2).at(1).toInt(), 613);

    controller.expireCloseTimerForTest(2);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.first().at(0).toInt(), programId);
    QVERIFY(runner.isRunning() == false);
}

void TestProgramRunner::opensEveryZoneOfAStepTogether()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    controller.setMaxConcurrentZones(3);

    const int programId = bench.buildProgram({ StepSpec{ { 4, 1, 6 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy opened(&controller, &ZoneController::zoneOpened);

    QVERIFY(runner.startProgram(programId));

    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1, 4, 6 }));
    QCOMPARE(opened.count(), 3);
    for(const QList<QVariant>& arguments : opened) {
        QCOMPARE(arguments.at(1).toInt(), 600);
    }
    QVERIFY(runner.waitingZones().isEmpty());
}

void TestProgramRunner::aStepLargerThanTheCapRunsInWaves()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = bench.buildProgram({ StepSpec{ { 1, 2, 3 }, 300 } });
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy opened(&controller, &ZoneController::zoneOpened);
    QSignalSpy finished(&runner, &ProgramRunner::programFinished);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1, 2 }));
    QCOMPARE(runner.waitingZones(), QList<int>({ 3 }));

    controller.expireCloseTimerForTest(1);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 2, 3 }));
    QCOMPARE(opened.last().at(0).toInt(), 3);
    QCOMPARE(opened.last().at(1).toInt(), 300);
    QVERIFY(runner.waitingZones().isEmpty());

    controller.expireCloseTimerForTest(2);
    QCOMPARE(finished.count(), 0);
    controller.expireCloseTimerForTest(3);
    QCOMPARE(finished.count(), 1);
}

void TestProgramRunner::waitingZonesOpenInAscendingZoneNumber()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    controller.setMaxConcurrentZones(1);

    const int programId = bench.buildProgram({ StepSpec{ { 7, 2, 5 }, 120 } });
    ProgramRunner runner(&controller, &bench.source);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 2 }));
    QCOMPARE(runner.waitingZones(), QList<int>({ 5, 7 }));

    controller.expireCloseTimerForTest(2);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 5 }));

    controller.expireCloseTimerForTest(5);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 7 }));
}

void TestProgramRunner::stepCompletesOnlyWhenEveryZoneHasOpenedAndClosed()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = bench.buildProgram({ StepSpec{ { 1, 2 }, 300 }, StepSpec{ { 3 }, 200 } });
    ProgramRunner runner(&controller, &bench.source);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(runner.stepNumber(), 1);

    controller.expireCloseTimerForTest(1);
    QCOMPARE(runner.stepNumber(), 1);
    QVERIFY(controller.isOpen(3) == false);

    controller.expireCloseTimerForTest(2);
    QCOMPARE(runner.stepNumber(), 2);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 3 }));
}

void TestProgramRunner::takesOverAManuallyOpenZoneAndResetsItsDeadline()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(3, 30));

    const int programId = bench.buildProgram({ StepSpec{ { 3 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy opened(&controller, &ZoneController::zoneOpened);

    QVERIFY(runner.startProgram(programId));

    QCOMPARE(opened.count(), 1);
    QCOMPARE(opened.first().at(0).toInt(), 3);
    QCOMPARE(opened.first().at(1).toInt(), 600);
    QVERIFY(controller.secondsRemaining(3) > 30);
    QVERIFY(runner.ownsZone(3));
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 3 }));

    // The takeover took no slot: a second zone still fits under the cap of 2.
    QVERIFY(controller.openZone(8, 60));
    QVERIFY(runner.ownsZone(8) == false);
}

void TestProgramRunner::perZoneStopCountsAsFinishingTheZonesShare()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = bench.buildProgram({ StepSpec{ { 1, 2 }, 600 }, StepSpec{ { 4 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);

    QVERIFY(runner.startProgram(programId));

    QVERIFY(controller.closeZone(1));
    QCOMPARE(runner.stepNumber(), 1);
    QVERIFY(runner.isRunning());

    QVERIFY(controller.closeZone(2));
    QCOMPARE(runner.stepNumber(), 2);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 4 }));
    QCOMPARE(aborted.count(), 0);
}

void TestProgramRunner::aManualZoneClosingFreesASlotForAWaitingZone()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(7, 600));
    QVERIFY(controller.openZone(8, 600));

    const int programId = bench.buildProgram({ StepSpec{ { 1 }, 300 } });
    ProgramRunner runner(&controller, &bench.source);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 7, 8 }));
    QCOMPARE(runner.waitingZones(), QList<int>({ 1 }));

    QVERIFY(controller.closeZone(8));
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1, 7 }));
    QVERIFY(runner.ownsZone(1));
    QVERIFY(runner.waitingZones().isEmpty());
}

void TestProgramRunner::allOffAbortsTheProgramAndOpensNothing()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(8, 600));
    const int programId = bench.buildProgram({ StepSpec{ { 1, 2 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1, 8 }));
    QCOMPARE(runner.waitingZones(), QList<int>({ 2 }));

    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);
    QSignalSpy opened(&controller, &ZoneController::zoneOpened);
    QVERIFY(controller.allOff());

    QCOMPARE(aborted.count(), 1);
    QCOMPARE(opened.count(), 0);
    QVERIFY(controller.openZoneNumbers().isEmpty());
    QVERIFY(runner.isRunning() == false);
}

void TestProgramRunner::watchdogTripAbortsTheProgram()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = bench.buildProgram({ StepSpec{ { 1 }, 600 }, StepSpec{ { 2 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);
    QVERIFY(runner.startProgram(programId));

    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);
    QSignalSpy finished(&runner, &ProgramRunner::programFinished);
    bench.backend.setLineValue(13, Gpio::Value::Active);
    controller.triggerWatchdogForTest();

    QCOMPARE(aborted.count(), 1);
    QCOMPARE(finished.count(), 0);
    QVERIFY(runner.isRunning() == false);
    QVERIFY(controller.openZoneNumbers().isEmpty());
}

void TestProgramRunner::disabledZoneIsSkipped()
{
    Bench bench;
    QVERIFY(bench.begin());
    QVERIFY(bench.setZoneEnabled(3, false));
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = bench.buildProgram({ StepSpec{ { 1, 3 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy finished(&runner, &ProgramRunner::programFinished);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1 }));
    QVERIFY(runner.waitingZones().isEmpty());

    controller.expireCloseTimerForTest(1);
    QCOMPARE(finished.count(), 1);
}

void TestProgramRunner::aStepOfOnlyDisabledZonesCompletesImmediately()
{
    Bench bench;
    QVERIFY(bench.begin());
    QVERIFY(bench.setZoneEnabled(3, false));
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = bench.buildProgram({ StepSpec{ { 3 }, 600 }, StepSpec{ { 5 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(runner.stepNumber(), 2);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 5 }));
}

void TestProgramRunner::abortClosesOnlyTheProgramsZones()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(8, 600));
    const int programId = bench.buildProgram({ StepSpec{ { 1 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);

    QVERIFY(runner.startProgram(programId));
    runner.abort();

    QCOMPARE(aborted.count(), 1);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 8 }));
}

void TestProgramRunner::firstOpenFailureReturnsFalseAndAborts()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = bench.buildProgram({ StepSpec{ { 1 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);

    bench.backend.refusedOffset = 5;
    bench.backend.refuse = true;
    QVERIFY(runner.startProgram(programId) == false);

    QCOMPARE(aborted.count(), 1);
    QVERIFY(runner.isRunning() == false);
    QVERIFY(controller.openZoneNumbers().isEmpty());
}

void TestProgramRunner::openFailureMidProgramAborts()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = bench.buildProgram({ StepSpec{ { 1 }, 600 }, StepSpec{ { 2 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);

    QVERIFY(runner.startProgram(programId));
    bench.backend.refusedOffset = 6;
    bench.backend.refuse = true;
    controller.expireCloseTimerForTest(1);

    QCOMPARE(aborted.count(), 1);
    QVERIFY(runner.isRunning() == false);
    QVERIFY(controller.openZoneNumbers().isEmpty());
}

void TestProgramRunner::fillSlotsOpensWaitingZonesAfterTheCapRises()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    controller.setMaxConcurrentZones(1);

    const int programId = bench.buildProgram({ StepSpec{ { 1, 2 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(runner.waitingZones(), QList<int>({ 2 }));

    controller.setMaxConcurrentZones(2);
    runner.fillSlots();

    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1, 2 }));
    QVERIFY(runner.waitingZones().isEmpty());
}

void TestProgramRunner::startingWhileRunningIsRejected()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int first = bench.buildProgram({ StepSpec{ { 1 }, 600 } }, "First");
    const int second = bench.buildProgram({ StepSpec{ { 2 }, 600 } }, "Second");
    ProgramRunner runner(&controller, &bench.source);

    QVERIFY(runner.startProgram(first));
    QVERIFY(runner.startProgram(second) == false);
    QCOMPARE(runner.runningProgramId(), first);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1 }));
}

void TestProgramRunner::aProgramWithNoStepsFinishesImmediately()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = bench.buildProgram({});
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy finished(&runner, &ProgramRunner::programFinished);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(finished.count(), 1);
    QVERIFY(runner.isRunning() == false);
}

void TestProgramRunner::aZoneInTwoStepsWatersInEach()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = bench.buildProgram({ StepSpec{ { 2 }, 600 }, StepSpec{ { 2 }, 300 } });
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy opened(&controller, &ZoneController::zoneOpened);

    QVERIFY(runner.startProgram(programId));
    controller.expireCloseTimerForTest(2);

    QCOMPARE(opened.count(), 2);
    QCOMPARE(opened.at(1).at(0).toInt(), 2);
    QCOMPARE(opened.at(1).at(1).toInt(), 300);
}

void TestProgramRunner::reportsItsNameStepAndWaitingZones()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(8, 600));
    const int programId = bench.buildProgram({ StepSpec{ { 5, 6, 7 }, 600 }, StepSpec{ { 1 }, 600 } }, "Morning Drip");
    ProgramRunner runner(&controller, &bench.source);

    QCOMPARE(runner.stepNumber(), 0);
    QCOMPARE(runner.stepCount(), 0);

    QVERIFY(runner.startProgram(programId));

    QCOMPARE(runner.runningProgramName(), QString("Morning Drip"));
    QCOMPARE(runner.stepNumber(), 1);
    QCOMPARE(runner.stepCount(), 2);
    QCOMPARE(runner.waitingZones(), QList<int>({ 6, 7 }));
    QVERIFY(runner.ownsZone(5));
    QVERIFY(runner.ownsZone(8) == false);
}

void TestProgramRunner::abortWhileIdleIsANoOp()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);
    runner.abort();
    QCOMPARE(aborted.count(), 0);
}

QTEST_MAIN(TestProgramRunner)
#include "tst_programrunner.moc"
```

- [ ] **Step 4: Build and run both suites**

```bash
cmake --build build -j 32
ctest --test-dir build --output-on-failure -R 'tst_zonecontroller|tst_programrunner'
```

Expected: both pass. A test that fails here is a finding against Task 1 or Task 3: fix the code, never loosen the assertion, and commit the fix separately as `fix:`.

- [ ] **Step 5: Commit**

```bash
git add IrrigationD/tests/tst_zonecontroller.cpp IrrigationD/tests/tst_programrunner.cpp
git commit -F - <<'EOF'
test: cover the zone cap, per-zone closes and stepped programs

The zone controller suite covers the cap and its refusal, re-open
without a slot, lowering the cap, per-zone deadlines and reasons, the
watchdog's set comparison, a retried close keeping its reason, a slot
opening a zone from inside zoneClosed, and close timers that never trip
a fast watchdog. The runner suite is rewritten for steps: waves,
ascending waiting order, takeover of a manual zone, per-zone stop,
manual closes freeing slots, all-off and watchdog aborts, and the
status accessors.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---

### Task 13: Daemon tests — the queue, the scheduler, the repository and the migration

Spec §8: queue order, duplicate suppression, the rain/master re-check at dequeue, STOP emptying the queue with the right outcomes, `dropped_restart` recording at startup, the step migration against a populated database. Review Focus 2 and 5.

**Files:**
- Create: `IrrigationD/tests/tst_programqueue.cpp`
- Modify: `IrrigationD/tests/CMakeLists.txt`
- Modify: `IrrigationD/tests/tst_scheduler.cpp`, `IrrigationD/tests/tst_repository.cpp`, `IrrigationD/tests/tst_datasource.cpp`

**Interfaces:**
- Consumes: Task 2, 3 and 4 interfaces as produced; `TestClock` from `iclock.h`.

- [ ] **Step 1: Register `tst_programqueue` in `IrrigationD/tests/CMakeLists.txt`**

Add after the `tst_programrunner` block:

```cmake
irrigation_add_test(tst_programqueue
    tst_programqueue.cpp
    ../src/programqueue.cpp
    ../src/programrunner.cpp
    ../src/runrequest.cpp
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

- [ ] **Step 2: Create `IrrigationD/tests/tst_programqueue.cpp`**

```cpp
#include <QTest>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimeZone>

#include <Kanoop/pi/mockbackend.h>

#include "database/irrigationdatasource.h"
#include "iclock.h"
#include "model/zone.h"
#include "programqueue.h"
#include "programrunner.h"
#include "zonecontroller.h"

static QMap<int, quint32> eightZones()
{
    return { {1,5}, {2,6}, {3,12}, {4,13}, {5,16}, {6,19}, {7,20}, {8,21} };
}

class Rig
{
public:
    Rig() :
        source(dir.filePath("irrigation.db")),
        clock(QDateTime(QDate(2026, 10, 1), QTime(13, 0), QTimeZone::UTC))
    {
    }

    bool begin()
    {
        return source.open() && backend.openChipByLabel("mock");
    }

    // One step of one zone for 600 s; the zone number doubles as the way a test finishes it.
    int buildProgram(const QString& name, int zoneNumber)
    {
        Program program;
        program.name = name;
        if(source.insertProgram(program) == false) {
            return 0;
        }

        ProgramStep step;
        step.programId = program.id;
        step.sequence = 1;
        step.durationSeconds = 600;
        for(const Zone& zone : source.allZones()) {
            if(zone.number == zoneNumber) {
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

    int firingCount()
    {
        bool ok = false;
        QSqlQuery query = source.rawQuery("SELECT COUNT(*) FROM fired_instants", &ok);
        return ok == true && query.next() == true ? query.value(0).toInt() : -1;
    }

    QTemporaryDir dir;
    IrrigationDataSource source;
    MockBackend backend;
    TestClock clock;
};

static const QDateTime DueAt = QDateTime(QDate(2026, 10, 1), QTime(13, 0), QTimeZone::UTC);

class TestProgramQueue : public QObject
{
    Q_OBJECT
private slots:
    void startsImmediatelyWhenIdleAndRecordsRan();
    void queuesBehindARunningProgramAndLeavesItQueued();
    void runsQueuedProgramsFirstInFirstOut();
    void aSecondDueTimeForAQueuedProgramIsSkippedDuplicate();
    void aRunningProgramMayHoldOneQueuedEntry();
    void manualRunStartsWhenIdleAndWritesNoFiring();
    void manualRunIsRefusedWhileRunningOrQueued();
    void rainDelaySetWhileWaitingSkipsAtDequeue();
    void masterDisabledWhileWaitingSkipsAtDequeue();
    void manualEntryIgnoresTheRainDelayAtDequeue();
    void dropAllRecordsDroppedStopForScheduledEntries();
    void dropAllBeforeAbortLeavesEveryValveClosed();
    void abortingWithAQueuedEntryStartsIt();
    void aDeletedQueueStartsNothingWhenTheRunnerAborts();
    void recordRestartDropsMarksOnlyQueuedFirings();
    void entriesReportHeadFirstWithQueueTimes();
};

void TestProgramQueue::startsImmediatelyWhenIdleAndRecordsRan()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int programId = rig.buildProgram("Front", 1);
    QVERIFY(rig.insertQueuedFiring(programId, 31, DueAt));

    queue.enqueueScheduled(programId, 31, DueAt);

    QCOMPARE(runner.runningProgramId(), programId);
    QCOMPARE(rig.outcomeFor(programId, 31), QString("ran"));
    QVERIFY(queue.entries().isEmpty());
}

void TestProgramQueue::queuesBehindARunningProgramAndLeavesItQueued()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int first = rig.buildProgram("Front", 1);
    const int second = rig.buildProgram("Back", 2);
    QVERIFY(rig.insertQueuedFiring(first, 31, DueAt));
    QVERIFY(rig.insertQueuedFiring(second, 32, DueAt));

    queue.enqueueScheduled(first, 31, DueAt);
    queue.enqueueScheduled(second, 32, DueAt);

    QCOMPARE(runner.runningProgramId(), first);
    QCOMPARE(queue.entries().count(), 1);
    QCOMPARE(queue.entries().first().programId, second);
    QCOMPARE(rig.outcomeFor(second, 32), QString("queued"));
    QVERIFY(controller.isOpen(2) == false);
}

void TestProgramQueue::runsQueuedProgramsFirstInFirstOut()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int a = rig.buildProgram("A", 1);
    const int b = rig.buildProgram("B", 2);
    const int c = rig.buildProgram("C", 3);
    for(int programId : { a, b, c }) {
        QVERIFY(rig.insertQueuedFiring(programId, programId + 100, DueAt));
        queue.enqueueScheduled(programId, programId + 100, DueAt);
    }

    QCOMPARE(runner.runningProgramId(), a);
    controller.expireCloseTimerForTest(1);
    QCOMPARE(runner.runningProgramId(), b);
    controller.expireCloseTimerForTest(2);
    QCOMPARE(runner.runningProgramId(), c);

    QCOMPARE(rig.outcomeFor(a, a + 100), QString("ran"));
    QCOMPARE(rig.outcomeFor(b, b + 100), QString("ran"));
    QCOMPARE(rig.outcomeFor(c, c + 100), QString("ran"));
}

void TestProgramQueue::aSecondDueTimeForAQueuedProgramIsSkippedDuplicate()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int running = rig.buildProgram("Running", 1);
    const int waiting = rig.buildProgram("Waiting", 2);
    QVERIFY(rig.insertQueuedFiring(running, 31, DueAt));
    QVERIFY(rig.insertQueuedFiring(waiting, 32, DueAt));
    QVERIFY(rig.insertQueuedFiring(waiting, 33, DueAt.addSecs(60)));

    queue.enqueueScheduled(running, 31, DueAt);
    queue.enqueueScheduled(waiting, 32, DueAt);
    queue.enqueueScheduled(waiting, 33, DueAt.addSecs(60));

    QCOMPARE(queue.entries().count(), 1);
    QCOMPARE(rig.outcomeFor(waiting, 32), QString("queued"));
    QCOMPARE(rig.outcomeFor(waiting, 33), QString("skipped_duplicate"));
}

void TestProgramQueue::aRunningProgramMayHoldOneQueuedEntry()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int programId = rig.buildProgram("Twice", 1);
    QVERIFY(rig.insertQueuedFiring(programId, 31, DueAt));
    QVERIFY(rig.insertQueuedFiring(programId, 32, DueAt.addSecs(300)));

    queue.enqueueScheduled(programId, 31, DueAt);
    queue.enqueueScheduled(programId, 32, DueAt.addSecs(300));

    QCOMPARE(queue.entries().count(), 1);
    QCOMPARE(rig.outcomeFor(programId, 32), QString("queued"));

    QSignalSpy started(&runner, &ProgramRunner::programStarted);
    controller.expireCloseTimerForTest(1);

    QCOMPARE(started.count(), 1);
    QCOMPARE(runner.runningProgramId(), programId);
    QCOMPARE(rig.outcomeFor(programId, 32), QString("ran"));
}

void TestProgramQueue::manualRunStartsWhenIdleAndWritesNoFiring()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int programId = rig.buildProgram("Manual", 4);

    QCOMPARE(queue.enqueueManual(programId), RunRequest::Refusal::None);
    QCOMPARE(runner.runningProgramId(), programId);
    QCOMPARE(rig.firingCount(), 0);
}

void TestProgramQueue::manualRunIsRefusedWhileRunningOrQueued()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int running = rig.buildProgram("Running", 1);
    const int waiting = rig.buildProgram("Waiting", 2);

    QCOMPARE(queue.enqueueManual(running), RunRequest::Refusal::None);
    QCOMPARE(queue.enqueueManual(running), RunRequest::Refusal::AlreadyQueued);
    QCOMPARE(queue.enqueueManual(waiting), RunRequest::Refusal::None);
    QCOMPARE(queue.enqueueManual(waiting), RunRequest::Refusal::AlreadyQueued);
    QCOMPARE(queue.entries().count(), 1);
}

void TestProgramQueue::rainDelaySetWhileWaitingSkipsAtDequeue()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int first = rig.buildProgram("First", 1);
    const int second = rig.buildProgram("Second", 2);
    QVERIFY(rig.insertQueuedFiring(first, 31, DueAt));
    QVERIFY(rig.insertQueuedFiring(second, 32, DueAt));
    queue.enqueueScheduled(first, 31, DueAt);
    queue.enqueueScheduled(second, 32, DueAt);

    QVERIFY(rig.source.setSettingValue("rain_delay_until", DueAt.addDays(1).toString(Qt::ISODate)));
    controller.expireCloseTimerForTest(1);

    QVERIFY(runner.isRunning() == false);
    QVERIFY(controller.openZoneNumbers().isEmpty());
    QCOMPARE(rig.outcomeFor(second, 32), QString("skipped_rain"));
    QVERIFY(queue.entries().isEmpty());
}

void TestProgramQueue::masterDisabledWhileWaitingSkipsAtDequeue()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int first = rig.buildProgram("First", 1);
    const int second = rig.buildProgram("Second", 2);
    QVERIFY(rig.insertQueuedFiring(first, 31, DueAt));
    QVERIFY(rig.insertQueuedFiring(second, 32, DueAt));
    queue.enqueueScheduled(first, 31, DueAt);
    queue.enqueueScheduled(second, 32, DueAt);

    QVERIFY(rig.source.setSettingValue("master_enabled", "0"));
    controller.expireCloseTimerForTest(1);

    QVERIFY(runner.isRunning() == false);
    QCOMPARE(rig.outcomeFor(second, 32), QString("skipped_disabled"));
}

void TestProgramQueue::manualEntryIgnoresTheRainDelayAtDequeue()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int first = rig.buildProgram("First", 1);
    const int second = rig.buildProgram("Second", 2);
    QCOMPARE(queue.enqueueManual(first), RunRequest::Refusal::None);
    QCOMPARE(queue.enqueueManual(second), RunRequest::Refusal::None);

    QVERIFY(rig.source.setSettingValue("rain_delay_until", DueAt.addDays(1).toString(Qt::ISODate)));
    controller.expireCloseTimerForTest(1);

    QCOMPARE(runner.runningProgramId(), second);
    QVERIFY(controller.isOpen(2));
}

void TestProgramQueue::dropAllRecordsDroppedStopForScheduledEntries()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int running = rig.buildProgram("Running", 1);
    const int scheduled = rig.buildProgram("Scheduled", 2);
    const int manual = rig.buildProgram("Manual", 3);
    QVERIFY(rig.insertQueuedFiring(running, 31, DueAt));
    QVERIFY(rig.insertQueuedFiring(scheduled, 32, DueAt));
    queue.enqueueScheduled(running, 31, DueAt);
    queue.enqueueScheduled(scheduled, 32, DueAt);
    QCOMPARE(queue.enqueueManual(manual), RunRequest::Refusal::None);
    QCOMPARE(queue.entries().count(), 2);

    queue.dropAll(FiredInstant::Outcome::DroppedStop);

    QVERIFY(queue.entries().isEmpty());
    QCOMPARE(rig.outcomeFor(scheduled, 32), QString("dropped_stop"));
    QCOMPARE(rig.outcomeFor(running, 31), QString("ran"));
    QCOMPARE(rig.firingCount(), 2);
}

void TestProgramQueue::dropAllBeforeAbortLeavesEveryValveClosed()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int running = rig.buildProgram("Running", 1);
    const int waiting = rig.buildProgram("Waiting", 2);
    QVERIFY(rig.insertQueuedFiring(running, 31, DueAt));
    QVERIFY(rig.insertQueuedFiring(waiting, 32, DueAt));
    queue.enqueueScheduled(running, 31, DueAt);
    queue.enqueueScheduled(waiting, 32, DueAt);

    queue.dropAll(FiredInstant::Outcome::DroppedStop);
    runner.abort();
    QVERIFY(controller.allOff());

    QVERIFY(runner.isRunning() == false);
    QVERIFY(controller.openZoneNumbers().isEmpty());
    QCOMPARE(rig.outcomeFor(waiting, 32), QString("dropped_stop"));
}

void TestProgramQueue::abortingWithAQueuedEntryStartsIt()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int running = rig.buildProgram("Running", 1);
    const int waiting = rig.buildProgram("Waiting", 2);
    QCOMPARE(queue.enqueueManual(running), RunRequest::Refusal::None);
    QCOMPARE(queue.enqueueManual(waiting), RunRequest::Refusal::None);

    runner.abort();

    QCOMPARE(runner.runningProgramId(), waiting);
    QVERIFY(controller.isOpen(2));
}

void TestProgramQueue::aDeletedQueueStartsNothingWhenTheRunnerAborts()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue* queue = new ProgramQueue(&runner, &rig.source, &rig.clock);

    const int running = rig.buildProgram("Running", 1);
    const int waiting = rig.buildProgram("Waiting", 2);
    QCOMPARE(queue->enqueueManual(running), RunRequest::Refusal::None);
    QCOMPARE(queue->enqueueManual(waiting), RunRequest::Refusal::None);

    delete queue;
    runner.abort();

    QVERIFY(runner.isRunning() == false);
    QVERIFY(controller.openZoneNumbers().isEmpty());
}

void TestProgramQueue::recordRestartDropsMarksOnlyQueuedFirings()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int programId = rig.buildProgram("Lost", 1);
    QVERIFY(rig.insertQueuedFiring(programId, 31, DueAt));
    QVERIFY(rig.insertQueuedFiring(programId, 32, DueAt.addSecs(60)));

    FiredInstant ran;
    ran.programId = programId;
    ran.startTimeId = 33;
    ran.scheduledAtUtc = DueAt.addSecs(-3600);
    ran.outcome = FiredInstant::Outcome::Ran;
    QVERIFY(rig.source.recordFiring(ran));

    QVERIFY(queue.recordRestartDrops());

    QCOMPARE(rig.outcomeFor(programId, 31), QString("dropped_restart"));
    QCOMPARE(rig.outcomeFor(programId, 32), QString("dropped_restart"));
    QCOMPARE(rig.outcomeFor(programId, 33), QString("ran"));
}

void TestProgramQueue::entriesReportHeadFirstWithQueueTimes()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int a = rig.buildProgram("A", 1);
    const int b = rig.buildProgram("B", 2);
    const int c = rig.buildProgram("C", 3);
    QCOMPARE(queue.enqueueManual(a), RunRequest::Refusal::None);
    rig.clock.advance(5);
    QCOMPARE(queue.enqueueManual(b), RunRequest::Refusal::None);
    rig.clock.advance(5);
    QCOMPARE(queue.enqueueManual(c), RunRequest::Refusal::None);

    const QList<ProgramQueue::Entry> entries = queue.entries();
    QCOMPARE(entries.count(), 2);
    QCOMPARE(entries.at(0).programId, b);
    QCOMPARE(entries.at(0).queuedAtUtc, DueAt.addSecs(5));
    QCOMPARE(entries.at(1).programId, c);
    QCOMPARE(entries.at(1).queuedAtUtc, DueAt.addSecs(10));
    QVERIFY(entries.at(0).isScheduled() == false);
}

QTEST_MAIN(TestProgramQueue)
#include "tst_programqueue.moc"
```

- [ ] **Step 3: Flip the scheduler's due-firing outcome**

```bash
cd /home/spunak/src/punak/irrigation
sed -i 's/QString("ran")/QString("queued")/' IrrigationD/tests/tst_scheduler.cpp
grep -c 'QString("queued")' IrrigationD/tests/tst_scheduler.cpp
```

Expected: `7`. Every `ran` assertion in that file reads a row the scheduler wrote on its own, and the scheduler now writes `queued`.

- [ ] **Step 4: Add the step repository tests to `IrrigationD/tests/tst_repository.cpp`**

Declare and add before `QTEST_MAIN`:

```cpp
void TestRepository::programStepsRoundTripInSequenceOrder()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program program;
    program.name = "Backyard";
    QVERIFY(source.insertProgram(program));

    // Inserted out of sequence order; zone ids, sequences and durations are kept clear of
    // each other and of the program id so a transposed bind is observable.
    ProgramStep third;
    third.programId = program.id;
    third.sequence = 12;
    third.durationSeconds = 300;
    third.zoneIds = { 6 };
    QVERIFY(source.insertProgramStep(third));
    QVERIFY(third.id > 0);

    ProgramStep first;
    first.programId = program.id;
    first.sequence = 10;
    first.durationSeconds = 600;
    first.zoneIds = { 4, 7 };
    QVERIFY(source.insertProgramStep(first));

    ProgramStep second;
    second.programId = program.id;
    second.sequence = 11;
    second.durationSeconds = 450;
    second.zoneIds = { 8, 5, 3 };
    QVERIFY(source.insertProgramStep(second));

    bool ok = false;
    const ProgramStepList steps = source.stepsFor(program.id, &ok);
    QVERIFY(ok);
    QCOMPARE(steps.count(), 3);
    QCOMPARE(steps.at(0).id, first.id);
    QCOMPARE(steps.at(0).programId, program.id);
    QCOMPARE(steps.at(0).sequence, 10);
    QCOMPARE(steps.at(0).durationSeconds, 600);
    QCOMPARE(steps.at(0).zoneIds, QList<int>({ 4, 7 }));
    QCOMPARE(steps.at(1).id, second.id);
    QCOMPARE(steps.at(1).zoneIds, QList<int>({ 8, 5, 3 }));
    QCOMPARE(steps.at(1).durationSeconds, 450);
    QCOMPARE(steps.at(2).id, third.id);
    QCOMPARE(steps.at(2).zoneIds, QList<int>({ 6 }));
}

void TestRepository::deleteProgramStepsLeavesOtherProgramsIntact()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program doomed;
    doomed.name = "Doomed steps";
    QVERIFY(source.insertProgram(doomed));

    Program survivor;
    survivor.name = "Survivor";
    QVERIFY(source.insertProgram(survivor));

    // Two filler steps on the survivor push doomed's own step ids past both program ids.
    for(int i = 0; i < 2; i++) {
        ProgramStep filler;
        filler.programId = survivor.id;
        filler.sequence = 90 + i;
        filler.durationSeconds = 30;
        filler.zoneIds = { 1 + i };
        QVERIFY(source.insertProgramStep(filler));
    }

    ProgramStep doomedStep;
    doomedStep.programId = doomed.id;
    doomedStep.sequence = 1;
    doomedStep.durationSeconds = 120;
    doomedStep.zoneIds = { 3, 5 };
    QVERIFY(source.insertProgramStep(doomedStep));

    QVERIFY(source.deleteProgramSteps(doomed.id));

    QCOMPARE(source.stepsFor(doomed.id).count(), 0);
    QCOMPARE(source.stepsFor(survivor.id).count(), 2);

    bool ok = false;
    QSqlQuery orphans = source.rawQuery(
        QString("SELECT COUNT(*) FROM program_step_zones WHERE step_id = %1").arg(doomedStep.id), &ok);
    QVERIFY(ok);
    QVERIFY(orphans.next());
    QCOMPARE(orphans.value(0).toInt(), 0);
}

void TestRepository::deleteProgramCascadesThroughStepsAndTheirZones()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program program;
    program.name = "Doomed";
    QVERIFY(source.insertProgram(program));

    ProgramStep step;
    step.programId = program.id;
    step.sequence = 1;
    step.durationSeconds = 60;
    step.zoneIds = { 2, 4 };
    QVERIFY(source.insertProgramStep(step));

    QVERIFY(source.deleteProgram(program.id));

    bool ok = false;
    QSqlQuery steps = source.rawQuery("SELECT COUNT(*) FROM program_steps", &ok);
    QVERIFY(ok);
    QVERIFY(steps.next());
    QCOMPARE(steps.value(0).toInt(), 0);

    QSqlQuery zones = source.rawQuery("SELECT COUNT(*) FROM program_step_zones", &ok);
    QVERIFY(ok);
    QVERIFY(zones.next());
    QCOMPARE(zones.value(0).toInt(), 0);
}

void TestRepository::replaceFiringOutcomesChangesOnlyTheNamedOutcome()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    const QDateTime at(QDate(2026, 10, 1), QTime(13, 0), QTimeZone::UTC);
    const QList<FiredInstant::Outcome> outcomes = {
        FiredInstant::Outcome::Queued, FiredInstant::Outcome::Ran, FiredInstant::Outcome::Queued
    };
    for(int i = 0; i < outcomes.count(); i++) {
        FiredInstant instant;
        instant.programId = 7;
        instant.startTimeId = 40 + i;
        instant.scheduledAtUtc = at;
        instant.outcome = outcomes.at(i);
        QVERIFY(source.recordFiring(instant));
    }

    int changed = -1;
    QVERIFY(source.replaceFiringOutcomes(FiredInstant::Outcome::Queued, FiredInstant::Outcome::DroppedRestart, &changed));
    QCOMPARE(changed, 2);

    bool ok = false;
    QSqlQuery query = source.rawQuery("SELECT start_time_id, outcome FROM fired_instants ORDER BY start_time_id", &ok);
    QVERIFY(ok);
    QVERIFY(query.next());
    QCOMPARE(query.value(1).toString(), QString("dropped_restart"));
    QVERIFY(query.next());
    QCOMPARE(query.value(1).toString(), QString("ran"));
    QVERIFY(query.next());
    QCOMPARE(query.value(1).toString(), QString("dropped_restart"));
}

void TestRepository::everyOutcomeRoundTripsThroughItsStorageString_data()
{
    QTest::addColumn<int>("outcome");
    QTest::addColumn<QString>("stored");

    QTest::newRow("queued")            << static_cast<int>(FiredInstant::Outcome::Queued)           << QString("queued");
    QTest::newRow("dropped_stop")      << static_cast<int>(FiredInstant::Outcome::DroppedStop)      << QString("dropped_stop");
    QTest::newRow("dropped_restart")   << static_cast<int>(FiredInstant::Outcome::DroppedRestart)   << QString("dropped_restart");
    QTest::newRow("skipped_duplicate") << static_cast<int>(FiredInstant::Outcome::SkippedDuplicate) << QString("skipped_duplicate");
    QTest::newRow("skipped_disabled")  << static_cast<int>(FiredInstant::Outcome::SkippedDisabled)  << QString("skipped_disabled");
    QTest::newRow("skipped_busy")      << static_cast<int>(FiredInstant::Outcome::SkippedBusy)      << QString("skipped_busy");
}

void TestRepository::everyOutcomeRoundTripsThroughItsStorageString()
{
    QFETCH(int, outcome);
    QFETCH(QString, stored);

    const FiredInstant::Outcome value = static_cast<FiredInstant::Outcome>(outcome);
    QCOMPARE(FiredInstant::outcomeToString(value), stored);
    QCOMPARE(FiredInstant::outcomeFromString(stored), value);
}
```

- [ ] **Step 5: Update `IrrigationD/tests/tst_datasource.cpp`**

Add `#include <QFile>` and `#include <Kanoop/database/sqlparser.h>`.

In `createsSchemaOnFirstOpen` replace `QVERIFY(tables.contains("program_zones"));` with:

```cpp
    QVERIFY(tables.contains("program_steps"));
    QVERIFY(tables.contains("program_step_zones"));
    QVERIFY(tables.contains("program_zones") == false);
```

Add, declared after `recreatesOnMigrationFailure`:

```cpp
void TestDataSource::seedsTheZoneCap()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QCOMPARE(source.settingValue("max_concurrent_zones"), QString("2"));
}

void TestDataSource::migratesProgramZonesIntoOneZoneSteps()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("irrigation.db");

    {
        QSqlDatabase seed = QSqlDatabase::addDatabase("QSQLITE", "seed-steps-connection");
        seed.setDatabaseName(path);
        QVERIFY(seed.open());
        QSqlQuery query(seed);

        QFile initial(":/database/migrate/irrigation/1.0.0/01-initial.sql");
        QVERIFY(initial.open(QIODevice::ReadOnly));
        SqlParser parser(QString::fromUtf8(initial.readAll()));
        QVERIFY(parser.isValid());
        for(const QString& statement : parser.statements()) {
            QVERIFY2(query.exec(statement), qPrintable(query.lastError().text()));
        }

        QVERIFY(query.exec("CREATE TABLE info (id INTEGER PRIMARY KEY, sw_version TEXT NOT NULL)"));
        QVERIFY(query.exec("INSERT INTO info (id, sw_version) VALUES (1, '1.0.0')"));
        QVERIFY(query.exec("INSERT INTO programs (id, name) VALUES (40, 'Front'), (41, 'Back')"));
        // Ids, zones, sequences and durations all differ, and row 71 carries sequence 1,
        // so program 40's steps come back in the reverse of their id order.
        QVERIFY(query.exec("INSERT INTO program_zones (id, program_id, zone_id, sequence, duration_seconds) VALUES "
                           "(70, 40, 3, 2, 300), (71, 40, 6, 1, 600), (72, 41, 8, 1, 900)"));
        seed.close();
    }
    QSqlDatabase::removeDatabase("seed-steps-connection");

    IrrigationDataSource source(path);
    QVERIFY2(source.open(), qPrintable(source.errorText()));

    const QFileInfo dbInfo(path);
    QVERIFY(QDir(dbInfo.absolutePath())
                .entryList(QStringList() << dbInfo.fileName() + ".*.backup", QDir::Files).isEmpty());

    const ProgramStepList front = source.stepsFor(40);
    QCOMPARE(front.count(), 2);
    QCOMPARE(front.at(0).id, 71);
    QCOMPARE(front.at(0).sequence, 1);
    QCOMPARE(front.at(0).durationSeconds, 600);
    QCOMPARE(front.at(0).zoneIds, QList<int>({ 6 }));
    QCOMPARE(front.at(1).id, 70);
    QCOMPARE(front.at(1).sequence, 2);
    QCOMPARE(front.at(1).durationSeconds, 300);
    QCOMPARE(front.at(1).zoneIds, QList<int>({ 3 }));

    const ProgramStepList back = source.stepsFor(41);
    QCOMPARE(back.count(), 1);
    QCOMPARE(back.at(0).id, 72);
    QCOMPARE(back.at(0).durationSeconds, 900);
    QCOMPARE(back.at(0).zoneIds, QList<int>({ 8 }));

    QSqlQuery query(QSqlDatabase::database(source.connectionName()));
    QVERIFY(query.exec("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'program_zones'"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 0);

    QCOMPARE(source.settingValue("max_concurrent_zones"), QString("2"));

    QVERIFY(query.exec("SELECT sw_version FROM info WHERE id = 1"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), source.compiledDatabaseVersion());

    ProgramStep added;
    added.programId = 41;
    added.sequence = 2;
    added.durationSeconds = 60;
    added.zoneIds = { 1 };
    QVERIFY(source.insertProgramStep(added));
    QVERIFY(added.id > 72);

    QVERIFY(source.deleteProgram(40));
    QVERIFY(query.exec("SELECT COUNT(*) FROM program_step_zones WHERE step_id IN (70, 71)"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 0);
}
```

- [ ] **Step 6: Build and run the suites**

```bash
cmake --build build -j 32
ctest --test-dir build --output-on-failure -R 'tst_programqueue|tst_scheduler|tst_repository|tst_irrigationdatasource'
```

Expected: all four pass. A failure is a finding against Tasks 2–4: fix the code, never loosen the assertion, and commit the fix separately as `fix:`.

- [ ] **Step 7: Commit**

```bash
git add IrrigationD/tests/tst_programqueue.cpp IrrigationD/tests/CMakeLists.txt \
        IrrigationD/tests/tst_scheduler.cpp IrrigationD/tests/tst_repository.cpp IrrigationD/tests/tst_datasource.cpp
git commit -F - <<'EOF'
test: cover the program queue, step storage and the 1.1.0 migration

The new queue suite covers immediate starts, FIFO order, duplicate
suppression, the running program's one queued entry, manual refusals,
the rain and master re-checks at dequeue, STOP's dropped_stop, the
ordering that keeps STOP and teardown from starting the next program,
and the dropped_restart sweep. Repository tests cover steps, their
zone rows and the cascade; the data source migrates a populated 1.0.0
database into one-zone steps without recreating it. Scheduler
assertions read queued for a due firing.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---

### Task 14: Daemon tests — the control server

Spec §6: the 409 reasons, the per-zone stop route, steps in program bodies, the new status shape. Review Focus 4. `tst_controlserver` comes back from parking; most of its 3,700 lines still hold and need only the step conversion below.

**Files:**
- Modify: `IrrigationD/tests/tst_controlserver.cpp`
- Modify: `IrrigationD/tests/CMakeLists.txt` (unpark)

**Interfaces:**
- Consumes: Task 5 `ServerStatus`; Task 6 server signals, `setDecisionTimeout()`, `RunRequest`.

- [ ] **Step 1: Unpark the target**

Add to `IrrigationD/tests/CMakeLists.txt` after the `tst_scheduler` block:

```cmake
irrigation_add_test(tst_controlserver
    tst_controlserver.cpp
    ../src/irrigationcontrolserver.cpp
    ../src/json/statusjson.cpp
    ../src/json/programjson.cpp
    ../src/scheduler.cpp
    ../src/runrequest.cpp
    ../src/database/irrigationdatasource.cpp
    ../src/database/irrigation.qrc
    ../src/model/zone.cpp
    ../src/model/program.cpp
    ../src/model/programstarttime.cpp
    ../src/model/programstep.cpp
    ../src/model/firedinstant.cpp
)
```

- [ ] **Step 2: Replace the program helpers in the anonymous namespace**

Replace `#include "model/programzone.h"` with `#include "model/programstep.h"` and add `#include "runrequest.h"`.

Replace `seedProgramDirect`:

```cpp
    bool seedProgramDirect(const QString& dbPath,
                           Program& program,
                           QList<ProgramStartTime>& startTimes,
                           ProgramStepList& steps)
    {
        IrrigationDataSource seed(dbPath);
        if(seed.open() == false) {
            return false;
        }
        if(seed.insertProgram(program) == false) {
            return false;
        }
        for(ProgramStartTime& startTime : startTimes) {
            startTime.programId = program.id;
            if(seed.insertStartTime(startTime) == false) {
                return false;
            }
        }
        for(ProgramStep& step : steps) {
            step.programId = program.id;
            if(seed.insertProgramStep(step) == false) {
                return false;
            }
        }
        return true;
    }
```

Replace `programZoneJson` with:

```cpp
    QJsonObject stepJson(const QList<int>& zoneIds, int durationSeconds)
    {
        QJsonArray zones;
        for(int zoneId : zoneIds) {
            zones.append(zoneId);
        }
        return QJsonObject{ { "zones", zones }, { "durationSeconds", durationSeconds } };
    }
```

In `baseValidProgramBody()` delete the `zone` object and replace `{ "zones", QJsonArray{ zone } }` with `{ "steps", QJsonArray{ stepJson({ 2 }, 300) } }`.

In `withProgramField()` rename the `"zone"` scope to `"step"` and point it at `"steps"`:

```cpp
        else if(scope == QString("step")) {
            QJsonArray array = body.value("steps").toArray();
            QJsonObject entry = array.at(0).toObject();
            entry[key] = value;
            array[0] = entry;
            body["steps"] = array;
        }
```

and change its comment to `// scope selects which part of the body the override lands in: "root" for a top-level program field, "startTime" for startTimes[0], "step" for steps[0].`

In `programValidationRows()` replace the five zone rows (`durationSeconds zero` through `sequence as a JSON string`) with:

```cpp
        add("durationSeconds zero", withProgramField(baseValidProgramBody(), "step", "durationSeconds", 0));
        add("durationSeconds as a JSON string", withProgramField(baseValidProgramBody(), "step", "durationSeconds", "300"));
        add("step zone names no zone", withProgramField(baseValidProgramBody(), "step", "zones", QJsonArray{ 9999 }));
        add("step zone as a JSON string", withProgramField(baseValidProgramBody(), "step", "zones", QJsonArray{ "2" }));
        add("step with no zones", withProgramField(baseValidProgramBody(), "step", "zones", QJsonArray{}));
        add("step lists a zone twice", withProgramField(baseValidProgramBody(), "step", "zones", QJsonArray{ 2, 2 }));
        add("step zones not an array", withProgramField(baseValidProgramBody(), "step", "zones", 2));
        add("steps not an array", withProgramField(baseValidProgramBody(), "root", "steps", QJsonObject{}));
```

In `daysOfWeekProgramBody()` rename the last parameter to `steps` and the key to `"steps"`.

In `commitFailureTriggerSql()` rename the parameter to `stepDurationSeconds` and change `AFTER INSERT ON program_zones` to `AFTER INSERT ON program_steps`.

Replace `SeededProgram`, `compareProgramJsonWithSeed` and `compareStoredProgramWithSeed`'s zone parts:

```cpp
    struct SeededProgram
    {
        Program program;
        QList<ProgramStartTime> startTimes;
        ProgramStepList steps;
    };
```

```cpp
        const QJsonArray gotSteps = got.value("steps").toArray();
        QCOMPARE(gotSteps.count(), seed.steps.count());
        for(int i = 0; i < seed.steps.count(); i++) {
            const QJsonObject gotStep = gotSteps.at(i).toObject();
            QCOMPARE(gotStep.value("id").toInt(), seed.steps.at(i).id);
            QCOMPARE(gotStep.value("durationSeconds").toInt(), seed.steps.at(i).durationSeconds);
            const QJsonArray gotZones = gotStep.value("zones").toArray();
            QCOMPARE(gotZones.count(), seed.steps.at(i).zoneIds.count());
            for(int j = 0; j < gotZones.count(); j++) {
                QCOMPARE(gotZones.at(j).toInt(), seed.steps.at(i).zoneIds.at(j));
            }
        }
```

```cpp
        const ProgramStepList storedSteps = source.stepsFor(seed.program.id);
        QCOMPARE(storedSteps.count(), seed.steps.count());
        for(int i = 0; i < seed.steps.count(); i++) {
            QCOMPARE(storedSteps.at(i).id, seed.steps.at(i).id);
            QCOMPARE(storedSteps.at(i).programId, seed.program.id);
            QCOMPARE(storedSteps.at(i).sequence, seed.steps.at(i).sequence);
            QCOMPARE(storedSteps.at(i).durationSeconds, seed.steps.at(i).durationSeconds);
            QCOMPARE(storedSteps.at(i).zoneIds, seed.steps.at(i).zoneIds);
        }
```

Add a responder helper:

```cpp
    // Completes every run request on the server's own thread, the way the daemon completes
    // it on the valve thread.
    void answerRunRequests(IrrigationControlServer& server, RunRequest::Refusal refusal, const QString& message)
    {
        QObject::connect(&server, &IrrigationControlServer::manualZoneRunRequested, &server,
                         [refusal, message](int, int, const RunRequestPtr& decision)
        {
            decision->complete(refusal, message);
        });
        QObject::connect(&server, &IrrigationControlServer::programRunRequested, &server,
                         [refusal, message](int, const RunRequestPtr& decision)
        {
            decision->complete(refusal, message);
        });
    }
```

- [ ] **Step 3: Convert the program-route tests to steps**

Apply these rules to every test body. They are mechanical; each keeps the test's subject.

| Old | New |
|---|---|
| `ProgramZone x; x.zoneId = Z; x.sequence = S; x.durationSeconds = D;` | `ProgramStep x; x.zoneIds = { Z }; x.sequence = S; x.durationSeconds = D;` |
| `QList<ProgramZone>`, `ProgramZoneList` | `ProgramStepList` |
| `source.zonesFor(…)`, `verify.zonesFor(…)` | `….stepsFor(…)` |
| `.zoneId` on a stored step or a `ProgramStep` | `.zoneIds.at(0)` |
| request literal `QJsonObject{ { "zoneId", Z }, { "sequence", S }, { "durationSeconds", D } }`, `programZoneJson(Z, S, D)` | `stepJson({ Z }, D)` |
| request key `"zones"` holding those objects | `"steps"` |
| response `.value("zones")` | `.value("steps")` |
| response element `.value("zoneId").toInt()` | `.value("zones").toArray().at(0).toInt()` |
| response element `.value("sequence")` assertions | delete |
| SQL `ON program_zones`, `"program_zones"` in `abortTriggerSql` and `tableRowCount` | `program_steps` |
| row names `insertProgramZone at …` | `insertProgramStep at …` |

Rename the tests whose names say zones for program zones, in both the slot list and the definition:

| Old | New |
|---|---|
| `programsGetReturnsFieldsStartTimesZonesAndPerProgramNextRunUtc` | `programsGetReturnsFieldsStartTimesStepsAndPerProgramNextRunUtc` |
| `programPostAnswers500WhenAZoneInsertFailsWritingNothing` | `programPostAnswers500WhenAStepInsertFailsWritingNothing` |
| `programPutReplacesProgramZonesAsSent` | `programPutReplacesProgramStepsAsSent` |
| `programPutAnswers500WhenAZoneDeleteFailsLeavingZonesIntact` | `programPutAnswers500WhenAStepDeleteFailsLeavingStepsIntact` |
| `programPutWithEmptyZonesRemovesEveryStoredZone` | `programPutWithEmptyStepsRemovesEveryStoredStep` |
| `programPutChangesOnlyItsOwnProgramAndReportsStoredZoneIds` | `programPutChangesOnlyItsOwnProgramAndReportsStoredStepIds` |
| `programDeleteRemovesProgramCascadingStartTimesAndZonesLeavesOtherProgramsIntact` | `programDeleteRemovesProgramCascadingStartTimesAndStepsLeavesOtherProgramsIntact` |

In `programPostWriteStepFailureAnswersErrorAndWritesNothing_data` add a row that fails on a step's zone row:

```cpp
    QTest::newRow("insertProgramStep zone row at the second step")
        << QStringList{ abortTriggerSql("INSERT", "program_step_zones", "NEW.zone_id = 4") }
        << body << 500;
```

and in the test body add `QCOMPARE(tableRowCount(verify, "program_step_zones"), 0);` beside the `program_steps` count.

`seedRenumberedZone` and `programPostAcceptsAZoneIdThatDiffersFromItsRenumberedZoneNumber` keep their subject: a step stores the zone id.

- [ ] **Step 4: Rewrite the status tests**

Replace the expected key list in `statusKeySetMatchesTheSerializer`:

```cpp
    const QStringList expected = {
        "masterEnabled", "maxConcurrentZones", "nextRunUtc", "program", "queue",
        "rainDelayUntilUtc", "running", "stopHeld", "timezone"
    };
```

Replace the type checks in `statusFieldTypesMatchTheWebDecoder`:

```cpp
    QVERIFY(body.value("running").isArray());
    QVERIFY(body.value("program").isNull());
    QVERIFY(body.value("queue").isArray());
    QVERIFY(body.value("maxConcurrentZones").isDouble());
    QVERIFY(body.value("nextRunUtc").isString());
    QVERIFY(body.value("timezone").isString());
    QVERIFY(body.value("masterEnabled").isBool());
    QVERIFY(body.value("stopHeld").isBool());
    QVERIFY(body.value("rainDelayUntilUtc").isString());
```

In `updateStatusFromTheTestThreadAppearsInTheNextStatusGet` replace the `runningZone`/`secondsRemaining` fields of `first` and `second` with:

```cpp
    first.running = { RunningZoneStatus{ 4, 137, false }, RunningZoneStatus{ 6, 1712, true } };
    first.programId = 2;
    first.programName = "Morning Drip";
    first.programStep = 1;
    first.programStepCount = 2;
    first.waitingZones = { 7 };
    first.queue = { QueuedProgramStatus{ 1, "Summer", QDateTime(QDate(2026, 9, 20), QTime(13, 0, 4), QTimeZone::UTC) } };
    first.maxConcurrentZones = 2;
```

```cpp
    second.running = { RunningZoneStatus{ 7, 42, false } };
    second.maxConcurrentZones = 3;
```

replace each poll-loop condition with `firstBody.value("running").toArray().count() == 2` and `secondBody.value("program").isNull() && secondBody.value("running").toArray().count() == 1`, and replace the `runningZone`/`secondsRemaining` assertions with:

```cpp
    const QJsonArray firstRunning = firstBody.value("running").toArray();
    QCOMPARE(firstRunning.count(), 2);
    QCOMPARE(firstRunning.at(0).toObject().value("zone").toInt(), 4);
    QCOMPARE(firstRunning.at(0).toObject().value("secondsRemaining").toInt(), 137);
    QCOMPARE(firstRunning.at(0).toObject().value("source").toString(), QString("manual"));
    QCOMPARE(firstRunning.at(1).toObject().value("zone").toInt(), 6);
    QCOMPARE(firstRunning.at(1).toObject().value("source").toString(), QString("program"));
    const QJsonObject firstProgram = firstBody.value("program").toObject();
    QCOMPARE(firstProgram.value("id").toInt(), 2);
    QCOMPARE(firstProgram.value("name").toString(), QString("Morning Drip"));
    QCOMPARE(firstProgram.value("step").toInt(), 1);
    QCOMPARE(firstProgram.value("stepCount").toInt(), 2);
    QCOMPARE(firstProgram.value("waitingZones").toArray(), QJsonArray({ 7 }));
    const QJsonArray firstQueue = firstBody.value("queue").toArray();
    QCOMPARE(firstQueue.count(), 1);
    QCOMPARE(firstQueue.at(0).toObject().value("programId").toInt(), 1);
    QCOMPARE(firstQueue.at(0).toObject().value("name").toString(), QString("Summer"));
    QCOMPARE(firstQueue.at(0).toObject().value("queuedAtUtc").toString(), QString("2026-09-20T13:00:04Z"));
    QCOMPARE(firstBody.value("maxConcurrentZones").toInt(), 2);
```

```cpp
    QCOMPARE(secondBody.value("running").toArray().at(0).toObject().value("zone").toInt(), 7);
    QCOMPARE(secondBody.value("running").toArray().at(0).toObject().value("secondsRemaining").toInt(), 42);
    QVERIFY(secondBody.value("program").isNull());
    QCOMPARE(secondBody.value("queue").toArray().count(), 0);
    QCOMPARE(secondBody.value("maxConcurrentZones").toInt(), 3);
```

The remaining `nextRunUtc`, `rainDelayUntilUtc`, `timezone`, `masterEnabled` and `stopHeld` assertions stay.

- [ ] **Step 5: Answer the run requests in the existing run tests**

Add `answerRunRequests(server, RunRequest::Refusal::None, QString());` directly after `QVERIFY(startServerOnLoopback(server));` in `zoneRunEmitsManualZoneRunRequestedWithTheExactZoneAndSeconds`, `zoneRunSecondsBoundaryAcceptsOneRejectsZero` and `programRunEmitsProgramRunRequestedWithTheExactProgramId`. Their 202 expectations stand.

In `zoneRunDisabledZoneReturns409AndEmitsNothing` add after the status check:

```cpp
    QCOMPARE(QJsonDocument::fromJson(reply->readAll()).object().value("reason").toString(), QString("zone_disabled"));
```

In `settingsGetReturnsExactlyTheFourAllowlistedKeys` (rename to `settingsGetReturnsExactlyTheAllowlistedKeys`) use:

```cpp
    const QStringList expected = { "log_level", "master_enabled", "max_concurrent_zones", "max_zone_seconds", "rain_delay_until" };
```

and add `QCOMPARE(body.value("max_concurrent_zones").toString(), QString("2"));`.

Add rows to `settingsPutRejectsInvalidValue_data`:

```cpp
    QTest::newRow("max_concurrent_zones at the zero boundary") << QString("max_concurrent_zones") << QString("0");
    QTest::newRow("max_concurrent_zones above the ceiling") << QString("max_concurrent_zones") << QString("9");
    QTest::newRow("max_concurrent_zones non-numeric") << QString("max_concurrent_zones") << QString("two");
```

and to `settingsPutAcceptsValidValueAtBothEdges_data`:

```cpp
    QTest::newRow("max_concurrent_zones at the minimum") << QString("max_concurrent_zones") << QString("1");
    QTest::newRow("max_concurrent_zones at the ceiling") << QString("max_concurrent_zones") << QString("8");
```

- [ ] **Step 6: Add the decision and per-zone stop tests**

Declare each in the slot list and add the definitions before `QTEST_MAIN`:

```cpp
void TestControlServer::zoneRunRefusalAnswers409WithTheReason_data()
{
    QTest::addColumn<int>("refusal");
    QTest::addColumn<QString>("reason");

    QTest::newRow("cap reached")     << static_cast<int>(RunRequest::Refusal::CapReached)     << QString("cap_reached");
    QTest::newRow("stop held")       << static_cast<int>(RunRequest::Refusal::StopHeld)       << QString("stop_held");
    QTest::newRow("master disabled") << static_cast<int>(RunRequest::Refusal::MasterDisabled) << QString("master_disabled");
    QTest::newRow("zone disabled")   << static_cast<int>(RunRequest::Refusal::ZoneDisabled)   << QString("zone_disabled");
}

void TestControlServer::zoneRunRefusalAnswers409WithTheReason()
{
    QFETCH(int, refusal);
    QFETCH(QString, reason);

    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));
    answerRunRequests(server, static_cast<RunRequest::Refusal>(refusal), "2 zones already running");

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/zones/3/run", R"({"seconds": 60})");
    QCOMPARE(statusCode(reply), 409);

    const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
    QCOMPARE(body.value("reason").toString(), reason);
    QCOMPARE(body.value("error").toString(), QString("2 zones already running"));

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));
}

void TestControlServer::zoneRunFailureAnswers500WithTheMessage()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));
    answerRunRequests(server, RunRequest::Refusal::Failed, "injected write failure");

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/zones/3/run", R"({"seconds": 60})");
    QCOMPARE(statusCode(reply), 500);

    const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
    QCOMPARE(body.value("reason").toString(), QString("failed"));
    QCOMPARE(body.value("error").toString(), QString("injected write failure"));

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));
}

void TestControlServer::zoneRunUnansweredAnswers503WithinTheDecisionTimeout()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    server.setDecisionTimeout(TimeSpan::fromMilliseconds(300));
    QVERIFY(startServerOnLoopback(server));

    QElapsedTimer elapsed;
    elapsed.start();

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/zones/3/run", R"({"seconds": 60})");

    QCOMPARE(statusCode(reply), 503);
    QCOMPARE(QJsonDocument::fromJson(reply->readAll()).object().value("reason").toString(), QString("timeout"));
    QVERIFY(elapsed.elapsed() < 3000);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));
}

void TestControlServer::stopReturnsWhileAZoneRunDecisionIsPending()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    server.setDecisionTimeout(TimeSpan::fromSeconds(1));
    QVERIFY(startServerOnLoopback(server));

    QSignalSpy requested(&server, &IrrigationControlServer::manualZoneRunRequested);

    QNetworkAccessManager manager;
    QNetworkRequest request(QUrl(QString("http://127.0.0.1:%1/admin/zones/3/run").arg(server.boundPort())));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply* reply = manager.post(request, QByteArray(R"({"seconds": 60})"));
    QVERIFY(requested.wait(5000));

    QElapsedTimer elapsed;
    elapsed.start();
    QVERIFY(server.stop(TimeSpan::fromSeconds(8)));
    QVERIFY(elapsed.elapsed() < 5000);

    reply->abort();
}

void TestControlServer::zoneStopEmitsZoneStopRequestedWithTheZoneNumber()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");
    seedRenumberedZone(dbPath, 99, true);

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QSignalSpy spy(&server, &IrrigationControlServer::zoneStopRequested);

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/zones/99/stop", QByteArray());
    QCOMPARE(statusCode(reply), 202);

    if(spy.count() == 0) {
        QVERIFY(spy.wait(5000));
    }
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toInt(), 99);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));
}

void TestControlServer::zoneStopUnknownZoneReturns404AndEmitsNothing()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QSignalSpy spy(&server, &IrrigationControlServer::zoneStopRequested);

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/zones/42/stop", QByteArray());
    QCOMPARE(statusCode(reply), 404);

    spy.wait(200);
    QCOMPARE(spy.count(), 0);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));
}

void TestControlServer::programRunAlreadyQueuedAnswers409()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");

    Program program;
    program.name = "Queued";
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = 1;
    QList<ProgramStartTime> startTimes;
    ProgramStep step;
    step.zoneIds = { 2 };
    step.sequence = 1;
    step.durationSeconds = 60;
    ProgramStepList steps{ step };
    QVERIFY(seedProgramDirect(dbPath, program, startTimes, steps));

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));
    answerRunRequests(server, RunRequest::Refusal::AlreadyQueued, "that program is already running or queued");

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(),
                                    QString("/admin/programs/%1/run").arg(program.id), QByteArray());
    QCOMPARE(statusCode(reply), 409);
    QCOMPARE(QJsonDocument::fromJson(reply->readAll()).object().value("reason").toString(), QString("already_queued"));

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));
}

void TestControlServer::programPostRoundTripsAMultiZoneStepInOrder()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    const QByteArray body = daysOfWeekProgramBody("Drip", 5, true, QJsonArray{ utcStartTimeJson(360) },
                                                  QJsonArray{ stepJson({ 6, 5 }, 1800), stepJson({ 1 }, 600) });

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/programs", body);
    QCOMPARE(statusCode(reply), 201);

    const QJsonArray steps = QJsonDocument::fromJson(reply->readAll()).object().value("steps").toArray();
    QCOMPARE(steps.count(), 2);
    QCOMPARE(steps.at(0).toObject().value("zones").toArray(), QJsonArray({ 6, 5 }));
    QCOMPARE(steps.at(0).toObject().value("durationSeconds").toInt(), 1800);
    QVERIFY(steps.at(0).toObject().value("id").toInt() > 0);
    QCOMPARE(steps.at(1).toObject().value("zones").toArray(), QJsonArray({ 1 }));

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));
}
```

- [ ] **Step 7: Build and run**

```bash
cmake --build build -j 32
ctest --test-dir build --output-on-failure -R tst_controlserver
```

Expected: passes. A failure is a finding against Tasks 5–6: fix the code, never loosen the assertion, and commit the fix separately as `fix:`.

- [ ] **Step 8: Commit**

```bash
git add IrrigationD/tests/tst_controlserver.cpp IrrigationD/tests/CMakeLists.txt
git commit -F - <<'EOF'
test: cover run decisions, per-zone stop and steps in the control server

tst_controlserver comes back with program routes converted to steps,
the new status shape, and the max_concurrent_zones setting. New tests
cover each 409 reason, the 500 failure body, the bounded 503 when no
decision arrives, a stop() that returns while a decision is pending,
the per-zone stop route, already_queued on a program run, and a
multi-zone step round trip.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---

### Task 15: Web tests

Spec §8: the multi-row Now screen, tile disabling at the cap, the 409 message, the step editor and its wave warning. Plus the decoder, client, polling and settings behaviour that Tasks 7–9 changed. The five parked files come back.

**Files:**
- Modify: `web/tsconfig.json`, `web/vitest.config.ts` (unpark)
- Modify: `web/src/test/fixtures.ts`
- Modify: `web/src/api/decode.test.ts`, `web/src/api/client.test.ts`, `web/src/programs/dayRule.test.ts`
- Modify: `web/src/hooks/useStatus.test.ts`
- Modify: `web/src/screens/NowScreen.test.tsx`, `web/src/screens/ProgramEditor.test.tsx`, `web/src/screens/ProgramsScreen.test.tsx`, `web/src/screens/SettingsScreen.test.tsx`

**Interfaces:**
- Consumes: Tasks 7–9 as produced.

- [ ] **Step 1: Unpark**

Delete the `"exclude"` key from `web/tsconfig.json`. Restore `web/vitest.config.ts`'s exclude to:

```ts
    exclude: ['**/node_modules/**', '**/dist/**', 'src/**/*.tz.test.ts'],
```

- [ ] **Step 2: Add a capped status fixture to `web/src/test/fixtures.ts`**

```ts
/** Zone 5 belongs to the running program, zone 1 was started by hand; the cap of 2 is full. */
export const cappedStatus: Status = {
  ...idleStatus,
  running: [
    { zone: 1, secondsRemaining: 240, source: 'manual' },
    { zone: 5, secondsRemaining: 1712, source: 'program' },
  ],
  program: { id: 2, name: 'Morning Drip', step: 1, stepCount: 2, waitingZones: [7] },
  queue: [{ programId: 1, name: 'Summer', queuedAtUtc: '2026-09-14T13:00:04Z' }],
}
```

- [ ] **Step 3: Convert `web/src/api/decode.test.ts`**

Replace `goodStatus` and the `decodeStatus` describe block:

```ts
const goodStatus = {
  running: [
    { zone: 5, secondsRemaining: 1712, source: 'program' },
    { zone: 1, secondsRemaining: 240, source: 'manual' },
  ],
  program: { id: 2, name: 'Morning Drip', step: 1, stepCount: 2, waitingZones: [7] },
  queue: [{ programId: 1, name: 'Summer', queuedAtUtc: '2026-09-13T13:00:04Z' }],
  maxConcurrentZones: 2,
  nextRunUtc: '2026-09-13T13:00:00Z',
  timezone: 'America/Los_Angeles',
  masterEnabled: true,
  stopHeld: false,
  rainDelayUntilUtc: '',
}

describe('decodeStatus', () => {
  it('reads every field', () => {
    const status = decodeStatus(goodStatus)
    expect(status.running).toEqual([
      { zone: 5, secondsRemaining: 1712, source: 'program' },
      { zone: 1, secondsRemaining: 240, source: 'manual' },
    ])
    expect(status.program).toEqual({ id: 2, name: 'Morning Drip', step: 1, stepCount: 2, waitingZones: [7] })
    expect(status.queue).toEqual([{ programId: 1, name: 'Summer', queuedAtUtc: '2026-09-13T13:00:04Z' }])
    expect(status.maxConcurrentZones).toBe(2)
    expect(status.nextRunUtc).toBe('2026-09-13T13:00:00Z')
    expect(status.timezone).toBe('America/Los_Angeles')
    expect(status.masterEnabled).toBe(true)
  })

  it('reads a null program as no program running', () => {
    expect(decodeStatus({ ...goodStatus, program: null }).program).toBeNull()
  })

  it('throws when program is absent', () => {
    const { program, ...withoutProgram } = goodStatus
    expect(program).toBeDefined()
    expect(() => decodeStatus(withoutProgram)).toThrow(/status\.program/)
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
    expect(() => decodeStatus({ ...goodStatus, maxConcurrentZones: '2' })).toThrow(/maxConcurrentZones/)
  })

  it('throws when a boolean arrives as a string', () => {
    expect(() => decodeStatus({ ...goodStatus, masterEnabled: 'true' })).toThrow(/masterEnabled/)
  })

  it('names the running entry with an unknown source', () => {
    const running = [{ zone: 5, secondsRemaining: 10, source: 'timer' }]
    expect(() => decodeStatus({ ...goodStatus, running })).toThrow(/running\[0\]\.source/)
  })

  it('names the waiting zone that is not a number', () => {
    const program = { ...goodStatus.program, waitingZones: [7, '8'] }
    expect(() => decodeStatus({ ...goodStatus, program })).toThrow(/waitingZones\[1\]/)
  })
})
```

In `goodProgram` replace the `zones` line with `steps: [{ id: 7, zones: [9, 11], durationSeconds: 600 }],`. Replace `'reads nested start times and zones'` and `'sorts zones by sequence'` with:

```ts
  it('reads nested start times and steps', () => {
    const program = decodePrograms([goodProgram])[0]
    expect(program?.startTimes[0]?.minutesAfterMidnight).toBe(360)
    expect(program?.steps).toEqual([{ zones: [9, 11], durationSeconds: 600 }])
  })

  it('keeps steps in the order the daemon sent them', () => {
    const twoSteps = {
      ...goodProgram,
      steps: [
        { id: 8, zones: [10], durationSeconds: 300 },
        { id: 7, zones: [9], durationSeconds: 600 },
      ],
    }
    expect(decodePrograms([twoSteps])[0]?.steps.map((step) => step.zones[0])).toEqual([10, 9])
  })

  it('names the step zone that is not a number', () => {
    const badStep = { ...goodProgram, steps: [{ id: 7, zones: ['9'], durationSeconds: 600 }] }
    expect(() => decodePrograms([badStep])).toThrow(/steps\[0\]\.zones\[0\]/)
  })
```

- [ ] **Step 4: Convert `web/src/api/client.test.ts`**

Add `stopZone` to the import list. In `'posts a program draft with no ids'` replace both `zones: [{ zoneId: 9, sequence: 1, durationSeconds: 300 }],` lines with `steps: [{ zones: [9], durationSeconds: 300 }],`. Add:

```ts
  it('stops one zone by zone number with no body', async () => {
    const { calls } = installFetchStub(() => ({ status: 202 }))
    await stopZone(3)
    expect(calls[0]?.url).toBe('/api/zones/3/stop')
    expect(calls[0]?.method).toBe('POST')
    expect(calls[0]?.body).toBeNull()
  })

  it('carries the refusal reason of a 409', async () => {
    installFetchStub(() => ({ status: 409, body: { error: '2 zones already running', reason: 'cap_reached' } }))

    const error = await runZone(zoneFixtures[2]!, 600).catch((caught: unknown) => caught)
    expect(error).toBeInstanceOf(ApiError)
    expect((error as ApiError).status).toBe(409)
    expect((error as ApiError).message).toBe('2 zones already running')
    expect((error as ApiError).reason).toBe('cap_reached')
  })

  it('leaves the reason null when the body has none', async () => {
    installFetchStub(() => ({ status: 404, body: { error: 'unknown zone' } }))
    const error = await runZone(zoneFixtures[2]!, 600).catch((caught: unknown) => caught)
    expect((error as ApiError).reason).toBeNull()
  })
```

- [ ] **Step 5: Convert `web/src/programs/dayRule.test.ts`**

Replace the `totalRuntimeSeconds` block:

```ts
describe('totalRuntimeSeconds', () => {
  it('sums single-zone steps', () => {
    expect(totalRuntimeSeconds([{ zones: [7], durationSeconds: 600 }, { zones: [9], durationSeconds: 300 }], 2)).toBe(900)
  })

  it('counts a step that fits under the cap once', () => {
    expect(totalRuntimeSeconds([{ zones: [7, 8], durationSeconds: 600 }], 2)).toBe(600)
  })

  it('counts a step larger than the cap once per wave', () => {
    expect(totalRuntimeSeconds([{ zones: [7, 8, 9], durationSeconds: 600 }], 2)).toBe(1200)
    expect(totalRuntimeSeconds([{ zones: [7, 8, 9], durationSeconds: 600 }], 1)).toBe(1800)
  })

  it('treats a cap below one as one', () => {
    expect(totalRuntimeSeconds([{ zones: [7, 8], durationSeconds: 60 }], 0)).toBe(120)
  })

  it('is zero for an empty program', () => {
    expect(totalRuntimeSeconds([], 2)).toBe(0)
  })
})
```

In `toDraft`, replace the expected `zones` with `steps: [{ zones: [7], durationSeconds: 600 }, { zones: [9], durationSeconds: 300 }],` and the second test's `draft.zones.map((zone) => zone.zoneId)` with `draft.steps.map((step) => step.zones[0])`.

- [ ] **Step 6: Extend `web/src/hooks/useStatus.test.ts`**

Import `cappedStatus` and `idleStatus`, and add:

```ts
  it('polls every 2 s while a program waits for a slot with nothing open', async () => {
    const waiting = {
      ...idleStatus,
      program: { id: 2, name: 'Morning Drip', step: 1, stepCount: 1, waitingZones: [7] },
    }
    const getStatus = vi.spyOn(client, 'getStatus').mockResolvedValue(waiting)
    renderHook(() => useStatus())
    await settle()

    await act(async () => {
      await vi.advanceTimersByTimeAsync(RUNNING_POLL_MS)
    })
    expect(getStatus).toHaveBeenCalledTimes(2)
  })

  it('polls every 2 s while only the queue is non-empty', async () => {
    const queued = { ...idleStatus, queue: cappedStatus.queue }
    const getStatus = vi.spyOn(client, 'getStatus').mockResolvedValue(queued)
    renderHook(() => useStatus())
    await settle()

    await act(async () => {
      await vi.advanceTimersByTimeAsync(RUNNING_POLL_MS)
    })
    expect(getStatus).toHaveBeenCalledTimes(2)
  })
```

- [ ] **Step 7: Update and extend `web/src/screens/NowScreen.test.tsx`**

The big STOP and each row's "Stop zone N" both match `/stop/i`. Replace all six `screen.getByRole('button', { name: /stop/i })` with `screen.getByRole('button', { name: 'STOP' })`. Import `cappedStatus` and add:

```tsx
describe('NowScreen with several zones open', () => {
  it('lists one row per open zone with its countdown and tag', async () => {
    render(<NowScreen status={cappedStatus} polls={1} refresh={refresh} />)

    const manual = await screen.findByTestId('running-zone-1')
    expect(manual).toHaveTextContent('Front lawn')
    expect(manual).toHaveTextContent('4:00')
    expect(manual).toHaveTextContent(/manual/i)

    const program = screen.getByTestId('running-zone-5')
    expect(program).toHaveTextContent('Vegetable bed')
    expect(program).toHaveTextContent('28:32')
    expect(program).toHaveTextContent(/program/i)
  })

  it('shows the program step with its waiting zone and the queue', async () => {
    render(<NowScreen status={cappedStatus} polls={1} refresh={refresh} />)

    expect(await screen.findByTestId('program-line')).toHaveTextContent('Morning Drip — step 1 of 2, zone 7 waiting')
    expect(screen.getByTestId('queue-line')).toHaveTextContent('Queued: Summer')
  })

  it('stops one zone by its zone number', async () => {
    const user = userEvent.setup()
    const stopZone = vi.spyOn(client, 'stopZone').mockResolvedValue(undefined)
    render(<NowScreen status={cappedStatus} polls={1} refresh={refresh} />)

    await user.click(await screen.findByRole('button', { name: 'Stop zone 5' }))

    expect(stopZone).toHaveBeenCalledWith(5)
    await waitFor(() => {
      expect(refresh).toHaveBeenCalled()
    })
    expect(client.stopAll).not.toHaveBeenCalled()
  })

  it('glows every open tile', async () => {
    render(<NowScreen status={cappedStatus} polls={1} refresh={refresh} />)
    expect(await screen.findByTestId('zone-tile-1')).toHaveAttribute('data-running', 'true')
    expect(screen.getByTestId('zone-tile-5')).toHaveAttribute('data-running', 'true')
    expect(screen.getByTestId('zone-tile-2')).toHaveAttribute('data-running', 'false')
  })

  it('disables Run on closed tiles at the cap and says why', async () => {
    render(<NowScreen status={cappedStatus} polls={1} refresh={refresh} />)

    const closed = await screen.findByTestId('zone-tile-2')
    expect(within(closed).getByRole('button', { name: /run/i })).toBeDisabled()
    expect(within(screen.getByTestId('zone-tile-1')).getByRole('button', { name: /running/i })).toBeEnabled()
    expect(screen.getByTestId('cap-hint')).toHaveTextContent('2 of 2 running')
  })

  it('keeps every tile live below the cap', async () => {
    render(<NowScreen status={runningStatus} polls={1} refresh={refresh} />)

    expect(within(await screen.findByTestId('zone-tile-2')).getByRole('button', { name: /run/i })).toBeEnabled()
    expect(screen.queryByTestId('cap-hint')).toBeNull()
  })

  it('shows the reason a run was refused', async () => {
    const user = userEvent.setup()
    vi.spyOn(client, 'runZone').mockRejectedValue(new ApiError(409, '2 zones already running', 'cap_reached'))
    render(<NowScreen status={runningStatus} polls={1} refresh={refresh} />)

    await user.click(within(await screen.findByTestId('zone-tile-2')).getByRole('button', { name: /run/i }))

    expect(await screen.findByRole('alert')).toHaveTextContent('2 zones already running')
  })
})
```

Add `vi.spyOn(client, 'stopZone').mockResolvedValue(undefined)` to the file's `beforeEach`.

- [ ] **Step 8: Convert and extend `web/src/screens/ProgramEditor.test.tsx`**

`renderEditor` takes a cap and passes it:

```tsx
function renderEditor(program: Parameters<typeof ProgramEditor>[0]['program'], maxConcurrentZones = 2) {
  return render(
    <ProgramEditor
      program={program}
      zones={zoneFixtures}
      controllerZone={LA}
      maxConcurrentZones={maxConcurrentZones}
      onDone={onDone}
      onCancel={onCancel}
    />,
  )
}

/** Adds a step and puts the zone with database id `zoneId` in it. */
async function addStepWithZone(user: ReturnType<typeof userEvent.setup>, stepNumber: number, zoneId: number) {
  await user.click(screen.getByRole('button', { name: 'Add step' }))
  await user.selectOptions(screen.getByLabelText(`Add a zone to step ${stepNumber}`), String(zoneId))
}
```

In `validationError`: replace every `zones: [{ zoneId: 7, sequence: 1, durationSeconds: 60 }]` with `steps: [{ zones: [7], durationSeconds: 60 }]`; the `'requires at least one zone'` test becomes:

```tsx
  it('requires at least one step', () => {
    expect(validationError({ ...base, name: 'X', dowMask: 1, steps: [] })).toMatch(/step/i)
  })

  it('requires a zone in every step', () => {
    expect(
      validationError({ ...base, name: 'X', dowMask: 1, steps: [{ zones: [7], durationSeconds: 60 }, { zones: [], durationSeconds: 60 }] }),
    ).toMatch(/step 2 needs at least one zone/i)
  })
```

and `'rejects a zero-length zone run'` uses `steps: [{ zones: [7], durationSeconds: 0 }]`.

Replace every `await user.click(screen.getByRole('button', { name: /add zone/i }))` that was followed by a `selectOptions(… /zone N valve/ …, 'X')` with `await addStepWithZone(user, N, X)`, and every one with no `selectOptions` after it with `await addStepWithZone(user, 1, 7)`. Then:

- `'posts the draft the form describes'`: set the duration with `screen.getByLabelText('Step 1 minutes')` and expect `steps: [{ zones: [9], durationSeconds: 300 }]` in place of `zones`.
- `'stores the zone database id as zoneId'` becomes `'stores the zone database id in a step'`: the picker is `screen.getByLabelText('Add a zone to step 1')` after clicking `Add step`, its `/3 · Roses/` option still has value `'9'`, and the assertion reads `createProgram.mock.calls[0]![0].steps[0]!.zones[0]` toBe `9`.
- `'loads every field of the existing program'`: replace the four zone lines with

```tsx
    expect(screen.getByTestId('step-1')).toHaveTextContent('1 · Front lawn')
    expect(screen.getByLabelText('Step 1 minutes')).toHaveValue(10)
    expect(screen.getByTestId('step-2')).toHaveTextContent('3 · Roses')
    expect(screen.getByLabelText('Step 2 minutes')).toHaveValue(5)
```

- `'puts the full draft under the program id'`: expect `draft.steps` toEqual `[{ zones: [7], durationSeconds: 600 }, { zones: [9], durationSeconds: 300 }]`.
- `'save renumbers zone sequence from the final array order after a move'` becomes `'saves steps in their moved order'`: click `'Move step 2 up'`, expect `steps` `[{ zones: [9], durationSeconds: 300 }, { zones: [7], durationSeconds: 600 }]`.
- `'save renumbers zone sequence from the final array order after removing a middle zone'` becomes `'saves the remaining steps in order after removing a middle one'`: `await addStepWithZone(user, 3, 11)`, click `'Remove step 2'`, expect `steps.map((step) => step.zones)` toEqual `[[7], [11]]`.

Add:

```tsx
describe('steps', () => {
  it('adds zones to a step as chips and removes them again', async () => {
    const user = userEvent.setup()
    renderEditor(null)

    await addStepWithZone(user, 1, 7)
    await user.selectOptions(screen.getByLabelText('Add a zone to step 1'), '9')

    const step = screen.getByTestId('step-1')
    expect(step).toHaveTextContent('1 · Front lawn')
    expect(step).toHaveTextContent('3 · Roses')

    await user.click(screen.getByRole('button', { name: 'Remove 1 · Front lawn from step 1' }))
    expect(screen.getByTestId('step-1')).not.toHaveTextContent('Front lawn')
  })

  it('offers only zones the step does not already hold', async () => {
    const user = userEvent.setup()
    renderEditor(null)

    await addStepWithZone(user, 1, 7)

    const picker = screen.getByLabelText('Add a zone to step 1')
    expect(within(picker).queryByRole('option', { name: /1 · Front lawn/ })).toBeNull()
    expect(within(picker).getByRole('option', { name: /3 · Roses/ })).toBeInTheDocument()
  })

  it('warns that a step with more zones than the cap runs in waves', async () => {
    const user = userEvent.setup()
    renderEditor(null, 2)

    await addStepWithZone(user, 1, 7)
    await user.selectOptions(screen.getByLabelText('Add a zone to step 1'), '8')
    expect(screen.queryByTestId('wave-warning-1')).toBeNull()

    await user.selectOptions(screen.getByLabelText('Add a zone to step 1'), '9')
    expect(screen.getByTestId('wave-warning-1')).toHaveTextContent(/runs in waves/i)
  })

  it('totals the runtime assuming waves', async () => {
    const user = userEvent.setup()
    renderEditor(null, 2)

    await addStepWithZone(user, 1, 7)
    await user.selectOptions(screen.getByLabelText('Add a zone to step 1'), '8')
    await user.selectOptions(screen.getByLabelText('Add a zone to step 1'), '9')
    await addStepWithZone(user, 2, 10)
    await user.clear(screen.getByLabelText('Step 2 minutes'))
    await user.type(screen.getByLabelText('Step 2 minutes'), '5')

    // Step 1: three zones under a cap of two, 10 min each wave; step 2: 5 min.
    expect(screen.getByTestId('editor-total')).toHaveTextContent('25 min')
  })

  it('refuses to save a step with no zone', async () => {
    const user = userEvent.setup()
    const createProgram = vi.spyOn(client, 'createProgram').mockResolvedValue(undefined)
    renderEditor(null)

    await user.type(screen.getByLabelText(/program name/i), 'Empty step')
    await user.click(screen.getByRole('button', { name: 'Mon' }))
    await user.click(screen.getByRole('button', { name: 'Add step' }))
    await user.click(screen.getByRole('button', { name: /save/i }))

    expect(await screen.findByRole('alert')).toHaveTextContent(/step 1 needs at least one zone/i)
    expect(createProgram).not.toHaveBeenCalled()
  })
})
```

- [ ] **Step 9: Convert and extend `web/src/screens/ProgramsScreen.test.tsx`**

In `eveningProgram` replace `zones` with `steps: [{ zones: [8], durationSeconds: 1200 }],`. In `'shows the zone sequence with each duration and the computed total'` (rename `'shows the steps with each duration and the computed total'`) change the test id to `'step-sequence'`; the expected texts stay. In `'sends the whole program when the enable toggle flips'` replace the expected `zones` with `steps: [{ zones: [7], durationSeconds: 600 }, { zones: [9], durationSeconds: 300 }],`. Add:

```tsx
  it('joins the zones of a step and totals the runtime assuming waves', async () => {
    const drip: Program = { ...morningProgram, id: 3, steps: [{ zones: [7, 8, 9], durationSeconds: 600 }] }
    vi.spyOn(client, 'getPrograms').mockResolvedValue([drip])

    const { unmount } = render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)
    const program = await screen.findByTestId('program-3')
    expect(within(program).getByTestId('step-sequence')).toHaveTextContent('1. Front lawn + Side strip + Roses 10 min')
    expect(within(program).getByTestId('total-runtime')).toHaveTextContent('20 min')
    unmount()

    render(<ProgramsScreen status={{ ...idleStatus, maxConcurrentZones: 3 }} polls={1} refresh={refresh} />)
    expect(within(await screen.findByTestId('program-3')).getByTestId('total-runtime')).toHaveTextContent('10 min')
  })
```

- [ ] **Step 10: Extend `web/src/screens/SettingsScreen.test.tsx`**

```tsx
  it('saves max zones at once as a string', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)
    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)

    const field = await screen.findByLabelText(/max zones at once/i)
    expect(field).toHaveValue(2)
    await user.clear(field)
    await user.type(field, '3')
    await user.click(screen.getByRole('button', { name: 'Save max zones' }))

    await waitFor(() => {
      expect(putSettings).toHaveBeenCalledWith({ max_concurrent_zones: '3' })
    })
  })

  it('refuses max zones outside 1 to 8', async () => {
    const user = userEvent.setup({ advanceTimers: vi.advanceTimersByTime })
    const putSettings = vi.spyOn(client, 'putSettings').mockResolvedValue(undefined)
    render(<SettingsScreen status={idleStatus} polls={1} refresh={refresh} />)

    const field = await screen.findByLabelText(/max zones at once/i)
    await user.clear(field)
    await user.type(field, '9')
    await user.click(screen.getByRole('button', { name: 'Save max zones' }))

    expect(await screen.findByRole('alert')).toHaveTextContent(/1 to 8/)
    expect(putSettings).not.toHaveBeenCalled()
  })
```

Match the `userEvent.setup(...)` call the file's other typing tests use if it differs.

- [ ] **Step 11: Typecheck, test and build**

```bash
npm --prefix web run typecheck
npm --prefix web test
npm --prefix web run build
```

Expected: all three exit zero. A failure is a finding against Tasks 7–9: fix the code, never loosen the assertion, and commit the fix separately as `fix:`.

- [ ] **Step 12: Commit**

```bash
git add web/tsconfig.json web/vitest.config.ts web/src/test/fixtures.ts \
        web/src/api/decode.test.ts web/src/api/client.test.ts web/src/programs/dayRule.test.ts \
        web/src/hooks/useStatus.test.ts web/src/screens/NowScreen.test.tsx \
        web/src/screens/ProgramEditor.test.tsx web/src/screens/ProgramsScreen.test.tsx \
        web/src/screens/SettingsScreen.test.tsx
git commit -F - <<'EOF'
test: cover concurrent zones in the web app

The Now screen tests cover one row per open zone with its tag and
countdown, per-zone stop by zone number, the program and queue lines,
glowing tiles, Run disabled at the cap with its hint, and the 409
sentence. The editor tests cover zone chips, the picker, the wave
warning and the wave-aware total; the list, decoder, client, polling
and settings tests move to steps, refusal reasons and max zones at
once. The parked test files are back.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
EOF
```

---

### Task 16: Bench verification on the controller

Spec §8's closing paragraph: two manual zones, a two-zone step, a third run refused at cap 2, and STOP with a queued program. Plus a per-zone stop and the restart sweep. Runs on the real Pi with relays wired to real solenoids, after Task 15 is green.

The image's `irrigationd` is built from a GitHub `SRCREV` the owner has not pushed, so this task deploys **without reflashing**: a cross-built binary and the web bundle copied over the installed ones, with backups beside them.

> **⚠ Safety trap.** On this low-trigger relay board a released GPIO line falls to the pad pull-down and **energises its relay**. Never `systemctl stop irrigationd` while relays are wired. Use `systemctl restart`, which re-requests the lines within a second, and confirm afterwards through `/api/status` that `running` is empty. The only scripted stop in this task is inside the rollback one-liner, which starts the daemon again in the same command.

> **Water.** Each run below clicks a real relay and, if the supply valve is open, waters a real zone. Ask the owner whether the water is on before Step 6, and keep every run short.

**Files:** none in the repository. Backups on the Pi: `/usr/bin/irrigationd.pre-concurrent`, `/var/lib/irrigationd/irrigation.db.pre-concurrent`, `/var/www/irrigation/html.pre-concurrent`.

**Interfaces:**
- Consumes: the whole branch; the Yocto SDK at `/home/spunak/opt/poky/5.2.4` (a symlink to `/opt/poky/5.2.4`, the path the Qt Creator RPi kit uses); the controller at `root@irrigation.local`, empty password.

Every command block below defines its own helpers, because the Bash tool keeps no shell functions between calls:

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
pcp() { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w scp -o PubkeyAuthentication=no "$1" "root@irrigation.local:$2" </dev/null; }
api() { pi "curl -s -w '\n%{http_code}\n' $1"; }
```

- [ ] **Step 1: Confirm the controller is reachable and idle**

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
pi 'systemctl is-active irrigationd; curl -s http://127.0.0.1/api/status; echo; ls /usr/lib/libQt6HttpServer.so.6* /usr/lib/libgpiod.so.3*'
ls /home/spunak/opt/poky/5.2.4/sysroots/cortexa72-poky-linux/usr/lib/libQt6HttpServer.so.6* \
   /home/spunak/opt/poky/5.2.4/sysroots/cortexa72-poky-linux/usr/lib/libgpiod.so.3*
```

Expected: `active`, an old-shape status with `"runningZone":0`, and the same `libQt6HttpServer` and `libgpiod` sonames on both sides. A soname mismatch means the SDK and the image came from different builds: stop and tell the owner.

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

Expected: `ELF 64-bit LSB pie executable, ARM aarch64`. `IRRIGATION_USE_MOLD=OFF` keeps the host's `mold` out of a cross link.

- [ ] **Step 3: Build the web bundle**

```bash
cd /home/spunak/src/punak/irrigation
npm --prefix web run build
tar -C web/dist -czf build/bench-web.tgz .
```

- [ ] **Step 4: Back up the binary, the database and the bundle on the Pi**

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
pi 'ls -d /usr/bin/irrigationd.pre-concurrent /var/lib/irrigationd/irrigation.db.pre-concurrent /var/www/irrigation/html.pre-concurrent 2>/dev/null'
```

Expected: no output. If any backup already exists, stop and ask the owner; never overwrite one.

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
pi 'cp -a /usr/bin/irrigationd /usr/bin/irrigationd.pre-concurrent &&
    sqlite3 /var/lib/irrigationd/irrigation.db ".backup /var/lib/irrigationd/irrigation.db.pre-concurrent" &&
    cp -a /var/www/irrigation/html /var/www/irrigation/html.pre-concurrent &&
    sqlite3 /var/lib/irrigationd/irrigation.db.pre-concurrent "SELECT COUNT(*) FROM program_zones; SELECT sw_version FROM info;"'
```

Record the `program_zones` count; expect `1.0.0` as the version. `.backup` takes a consistent copy of a live WAL database; a plain `cp` of the `.db` file does not.

- [ ] **Step 5: Deploy and restart**

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
pcp() { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w scp -o PubkeyAuthentication=no "$1" "root@irrigation.local:$2" </dev/null; }
cd /home/spunak/src/punak/irrigation
pcp build/bench-arm64/IrrigationD/irrigationd /usr/bin/irrigationd.new
pcp build/bench-web.tgz /tmp/bench-web.tgz
pi 'chmod 0755 /usr/bin/irrigationd.new && mv /usr/bin/irrigationd.new /usr/bin/irrigationd && systemctl restart irrigationd'
pi 'sleep 3; systemctl is-active irrigationd; journalctl -u irrigationd -n 40 --no-pager'
pi 'sqlite3 /var/lib/irrigationd/irrigation.db "SELECT sw_version FROM info; SELECT COUNT(*) FROM program_steps; SELECT COUNT(*) FROM program_step_zones; SELECT value FROM settings WHERE key = '"'"'max_concurrent_zones'"'"';"'
pi 'ls /var/lib/irrigationd/*.backup 2>/dev/null'
pi 'rm -rf /var/www/irrigation/html/* && tar -C /var/www/irrigation/html -xzf /tmp/bench-web.tgz && rm /tmp/bench-web.tgz && ls /var/www/irrigation/html'
pi 'curl -s http://127.0.0.1/api/status'
```

The binary goes in through `irrigationd.new` and `mv`: writing over a running executable fails with "Text file busy", and `mv` swaps the directory entry atomically.

Expected: `active`; the journal shows the migration to 1.1.0 with no "recreating the database" line; `1.1.0`; `program_steps` and `program_step_zones` both equal the `program_zones` count from Step 4; `2`; no `*.backup` file; `index.html` in the bundle; and a status with `"running":[]`, `"program":null`, `"queue":[]`, `"maxConcurrentZones":2`.

If anything here fails, roll back (Step 11) before investigating.

- [ ] **Step 6: Two manual zones, then a third refused at the cap**

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
api() { pi "curl -s -w '\n%{http_code}\n' $1"; }
api "-X POST -H 'content-type: application/json' -d '{\"seconds\":60}' http://127.0.0.1/api/zones/1/run"
api "-X POST -H 'content-type: application/json' -d '{\"seconds\":60}' http://127.0.0.1/api/zones/2/run"
api "http://127.0.0.1/api/status"
api "-X POST -H 'content-type: application/json' -d '{\"seconds\":60}' http://127.0.0.1/api/zones/3/run"
```

Expected: `202`, `202`; status `running` holds zones 1 and 2, both `manual`; the third answers `409` with `"reason":"cap_reached"` and `"error":"2 zones already running"`. Relays 1 and 2 click on; relay 3 stays off. Ask the owner to confirm the two relay LEDs.

- [ ] **Step 7: Per-zone stop, then STOP**

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
api() { pi "curl -s -w '\n%{http_code}\n' $1"; }
api "-X POST http://127.0.0.1/api/zones/1/stop"
api "http://127.0.0.1/api/status"
api "-X POST http://127.0.0.1/api/stop"
api "http://127.0.0.1/api/status"
```

Expected: `202`; `running` holds only zone 2; `202`; `running` is empty.

- [ ] **Step 8: A two-zone step, a manual queue entry, and STOP with a scheduled entry queued**

Create two test programs. `Bench A` is disabled so it never fires on its own and runs zones 1 and 2 together for 300 s. `Bench B` is enabled with a start time three minutes from now in the controller's zone, so it comes due while `Bench A` runs and queues as a scheduled entry.

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
api() { pi "curl -s -w '\n%{http_code}\n' $1"; }
pi 'date +%H:%M; timedatectl show -p Timezone --value; curl -s http://127.0.0.1/api/zones'
```

From the output take the controller's zone id and the zone **ids** of zones 1, 2 and 3 (`id` field; equal to the numbers on a freshly seeded database, but read them). Compute `M = (HH*60 + MM + 3) % 1440` from the printed time. Then, substituting `ID1`, `ID2`, `ID3`, `M` and `TZ`:

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
api() { pi "curl -s -w '\n%{http_code}\n' $1"; }
api "-X POST -H 'content-type: application/json' -d '{\"name\":\"Bench A\",\"enabled\":false,\"dayMode\":\"DaysOfWeek\",\"dowMask\":127,\"intervalDays\":0,\"anchorDate\":\"\",\"startTimes\":[{\"minutesAfterMidnight\":180,\"timezone\":\"TZ\"}],\"steps\":[{\"zones\":[ID1,ID2],\"durationSeconds\":300}]}' http://127.0.0.1/api/programs"
api "-X POST -H 'content-type: application/json' -d '{\"name\":\"Bench B\",\"enabled\":true,\"dayMode\":\"DaysOfWeek\",\"dowMask\":127,\"intervalDays\":0,\"anchorDate\":\"\",\"startTimes\":[{\"minutesAfterMidnight\":M,\"timezone\":\"TZ\"}],\"steps\":[{\"zones\":[ID3],\"durationSeconds\":60}]}' http://127.0.0.1/api/programs"
```

Expected: `201` twice. Record both program ids as `A` and `B`. Then:

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
api() { pi "curl -s -w '\n%{http_code}\n' $1"; }
api "-X POST http://127.0.0.1/api/programs/A/run"
api "http://127.0.0.1/api/status"
api "-X POST http://127.0.0.1/api/programs/A/run"
```

Expected: `202`; `running` holds zones 1 and 2, both `program`, and `program` reads `Bench A`, step 1 of 1, no waiting zones; the second run answers `409` `already_queued`.

Wait until the controller clock passes minute `M`, then:

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
api() { pi "curl -s -w '\n%{http_code}\n' $1"; }
api "http://127.0.0.1/api/status"
pi 'sqlite3 /var/lib/irrigationd/irrigation.db "SELECT program_id, start_time_id, scheduled_at_utc, outcome FROM fired_instants ORDER BY id DESC LIMIT 3;"'
api "-X POST http://127.0.0.1/api/stop"
api "http://127.0.0.1/api/status"
pi 'sqlite3 /var/lib/irrigationd/irrigation.db "SELECT program_id, outcome FROM fired_instants WHERE program_id = B;"'
```

Expected: before STOP, `queue` holds `Bench B` and its row reads `queued`; STOP answers `202`; afterwards `running` is empty, `program` is `null`, `queue` is empty, and `Bench B`'s row reads `dropped_stop`. Zone 3's relay never clicks.

- [ ] **Step 9: The restart sweep**

Edit `Bench B`'s start time to three minutes from now again (`PUT /api/programs/B` with the same body and a new `M`), start `Bench A`, wait for `Bench B` to queue as in Step 8, then restart the daemon:

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
api() { pi "curl -s -w '\n%{http_code}\n' $1"; }
pi 'systemctl restart irrigationd; sleep 3; systemctl is-active irrigationd'
api "http://127.0.0.1/api/status"
pi 'journalctl -u irrigationd -n 30 --no-pager | grep -i dropped_restart'
pi 'sqlite3 /var/lib/irrigationd/irrigation.db "SELECT program_id, outcome FROM fired_instants WHERE program_id = B ORDER BY id DESC LIMIT 1;"'
```

Expected: `active`; `running` is empty (the restart released and re-requested every line and nothing reopened); the journal line "1 queued programs were lost to a restart and recorded dropped_restart"; the row reads `dropped_restart`.

- [ ] **Step 10: Clean up and look at the web app**

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
api() { pi "curl -s -w '\n%{http_code}\n' $1"; }
api "-X DELETE http://127.0.0.1/api/programs/A"
api "-X DELETE http://127.0.0.1/api/programs/B"
api "http://127.0.0.1/api/status"
```

Expected: `204` twice and an idle status. Then ask the owner to open the app on a phone, run two zones from the tiles, check the rows, tags, per-zone Stop, the disabled tiles and "2 of 2 running" hint, a refused third run's message, a program with a two-zone step in the editor with its wave warning at cap 1, and the "Max zones at once" field. Leave the backups in place; the owner removes them.

- [ ] **Step 11: Rollback (only if something above failed)**

One command, so the lines are released for well under a second:

```bash
pi()  { SSH_ASKPASS=/bin/true SSH_ASKPASS_REQUIRE=force DISPLAY=none setsid -w ssh -o PubkeyAuthentication=no root@irrigation.local "$1" </dev/null; }
pi 'systemctl stop irrigationd;
    cp -a /usr/bin/irrigationd.pre-concurrent /usr/bin/irrigationd;
    rm -f /var/lib/irrigationd/irrigation.db-wal /var/lib/irrigationd/irrigation.db-shm;
    cp -a /var/lib/irrigationd/irrigation.db.pre-concurrent /var/lib/irrigationd/irrigation.db;
    systemctl start irrigationd;
    rm -rf /var/www/irrigation/html/* && cp -a /var/www/irrigation/html.pre-concurrent/. /var/www/irrigation/html/;
    sleep 3; systemctl is-active irrigationd; curl -s http://127.0.0.1/api/status'
```

The `-wal` and `-shm` files belong to the migrated database; restoring the main file next to them corrupts it. The 1.0.0 binary will not open a 1.1.0 database correctly, so the database and the binary always roll back together.

Expected: `active` and an old-shape status with `"runningZone":0`.

---

## Spec coverage

| Spec | Requirement | Task (code) | Task (test) |
|---|---|---|---|
| §2 | `max_concurrent_zones`, integer 1–8, default 2 | 1, 2, 6 | 12, 13, 14 |
| §2 | Enforced by the zone controller | 1 | 12 |
| §2 | Lowering the cap closes nothing | 1 | 12 |
| §2 | Re-open resets the deadline and takes no slot | 1 | 12 |
| §3.1 | Manual run alongside; refused at the cap with a reason | 1, 6 | 12, 14, 16 |
| §3.1 | Stop-held, master-off, zone-disabled refusals kept | 6 | 14 |
| §3.1 | A manual run no longer aborts the program | 6 | 12 (`takesOverAManuallyOpenZone…`, `aManualZoneClosingFreesASlot…`), 16 |
| §3.2 | Per-zone stop; counts as the zone's share of a step | 1, 6 | 12, 14, 16 |
| §3.3 | STOP closes everything, aborts the program, empties the queue with `dropped_stop` | 4 | 13, 16 |
| §3.3 | Held STOP refuses opens, records `skipped_stop` | 4, 6 | 14 (`stop_held` reason) |
| §3.4 | Steps; every zone runs the full duration from its own open | 3 | 12 |
| §3.4 | Waiting zones; ascending zone number | 3 | 12 |
| §3.4 | Step completes when every zone opened and closed | 3 | 12 |
| §3.4 | Disabled zones skipped; all-disabled step completes at once | 3 | 12 |
| §3.4 | Takeover of an open manual zone | 3 | 12 |
| §3.4 | Waves are legal; the editor warns | 3, 8 | 12, 15 |
| §3.5 | One program at a time, FIFO, one entry per program, `skipped_duplicate` | 4 | 13 |
| §3.5 | A running program may hold one queued entry | 4 | 13 |
| §3.5 | Manual run-program joins the queue; refused if running or queued | 4, 6 | 13, 14, 16 |
| §3.5 | Rain/master re-check at dequeue → `skipped_rain` | 4 | 13 |
| §3.5 | Manual zone runs never queue | 6 | 16 |
| §3.5 | Queue in memory; `dropped_restart` and a warning at startup | 4 | 13, 16 |
| §4.1 | Cap instead of mutual exclusion; one bank write per request | 1 | 12 |
| §4.2 | Per-zone deadline and single-shot close | 1 | 12 |
| §4.3 | Duration clamp per zone | 1 | 12 (existing `durationIsClampedToTheCeiling`) |
| §4.4 | Watchdog compares the expected set; latch release | 1 | 12 |
| §4.5 | Count check against the cap the zones opened under | 1 | 12 (`loweringTheCap…` non-trip side) |
| §4 | Per-zone close reason: deadline, stop, all-off, watchdog | 1 | 12 |
| §5 | `program_steps`, `program_step_zones`; `program_zones` dropped | 2, 6 | 13 |
| §5 | Migration into one-zone steps, same sequence and duration | 2 | 13, 16 |
| §5 | New outcomes; insert `queued`, update on leaving | 2, 4 | 13 |
| §5 | Schema and migration change together | 2 | 13 |
| §6 | `POST /admin/zones/{n}/stop` | 6 | 14 |
| §6 | 409 with reason; decision on the valve thread; reply waits | 4, 6 | 14 |
| §6 | `steps` in program bodies | 6 | 14 |
| §6 | New `/admin/status` shape | 5 | 14 |
| §6 | Curl run and STOP keep working | 6 | 16 |
| §7 | Now: rows, per-zone Stop, tags, step line, queue line, big STOP | 7 | 15 |
| §7 | Tiles glow; Run disabled at the cap with "N of N running"; 409 reason shown | 7 | 15 |
| §7 | Editor: step rows, chips, "+ zone", duration, reorder, delete, waves warning, wave-aware total | 8 | 15 |
| §7 | Settings "Max zones at once" 1–8 | 9 | 15 |
| §7 | Polling 2 s while anything runs or waits | 7 | 15 |
| §8 | Daemon, web and bench tests | 12–16 | — |
| Preamble | Base design §5.1, §5.5, §6, §7, §8 updated | 10 | 11 |

---

## Known gaps

- **The count check (§4.5) has no discriminating test.** With honest bookkeeping the expected set never exceeds the cap the zones opened under, so any over-count read-back is also a set mismatch and trips on that first. Task 12 pins the side that matters in practice: lowering the cap must never trip it.
- **`IrrigationDaemon` has no unit tests.** Its orderings (STOP, teardown, restart sweep, publish-before-complete) are pinned through `ProgramQueue` tests in Task 13 and exercised end to end on the bench in Task 16, and Task 11 reads each against its trap comment.
- **The status snapshot carries no per-zone deadline instant.** Each running row's countdown starts from `secondsRemaining` at the poll, as the single running card did.
