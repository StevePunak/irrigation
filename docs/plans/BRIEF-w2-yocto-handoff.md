# Handoff — the irrigation Yocto layer, from the W2 session

You are taking over `meta-rpi4-irrigation` in `~/src/punak/rpi`. The session that
built it hit its account's usage cap and stopped. This document is the handoff;
it is written for someone on a different Claude account, so we cannot message
each other and the repo is the only channel.

Four of the plan's six tasks are complete and reviewed. Two remain, and both are
blocked on something only the user can do.

---

## Read this first: the built artifacts are stale

The user changed the zone map at 14:32 today, after the last image build at
12:21. Every source file in the layer is migrated. **Nothing has been rebuilt.**

```
Zones 1-8 were   5, 6, 13, 16, 19, 20, 21, 26
Zones 1-8 are    5, 6, 12, 13, 16, 19, 20, 21
```

BCM 26 is gone because the GeeekPi proto-HAT does not break it out — its strip
labels 25 signals and 26 is not among them, confirmed on the physical board. The
eight new offsets are the rightmost eight pads in silkscreen order, so zone N
lands on pad N and the ribbon to the relay board runs straight. BCM 12 is in the
same 9-27 pull-down class as the line it replaced, so the boot-safety analysis
carries over unchanged.

Migrated already, by the user, in commits `4107d58` and `5eda616`:

| File | Now reads |
|---|---|
| `kas/rpi4-irrigation.yml` | `gpio=5,6,12,13,16,19,20,21=op,dh` |
| `.../irrigation-init/files/irrigationd.ini` | `zones="1=5,2=6,3=12,4=13,5=16,6=19,7=20,8=21"` |
| `.../images/rpi4-irrigation-image.bb` | both assertion strings |

Still carrying the OLD map, because it predates the change:

```
build/tmp/deploy/images/raspberrypi4-64/bootfiles/config.txt:250
    gpio=5,6,13,16,19,20,21,26=op,dh
```

So: **rebuild before you trust anything in `build/tmp/deploy/`.** The image
recipe now carries an assertion that `bbfatal`s if the expected `gpio=` line is
missing from the generated `config.txt`, so a rebuild either passes cleanly or
tells you loudly. It has never run against the new map. That is the first thing
to verify, and it is unverified today.

This is the fourth time on this branch that the gap between "the source is
right" and "the shipped thing is right" has caught someone. Treat it as the
house failure mode, not an accident.

---

## Territory

| Tree | Owner |
|---|---|
| `~/src/punak/rpi` | **you** — the whole Yocto layer |
| `~/src/punak/irrigation/docs/plans/2026-09-13-irrigation-yocto-layer.md` | **you** — your plan |
| `~/src/punak/irrigation/.superpowers/sdd/2026-09-13-irrigation-yocto-layer/` | **you** — the ledger |
| `~/src/punak/irrigation/IrrigationD/`, `KanoopCommonQt`, `KanoopDatabaseQt`, `KanoopPiQt` | the daemon session (W3) |
| `~/src/punak/irrigation/docs/plans/2026-09-12-irrigationd.md` | W3 |
| `~/src/punak/irrigation/.superpowers/sdd/2026-09-12-irrigationd/` | W3's ledger — read freely, never write |
| `~/src/punak/irrigation/web/` | the web session (W1) |
| `~/src/punak/irrigation/docs/plans/2026-09-13-irrigation-web.md` | W1 |

Three sessions commit to `~/src/punak/irrigation`. Run `git status --porcelain`
before every commit there and stage only your own paths. Never `git add -A`.

`~/src/punak/rpi` also carries the user's unrelated in-flight work in
`gateway-admin/frontend/src/pages/Login.tsx` and
`meta-rpi4-gateway/recipes-connectivity/gateway-ssl-init/files/gateway-ssl-init.sh`.
Never stage either, never revert either. They have been sitting modified and
unstaged all day and that is correct.

---

## What exists

20 commits on `master` in `~/src/punak/rpi`, `4c084eb..5eda616`. Nothing pushed.

| Recipe | What it does |
|---|---|
| `irrigation-init` | systemd unit, `/etc/irrigationd.ini`, `/var/lib/irrigationd`, hostname |
| `nginx-irrigation-config` | site serving the bundle, reverse-proxying `/api` → the daemon's `/admin` |
| `irrigation-web` | React bundle installed from `~/src/punak/irrigation/web/dist` |
| `systemd-networkd-irrigation-config` | DHCP wired + wireless, resolved stub, wait-online `--any` drop-in |
| `rpi4-irrigation-image` | the appliance image |

Plus `kas/rpi4-irrigation.yml` and an `irrigation` target in both build scripts.

