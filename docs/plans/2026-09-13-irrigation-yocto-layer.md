# Irrigation Yocto Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `meta-rpi4-irrigation`, the Yocto layer that cross-builds `irrigationd`, serves the web bundle, holds the valves shut from the bootloader onward, and produces a flashable `rpi4-irrigation-image`.

**Architecture:** A single layer sibling to `meta-rpi4-gateway` in `~/src/punak/rpi`, driven by its own kas configuration. Four recipes and an image: the daemon cross-built from a pinned `gitsm://` revision, the React bundle installed as static files, an nginx site that serves those files and reverse-proxies `/api`, and an init recipe owning the systemd unit, the runtime directories and the default configuration. Boot-time GPIO safety lives in `config.txt` through `RPI_EXTRA_CONFIG`, because a package cannot write to the FAT boot partition.

**Tech Stack:** Yocto walnascar (poky 5.2.4), kas 5.2, meta-qt6 6.10, meta-raspberrypi, meta-openembedded (meta-oe, meta-networking, meta-python, meta-webserver), systemd, nginx, SQLite.

**Spec:** `docs/design/2026-09-05-irrigation-design.md` — section 9, with section 2.4 for the GPIO assignments and section 5 for the daemon's configuration keys. The plan argues from the spec; executors read both.

## Global Constraints

These apply to every task. A task's requirements implicitly include this section.

- **Every bitbake invocation runs inside the container. Never run bitbake on the Fedora host.** Fedora 44's `tar` imports `openat2@GLIBC_2.43`, a syscall `pseudo` 1.9.0 does not wrap, so every `do_package` fails with `got *at() syscall for unknown directory` and `tar: Cannot mkdir: Bad address`. The runner is:

  ```bash
  podman run --rm --userns=keep-id --pids-limit=-1 \
    -v /home/spunak/src/punak:/home/spunak/src/punak \
    -w /home/spunak/src/punak/rpi \
    localhost/yocto-walnascar:ubuntu-22.04 \
    bash -lc '<command>'
  ```

  **The tree must be mounted at its own host path.** Mounting it anywhere else changes `TOPDIR`, and bitbake refuses the build with `TMPDIR has changed location`, which costs a full from-scratch rebuild.

- **Never run `git push`.** Commits stay local. The user controls all remote pushes.
- **Never discard uncommitted changes.** `~/src/punak/rpi` carries unrelated in-flight work in `gateway-admin/frontend/src/pages/Login.tsx` and `meta-rpi4-gateway/recipes-connectivity/gateway-ssl-init/files/gateway-ssl-init.sh`. Never stage them, never revert them, never `git add -A`.
- **`PREFERRED_VERSION_*` binds only from a configuration file.** Setting it in a recipe is inert and silently does nothing. It belongs in the kas `local_conf_header`.
- `LAYERSERIES_COMPAT_meta-rpi4-irrigation = "walnascar"`.
- Every recipe declares `LICENSE` and `LIC_FILES_CHKSUM`. Local recipes use:
  ```
  LICENSE = "MIT"
  LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
  ```
- **`SRC_URI` `file://` entries unpack to `${UNPACKDIR}`, not `${WORKDIR}`.** walnascar moved them. A recipe that installs from `${WORKDIR}/foo.service` fails with a file-not-found during `do_install`.
- systemd units are enabled with a preset file under `${libdir}/systemd/system-preset`, following the pattern every recipe in `meta-rpi4-gateway` already uses.
- Layer collection names for `LAYERDEPENDS`, verified in each layer's `conf/layer.conf`: `core`, `raspberrypi`, `qt6-layer`, `openembedded-layer`, `networking-layer`, `webserver`, `meta-python`.
- Commit messages are conventional commits and end with:
  ```
  Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
  ```

## Three landmines the spec already names

Each of these produces a clean build and fails later, so they are called out before the tasks that plant them.

1. **`PACKAGECONFIG:append:pn-qtbase = " sql-sqlite"`.** meta-qt6 enables the SQLite driver only through `PACKAGECONFIG_KDE`, gated on the `kde` distro feature. A headless image does not set it, so qtbase builds with no SQL drivers at all and `QSqlDatabase::addDatabase("QSQLITE")` fails at runtime after a completely clean build with no warning.

2. **`SRC_URI` uses `gitsm://`, never `git://`.** The Kanoop libraries are consumed as submodules through `add_subdirectory`. A plain git fetch produces a source tree that configures successfully and then fails to link.

3. **The `gpio=` line in `config.txt` holds the relay inputs de-asserted from the bootloader onward.** Removing it opens every valve at power-up. Measured on the target: six of the eight zone lines come up asserted, because BCM 9-27 default to pull-down.

---

## File Structure

Everything below `~/src/punak/rpi/` unless stated otherwise.

| Path | Responsibility |
|---|---|
| `meta-rpi4-irrigation/conf/layer.conf` | Layer registration, priority, dependencies, walnascar compatibility. |
| `kas/rpi4-irrigation.yml` | Layer manifest and every `local.conf` override, including the two landmine settings and `RPI_EXTRA_CONFIG`. |
| `scripts/build-image.sh`, `scripts/build-sdk.sh` | Gain an `irrigation` target case alongside `rpi4`, `rpi5` and `gateway`. |
| `meta-rpi4-irrigation/recipes-core/irrigation-init/` | systemd unit, default `/etc/irrigationd.ini`, `/var/lib/irrigationd`, the `irrigation` service user. |
| `meta-rpi4-irrigation/recipes-daemon/irrigationd/irrigationd_1.0.bb` | Cross-builds the daemon from a pinned `gitsm://` revision. |
| `meta-rpi4-irrigation/recipes-httpd/irrigation-web/irrigation-web_1.0.bb` | Installs `web/dist` to `/var/www/irrigation/html`, fails the build when the bundle is absent. |
| `meta-rpi4-irrigation/recipes-httpd/nginx-irrigation-config/` | nginx site: static root plus the `/api` reverse proxy. |
| `meta-rpi4-irrigation/recipes-images/images/rpi4-irrigation-image.bb` | `core-image-base` plus the above, avahi, sqlite3, tzdata, wpa-supplicant, SSH. |

---

### Task 1: Layer skeleton, kas configuration and build scripts

