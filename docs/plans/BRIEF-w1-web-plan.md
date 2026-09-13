# Brief for Irrigation (W1) — write the web frontend implementation plan

Another session is driving the daemon build in this same repo. You and it work
disjoint subtrees; there is no file you both touch. The two sessions are on
different accounts and cannot message each other, so the repo is the only
coordination channel: read the ledger and the plans, and leave your own work
where the other session can find it.

Repo: `/home/spunak/src/punak/irrigation`, branch `feature/superproject`.

## What to do

Write an implementation plan for the web interface, from **section 8** of
`docs/design/2026-09-05-irrigation-design.md`. Use the
`superpowers:writing-plans` skill. Save it to
`docs/plans/2026-09-13-irrigation-web.md`.

`docs/plans/2026-09-12-irrigationd.md` is the format reference — match its
header, its Global Constraints block, its File Structure table, and its task
shape.

## Read first

- `docs/design/2026-09-05-irrigation-design.md` — section 8 is your scope,
  section 7 is the REST API you consume, section 5 says what the daemon does.
- `docs/plans/2026-09-12-irrigationd.md` — Task 9 is `IrrigationControlServer`
  and defines the endpoints you call. Its text is already extracted to
  `.superpowers/sdd/2026-09-12-irrigationd/task-9-brief.md` if that reads
  easier.
- `.superpowers/sdd/2026-09-12-irrigationd/progress.md` — the daemon build's
  ledger. Tells you what already exists and every ruling made so far.

Section 8 is thin: three screens, a polling interval, a timezone rule. Expect
to make decisions the spec does not make for you. Make them, state them in the
plan, and do not invent scope beyond those three screens.

## Facts the spec does not carry

- The daemon binds `127.0.0.1:8080` and nginx reverse-proxies `/api` to it. The
  browser never talks to the daemon directly.
- Every timestamp the API returns is UTC, and section 8 requires rendering in
  the **controller's** timezone as reported by `/admin/status`. The obvious
  implementation uses the browser's zone and looks correct on a laptop sitting
  in the same timezone as the controller.
- The stop control is an e-stop. It must not sit behind a confirmation dialog
  that a wet person with muddy hands has to dismiss.
- Zone numbers are 1-8 and are what the API addresses a valve by.

## Constraints

- **Never run `git push`.** Commits stay local; the user controls all pushes.
- **Never discard uncommitted changes** — no `git checkout --`, `git restore`,
  `git clean -f`, `git reset --hard` — and do not delete untracked files you
  did not create. There is work in flight in this tree.
- **Stay inside `web/`.** Do not touch `IrrigationD/`, the Kanoop submodules, or
  `docs/plans/2026-09-12-irrigationd.md`.
- **No "it's X, not Y" antithesis** anywhere — plan, commit messages, code
  comments. The construction that defines something by negating an alternative.
  Test: delete the negated clause; if the sentence still says everything it
  said, cut it.
- **Comments state traps, not reasoning.** A comment earns its place only when a
  plausible future edit would silently break behaviour and the code cannot show
  why. Design rationale belongs in the commit message. Three daemon tasks have
  each lost a review round to this rule.
- Commit messages are conventional commits, signed with **your own** session's
  attribution trailer.

## The bar that matters

Every task on the daemon side has been sent back at least once, and every single
time it was for a test that could not fail:

- a migration engine no test ever executed
- three fields written and never read back
- two foreign keys whose equal values made a transposed bind invisible
- a `begin()` guard whose test passed with the guarded line deleted

When you write test steps into the plan, make each one answer: **what mutation
of the code would make this fail?** If you cannot name one, it is decoration.

## Report

When the plan is saved, say: the path, the task count, and any decision you made
that the spec did not make for you. Do not start executing it — the user chooses
the execution mode.