Tasks 1, 2, 4 and 5 are complete and reviewed clean. Task 5 took three fix
rounds plus two waves after the final whole-branch review.

---

## The two remaining tasks

### Task 3 — the `irrigationd` recipe. BLOCKED on two pushes.

The recipe does not exist. `rpi4-irrigation-image.bb` names `irrigationd` in
`IMAGE_INSTALL` anyway, so **a clean `kas build` at HEAD fails at parse with
`Nothing RPROVIDES 'irrigationd'`**. That is a known, deliberate state, not a
regression. Every image build so far has used a throwaway kas fragment:

```yaml
header:
  version: 14
local_conf_header:
  no-daemon: |
    IMAGE_INSTALL:remove = "irrigationd irrigation-web"
```

kas 5.2 refuses to concatenate config files across two different git repos, so
that fragment has to live inside the `rpi` tree for the build and be deleted
afterwards. Never commit it.

**The blocker is two pushes, and the user controls both.** Task 3 fetches with
`gitsm://`, which follows the superproject's recorded submodule SHAs:

1. the `irrigation` superproject branch carrying the `SRCREV` you pin, and
2. `KanoopCommonQt`'s `feature/appsettings-path-ctor` (commit `71c0a51` when
   last reported), pushed to the KanoopCommonQt remote.

If only the superproject lands, `do_fetch` dies naming the submodule rather than
the missing push, which is a genuinely confusing error to land on. W3 has both
recorded as a release blocker and will request them together.

W3's guidance on pinning, which is worth following exactly: **pin the
superproject sha W3 gives you, never a revision derived from a branch name.**
`71c0a51` sits on a branch deliberately kept off KanoopCommonQt's master, which
carries merged EPC pull requests from unrelated work. If the user later merges
that branch, the superproject pointer should be re-bumped and Task 3 re-pinned to
the new superproject sha — a branch commit can stop being reachable, a merged one
does not.

The plan's Task 3 text is otherwise sound. Two things to fold in that the plan
does not carry:

- `RDEPENDS:${PN} += "qtbase-plugins"` as a hard dependency. The QSQLITE driver
  reaches the image today only through `RRECOMMENDS:qtbase = "qtbase-plugins"`
  in `meta-qt6/recipes-qt/qt6/qt6.inc:46-49`. Set `NO_RECOMMENDATIONS = "1"` as a
  routine appliance slim-down and the SQL driver silently disappears; the daemon
  then throws at `KanoopDatabaseQt/src/database/datasource.cpp:28-29`. The plan
  already mentions this — make it an `RDEPENDS`, not a hope.
- After the recipe lands, rebuild WITHOUT the no-daemon fragment and run the two
  landmine assertions that have never been runnable: `qtbase-plugins` in the
  image manifest, and `libqsqlite.so` physically present under
  `.../rootfs/usr/lib/plugins/sqldrivers/`. A manifest entry and a file on disk
  are different claims.

### Task 6 — hardware bring-up. BLOCKED on the user.

Needs a physical Pi and the user's explicit confirmation of the flash target.
**Present `lsblk` output and wait.** `dd` to the wrong device destroys it with no
prompt and no undo.

Three things to carry to the bench that the plan does not say:

1. **External pull-up resistors on all eight relay inputs.** This is the one
   thing software cannot fix. `bcm2835_pmx_free()` reverts a released line to
   `GPIO_IN` and the pad's internal pull takes over. Six of the eight offsets are
   in BCM 9-27, which pull DOWN, which is asserted on a LOW-trigger board. So
   `kill -9` on the daemon, or any exit, opens six valves. The daemon's own
   library says so at `KanoopPiQt/include/Kanoop/pi/outputbank.h:15-19`. The
   `gpio=` line in `config.txt` covers firmware up to the first line request and
   nothing after.
2. **Verify the pull-down prediction with a meter before trusting it.** "Six of
   eight come up asserted" is a prediction about SoC pad defaults. Whether a weak
   ~50 kΩ pull-down sinks enough current through an opto-isolated relay input to
   fire the LED is a bench question. The `dh` fix is correct either way.
3. **`grep` the first boot's journal for `Unknown key`.** It is the fastest
   possible confirmation that the unit file parsed as intended.

The plan's Task 6 Step 5 verification is wrong: it curls
`http://<pi>/api/admin/status`, which is a third, incompatible reading of the API
contract. Use `http://<pi>/api/status`, which nginx rewrites to `/admin/status`.
W3 owns that document and has logged the correction.

---

## Rulings the W2 session made on the user's behalf

Every one is in the ledger with what it costs if wrong. Do not silently
re-litigate them; if you disagree, say so to the user.

