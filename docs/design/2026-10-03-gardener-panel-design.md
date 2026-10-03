# Gardener Panel — Design

Amends `2026-09-05-irrigation-design.md` and builds on
`2026-10-01-concurrent-zones-design.md`. Where this document and either of
those disagree, this one wins for the panel.

## 1. Goal

Let the gardener run, advance and stop zones at the box with no phone and no
network, and see what the controller is doing.

Two uses drive it:

- **Walking the yard.** Start zone 1, look at the heads, press to move to the
  next zone, and so on to the end.
- **Watering one spot.** Pick one zone and leave it running.

The hardware is in the box and proven on the bench: a second 16 mm momentary
button (RUN) beside STOP, and a TM1637 four-digit seven-segment display with a
centre colon behind a window in the right wall.

### Non-goals

- Setting the run time at the panel. One run time comes from Settings.
- A buzzer or any sound.
- Automatic brightness.
- Showing programs, the queue or the rain delay on the display beyond the
  zones they open.
- Any change to STOP.

## 2. Hardware contract

| Signal | BCM | Header pin | Notes |
|---|---|---|---|
| RUN, 1NO to GND | 24 | 18 | Active low. Pull-up from `gpio=24=ip,pu` at boot and the daemon's bias request after that. 20 ms kernel debounce, as STOP. |
| Display CLK | 18 | 12 | Push-pull output. The module has no pull-up on CLK. |
| Display DIO | 27 | 13 | Push-pull output, driven low through every acknowledge clock. |
| Display VCC, GND | — | — | HAT +3V3 and GND rows. |

New `[gpio]` keys in `irrigationd.ini`: `runButtonOffset=24`,
`displayClockOffset=18`, `displayDataOffset=27`. A missing key disables that
part of the panel and logs it; the daemon runs without it.

The display protocol, as measured on this module:

- LSB first, about 10 µs per edge.
- Each update sends the data command `0x40`, then the address command `0xC0`
  followed by four segment bytes, then the display-control command
  `0x88 | brightness`.
- DIO is driven low during each acknowledge clock, so the chip's
  acknowledge pull-down never meets a driven high. A DIO run that leaned on
  the module's pull-up stayed blank on the bench; push-pull on both lines
  worked first time.

Brightness is fixed at 4 of 0–7, the level read comfortably in the shade
during bring-up.

## 3. Behaviour

### 3.1 Idle

A 12-hour clock in the controller's zone, `h:mm` with the leading digit blank
before 10 o'clock (` 6:42`). The colon blinks: on for half a second, off for
half a second. No AM/PM indicator.

### 3.2 Starting a run

1. A press shows the selection: the first enabled zone's number blinking in
   the left digit (on and off every half second) and the panel run time in
   minutes, steady, in the right two digits: `1 10`.
2. Each further press moves to the next enabled zone in ascending order,
   wrapping from the last enabled zone back to the first. Disabled zones are
   never offered.
3. Three seconds after the last press, the shown zone starts as a panel run.
   The zone digit stops blinking and the right two digits count down.

With no zone enabled, a press shows `nonE` for two seconds and returns to the
clock.

### 3.3 A panel run

- The display shows the panel zone and its minutes left, rounded up:
  `3 12`, `3 11`, down to `3  1`.
- A press closes the panel zone and opens the next enabled zone at once, for
  the full panel run time. There is no selection delay in this step.
- A press on the last enabled zone closes it and ends the panel run.
- When the panel zone's timer ends, the panel run ends. It never moves on by
  itself.
- When the panel run ends, the display returns to the clock, or to the
  running-zones view if anything else is still open.

### 3.4 Something else running

When zones are open that the panel did not start (a program, or a manual run
from the app or the API) and no panel run is active:

- The display shows one open zone and its minutes left, in the panel-run
  format. With more than one open, it moves to the next in ascending zone
  number every two seconds.
- A press **takes over**. It clears the controller exactly as an API STOP
  does: every zone closes, the running program aborts, and the queue empties,
  each dropped scheduled entry recorded `dropped_stop`. Then it shows the
  selection as in 3.2, step 1.

During a panel run, the display shows only the panel zone, even if a program
or an app run opens another zone alongside it.

### 3.5 Refusals and states