Produces a layer bitbake can see and a kas file that resolves. Every later task adds recipes into this skeleton, so it lands first.

**Files:**
- Create: `~/src/punak/rpi/meta-rpi4-irrigation/conf/layer.conf`
- Create: `~/src/punak/rpi/kas/rpi4-irrigation.yml`
- Modify: `~/src/punak/rpi/scripts/build-image.sh`
- Modify: `~/src/punak/rpi/scripts/build-sdk.sh`

**Interfaces:**
- Produces: the layer name `meta-rpi4-irrigation`, the kas file `kas/rpi4-irrigation.yml`, the image target name `rpi4-irrigation-image`, and the build-script target keyword `irrigation`. Tasks 2 through 6 all reference these exact strings.

- [ ] **Step 1: Create the layer configuration**

`meta-rpi4-irrigation/conf/layer.conf`:

```
# We have a conf and classes directory, add to BBPATH
BBPATH .= ":${LAYERDIR}"

# We have recipes-* directories, add to BBFILES
BBFILES += "${LAYERDIR}/recipes-*/*/*.bb \
            ${LAYERDIR}/recipes-*/*/*.bbappend"

BBFILE_COLLECTIONS += "meta-rpi4-irrigation"
BBFILE_PATTERN_meta-rpi4-irrigation = "^${LAYERDIR}/"
BBFILE_PRIORITY_meta-rpi4-irrigation = "10"

LAYERDEPENDS_meta-rpi4-irrigation = "core raspberrypi qt6-layer openembedded-layer networking-layer webserver"
LAYERSERIES_COMPAT_meta-rpi4-irrigation = "walnascar"
```

- [ ] **Step 2: Create the kas configuration**

`kas/rpi4-irrigation.yml`:

```yaml
header:
  version: 14

machine: raspberrypi4-64
distro: poky
target: rpi4-irrigation-image

repos:
  poky:
    url: https://git.yoctoproject.org/poky
    branch: walnascar
    layers:
      meta:
      meta-poky:
      meta-yocto-bsp:

  meta-openembedded:
    url: https://github.com/openembedded/meta-openembedded
    branch: walnascar
    layers:
      meta-oe:
      meta-networking:
      meta-python:
      meta-webserver:

  meta-raspberrypi:
    url: https://github.com/agherzan/meta-raspberrypi
    branch: walnascar
    layers:
      .:

  meta-qt6:
    url: https://code.qt.io/yocto/meta-qt6.git
    branch: "6.10"
    layers:
      .:

  meta-rpi4-irrigation:
    path: meta-rpi4-irrigation
    layers:
      .:

local_conf_header:
  meta-rpi4-irrigation: |
    # Init system
    INIT_MANAGER = "systemd"

    # Headless controller — no display, no audio, no compositor
    DISTRO_FEATURES:remove = "x11 directfb vulkan wayland opengl"
    DISTRO_FEATURES:append = " pam"

    GPU_MEM = "16"
    ENABLE_UART = "1"

    # Accept restricted licenses required for RPi WiFi/BT firmware
    LICENSE_FLAGS_ACCEPTED:append = " synaptics-killswitch"

    # meta-qt6 enables the SQLite driver only through PACKAGECONFIG_KDE,
    # which is gated on the kde distro feature. A headless image never sets
    # it, so without this line qtbase builds with no SQL drivers and
    # QSqlDatabase::addDatabase("QSQLITE") fails at runtime on a clean build.
    PACKAGECONFIG:append:pn-qtbase = " sql-sqlite"

    # meta-oe carries libgpiod 1.6.5 and 2.2.2 side by side and their APIs
    # share no call signatures.
    PREFERRED_VERSION_libgpiod = "2.2.2"

    # The gpio= line drives the relay inputs high — the inactive level for a
    # LOW-trigger board — from the bootloader onward. Six of the eight zone
    # lines come up asserted without it, because BCM 0-8 default to pull-up.
    RPI_EXTRA_CONFIG = 'dtparam=i2c_arm=on\ndtoverlay=i2c-rtc,ds3231\ngpio=5,6,12,13,16,19,20,21=op,dh'

    # Shared caches
    DL_DIR ?= "${TOPDIR}/../downloads"
    SSTATE_DIR ?= "${TOPDIR}/../sstate-cache"

    # Auto-clean work directories after each package builds
    INHERIT += "rm_work"

    # Parallelism — 48 cores
    BB_NUMBER_THREADS = "48"
    PARALLEL_MAKE = "-j 48"
```

- [ ] **Step 3: Verify the layer is registered**

Run, from `~/src/punak/rpi`:

```bash
podman run --rm --userns=keep-id --pids-limit=-1 \
  -v /home/spunak/src/punak:/home/spunak/src/punak \
  -w /home/spunak/src/punak/rpi \
  localhost/yocto-walnascar:ubuntu-22.04 \
  bash -lc 'kas shell kas/rpi4-irrigation.yml -c "bitbake-layers show-layers"'
```

Expected: a table listing `meta-rpi4-irrigation` with priority 10, and no `Layer ... depends on layer ... which is not enabled` error. If bitbake reports a missing dependency, the named collection is absent from the `repos:` block in Step 2 — add the layer there rather than deleting the dependency.

- [ ] **Step 4: Add the irrigation target to both build scripts**

In `scripts/build-image.sh` and `scripts/build-sdk.sh`, the target is selected by a `case` on `$1`. Add a fourth branch to each, immediately after the `gateway)` branch:

```bash
  irrigation)
    KAS_FILE="${REPO_ROOT}/kas/rpi4-irrigation.yml"
    PRIVATE_KAS="${REPO_ROOT}/../meta-rpi4-irrigation-private/kas/private.yml"
    IMAGE="rpi4-irrigation-image"
    ;;
```

and extend each usage line to read:

```bash
  *) echo "Usage: $0 [rpi4|rpi5|gateway|irrigation]" >&2; exit 1 ;;
```

- [ ] **Step 5: Verify the scripts accept the new target**

```bash
cd ~/src/punak/rpi && bash -n scripts/build-image.sh && bash -n scripts/build-sdk.sh && echo "syntax ok"
./scripts/build-image.sh bogus 2>&1 | head -1
```

Expected: `syntax ok`, then `Usage: ./scripts/build-image.sh [rpi4|rpi5|gateway|irrigation]`.