1. **Build scripts' usage strings.** The plan's edit would have deleted the
   `qemu-gateway` target `build-image.sh` already has. Kept it.
2. **`IMAGE=` in `build-sdk.sh` only.** `build-image.sh` has no such variable.
3. **The GPIO pull explanation.** The plan's landmine block blamed BCM 0-8
   pull-ups; the correct mechanism is BCM 9-27 pull-downs. The user has since
   corrected this in the plan and the design doc himself.
4. **Task 3 deferred, no invented `SRCREV`.**
5. **`irrigation-web`'s `bbfatal` firing IS the test** while no bundle exists.
   The guard was never weakened and no stub `dist/` was ever created.
6. **The image built behind a throwaway kas fragment** rather than by gutting the
   committed recipe.
7. **The stock nginx `default_server` site is removed** by
   `ROOTFS_POSTPROCESS_COMMAND`. `nginx.inc:123-126` symlinks it into
   `sites-enabled/` unconditionally and it claims the same two `listen 80
   default_server` lines ours does; two enabled means nginx exits at startup.
   Ours keeps `default_server` because the browser arrives at a bare IP with no
   `Host` header.
8. **Busybox stays.** The plan's `util-linux-base` does not exist anywhere.
   poky's real replacement, `packagegroup-core-base-utils`, RDEPENDs on `dhcpcd`
   — a second DHCP client on an image using systemd-networkd — plus
   `inetutils-telnet` and `inetutils-tftp`. The GNU userland the plan wanted is
   already explicitly installed and already wins via update-alternatives. Only
   the duplicate syslog was a real defect, and
   `VIRTUAL-RUNTIME_base-utils-syslog = ""` in the kas header fixed it.
9. **`--any` on wait-online.** An earlier ruling left `10-wired.network` at
   `RequiredForOnline=yes`; that fixed half a symmetric bug, because an unplugged
   `eth0` then blocks `network-online.target` for 120 s in exactly the wireless
   deployment this layer ships `wpa-supplicant` for. The drop-in supersedes it.
10. **The design doc wins the `/api` → `/admin` contract.** Three documents
    disagreed. `proxy_pass http://127.0.0.1:8080/admin/;` — the trailing slash is
    what performs the rewrite. W3 has recorded `/admin/*` as binding for its
    route table.
11. **Two commit messages amended in place** (Task 1's, and the networking fix's,
    which carried two factually false claims).
12. **A second fix wave was run after the final review**, against the skill's
    one-wave rule. Both were verified one-line Criticals blocking Task 6.

### The user's own decision, 2026-09-13

**`IMAGE_FEATURES` carries `empty-root-password allow-empty-password
allow-root-login`.** `debug-tweaks` no longer exists in this poky release;
`poky/meta/classes-recipe/core-image.bbclass:41` names those three as what it
used to expand to. The user chose this knowingly as a bring-up posture.

**IT MUST NOT SHIP.** The controller sits on a home LAN with nginx on port 80.
With these features anyone who can reach it owns it. There is a trap comment
above the line. Before deployment, replace it with a real credential decision —
the options put to the user were the gateway's `extrausers` block with a password
hash he supplies, or an SSH key with the root password left locked.

---

## Parked findings, for whoever picks these up

None block the remaining tasks. All are in the ledger with reasoning.

- **Five commit messages carry the banned "X, not Y" antithesis**: `b7f9b61`,
  `88fe1a9`, `085ba1d`, `d309efb`, `12270bc`. All local and unpushed, none at
  HEAD, so fixing them means a scripted rewrite of 13 commits in a tree the user
  works in. Left for the user to call. Note the pattern: `12270bc`'s *comment*
  handles the same contrast correctly by giving it its own clause; the *subject
  line* compressed it back into the banned form. Compression is where this
  construction keeps reappearing — watch for it when you write a subject.
- `location /api/` does not match a bare `/api`, which the frontend's
  `daemonPath()` permits. Dev proxy and nginx disagree on exactly one input.
  Nothing emits it today.
- nginx with a URI in `proxy_pass` renormalises `%2F` in a path segment. Bites
  the first time a name rather than a number lands in a route.
- `nginx-irrigation-config` hardcodes `root /var/www/irrigation/html` while
  `irrigation-web` installs to `${localstatedir}/www/irrigation/html`. They agree
  only because `localstatedir` is `/var`.
- Presets install to `${libdir}` rather than `${nonarch_libdir}`. Correct on
  aarch64; a repo-wide wart shared with `meta-rpi4-gateway`.
