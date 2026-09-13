# Handoff — irrigation daemon, Task 10 review through branch finish

Written 2026-09-13 ~15:00 by session **Irrigation (W3)**, which stopped on an
org monthly spend limit (429, session limit resets 15:10 America/Los_Angeles).
Intended for a fresh agent, possibly on a different account, with none of this
session's context.

Read this file, then read the ledger. Everything else is detail.

## Stop cause and what it interrupted

An Opus task reviewer for Task 10 was dispatched and **died before producing a
verdict** — it had read nothing but the brief when the API refused. So:

- **Task 10 has code committed but NO review.** It is not complete.
- Nothing is half-written on disk. The working tree is clean of this session's
  work. Every task's code is committed.

## Where everything is

- Repo: `~/src/punak/irrigation`, branch `feature/superproject`.
- **Ledger: `.superpowers/sdd/2026-09-12-irrigationd/progress.md`.** Read all of
  it. It names every commit, every ruling with its cost-if-wrong, and every
  parked finding. It is the recovery map and it is authoritative over this file
  where the two disagree.
- Per-task briefs and reports sit beside it as `task-N-brief.md` /
  `task-N-report.md`, plus `retrofit-{a,b,c,d}-report.md`.
- **House conventions: `.superpowers/sdd/2026-09-12-irrigationd/house-style.md`.**
  Every dispatch this session read it first. It indexes the real reference files
  (`~/src/epc/meta-qt-mains`, `~/src/punak/KanoopCommonQt/CLAUDE.md`) with their
  traps attached.
- Plan: `docs/plans/2026-09-12-irrigationd.md`. Spec:
  `docs/design/2026-09-05-irrigation-design.md` — **the spec is the binding
  authority; the plan argues from it.**
- Skill: `superpowers:subagent-driven-development`, scripts at
  `~/.claude/plugins/cache/claude-plugins-official/superpowers/6.3.0/skills/subagent-driven-development/scripts/`.

## Task state

| Task | State |
|---|---|
| 1 GPIO read-back (KanoopPiQt submodule) | complete |
| 2 skeleton, settings, signals | complete |
| 3 schema, migrations, data source | complete |
| 4 models and repository | complete |
| 5 ZoneController | complete |
| 6 clock seam and Scheduler | complete, `1737187..7f6df2b`, 2 fix rounds |
| 7 ProgramRunner | complete, `85bdcbd..34b0943`, 1 fix round |
| 8 StopButton | complete, `34b0943..7389b2e`, 2 fix rounds |
| 9 IrrigationControlServer | complete, `676a41c..22ae437`, 1 fix round |
| 10 assembly and systemd | **code committed `8cc86fb`, REVIEW NEVER RAN** |

A four-part house-pattern retrofit the user ordered mid-session also landed:
`82ceba3` (EnumToStringMap), `71c0a51` + `d9d4c9b` (AppSettings path ctor),
`9f11aeb` (AbstractThreadClass daemon), `1093ab8` + `85bdcbd` (Doxygen sweep and
unquoted-INI tolerance).

## What remains, in order

1. **Review Task 10** over `5325980..8cc86fb`. A path-scoped package already
   exists at `.superpowers/sdd/2026-09-12-irrigationd/review-t10-scoped.diff`,
   and the dispatch prompt that died is reconstructible from the ledger's Task 10
   entries. Use a capable model — this is the boot path.
2. **Task 10 fix round.** Two findings are ALREADY RULED and go back to the
   implementer regardless of what the review adds. Both are in the ledger with
   full reasoning:
   - **A stop button held at startup inhibits nothing.** `irrigationdaemon.cpp:63`
     reads `StopButton::isHeld()` and only logs. The unit ships `Restart=always`
     with `RestartSec=5`, so a daemon restarting while someone holds the
     emergency stop boots, notes it, and waters anyway. Every piece needed
     already exists: `begin()` reads the line's starting state for exactly this
     reason, and Task 8's fix made `_held` track both edges so a release clears
     it. Inhibit all runs, scheduled and manual, while the line reads held.
   - **`skipped_busy` is unreachable in production.** `Scheduler::tick()` records
     the firing as `Ran` before emitting `programDue`, and `recordFiring()` is
     `INSERT OR IGNORE`, so when `ProgramRunner` refuses the skip cannot
     overwrite the row and `fired_instants` claims the program ran. The
     record-before-emit ordering is correct and must stay — it is what makes a
     crash lose one run rather than double-water — so the fix is an outcome
     UPDATE after the fact, needing one new `IrrigationDataSource` method plus a
     call from the daemon where `startProgram()`'s false is already visible.
3. **Scoped re-review** of that fix round.
4. **The final test pass.** Tasks 6-10 delivered code only, under a standing user
   policy that tests are written at the end. So `tst_scheduler`,
   `tst_programrunner`, `tst_stopbutton`, `tst_controlserver` and `tst_daemon` do
   not exist. This is the highest-risk remaining work — see "What this project
   keeps getting wrong" below.