- [ ] **Step 6: Commit**

```bash
cd ~/src/punak/rpi
git add meta-rpi4-irrigation/conf/layer.conf kas/rpi4-irrigation.yml scripts/build-image.sh scripts/build-sdk.sh
git commit
```

Message:

```
feat: add the meta-rpi4-irrigation layer skeleton

Registers the layer, its kas manifest and an irrigation target in both
build scripts. The manifest carries the two settings that fail late:
qtbase builds with no SQL drivers unless sql-sqlite is added to its
PACKAGECONFIG, because meta-qt6 gates that on the kde distro feature a
headless image never sets; and libgpiod has two incompatible versions in
meta-oe with neither marked preferred.

RPI_EXTRA_CONFIG carries the config.txt fragment. A package cannot write
config.txt — it is generated by rpi-config's do_deploy onto the FAT boot
partition — so the GPIO safety line has to travel as a configuration
variable rather than a file an init recipe installs.
```

Do not stage `gateway-admin/frontend/src/pages/Login.tsx` or `meta-rpi4-gateway/recipes-connectivity/gateway-ssl-init/files/gateway-ssl-init.sh`; both carry unrelated in-flight work.

---

### Task 2: irrigation-init — systemd unit, default configuration and state directory

Produces everything the daemon needs to exist on the target before the daemon itself is packaged: its unit, its configuration file, and the directory its database lives in.

**Files:**
- Create: `meta-rpi4-irrigation/recipes-core/irrigation-init/irrigation-init_1.0.bb`
- Create: `meta-rpi4-irrigation/recipes-core/irrigation-init/files/irrigationd.service`
- Create: `meta-rpi4-irrigation/recipes-core/irrigation-init/files/irrigationd.ini`

**Interfaces:**
- Consumes: the layer skeleton from Task 1.
- Produces: the package name `irrigation-init`, the unit name `irrigationd.service`, the configuration path `/etc/irrigationd.ini`, and the state directory `/var/lib/irrigationd`. Task 3's daemon reads that configuration path, Task 4's nginx proxies to the port it declares, and Task 5's image installs this package.

The daemon's configuration keys are fixed by `IrrigationD/src/irrigationsettings.cpp` in the `irrigation` repository. They are, with their defaults: `gpio/zones`, `gpio/chipLabel` (`pinctrl-bcm2711`), `gpio/zoneActiveLow` (`true`), `gpio/stopButtonOffset` (`25`), `limits/maxZoneSeconds` (`3600`), `server/bindAddress` (`127.0.0.1`), `server/listenPort` (`8080`), `database/path` (`/var/lib/irrigationd/irrigation.db`). The file below writes every one of them explicitly so the shipped configuration is readable on the target rather than implied by compiled-in defaults.

- [ ] **Step 1: Write the default configuration**

`meta-rpi4-irrigation/recipes-core/irrigation-init/files/irrigationd.ini`:

```ini
[gpio]
; zone=BCM offset. Zone numbers are what the API and the schedule address a
; valve by; the offsets are the wiring of this particular box.
zones=1=5,2=6,3=12,4=13,5=16,6=19,7=20,8=21
chipLabel=pinctrl-bcm2711
; The relay board is LOW-trigger. The kernel performs the inversion once,
; on the line request; the daemon speaks logical values everywhere else.
zoneActiveLow=true
stopButtonOffset=25

[limits]
maxZoneSeconds=3600

[server]
; Bound to loopback. nginx is the only thing that reaches it.
bindAddress=127.0.0.1
listenPort=8080

[database]
path=/var/lib/irrigationd/irrigation.db
```

- [ ] **Step 2: Write the systemd unit**

`meta-rpi4-irrigation/recipes-core/irrigation-init/files/irrigationd.service`:

```ini
[Unit]
Description=Irrigation controller daemon
Documentation=https://github.com/StevePunak/irrigation
After=network-online.target time-sync.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/bin/irrigationd --config /etc/irrigationd.ini
Restart=on-failure
RestartSec=5
WorkingDirectory=/var/lib/irrigationd

[Install]
WantedBy=multi-user.target
```

`--config` is spelled long. The daemon's parser also accepts `-c`, but `--verbose` has no short form: `v` collides with Qt's `addVersionOption()`, and `QCommandLineParser::addOption()` returns false silently on the clash rather than reporting it.

- [ ] **Step 3: Write the recipe**

`meta-rpi4-irrigation/recipes-core/irrigation-init/irrigation-init_1.0.bb`:

```
DESCRIPTION = "Irrigation controller service unit, default configuration and state directory"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://irrigationd.service \
    file://irrigationd.ini \
"

do_install() {
    install -d ${D}${sysconfdir}
    install -m 0644 ${UNPACKDIR}/irrigationd.ini ${D}${sysconfdir}/irrigationd.ini

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${UNPACKDIR}/irrigationd.service ${D}${systemd_system_unitdir}/irrigationd.service

    install -d -m 0755 ${D}${localstatedir}/lib/irrigationd

    install -d ${D}${libdir}/systemd/system-preset
    printf 'enable irrigationd.service\n' \
        > ${D}${libdir}/systemd/system-preset/90-irrigationd.preset
}

FILES:${PN} = " \
    ${sysconfdir}/irrigationd.ini \
    ${systemd_system_unitdir}/irrigationd.service \
    ${localstatedir}/lib/irrigationd \
    ${libdir}/systemd/system-preset/90-irrigationd.preset \
"

CONFFILES:${PN} = "${sysconfdir}/irrigationd.ini"
```

`CONFFILES` marks the INI so a package upgrade leaves an edited configuration in place instead of overwriting it.

- [ ] **Step 4: Build the recipe**

```bash
podman run --rm --userns=keep-id --pids-limit=-1 \
  -v /home/spunak/src/punak:/home/spunak/src/punak \
  -w /home/spunak/src/punak/rpi \
  localhost/yocto-walnascar:ubuntu-22.04 \
  bash -lc 'kas shell kas/rpi4-irrigation.yml -c "bitbake irrigation-init"'
```

Expected: `Tasks Summary: ... all succeeded`, with no `do_package` failure. A failure reading `got *at() syscall for unknown directory` means the build ran on the host rather than in the container.

- [ ] **Step 5: Assert the package contains exactly the four paths**