- A `dnf reinstall nginx` on the target restores the stock `default_server`
  symlink and nginx stops starting. A `nginx_%.bbappend` with a one-line
  `do_install:append` would kill it permanently; the `ROOTFS_POSTPROCESS_COMMAND`
  only cleans the rootfs.
- `dtoverlay=vc4-kms-v3d` is still emitted on a headless image with `gpu_mem=16`.
- The `irrigation-web` guard for "dist exists but holds no `index.html`" has
  never been exercised, because the bundle has never existed. Verify it when W1's
  bundle lands.

---

## Landmines that will cost you an afternoon

**Every bitbake invocation runs inside the container. Never on the Fedora host.**
Fedora 44's `tar` imports `openat2@GLIBC_2.43`, which `pseudo` 1.9.0 does not
wrap, so every `do_package` fails with `got *at() syscall for unknown directory`
and `tar: Cannot mkdir: Bad address`. It names whichever recipe happened to
package first, which makes it look like a recipe bug. It is not.

```bash
podman run --rm --userns=keep-id --pids-limit=-1 \
  -v /home/spunak/src/punak:/home/spunak/src/punak \
  -w /home/spunak/src/punak/rpi \
  localhost/yocto-walnascar:ubuntu-22.04 \
  bash -lc '<command>'
```

**Mount the tree at its own host path.** Anywhere else changes `TOPDIR`, bitbake
refuses with `TMPDIR has changed location`, and the recovery is a from-scratch
rebuild of a tree with 12,600 tasks in it.

Git fetches inside the container need the agent socket in two places — the
container, and bitbake's task environment, which exports only a fixed variable
set:

```bash
-v "$SSH_AUTH_SOCK":/ssh-agent
# and inside:
export SSH_AUTH_SOCK=/ssh-agent
export GIT_SSH_COMMAND="ssh -o StrictHostKeyChecking=accept-new"
export BB_ENV_PASSTHROUGH_ADDITIONS="$BB_ENV_PASSTHROUGH_ADDITIONS SSH_AUTH_SOCK GIT_SSH_COMMAND"
```

**A full image build exceeds the Bash tool's 10-minute cap.** Run it backgrounded
to a log file and poll the log.

**Extract a rootfs tarball into a freshly created directory every time**, or read
straight out of it with `tar -t`. A reviewer on this branch nearly filed a false
verdict because tar merged a new rootfs over a stale extraction and two-hour-old
service files sat there looking current.

**`git clean -fdx` destroys the SDD workspace.** It is git-ignored scratch.

**Assert a config fixture with the parser that will actually read it.** This
layer shipped a broken `/etc/irrigationd.ini` for several hours because its test
used Python's `configparser`, which accepts an unquoted comma-bearing value that
`QSettings` types as a `QStringList` and reads back as empty. Measured:

```
zones=1=5,2=6,...     -> QStringList  toString()=[]     0 pairs
zones="1=5,2=6,..."   -> QString      toString()=[...]  8 pairs
```

Keep the quotes. The daemon now tolerates both spellings, but the quoted form is
unambiguous under either reader.

---

## Constraints

- **Never run `git push`.** The user controls all pushes; a hook blocks it.
- **Never discard uncommitted changes** — no `git checkout --`, `git restore`,
  `git clean -f`, `git reset --hard` — and never delete untracked files you did
  not create.
- **Tests come at the end.** No TDD, no interleaved test tasks. Build and wiring
  assertions that prove an artifact is real are not unit tests and stay in the
  task that makes the claim.
- **Comments state traps, not reasoning.** A comment earns its place only when a
  plausible future edit would silently break behaviour and the code cannot show
  why. Rationale belongs in the commit message.
- **No "it's X, not Y" antithesis** anywhere. Delete the negated clause; if the
  sentence still says everything it said, cut the construction.
- Conventional commits, signed with **your own** session's attribution trailer.
- Address the user as "boss" in chat.

---

## Where the detail lives

`~/src/punak/irrigation/.superpowers/sdd/2026-09-13-irrigation-yocto-layer/`

- `progress.md` — the full ledger, 780 lines. Every ruling, every parked finding,
  every measured fact, in the order they happened. Read it before Task 3.
- `task-N-brief.md` / `task-N-report.md` — per-task requirements and reports,
  including the commands actually run and their real output.
- `final-fixes-report.md` — the two post-review fix waves.
- `review-*.diff`, `rereview-*.diff` — the review packages.

**Do not delete this workspace.** The subagent-driven-development skill says to
delete it when the final review is clean, but this plan is not complete — Tasks 3
and 6 have never run, and the ledger is the only record of the push dependency,
the route contract and the rulings above. Delete it after Task 6's bring-up notes
are written.