| State | Display | RUN |
|---|---|---|
| STOP held | `StOP` | Ignored |
| Controller faulted (watchdog latch) | `Err` | Ignored |
| Master enable off | `OFF` for two seconds on a press, then back | Starts nothing |
| Open refused at the cap | `FuLL` for two seconds, then back | The run does not start |

These apply at every point a panel run would open a zone: when a selection
commits, and on each press during a panel run. STOP held and a fault win
over everything else on the display.

### 3.6 Rules a panel run follows

A panel run is a manual zone run with a different source:

- The duration is the panel run time, clamped by the zone-duration ceiling
  like every manual run.
- It counts against the concurrency cap.
- It is refused for the same reasons as a manual run from the app: STOP held,
  master enable off, zone disabled, cap reached.
- STOP and an API STOP end it like any other run.
- Its status entries carry `source: "panel"`.

A program that comes due during a panel run starts or queues by its normal
rules.

### 3.7 A RUN line held low

Presses are falling edges. A line that stays low for more than ten seconds is
logged once as stuck. It produces no further presses until it has been seen
high again. At startup, a line that already reads low produces no press.

## 4. Settings

New settings key `panel_run_minutes`, an integer 1–60, default 10. The web
app's Settings screen gains a field for it, "Gardener panel run time
(minutes)". The fresh-install schema and the migration carry the default
together, per section 6 of the base design.

## 5. Status and web

- `running[].source` gains the value `"panel"`.
- The Now screen tags panel rows and tiles "panel", next to the existing
  "program" and "manual" tags.
- The display's state is not exposed over the API.

## 6. Boot and shutdown

### 6.1 Starting sooner

Today the daemon waits for the network to come up, about 16 seconds after
the kernel starts. It needs no network: its control server listens on
loopback behind nginx.

- The unit no longer orders itself after the network-online target.
- It orders itself after the RTC device. The RTC is what puts the right time
  on the system clock (5.9 s into the measured boot). The time-set target is
  reached earlier (5.5 s), on the timestamp saved at the last shutdown, and is
  not enough for a controller that schedules by wall clock.
- The daemon draws its first screen within one second of starting.

On the measured boot, this moves the daemon from 15.9 s to about 8 s of
kernel time, roughly ten seconds after power-on.

### 6.2 No stale screen

The TM1637 holds its last image for as long as it has power. A daemon exit
or a soft reboot does not clear it.

- Whenever the daemon stops, cleanly or not, the unit's stop-post hook runs
  the daemon executable in a one-shot mode. That mode writes `----` to the
  display and exits, and touches no other line.
- A daemon that starts overwrites whatever is on the display with its first
  draw.

So a stopped controller, and a reboot in progress, show `----`. A fresh
power-on shows nothing until the daemon's first draw.

## 7. Display failure

The display is informational. A failed line request or write is logged, and
the panel carries on without the display. RUN keeps working, and watering is
untouched. A daemon with no display lines configured runs RUN alone.

## 8. Testing

Daemon unit tests cover:

- **The panel state machine,** with a test clock and fake presses:
  - selection order and wrap over enabled zones only, plus the three-second
    commit
  - next zone during a run, and the end at the last zone and at the timer
  - takeover clearing a program, app zones and the queue with `dropped_stop`
  - each refusal display in 3.5, and STOP held and fault taking priority
  - alternation over other running zones
  - the stuck-line rule
- **Display formatting:** the clock, the zone-and-minutes format with
  rounding, the blink phases, and every glyph used (`StOP`, `Err`, `OFF`,
  `FuLL`, `nonE`, `----`).
- **TM1637 framing** against the in-memory GPIO backend: the line writes for
  one update, including DIO low through each acknowledge clock.
- `panel_run_minutes` validation and the migration default.

Web tests cover the "panel" tag and the Settings field.

A bench run on the controller ends it:

- power-on to clock time
- a walk through the enabled zones
- picking zone 3
- takeover of a running program
- STOP held showing `StOP`
- `kill -9` of the daemon leaving `----`
- a reboot showing `----` and then the clock

## 9. Rollout

The daemon and the web bundle can be deployed onto the running controller,
as the concurrent-zones bench run was. The unit-file ordering, the stop-post
hook, and `gpio=24=ip,pu` (already added by hand on the controller) belong
in the image. They ride the next reflash together with the other on-device
changes waiting for it.
