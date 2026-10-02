# Concurrent Zones — Design

Amends `2026-09-05-irrigation-design.md`. Where the two disagree, this document
wins; sections 5.1, 5.5, 6, 7 and 8 of the base design are updated when this
lands.

## 1. Goal

Run more than one zone at a time, both by hand and inside programs, without
letting the controller open more valves than the water supply and the
transformer can carry.

Two uses drive it:

- **Manual overlap.** Start zone 3 from the app while zone 1 keeps running,
  each on its own timer.
- **Parallel program steps.** A program runs drip zones 5 and 6 together for
  30 minutes, then zone 1 alone.

### Non-goals

- Flow-aware scheduling. There is no per-zone GPM figure and no supply limit
  beyond the zone-count cap.
- Running two programs at the same time. Programs queue.
- Persisting the program queue across a daemon restart.
- The gardener's panel (RUN/NEXT button, display). It is designed separately
  and inherits these rules.

## 2. The concurrency cap

A setting, **max zones at once**, bounds how many zones may be open together.

- Settings key `max_concurrent_zones`, integer 1–8, default **2**.
- Enforced by the zone controller, the sole owner of the GPIO lines, so no
  caller above it can open past the cap.
- Lowering the cap never closes an open zone. It only refuses new opens until
  the count falls below the new value.
- Re-opening a zone that is already open does not count against the cap. It
  sets that zone's deadline to now plus the requested duration.

At cap 1 the controller still differs from today's: a second manual run is
refused instead of replacing the open zone.

## 3. Runtime rules

### 3.1 Manual zone runs

- A manual run opens the zone alongside whatever is already open if a slot is
  free.