5. **Whole-branch review** on the most capable model available, over
   `e105d46..HEAD` path-scoped to `IrrigationD KanoopCommonQt KanoopPiQt`. Point
   it at the ledger's deferred and parked lines.
6. **`superpowers:finishing-a-development-branch`**, and before deleting the
   workspace, collect every ledger line containing `Ruling:` into the final
   message. There are 52. That list is the only place the decisions taken on the
   user's behalf reach him.

## Push state — corrected, do not repeat my error

**`origin/feature/superproject` DOES NOT EXIST. 75 commits, nothing pushed.**

I earlier reported "0 unpushed" from `git log origin/feature/superproject..HEAD
2>/dev/null | wc -l`, which swallowed a fatal and counted zero lines. Never
redirect stderr on a command whose failure mode is an empty result you would act
on.

**Two pushes are needed, and they are coupled:**

1. `irrigation` superproject, branch `feature/superproject`.
2. `KanoopCommonQt` submodule, branch `feature/appsettings-path-ctor`, commit
   `71c0a51`, pushed to the KanoopCommonQt remote.

The second exists because retrofit C added a path constructor to `AppSettings`
in the submodule and bumped the superproject's pointer in `d9d4c9b`. W2's Yocto
recipe fetches with `gitsm://`, which follows the recorded submodule SHA, so a
superproject push alone makes `do_fetch` fail with an error naming the
submodule rather than the missing push. **Ask the user for both together.** Only
the user pushes; a hook blocks it.

The submodule branch was created with `--no-track` off local `master`, which
carries merged EPC pull requests from other work — an irrigation change does not
belong on its trunk unilaterally. If the user later merges it, the superproject
pointer should be re-bumped to the merged commit.

## Peer sessions — three sessions share this ONE working tree and branch

- **Irrigation (W1)** owns the web frontend (`web/`) and
  `docs/plans/2026-09-13-irrigation-web.md`.
- **Irrigation (W2)** owns `~/src/punak/rpi` (the Yocto layer) and its own
  ledger at `.superpowers/sdd/2026-09-13-irrigation-yocto-layer/`. Read it
  freely, never write it.
- **Yours:** `IrrigationD/`, the Kanoop submodules,
  `docs/plans/2026-09-12-irrigationd.md`, and
  `.superpowers/sdd/2026-09-12-irrigationd/`.

Consequences, all learned the hard way this session:

- **Never `git add -A`.** Run `git status --porcelain` and stage by explicit
  path, every commit.