```bash
podman run --rm --userns=keep-id --pids-limit=-1 \
  -v /home/spunak/src/punak:/home/spunak/src/punak \
  -w /home/spunak/src/punak/rpi \
  localhost/yocto-walnascar:ubuntu-22.04 \
  bash -lc 'kas shell kas/rpi4-irrigation.yml -c "oe-pkgdata-util list-pkg-files irrigation-init"'
```

Expected, all four present:

```
/etc/irrigationd.ini
/lib/systemd/system/irrigationd.service
/usr/lib/systemd/system-preset/90-irrigationd.preset
/var/lib/irrigationd
```

An empty listing means `do_install` wrote nothing — the usual cause is reading from `${WORKDIR}` instead of `${UNPACKDIR}`, which walnascar changed.

- [ ] **Step 6: Assert the shipped INI parses as the daemon will read it**

The daemon reads this file with `QSettings(path, QSettings::IniFormat)`. Confirm the section and key names round-trip, and that the zone map has eight entries:

```bash
python3 - <<'EOF'
import configparser
c = configparser.ConfigParser()
c.read('/home/spunak/src/punak/rpi/meta-rpi4-irrigation/recipes-core/irrigation-init/files/irrigationd.ini')
zones = dict(p.split('=') for p in c['gpio']['zones'].split(','))
assert sorted(zones) == [str(n) for n in range(1, 9)], zones
assert sorted(int(v) for v in zones.values()) == [5, 6, 12, 13, 16, 19, 20, 21], zones
assert len(set(zones.values())) == 8, "two zones share a GPIO offset"
assert c['server']['listenPort'] == '8080'
assert c['database']['path'] == '/var/lib/irrigationd/irrigation.db'
print("ini ok:", zones)
EOF
```

Expected: `ini ok:` followed by the eight pairs. The duplicate-offset assertion is the one that matters — two zones sharing an offset would open two valves from one command, and nothing downstream checks for it.

- [ ] **Step 7: Commit**

```bash
cd ~/src/punak/rpi
git add meta-rpi4-irrigation/recipes-core/irrigation-init
git commit
```

Message:

```
feat: add irrigation-init with the unit, default config and state directory

Ships /etc/irrigationd.ini with every key the daemon reads written out
explicitly, rather than leaving the shipped configuration implied by
compiled-in defaults. The service binds to loopback; nginx is the only
thing that reaches it.

The daemon runs as root. It needs the GPIO character device and its own
state directory, and this is a single-purpose appliance image with no
other local users. Dropping to a service account in the gpio group is
worth doing when the image grows a second service.
```

---

### Task 3: irrigationd — the cross-built daemon

Produces the daemon binary as a package. This is the task the whole layer exists for.

**Files:**
- Create: `meta-rpi4-irrigation/recipes-daemon/irrigationd/irrigationd_1.0.bb`

**Interfaces:**
- Consumes: the layer skeleton from Task 1 and the configuration path `/etc/irrigationd.ini` from Task 2.
- Produces: the package name `irrigationd` installing `/usr/bin/irrigationd`. Task 5's image installs it; Task 2's unit already invokes it at that path.

**Two fetcher facts walnascar splits, and the split is easy to get backwards:** a `git://` or `gitsm://` fetch unpacks to `${WORKDIR}/git`, so `S = "${WORKDIR}/git"`; a `file://` entry unpacks to `${UNPACKDIR}`, which defaults to `${WORKDIR}/sources-unpack`. This recipe has no `file://` entries and therefore uses `${WORKDIR}/git` throughout.

**Why `gitsm://`:** the superproject's top-level `CMakeLists.txt` pulls `KanoopCommonQt`, `KanoopDatabaseQt` and `KanoopPiQt` in through `add_subdirectory`. They are git submodules. A `git://` fetch produces a tree where those directories are empty, CMake's `add_subdirectory` fails at configure time, and the error names a missing `CMakeLists.txt` rather than a missing submodule.

- [ ] **Step 1: Determine the revision to pin**

The daemon lives in a separate repository. Get the commit to pin:

```bash
cd ~/src/punak/irrigation && git rev-parse HEAD
```

Pin the head of the branch that passed the daemon plan's final whole-branch review. Do not pin a branch name without a `SRCREV`: bitbake would then rebuild from a moving target and the image would stop being reproducible.

The commit must be pushed before the recipe can fetch it. Verify with `git ls-remote origin <sha>` — if that prints nothing, the revision exists only locally and `do_fetch` will fail.

- [ ] **Step 2: Write the recipe**

`meta-rpi4-irrigation/recipes-daemon/irrigationd/irrigationd_1.0.bb`:

```
DESCRIPTION = "Irrigation controller daemon — owns the eight valve outputs and serves the REST API"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

DEPENDS = "qtbase qthttpserver libgpiod"

# gitsm, because the Kanoop libraries are submodules consumed through
# add_subdirectory. A git:// fetch configures against empty directories.
SRC_URI = "gitsm://github.com/StevePunak/irrigation.git;protocol=ssh;user=git;branch=master"
SRCREV = "PUT_THE_SHA_FROM_STEP_1_HERE"

S = "${WORKDIR}/git"

inherit qt6-cmake

EXTRA_OECMAKE += " \
    -DIRRIGATION_USE_MOLD=OFF \
    -DBUILD_TESTING=OFF \
    -DBUILD_SHARED_LIBS=OFF \
"

# The SQLite driver is a dlopened plugin, so no shared-library scan can
# discover it. Without this the daemon starts and fails its first query.
RDEPENDS:${PN} += "qtbase-plugins"
```

Each `EXTRA_OECMAKE` entry closes a specific hole:

- `IRRIGATION_USE_MOLD=OFF` — the top-level `CMakeLists.txt` runs `find_program(MOLD_EXECUTABLE mold)` and adds `-fuse-ld=mold` when it hits. Under bitbake that search can reach the build host's `mold`, which is not the cross linker this build is supposed to use.
- `BUILD_TESTING=OFF` — `IrrigationD/CMakeLists.txt` guards `add_subdirectory(tests)` on it. The test targets link `Qt6::Test`, which is not in `DEPENDS`.
- `BUILD_SHARED_LIBS=OFF` — the three Kanoop libraries are declared with `qt_add_library` and no explicit type, so they follow this variable. Static keeps the package a single binary with nothing to install alongside it. Built shared, they would be produced but never installed, because the superproject adds them with `EXCLUDE_FROM_ALL` and their own `install(TARGETS ... LIBRARY ...)` rules never run — leaving a daemon that fails to start on a missing `.so`.

