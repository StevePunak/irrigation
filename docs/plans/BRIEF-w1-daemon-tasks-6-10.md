# Brief for Irrigation (W1) — daemon Tasks 6-10

Take this only when the other session hands it over. Tasks 1-5 are complete and
reviewed; 6-10 are not started. If the other session is still live, do not
start — two controllers running fix rounds against the same files is how you
get a merge conflict in `zonecontroller.cpp`, which is the one file in this
project where a bad merge means a valve stuck open.

## Where everything is

- Repo: `~/src/punak/irrigation`, branch `feature/superproject`.
- Plan: `docs/plans/2026-09-12-irrigationd.md` — ten tasks.
- Spec: `docs/design/2026-09-05-irrigation-design.md` — sections 5, 6, 7, 10.
- **Ledger: `.superpowers/sdd/2026-09-12-irrigationd/progress.md`.** Read this
  first and read all of it. It names every commit, every ruling with its
  cost-if-wrong, and every parked finding. It is the recovery map.
- Briefs for all ten tasks are already extracted next to the ledger as
  `task-N-brief.md`. Do not make an implementer read the whole plan.
- Skill: `superpowers:subagent-driven-development`. Its scripts live at
  `~/.claude/plugins/cache/claude-plugins-official/superpowers/6.3.0/skills/subagent-driven-development/scripts/`.

## State at handoff

| Task | Status |
|---|---|
| 1 GPIO read-back (KanoopPiQt submodule) | complete, `daf25a9..abaab14` |
| 2 skeleton, settings, signals | complete, `6769c64..828cc23` |
| 3 schema, migrations, data source | complete, `828cc23..2632449` |
| 4 models and repository | complete, `2632449..9fadf7c` |
| 5 ZoneController | complete, `9fadf7c..bfc8374` + submodule `0e49341` |
| 6 clock seam and Scheduler | not started |
| 7 ProgramRunner | not started |
| 8 StopButton | not started |
| 9 IrrigationControlServer | not started |
| 10 assembly and systemd | not started |

Record BASE with `git rev-parse HEAD` before dispatching each implementer. The
review package script needs it, and `HEAD~1` silently truncates a multi-commit
task.

## Two plan amendments that override the task text

1. **`bool allOff()`**, amended at commit `dcd3e09`. The plan's Interfaces block
   said `void`. An e-stop whose caller cannot learn the shutdown failed leaves a
   valve open with nothing watching it. Tasks 7 and 9 both call this.
2. **Tests come at the end**, amended at commit `0e2fbd5`. Tasks 6-10 deliver
   code only. The "Write the failing test" and "Run it to verify it fails" steps
   in their briefs are superseded — say so explicitly in every dispatch, because
   the brief text will tell the implementer otherwise. Unit tests for 6-10 are
   written in one pass after that code settles. Build and wiring assertions that
   prove an artifact is real still belong to the task that makes the claim.

## What this project keeps getting wrong

Every task so far went back at least once, and almost every time it was a test
that could not fail:

- Task 1: a read-back test that could not tell the cache from the hardware.
- Task 3: a migration engine no test ever executed — every test built a fresh
  database that landed directly at the current version.
- Task 4: three fields written and never read back; then two foreign keys whose
  equal values made a transposed bind invisible; then a third round because
  "not fixable" turned out to mean "did not try `rawQuery`".
- Task 5: `begin()`'s guard test passed with the guarded line deleted, and the
  close-timer test passed with the close timer deleted, because the watchdog
  silently substituted and emitted the identical signal.

When the final test pass comes, make every test answer: **what mutation of the
production code would make this fail?** If you cannot name one, it is
decoration. Watch for aliasing — two ids, two offsets, two durations carrying
the same value in a fixture makes a transposition between them invisible.

## Constraints

- **Never run `git push`.** A hook blocks it; the user controls all pushes.
- **Never discard uncommitted changes** — no `git checkout --`, `git restore`,
  `git clean -f`, `git reset --hard` — and never delete untracked files you did
  not create. An implementer already deleted a pre-existing untracked
  `CMakeFiles/` believing it had created it; carry this rule in every dispatch.
- `set(CMAKE_CXX_STANDARD 11)` is a floor Qt6 raises to C++17. Do not add
  `CMAKE_CXX_STANDARD_REQUIRED ON`.
- `-Wextra -Wall -Werror` stays clean.
- **Comments state traps, not reasoning.** Four tasks have lost rounds to this.
- **No "it's X, not Y" antithesis** anywhere, commit messages included.
- **Never specify `Qt::QueuedConnection` explicitly.**
- Every `.cpp` whose header declares `Q_OBJECT` ends with its moc include,
  path-prefixed relative to the include root, last line preceded by a blank.
- Instants are UTC. Schedule rules stay local wall-clock plus an IANA zone id;
  normalising a recurring rule to UTC at write time drifts an hour at each DST
  transition.
- Conventional commits, signed with **your own** session's attribution trailer.

## Budget

The user is watching usage. Implementers go on the cheapest model that can do
the job — most of these tasks contain the code to write in the brief. Spend the
capable models on reviews of anything touching GPIO state, scheduling
arithmetic, or the boot path.

## When you finish

Whole-branch review on the most capable model available, then
`superpowers:finishing-a-development-branch`. The parked minors at the end of
the ledger are inputs to that review.
