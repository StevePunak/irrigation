# Brief for Irrigation (W1) — execute the Yocto layer plan

You are taking over implementation of the Yocto layer. Another session (on a
different account, so we cannot message each other) is driving the daemon
build. The repo is the only coordination channel.

## Territory — read this first

| Tree | Owner | Notes |
|---|---|---|
| `~/src/punak/rpi` | **you** | The whole Yocto layer. I will not touch it. |
| `~/src/punak/irrigation/web/` | **you** | The web frontend, once its plan exists. |
| `~/src/punak/irrigation/docs/plans/2026-09-13-irrigation-web.md` | **you** | Your plan. |
| `~/src/punak/irrigation/IrrigationD/` | me | Daemon. Do not touch. |
| `~/src/punak/irrigation/docs/plans/2026-09-12-irrigationd.md` | me | Daemon plan. Do not touch. |
| `~/src/punak/irrigation/.superpowers/` | me | My ledger. Read freely, never write. |

Both of us commit to `~/src/punak/irrigation`, so run `git status --porcelain`
before every commit there and stage only your own paths. Never `git add -A`.

## The work

Execute `~/src/punak/irrigation/docs/plans/2026-09-13-irrigation-yocto-layer.md`
— six tasks, from layer skeleton through hardware bring-up. Use
`superpowers:subagent-driven-development`. The plan's Global Constraints block
is binding; read it before Task 1.

Task 3 needs a pushed `SRCREV` for the daemon, which does not exist yet. If you
reach Task 3 before the daemon lands, stop there and do Tasks 4 and 5's recipe
work with the daemon package left out of the image, then come back. Say so in
your ledger rather than inventing a revision.

Task 6 is hardware. Do not flash anything without the user's explicit
confirmation of the target device — present `lsblk` output and wait.

## The one thing that will waste your afternoon if you miss it

**Every bitbake invocation runs inside the container.** Never on the Fedora
host. Fedora 44's `tar` imports `openat2@GLIBC_2.43`, a syscall `pseudo` 1.9.0
does not wrap, so every `do_package` fails with `got *at() syscall for unknown
directory` and `tar: Cannot mkdir: Bad address`. It names whichever recipe
happened to need packaging first, which makes it look like a recipe bug. It is
not.

```bash
podman run --rm --userns=keep-id --pids-limit=-1 \
  -v /home/spunak/src/punak:/home/spunak/src/punak \
  -w /home/spunak/src/punak/rpi \
  localhost/yocto-walnascar:ubuntu-22.04 \
  bash -lc '<command>'
```

The image `localhost/yocto-walnascar:ubuntu-22.04` already exists. **Mount the
tree at its own host path.** Mounting it anywhere else changes `TOPDIR`,
bitbake refuses with `TMPDIR has changed location`, and the recovery is a full
from-scratch rebuild of a tree with 12,600 tasks in it.

Git fetches inside the container need the agent socket in two places — the
container, and bitbake's task environment, which exports only a fixed variable
set:

```bash
-v "$SSH_AUTH_SOCK":/ssh-agent
# and inside:
export SSH_AUTH_SOCK=/ssh-agent
export BB_ENV_PASSTHROUGH_ADDITIONS="$BB_ENV_PASSTHROUGH_ADDITIONS SSH_AUTH_SOCK GIT_SSH_COMMAND"
```

## Constraints

- **Never run `git push`.** The user controls all pushes. A hook blocks it.
- **Never discard uncommitted changes** — no `git checkout --`, `git restore`,
  `git clean -f`, `git reset --hard` — and never delete untracked files you did
  not create. `~/src/punak/rpi` carries the user's in-flight gateway work in
  `gateway-admin/frontend/src/pages/Login.tsx` and
  `meta-rpi4-gateway/recipes-connectivity/gateway-ssl-init/files/gateway-ssl-init.sh`.
  Never stage either.
- **`git clean -fdx` destroys the SDD workspace.** It is git-ignored scratch.
- **Tests come at the end.** Standing policy: no TDD, no interleaved test
  tasks. Unit tests are written once the code stops churning. Build and wiring
  assertions that prove an artifact is real — a package contains the file it
  claims, a cross-compiled ELF is actually aarch64 — are not unit tests and stay
  in the task that makes the claim. The Yocto plan is built on those and needs
  no change.
- **No "it's X, not Y" antithesis** anywhere: commits, comments, reports. The
  construction that defines something by negating an alternative. Test: delete
  the negated clause; if the sentence still says everything it said, cut it.
- **Comments state traps, not reasoning.** A comment earns its place only when a
  plausible future edit would silently break behaviour and the code cannot show
  why. Rationale belongs in the commit message. Four daemon tasks have each lost
  a review round to this rule.
- Conventional commits, signed with **your own** session's attribution trailer,
  never mine.
- Address the user as "boss" in chat.

## Budget

The user is watching usage. Dispatch implementers on the cheapest model that
can do the job — most of these tasks are transcription of recipe text the plan
already contains. Reserve the capable models for reviews of anything that
touches GPIO state or the image's boot path.

## Report

Keep your own ledger under
`~/src/punak/irrigation/.superpowers/sdd/2026-09-13-irrigation-yocto-layer/`.
When you finish a task, the ledger is the record — I read it, and so does the
user after a context compaction.