- [ ] **Step 3: Fetch and build**

SSH fetching needs the agent socket inside the container and inside bitbake's task environment. bitbake exports only a fixed set of variables into `do_fetch`, so `SSH_AUTH_SOCK` has to be named in `BB_ENV_PASSTHROUGH_ADDITIONS` or the fetch fails with a permission denied that looks like a missing key:

```bash
podman run --rm --userns=keep-id --pids-limit=-1 \
  -v /home/spunak/src/punak:/home/spunak/src/punak \
  -v "$SSH_AUTH_SOCK":/ssh-agent \
  -w /home/spunak/src/punak/rpi \
  localhost/yocto-walnascar:ubuntu-22.04 \
  bash -lc 'export SSH_AUTH_SOCK=/ssh-agent
export GIT_SSH_COMMAND="ssh -o StrictHostKeyChecking=accept-new"
export BB_ENV_PASSTHROUGH_ADDITIONS="$BB_ENV_PASSTHROUGH_ADDITIONS SSH_AUTH_SOCK GIT_SSH_COMMAND"
kas shell kas/rpi4-irrigation.yml -c "bitbake irrigationd"'
```

Expected: `Tasks Summary: ... all succeeded`.

If `do_fetch` fails with `Permission denied (publickey)`, the agent socket did not reach the task — check that both variables appear in `BB_ENV_PASSTHROUGH_ADDITIONS`. If it fails with `Submodule ... could not be fetched`, the `SRC_URI` says `git://` rather than `gitsm://`.

- [ ] **Step 4: Assert the binary is packaged and is an aarch64 ELF**

```bash
podman run --rm --userns=keep-id --pids-limit=-1 \
  -v /home/spunak/src/punak:/home/spunak/src/punak \
  -w /home/spunak/src/punak/rpi \
  localhost/yocto-walnascar:ubuntu-22.04 \
  bash -lc 'kas shell kas/rpi4-irrigation.yml -c "oe-pkgdata-util list-pkg-files irrigationd"'

file $(find ~/src/punak/rpi/build/tmp/work -path '*irrigationd*/image/usr/bin/irrigationd' | head -1)
```

Expected: the package listing contains `/usr/bin/irrigationd`, and `file` reports `ELF 64-bit LSB ... ARM aarch64`. An `x86-64` result means the recipe built natively.

- [ ] **Step 5: Assert the Kanoop libraries were linked statically**

This is the check that catches a `BUILD_SHARED_LIBS` regression, which otherwise produces a package that installs cleanly and then fails to start on the target:

```bash
BIN=$(find ~/src/punak/rpi/build/tmp/work -path '*irrigationd*/image/usr/bin/irrigationd' | head -1)
podman run --rm -v /home/spunak/src/punak:/home/spunak/src/punak \
  localhost/yocto-walnascar:ubuntu-22.04 \
  bash -lc "readelf -d '$BIN' | grep NEEDED"
```

Expected: `NEEDED` entries for `libQt6Core.so.6`, `libQt6Network.so.6`, `libQt6HttpServer.so.6`, `libQt6Sql.so.6`, `libgpiod.so.3`, `libstdc++`, `libc` and friends — and **no** entry naming `KanoopCommonQt`, `KanoopDatabaseQt` or `KanoopPiQt`. Any Kanoop `NEEDED` entry means the libraries built shared and were never installed.

- [ ] **Step 6: Commit**

```bash
cd ~/src/punak/rpi
git add meta-rpi4-irrigation/recipes-daemon/irrigationd
git commit
```

Message:

```
feat: cross-build irrigationd from a pinned revision

Fetches with gitsm because the Kanoop libraries are submodules consumed
through add_subdirectory; a plain git fetch configures against empty
directories.

Three CMake options each close a hole that otherwise surfaces late.
IRRIGATION_USE_MOLD=OFF stops find_program reaching the build host's
linker. BUILD_TESTING=OFF keeps the test targets, which link Qt6::Test,
out of a build that does not depend on it. BUILD_SHARED_LIBS=OFF keeps the
Kanoop libraries static: the superproject adds them with EXCLUDE_FROM_ALL,
so built shared they would be produced and never installed.

The SQLite driver is dlopened and no shared-library scan finds it, so the
runtime dependency on qtbase-plugins is declared by hand.
```

---

### Task 4: Web delivery — the static bundle and the nginx site

Produces the two packages that put the frontend in front of a browser: the built React bundle as static files, and an nginx site that serves them and reverse-proxies `/api` to the daemon.

**Files:**
- Create: `meta-rpi4-irrigation/recipes-httpd/irrigation-web/irrigation-web_1.0.bb`
- Create: `meta-rpi4-irrigation/recipes-httpd/nginx-irrigation-config/nginx-irrigation-config_1.0.bb`
- Create: `meta-rpi4-irrigation/recipes-httpd/nginx-irrigation-config/files/sites-available/irrigation.conf`

**Interfaces:**
- Consumes: the layer skeleton from Task 1 and the `server/listenPort` value (`8080`) from Task 2's configuration.
- Produces: the package names `irrigation-web` and `nginx-irrigation-config`, the document root `/var/www/irrigation/html`, and an nginx site listening on port 80. Task 5's image installs both.

The bundle is built outside Yocto and installed from the tree, following the pattern `gateway-admin-frontend_1.0.bb` already sets in `meta-rpi4-gateway`. Building it inside a recipe would need nodejs plus an offline npm cache, and nothing in this project has needed that yet.

The bundle lives in the **irrigation** repository, which is a sibling of the **rpi** repository, so the recipe reaches across trees. That path is the one thing likely to break when someone relocates a checkout, so it is a variable with a default rather than a literal buried in `do_install`.

- [ ] **Step 1: Write the static bundle recipe**

`meta-rpi4-irrigation/recipes-httpd/irrigation-web/irrigation-web_1.0.bb`:

```
DESCRIPTION = "Irrigation controller web UI (React static bundle)"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# The bundle is built outside Yocto, in the sibling irrigation repository.
# Override in a local.conf when the checkout lives elsewhere.
IRRIGATION_WEB_DIST ?= "${THISDIR}/../../../../irrigation/web/dist"

do_install() {
    if [ ! -d "${IRRIGATION_WEB_DIST}" ]; then
        bbfatal "Web bundle not found at ${IRRIGATION_WEB_DIST}. Run 'npm run build' in the irrigation repository's web/ directory before building the image, or set IRRIGATION_WEB_DIST."
    fi
    if [ ! -f "${IRRIGATION_WEB_DIST}/index.html" ]; then
        bbfatal "No index.html in ${IRRIGATION_WEB_DIST}. The directory exists but holds no bundle."
    fi

    install -d ${D}/var/www/irrigation/html
    cp -r ${IRRIGATION_WEB_DIST}/* ${D}/var/www/irrigation/html/
}

FILES:${PN} = "/var/www/irrigation/"
```

The second guard matters: a stale empty `dist/` left behind by an interrupted build passes the directory check, installs nothing, and produces an image that serves an nginx 403 with no clue why.

- [ ] **Step 2: Write the nginx site**

`meta-rpi4-irrigation/recipes-httpd/nginx-irrigation-config/files/sites-available/irrigation.conf`:

```nginx
server {
    listen 80 default_server;
    listen [::]:80 default_server;
    server_name _;

    root /var/www/irrigation/html;
    index index.html;

    # The daemon binds loopback. This proxy is the only path to it.
    location /api/ {
        proxy_pass http://127.0.0.1:8080;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
    }

    # Single-page app: every route that is not a file resolves to index.html.
    location / {
        try_files $uri $uri/ /index.html;
    }
}
```

- [ ] **Step 3: Write the nginx config recipe**

`meta-rpi4-irrigation/recipes-httpd/nginx-irrigation-config/nginx-irrigation-config_1.0.bb`:

```
DESCRIPTION = "nginx site configuration for the irrigation controller"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

RDEPENDS:${PN} = "nginx"

SRC_URI = "file://sites-available/irrigation.conf"

do_install() {
    install -d ${D}${sysconfdir}/nginx/sites-available
    install -d ${D}${sysconfdir}/nginx/sites-enabled
    install -m 0644 ${UNPACKDIR}/sites-available/irrigation.conf \
        ${D}${sysconfdir}/nginx/sites-available/irrigation.conf
    ln -s ../sites-available/irrigation.conf \
        ${D}${sysconfdir}/nginx/sites-enabled/irrigation.conf

    install -d ${D}${libdir}/systemd/system-preset
    printf 'enable nginx.service\n' \
        > ${D}${libdir}/systemd/system-preset/91-nginx.preset
}

FILES:${PN} = " \
    ${sysconfdir}/nginx/sites-available/ \
    ${sysconfdir}/nginx/sites-enabled/ \
    ${libdir}/systemd/system-preset/91-nginx.preset \
"

CONFFILES:${PN} = "${sysconfdir}/nginx/sites-available/irrigation.conf"
```

Both this site and poky's stock nginx configuration declare `default_server` on port 80. Step 5 checks for that collision, which nginx reports only at startup on the target, long after the build looks clean.

- [ ] **Step 4: Build both recipes**

```bash
podman run --rm --userns=keep-id --pids-limit=-1 \
  -v /home/spunak/src/punak:/home/spunak/src/punak \
  -w /home/spunak/src/punak/rpi \
  localhost/yocto-walnascar:ubuntu-22.04 \
  bash -lc 'kas shell kas/rpi4-irrigation.yml -c "bitbake nginx-irrigation-config irrigation-web"'
```

If `irrigation-web` fails with the `bbfatal` from Step 1, the bundle has not been built. Run `npm run build` in `~/src/punak/irrigation/web` first. That failure is the recipe working as designed; do not weaken the guard to get past it.

- [ ] **Step 5: Assert the site is installed, enabled, and does not collide**

```bash
podman run --rm --userns=keep-id --pids-limit=-1 \
  -v /home/spunak/src/punak:/home/spunak/src/punak \
  -w /home/spunak/src/punak/rpi \
  localhost/yocto-walnascar:ubuntu-22.04 \
  bash -lc 'kas shell kas/rpi4-irrigation.yml -c "oe-pkgdata-util list-pkg-files nginx-irrigation-config irrigation-web"'
```

Expected: `/etc/nginx/sites-available/irrigation.conf`, its symlink under `sites-enabled/`, the nginx preset, and `/var/www/irrigation/html/index.html` among the bundle's files.

Then check for a second `default_server` on port 80 in whatever poky's nginx recipe ships:

```bash
podman run --rm --userns=keep-id --pids-limit=-1 \
  -v /home/spunak/src/punak:/home/spunak/src/punak \
  -w /home/spunak/src/punak/rpi \
  localhost/yocto-walnascar:ubuntu-22.04 \
  bash -lc 'grep -rn "listen.*80" meta-openembedded/meta-webserver/recipes-httpd/nginx/files/ 2>/dev/null || echo "no stock site files"'
```

If a stock site also claims `default_server` on 80, nginx exits at startup with `duplicate default server for 0.0.0.0:80`. Resolve it by removing the stock site in the image recipe rather than by dropping `default_server` from ours — the irrigation site must answer a bare IP with no `Host` header, which is how the browser will arrive.

- [ ] **Step 6: Commit**

```bash
cd ~/src/punak/rpi
git add meta-rpi4-irrigation/recipes-httpd
git commit
```

Message:

```
feat: serve the irrigation web bundle and proxy the API

The bundle is built outside Yocto and installed from the sibling
irrigation checkout, following the pattern gateway-admin-frontend already
sets. The path is a variable so a relocated checkout is a one-line
override rather than a recipe edit.

Two guards on the install: a missing dist directory and a dist directory
holding no index.html. The second is the one that matters — an empty
bundle left by an interrupted npm build passes a directory check, installs
nothing, and serves a 403 with nothing in the build log to explain it.
```

---

### Task 5: rpi4-irrigation-image

Produces the flashable image. Everything the previous four tasks built gets installed here, plus the runtime pieces the spec names.

**Files:**
- Create: `meta-rpi4-irrigation/recipes-images/images/rpi4-irrigation-image.bb`