- At the cap, the run is **refused** and the caller is told why ("2 zones
  already running"). Nothing that is running is closed to make room.
- Re-running an open zone resets its timer (section 2).
- The existing refusals stay: STOP held, master enable off, zone disabled.
- A manual run no longer aborts a running program. The program keeps running;
  the manual zone occupies a slot.

### 3.2 Per-zone stop

A new request closes one zone. If that zone belongs to the current program
step, closing it counts as that zone finishing its share of the step, and the
program carries on.

### 3.3 STOP

The API stop and the physical STOP button keep one meaning: **everything off
and stays off**.

- Every open zone closes.
- The running program is aborted.
- The program queue is emptied. Each dropped scheduled entry is recorded with
  outcome `dropped_stop`.
- While the button is held, every open request is refused and every program
  that comes due is recorded `skipped_stop`, as today.

### 3.4 Programs and steps

A program is an ordered list of **steps**. A step is a non-empty set of zones
and one duration.

- The runner opens every zone in the step. Each zone runs for the step's full
  duration measured from the moment it opened.
- A step zone that does not fit under the cap **waits** for a slot. It opens
  when one frees and then runs its full duration. A step zone is never skipped
  for lack of a slot.
- When the waiting zones of a step compete for one freed slot, they open in
  ascending zone number.
- The step completes when every one of its zones has opened and closed. The
  runner then starts the next step.
- A disabled zone in a step is skipped, as today. A step whose zones are all
  disabled completes immediately.
- If a step zone is already open when the step starts (a manual run), the step
  takes it over: its deadline becomes now plus the step duration.

A step with more zones than the cap is legal. It simply runs in waves. The
editor shows a warning on such a step so it is visibly slower than it looks.

### 3.5 Program queue

At most one program runs at a time. A program that comes due while another is
running is **queued** and runs in full when its turn comes.

- First in, first out.
- A program may hold at most one queue entry. A start time that comes due while
  the same program is already queued is recorded `skipped_duplicate`. A program
  that is running may also have one queued entry, so two start times close
  together both water.
- A manual "run program now" request joins the same queue. If the program is
  already running or queued, the request is refused.
- When an entry reaches the head of the queue, the rain delay and master enable
  are checked again. A rain delay set while the entry waited records it
  `skipped_rain` and nothing opens.
- Manual zone runs never wait in the queue. They either fit under the cap or
  are refused.
- The queue lives in memory. A daemon restart loses it; each lost scheduled
  entry is recorded `dropped_restart` at the next startup and a warning is
  logged.

## 4. Safety contract changes

Section 5.1 of the base design becomes:

1. **Cap instead of mutual exclusion.** An open request that would exceed
   `max_concurrent_zones` fails, and all line changes for one request still go
   out in a single bank write.
2. **No open without a deadline.** Unchanged, now per zone: every open zone has
   its own deadline and its own single-shot close.
3. **Duration clamp.** Unchanged, applied per zone.
4. **Watchdog.** The periodic tick reads the bank back and compares it with the
   expected *set* of open zones. A zone past its deadline, a line that differs
   from the expected set, or a failed read-back trips the latch exactly as
   today. The latch's release condition becomes "a later tick reads back exactly
   the expected set, with no zone past its deadline".
5. **Count check.** The watchdog also trips if the number of asserted lines ever
   exceeds the cap in force when they were opened. This is the read-back check
   that the cap is real.

`allOff()` is still callable from anywhere and still wins over everything.

The zone controller reports a closure per zone with the reason (deadline,
per-zone stop, all-off, watchdog), so the runner can tell a finished step zone
from a STOP.

## 5. Data model

Steps replace the flat zone list.

```
program_steps         id, program_id, sequence, duration_seconds
program_step_zones    id, step_id, zone_id
```

`program_zones` is dropped. The migration turns each existing `program_zones`
row into a one-zone step with the same sequence and duration, so every existing
program behaves exactly as before.

`fired_instants.outcome` gains `queued`, `dropped_stop`, `dropped_restart` and
`skipped_duplicate`. A firing is inserted as `queued` the moment it comes due and
updated to its final outcome (`ran`, `skipped_rain`, `dropped_stop`,
`dropped_restart`) when it leaves the queue. A program that starts immediately
is recorded `ran` directly. `skipped_busy` is no longer produced; old rows keep
it.

New settings key: `max_concurrent_zones` (integer, default 2).

Both the fresh-install schema and the migration change together, per section 6
of the base design.

## 6. REST API

```
POST   /admin/zones/{n}/run      { "seconds": N }     unchanged path
POST   /admin/zones/{n}/stop                          new
POST   /admin/programs/{id}/run                       joins the queue
POST   /admin/stop                                    unchanged path
```

- A run that is refused answers **409** with a JSON body naming the reason
  (`cap_reached`, `stop_held`, `master_disabled`, `zone_disabled`,
  `already_queued`). The decision is made on the thread that owns the valves,
  and the HTTP reply waits for it. Today's "accepted, decided later" reply
  cannot report a refusal.
- Program bodies carry `steps: [{ zones: [zoneId…], durationSeconds }]` in place
  of the flat zone list.

`/admin/status` changes shape. `runningZone` and `secondsRemaining` are removed:

```
{
  "running":  [ { "zone": 5, "secondsRemaining": 1712, "source": "program" },
                { "zone": 1, "secondsRemaining": 240,  "source": "manual" } ],
  "program":  { "id": 2, "name": "Morning Drip", "step": 1, "stepCount": 2,
                "waitingZones": [7] },
  "queue":    [ { "programId": 1, "name": "Summer", "queuedAtUtc": "…" } ],
  "maxConcurrentZones": 2,
  "nextRunUtc": "…", "rainDelayUntilUtc": "…", "masterEnabled": true,
  "stopHeld": false, "timezone": "America/Los_Angeles"
}
```

`program` is `null` when no program runs; `queue` is empty when nothing waits.
The curl commands for running a zone and for STOP keep working unchanged.

## 7. Web interface

- **Now.** The single running card becomes a list with one row per open zone:
  name, countdown, a Stop button, and a program/manual tag. Under it, the
  running program's step ("Morning Drip — step 1 of 2, zone 7 waiting") and a
  queue line ("Queued: Summer"). The big STOP stays.
- **Zone tiles.** Every open zone's tile glows. At the cap, Run is disabled on
  tiles that are not open, with a "2 of 2 running" hint. A refused run's reason
  from the 409 is shown as the error.
- **Program editor.** Each row is a step: zone chips with a "+ zone" picker, one
  duration, reorder and delete. A step with more zones than the cap shows "runs
  in waves" under it. The computed total runtime assumes waves.
- **Settings.** A "Max zones at once" field, 1–8.

Polling stays as today: every 2 s while anything runs or waits, 15 s otherwise.

## 8. Testing

Daemon unit tests cover:
- the cap at the controller
- deadline bookkeeping per zone
- the watchdog's set comparison and count check
- step waves and the zone-number order of waiting zones
- step takeover of a manually open zone
- per-zone stop inside a step
- queue order, duplicate suppression, the rain/master re-check at dequeue, and
  STOP emptying the queue with the right outcomes
- `dropped_restart` recording at startup
- the step migration against a populated database

Web tests cover:
- the multi-row Now screen
- tile disabling at the cap
- the 409 message
- the step editor and its wave warning

A bench run on the controller ends it: two manual zones, a two-zone step, a
third run refused at cap 2, and STOP with a queued program.