- **Never amend a commit that is not HEAD**, and check HEAD has not moved even
  then. Two commit-message blemishes were parked rather than amended for exactly
  this reason (`bfc8374`, `251cc73`); one was amended safely because it was still
  HEAD with a clean tree (Task 7's, now `34b0943`).
- **`scripts/review-package` over a SHA range is not your task's diff.** A peer
  commit landing between your base and head pulls its content in — one package
  came back 212KB of someone else's markdown. Every package this session was
  rebuilt path-scoped to `IrrigationD/`. Keep doing that.

## Open items needing the USER, not you

1. **Should a malformed `gpio/zones` pair be a hard startup failure?** Today
   malformed entries are dropped and the survivors keep their numbers — the
   documented contract, deliberate in the plan. A warning per skipped pair now
   exists. Turning one typo into a daemon that refuses to start is a product
   decision. Raised, unanswered.
2. **Should a manual run be allowed during a rain delay?** I ruled
   `master_enabled` is a hard disable that manual cannot override, and that
   `rain_delay_until` does NOT block a manual run, on the grounds that a human
   pressing run is present and explicit in a way a schedule is not. The second
   half is his call. Raised, unanswered.
3. **`User=root` in the systemd unit.** It works for GPIO and the state
   directory; least privilege wants a Yocto-created service user in the `gpio`
   group, which spans this repo and W2's layer. Raised, unanswered.

## A change that landed under this session at 14:19

Commit `35a4141`, from another session, **changed the zone GPIO map**: the
GeeekPi proto-HAT does not break out BCM 26, so zones 1-8 are now BCM
5, 6, 12, 13, 16, 19, 20, 21. It touched `tst_settings.cpp`,
`tst_zonecontroller.cpp` and three documents — no production code, because the
map lives in the INI.

Two consequences:
- Task 10's live verification transcript predates it and names the old pins
  (it reports driving `gpio-16` for zone 4; under the new map zone 4 is BCM 13).
  The daemon reads the map from the INI, so this invalidates the transcript's
  pin numbers, not its conclusions.
- **W2's Yocto layer ships `/etc/irrigationd.ini`** and needs the new values.
  W2 has been told.

## Standing constraints on every dispatch

- **Never `git push`.** A hook blocks it; the user controls all pushes.
- **Never discard uncommitted changes** — no `git checkout --`, `git restore`,
  `git clean -f`, `git reset --hard` — and never delete an untracked file or
  directory you did not create. An implementer once deleted a pre-existing
  untracked `CMakeFiles/` believing it had created it. There is an untracked
  `web/` tree belonging to W1; leave it alone.
- `set(CMAKE_CXX_STANDARD 11)` is a floor Qt6 raises to C++17. Do not add
  `CMAKE_CXX_STANDARD_REQUIRED ON`.
- `-Wextra -Wall -Werror` stays clean.
- **Comments state traps, never reasoning.** A comment earns its place only when
  it states something the code cannot show that a plausible edit would silently
  break. Five tasks lost review rounds to this.
- **No "it is X, not Y" antithesis** anywhere, commit messages included. Delete
  the negated clause; if the sentence still says everything, the construction
  was doing nothing.
- **Never a Doxygen comment on a member variable.** Absolute, from
  KanoopCommonQt's own CLAUDE.md.
- **Never specify `Qt::QueuedConnection`** and never comment about connection
  types.
- Every `.cpp` whose header declares `Q_OBJECT` ends with its moc include,
  path-prefixed relative to `IrrigationD/src`, last line, preceded by a blank.
- Instants are UTC; schedule rules stay local wall-clock plus an IANA zone id.
- Conventional commits signed with **your own** session's attribution trailer.
- Budget: implementers on the cheapest model that can do the job — most briefs
  contain the code. Spend capable models on reviews of GPIO state, scheduling
  arithmetic, and the boot path.

## What this project keeps getting wrong

**Tests that cannot fail.** Every task so far went back at least once, and
almost every time it was a test whose passing proved nothing:

- Task 1: a read-back test that could not tell the cache from the hardware.
- Task 3: a migration engine no test ever executed.
- Task 4: three fields written and never read back; then two foreign keys whose
  equal values made a transposed bind invisible.
- Task 5: `begin()`'s guard test passed with the guarded line deleted, and the
  close-timer test passed with the close timer deleted, because the watchdog
  silently substituted and emitted the identical signal.
- W2's Yocto layer: an INI validated with Python's `configparser`, which parses
  the unquoted form happily, while the real reader (QSettings) returned an empty
  map and the daemon requested zero GPIO lines.

**When the final test pass comes, make every test answer: what mutation of the
production code would make this fail?** If you cannot name one, it is
decoration. Watch for aliasing — two ids, two offsets, two durations carrying
the same value in a fixture makes a transposition between them invisible. And a
fixture must be read by the code that will actually read it in production.

Note also: Tasks 6-10 rest almost entirely on throwaway scratch harnesses that
were deleted. The 21-test suite exercises none of that code. Several reports
name the exact scenarios they drove; port them rather than starting cold.

## Errors I made, so you do not repeat them

- **I claimed foreign keys were not enforced** and built a three-task defect on
  top of it. They are — `DataSource::openConnection()` calls
  `setSqliteForeignKeyChecking(true)` at `KanoopDatabaseQt/src/database/datasource.cpp:71`.
  I had grepped for `setForeignKeys|foreignKeys`; the real name is
  `setSqliteForeignKeyChecking`, which that pattern never matches, and I read the
  empty result as proof of absence. **Suspect the query before the scope.**
- **I misrouted two mid-flight corrections to the wrong subagent**, both times
  taking the first id from a paired dispatch when I meant the second. Both
  landed on read-only reviewers that refused them, so nothing broke, but one
  implementer never received guidance I believed I had sent. Re-read which id
  belongs to which role before any follow-up.
- **I told an implementer "do not lean on `isHeld()`"** meaning "do not use it to
  detect a failed `begin()`", and it was loose enough to read as "ignore a held
  button". That ambiguity is the origin of the held-stop-button hole now queued
  for the fix round.
- **I ordered a re-entrancy guard for a Minor that made the emergency stop
  strictly worse** — it tested `_pin != nullptr` while `_pin` was assigned before
  both failure returns, so one transient failure left the stop dark for the life
  of the process. The scoped re-review caught it. Fix diffs are where
  regressions enter, because they look like tidying.
- **I reported "0 unpushed" from a command whose stderr I had redirected.** See
  the push section.

## Hardware facts worth carrying

- Six of eight zone lines come up ASSERTED at power-on (BCM 9-27 pull down;
  BCM 5 and 6 pull up and come up de-asserted). Both mitigations in design
  section 2.6 remain required. This is why construction order is a hardware
  contract: `ZoneController` is built and drives all eight lines inactive before
  the scheduler or HTTP server exist.
- `InputPin` defaults to `Gpio::Bias::AsIs` and `activeLow = false`
  (`inputpin.h:90,93`), both wrong for a button wired to ground. `StopButton`
  sets `PullUp` and `activeLow(true)` explicitly before `request()`, and W2's
  `config.txt` carries `gpio=25=ip,pu` as the firmware-side defence.
- `/etc/irrigationd.ini`'s `gpio/zones` value MUST be quoted. Unquoted, QSettings
  types it as a QStringList and `.toString()` returns empty, so the daemon
  requests zero lines and dies on a bare EINVAL. The daemon now tolerates both
  spellings; quote it anyway.