**Interfaces:**
- Consumes: `irrigation-init`, `irrigationd`, `irrigation-web` and `nginx-irrigation-config` from Tasks 2 through 4.
- Produces: the image target `rpi4-irrigation-image`, already named in Task 1's kas file and both build scripts.

Spec section 9 names the contents: `core-image-base` plus the layer's own packages, avahi, sqlite3, tzdata, wpa-supplicant and SSH. `tzdata` is load-bearing rather than a convenience — the daemon stores schedule rules as local wall-clock plus an IANA zone id and resolves them with `QDateTime::TransitionResolution`. Without the zone database on the target every lookup silently falls back to UTC, and a 6am program fires at the wrong hour with no error anywhere.

- [ ] **Step 1: Write the image recipe**

`meta-rpi4-irrigation/recipes-images/images/rpi4-irrigation-image.bb`:

```
DESCRIPTION = "Irrigation controller appliance image for Raspberry Pi 4B"

require recipes-core/images/core-image-base.bb

# Replace busybox with full GNU utilities
VIRTUAL-RUNTIME_base-utils = "util-linux-base"
VIRTUAL-RUNTIME_base-utils-hwclock = "util-linux-hwclock"
VIRTUAL-RUNTIME_base-utils-syslog = ""
VIRTUAL-RUNTIME_login-manager = "shadow"
IMAGE_INSTALL:remove = "busybox busybox-udhcpc busybox-udhcpd busybox-hwclock busybox-syslog"

IMAGE_INSTALL:append = " \
    irrigationd \
    irrigation-init \
    irrigation-web \
    nginx \
    nginx-irrigation-config \
    libgpiod \
    libgpiod-tools \
    sqlite3 \
    tzdata \
    avahi-daemon \
    avahi-libnss-mdns \
    wpa-supplicant \
    systemd-networkd-config \
    coreutils \
    util-linux \
    procps \
    findutils \
    grep \
    gawk \
    sed \
    tar \
    bash \
    shadow \
    rsync \
    less \
    file \
    strace \
    i2c-tools \
"

IMAGE_FEATURES += "ssh-server-openssh package-management"

PACKAGE_CLASSES = "package_rpm"
```

`libgpiod-tools` earns its place: `gpioget`, `gpioset` and `gpioinfo` are how a stuck valve gets diagnosed on the target without a debugger. `i2c-tools` is there for the DS3231 real-time clock the `config.txt` fragment enables.

- [ ] **Step 2: Build the image**

```bash
cd ~/src/punak/rpi
podman run --rm --userns=keep-id --pids-limit=-1 \
  -v /home/spunak/src/punak:/home/spunak/src/punak \
  -v "$SSH_AUTH_SOCK":/ssh-agent \
  -w /home/spunak/src/punak/rpi \
  localhost/yocto-walnascar:ubuntu-22.04 \
  bash -lc 'export SSH_AUTH_SOCK=/ssh-agent
export GIT_SSH_COMMAND="ssh -o StrictHostKeyChecking=accept-new"
export BB_ENV_PASSTHROUGH_ADDITIONS="$BB_ENV_PASSTHROUGH_ADDITIONS SSH_AUTH_SOCK GIT_SSH_COMMAND"
kas build kas/rpi4-irrigation.yml'
```

This is a long run the first time. qtbase alone is most of an hour on a cold sstate, and the `sql-sqlite` PACKAGECONFIG means it cannot be shared with the `rpi4-qt6-image` build's qtbase.

Expected: `Tasks Summary: ... all succeeded`, and an image at `build/tmp/deploy/images/raspberrypi4-64/rpi4-irrigation-image-raspberrypi4-64.rootfs.wic.bz2`.

- [ ] **Step 3: Assert the four landmine conditions in the built artifacts**

Each of these fails at runtime rather than at build time, so they get checked here where the evidence is still on disk.

```bash
cd ~/src/punak/rpi
D=build/tmp/deploy/images/raspberrypi4-64

echo "--- 1. the GPIO safety line reached config.txt"
grep -n 'gpio=5,6,12,13,16,19,20,21=op,dh' $D/bootfiles/config.txt

echo "--- 2. the SQLite driver plugin is in the image manifest"
grep -E 'qtbase-plugins|^sqlite3 ' $D/rpi4-irrigation-image-raspberrypi4-64.rootfs.manifest

echo "--- 3. libgpiod is 2.2.x, not 1.6.x"
grep -E '^libgpiod' $D/rpi4-irrigation-image-raspberrypi4-64.rootfs.manifest

echo "--- 4. the daemon and its unit both landed"
grep -E '^irrigationd|^irrigation-init|^irrigation-web|^nginx-irrigation-config' \
  $D/rpi4-irrigation-image-raspberrypi4-64.rootfs.manifest
```

Expected: the `gpio=` line present in `config.txt`; `qtbase-plugins` in the manifest; every `libgpiod` line reading `2.2.2`; and all four layer packages listed. A `libgpiod` line reading `1.6.5` means `PREFERRED_VERSION_libgpiod` was written into a recipe rather than the kas `local_conf_header`, where it is inert.

- [ ] **Step 4: Assert the SQLite driver is physically present in the rootfs**

The manifest says a package was installed. It does not say the plugin file exists. Check the file:

```bash
cd ~/src/punak/rpi
find build/tmp/work/raspberrypi4_64-poky-linux/rpi4-irrigation-image/*/rootfs/usr/lib/plugins/sqldrivers -name 'libqsqlite.so' 2>/dev/null \
  || echo "MISSING — qtbase built with no SQL drivers"
```

Expected: a path ending in `libqsqlite.so`. `MISSING` means `PACKAGECONFIG:append:pn-qtbase = " sql-sqlite"` did not take, and the daemon will start cleanly and fail its first query on the target.

- [ ] **Step 5: Commit**

```bash
cd ~/src/punak/rpi
git add meta-rpi4-irrigation/recipes-images
git commit
```

Message:

```
feat: add the rpi4-irrigation-image appliance image

core-image-base plus the layer's four packages, nginx, and the runtime
pieces the daemon needs. tzdata is required rather than convenient: the
daemon stores schedule rules as local wall-clock plus an IANA zone id, and
without the zone database every lookup falls back to UTC and a morning
program fires at the wrong hour with nothing logged.

libgpiod-tools and i2c-tools ship so a stuck valve or a silent RTC can be
diagnosed on the target.
```

---

### Task 6: Flash and bring-up verification

Produces a controller that actually runs. Every check here is on hardware, because every failure mode this task looks for is invisible to the build.

**Files:** none. This task writes no code; it verifies the image the previous five produced.

**Interfaces:**
- Consumes: the image from Task 5.

- [ ] **Step 1: Flash the image**

```bash
cd ~/src/punak/rpi/build/tmp/deploy/images/raspberrypi4-64
bzcat rpi4-irrigation-image-raspberrypi4-64.rootfs.wic.bz2 | sudo dd of=/dev/sdX bs=4M status=progress conv=fsync
sync
```

Identify the device first with `lsblk` and confirm it against its size and model. Present the discovery output and get confirmation before writing — `dd` to the wrong device destroys whatever was there with no prompt and no undo.

- [ ] **Step 2: Verify the valves are shut before the daemon ever starts**

Power the Pi with the relay board connected and the daemon **masked**, so only `config.txt` is holding the lines:

```bash
systemctl mask irrigationd
reboot
```

After the reboot, meter COM/NO on all eight relay channels. Expected: all eight open, all eight LEDs dark.

This is the single most important check in the plan. Six of the eight zone lines come up asserted without the `gpio=` line, because BCM 9-27 default to pull-down and the board is LOW-trigger. If any channel is closed here, stop and fix `config.txt` before unmasking anything.

```bash
systemctl unmask irrigationd
```

- [ ] **Step 3: Verify the daemon starts and holds the lines**

```bash
systemctl start irrigationd
systemctl status irrigationd
journalctl -u irrigationd -n 50 --no-pager
gpioinfo | grep -E 'line +(5|6|12|13|16|19|20|21):'
```

Expected: the unit active, no errors in the journal, and every one of the eight lines showing as used by `irrigationd` and output. Meter the contacts again — still all eight open.

- [ ] **Step 4: Verify the database was created and the schema migrated**

```bash
ls -la /var/lib/irrigationd/
sqlite3 /var/lib/irrigationd/irrigation.db '.tables'
sqlite3 /var/lib/irrigationd/irrigation.db 'SELECT sw_version FROM info;'
sqlite3 /var/lib/irrigationd/irrigation.db 'SELECT number, name, enabled FROM zones ORDER BY number;'
```

Expected: the database present, `info`, `zones`, `programs`, `program_start_times`, `program_zones` and `fired_instants` among the tables, `sw_version` reading `1.0.0`, and eight seeded zones.

An empty database with the daemon running and no error in the journal is the `sql-sqlite` failure. Confirm with `ls /usr/lib/plugins/sqldrivers/` on the target.

- [ ] **Step 5: Verify the web interface and the API proxy**

From another machine on the same network:

```bash
curl -sS -o /dev/null -w '%{http_code}\n' http://<pi-address>/
curl -sS http://<pi-address>/api/admin/status | head -20
```

Expected: `200` for the static bundle, and JSON from the status endpoint. A `502` from `/api/` means nginx is up and the daemon is not; a `404` means the site config did not load or a stock nginx site won the `default_server` race.

- [ ] **Step 6: Verify the timezone database resolves**

```bash
sqlite3 /var/lib/irrigationd/irrigation.db \
  "INSERT INTO program_start_times (program_id, minutes_after_midnight, timezone) VALUES (1, 360, 'America/New_York');" 2>/dev/null
TZ=America/New_York date
ls /usr/share/zoneinfo/America/New_York
```

Expected: the zoneinfo file present and `date` rendering Eastern time. A missing file means `tzdata` did not make it into the image, and every schedule rule will silently resolve as UTC.

- [ ] **Step 7: Verify a crash does not open valves**

Open a zone, then kill the daemon rather than stopping it cleanly, and meter the contacts:

```bash
curl -sS -X POST http://<pi-address>/api/zones/3/open -d '{"seconds":300}' -H 'Content-Type: application/json'
# confirm zone 3 is closed at the relay, then:
systemctl kill -s SIGKILL irrigationd
```

Meter COM/NO on channel 3 immediately. Expected: the contact opens.

This settles a question the bench work left open. When `libgpiod` releases a line request the kernel frees the descriptor, and whether the pin keeps its last driven level or reverts to an input with its reset pull is not something to assume from documentation — on a LOW-trigger board with BCM 9-27 pulled down, reverting means the valve opens. If the contact stays closed, the image needs a `systemd` `ExecStopPost` or a udev rule to drive the lines inactive on daemon exit, and that is a finding for a follow-up task rather than something to improvise here.

- [ ] **Step 8: Record the results**

Write what each step actually showed, including anything that failed, to `docs/bring-up/2026-09-13-irrigation-image.md` in the irrigation repository. A bring-up whose results live only in a terminal scrollback has to be repeated the next time somebody asks whether the valves are safe at power-on.

---

## Self-Review

**Spec coverage.** Section 9 names five recipes; this plan builds four plus the image, and folds the `config.txt` fragment into `RPI_EXTRA_CONFIG` rather than the `irrigation-init` recipe the spec places it in. That deviation is deliberate and stated in Task 1: a package cannot write `config.txt`, which `rpi-config` generates onto the FAT boot partition at deploy time. Every other section 9 requirement maps to a task — `gitsm://` and the `SRCREV` pin to Task 3, `PREFERRED_VERSION_libgpiod` and the qtbase `PACKAGECONFIG` to Task 1, the image contents to Task 5, and the development loop to the SDK that is already built and installed.

**Known gap.** The spec's development loop — cross-compile on the host, rsync the binary to the target, restart the unit — has no task. It needs no layer work: the SDK at `/opt/poky/5.2.4` already carries `libgpiod` 2.2.2 and the Qt 6.10.3 sysroot. It is a documentation item for the bring-up notes in Task 6 Step 8.

**Type consistency.** The package names `irrigationd`, `irrigation-init`, `irrigation-web` and `nginx-irrigation-config`, the image name `rpi4-irrigation-image`, the document root `/var/www/irrigation/html`, the config path `/etc/irrigationd.ini`, the state directory `/var/lib/irrigationd` and the port `8080` are used identically in every task that names them.
