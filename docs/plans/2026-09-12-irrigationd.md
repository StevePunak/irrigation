# IrrigationD Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `irrigationd`, the Qt 6 daemon that owns the eight valve outputs, resolves programs into watering runs, and serves a REST API to the web front end.

**Architecture:** `ZoneController` is the sole owner of GPIO and enforces four safety invariants regardless of caller. `Scheduler` resolves program rules into due UTC instants against an injected clock and records every firing, making it idempotent across restarts. `ProgramRunner` walks one program's zone list, advanced by `ZoneController`'s completion signal rather than a timer of its own. Everything that blocks or listens — today just `IrrigationControlServer` — is an `AbstractThreadClass` on its own thread and talks to the valve owner exclusively through signals.

**Tech Stack:** Qt 6.10 (Core, Network, HttpServer, Sql, Test), CMake, SQLite via `KanoopDatabaseQt`, libgpiod 2.x via `KanoopPiQt`, `KanoopCommonQt` for logging/threading/settings.

**Spec:** `docs/design/2026-09-05-irrigation-design.md` — sections 5, 6, 7 and 10. The plan argues from the spec; executors read both.

---

## Global Constraints

These apply to every task. A task's requirements implicitly include this section.

- **Repository:** all work happens in `~/src/punak/irrigation` on a branch off `feature/superproject`, except Task 1 which is in `~/src/punak/KanoopPiQt` on `feature/libgpiod-v2`.
- **Never `git push`.** Commits stay local. The user controls all remote pushes.
- `set(CMAKE_CXX_STANDARD 11)` — the line every Kanoop library carries. **The effective standard is C++17 regardless.** `Qt6::Platform` exports `INTERFACE_COMPILE_FEATURES "cxx_std_17"`, and `CMAKE_CXX_STANDARD` is a floor CMake raises to satisfy a linked target's compile features, never a ceiling. Verified: the build emits `-std=gnu++17`. Do not add `CMAKE_CXX_STANDARD_REQUIRED ON`. Write valid C++17.
- `target_compile_options(${PROJ} PRIVATE -Wextra -Wall -Werror)` — warnings are errors.
- **Global namespace.** Kanoop libraries do not namespace their types; the include path carries the scoping. `ZoneController`, `Scheduler`, `AbstractThreadClass`. Enum values are scoped inside a holder class.
- **Every `.cpp` whose header declares `Q_OBJECT` ends with its moc include**, path-prefixed relative to the include root, as the last line preceded by a blank line: `#include "moc_zonecontroller.cpp"`. For `IrrigationD` the sources and headers sit together under `src/`, so the prefix is the subdirectory only.
- Code style per `meta-qt-mains/.claude/docs/codestyle-cpp.md`: `_underscorePrefixed` members, `camelCase` methods, `PascalCase` classes, `if(` with no space, explicit `== false` rather than `!`, function opening brace on its own line, control-structure brace on the same line, `catch`/`else` on their own line.
- **Primitive, enum and raw-pointer members get an in-class initializer in the header.** Never rely on a constructor to zero them.
- Doxygen on every public member. Single line: `/** @brief Summary. */`. Multi-line puts `@brief` on the line after `/**`.
- **Never specify `Qt::QueuedConnection` explicitly.** `Qt::AutoConnection` inspects the emitting and receiving threads at emit time and routes correctly. No comments justifying connection types.
- **Comments state traps, not reasoning.** A comment earns its place only when a plausible future edit would silently break behaviour and the code cannot show why. Design rationale, pattern names and descriptions of what previous code did wrong belong in the commit message.
- **Instants are UTC.** Every stored moment, log timestamp and API timestamp is UTC. Schedule *rules* stay local wall-clock plus an IANA zone id — normalising a recurring rule to UTC at write time makes it drift an hour at each DST transition.
- **Time zone resolution uses `QDateTime::TransitionResolution`** (Qt 6.7+): `Reject` for spring-forward gaps, `PreferBefore` for fall-back repeats.
- Commit messages are conventional commits and end with:
  ```
  Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01KjRDfium1CUyQogbnN1oHg
  ```

---

## File Structure

Everything below `IrrigationD/` unless stated otherwise.

| Path | Responsibility |
|---|---|
| `CMakeLists.txt` | Target `irrigationd`, links Qt6 Core/Network/HttpServer/Sql and the three Kanoop libraries. Carries `IRRIGATION_DB_VERSION`. |
| `src/main.cpp` | `QCoreApplication`, CLI parsing, logging setup, POSIX signal handling, constructs `IrrigationDaemon`. |
| `src/irrigationdaemon.{h,cpp}` | Lifecycle owner. Constructs every component in the mandated order and tears them down in reverse. |
| `src/irrigationsettings.{h,cpp}` | `AppSettings` subclass. INI-backed. Zone-to-GPIO map, chip label, clamp ceiling, bind address/port, database path. |
| `src/zonecontroller.{h,cpp}` | Sole owner of GPIO. Enforces the four safety invariants. |
| `src/scheduler.{h,cpp}` | Resolves enabled programs into due UTC instants. Injected clock. |
| `src/programrunner.{h,cpp}` | Walks one program's ordered zone list. |
| `src/stopbutton.{h,cpp}` | `InputPin` wrapper. Reads initial level, emits `pressed()`. |
| `src/iclock.h` | Clock seam. `SystemClock` and `TestClock`. |
| `src/database/irrigationdatasource.{h,cpp}` | `DataSource` subclass owning the migration flow and every query. |
| `src/database/schema.sql` | Full create-from-scratch schema. |
| `src/database/migrate/irrigation/<version>/NN-<name>.sql` | Per-version migration scripts. |
| `src/database/irrigation.qrc` | Registers `schema.sql` and every migration script as Qt resources. |
| `src/model/*.{h,cpp}` | `Zone`, `Program`, `ProgramStartTime`, `ProgramZone`, `FiredInstant` value types plus their list types. |
| `src/json/*.{h,cpp}` | Request and response body serialization. |
| `src/irrigationcontrolserver.{h,cpp}` | `AbstractThreadClass` running `QHttpServer` and every route handler on its own thread. |
| `tests/` | One `tst_*.cpp` per component, behind `BUILD_TESTING`. |
| `systemd/irrigationd.service` | Unit file. `Restart=always`. |

---

### Task 1: GPIO read-back in the IO library

**Repository: `~/src/punak/KanoopPiQt`, branch `feature/libgpiod-v2`.**

`ZoneController`'s watchdog (spec §5.1 invariant 4) has to verify line state against the kernel rather than against its own cache, and `StopButton` has to know whether the button is already held down when the daemon restarts under `Restart=always`. Neither is possible today: `IGpioBackend` has `setValues()` and no reader.

**Files:**
- Modify: `include/Kanoop/pi/gpiobackend.h`
- Modify: `include/Kanoop/pi/mockbackend.h`, `src/Kanoop/pi/mockbackend.cpp`
- Modify: `include/Kanoop/pi/libgpiodbackend.h`, `src/Kanoop/pi/libgpiodbackend.cpp`
- Modify: `include/Kanoop/pi/outputbank.h`, `src/Kanoop/pi/outputbank.cpp`
- Modify: `include/Kanoop/pi/inputpin.h`, `src/Kanoop/pi/inputpin.cpp`
- Test: `tests/tst_mockbackend.cpp`, `tests/tst_outputbank.cpp`, `tests/tst_inputpin.cpp`

**Interfaces:**
- Produces:
  - `virtual bool IGpioBackend::getValues(Gpio::RequestHandle handle, const QList<quint32>& offsets, QList<Gpio::Value>& values) = 0;`
  - `bool OutputBank::readValues(QMap<quint32, bool>& values);`
  - `bool InputPin::isAsserted(bool* ok = nullptr);`

- [ ] **Step 1: Write the failing MockBackend test**

In `tests/tst_mockbackend.cpp`:

```cpp
void TestMockBackend::getValuesReturnsWhatWasSet()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    Gpio::OutputRequest request;
    request.consumer = "test";
    request.offsets = { 5, 6, 13 };
    Gpio::RequestHandle handle = backend.requestOutputs(request);
    QVERIFY(handle != Gpio::InvalidRequest);

    QVERIFY(backend.setValues(handle, { 6 }, { Gpio::Value::Active }));

    QList<Gpio::Value> values;
    QVERIFY(backend.getValues(handle, { 5, 6, 13 }, values));
    QCOMPARE(values.count(), 3);
    QCOMPARE(values.at(0), Gpio::Value::Inactive);
    QCOMPARE(values.at(1), Gpio::Value::Active);
    QCOMPARE(values.at(2), Gpio::Value::Inactive);
}

void TestMockBackend::getValuesRejectsUnownedOffset()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    Gpio::OutputRequest request;
    request.consumer = "test";
    request.offsets = { 5 };
    Gpio::RequestHandle handle = backend.requestOutputs(request);

    QList<Gpio::Value> values;
    QVERIFY(backend.getValues(handle, { 5, 99 }, values) == false);
    QVERIFY(backend.errorText().isEmpty() == false);
}

void TestMockBackend::getValuesRejectsInvalidHandle()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    QList<Gpio::Value> values;
    QVERIFY(backend.getValues(Gpio::InvalidRequest, { 5 }, values) == false);
}
```

Declare all three in the class's `private slots:` block.

- [ ] **Step 2: Run the test to verify it fails**

```bash
cd ~/src/punak/KanoopPiQt
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target tst_mockbackend -j 32
```

Expected: compile failure, `'class MockBackend' has no member named 'getValues'`.

- [ ] **Step 3: Add the pure virtual to IGpioBackend**

In `include/Kanoop/pi/gpiobackend.h`, immediately after the `setValues()` declaration:

```cpp
    /**
     * @brief Reads the present logical level of @p offsets into @p values.
     *
     * Values are logical: active-low inversion was applied by the kernel when the
     * lines were requested, so a returned Active means the load is energised.
     *
     * @return True on success. On failure @p values is left untouched.
     */
    virtual bool getValues(Gpio::RequestHandle handle,
                           const QList<quint32>& offsets,
                           QList<Gpio::Value>& values) = 0;
```

- [ ] **Step 4: Implement it in MockBackend**

Declaration in `include/Kanoop/pi/mockbackend.h` alongside the other overrides:

```cpp
    /** @brief Reads the stored logical level of @p offsets into @p values. @return True on success. */
    virtual bool getValues(Gpio::RequestHandle handle,
                           const QList<quint32>& offsets,
                           QList<Gpio::Value>& values) override;
```

Definition in `src/Kanoop/pi/mockbackend.cpp`, following the shape of the existing `setValues()`:

```cpp
bool MockBackend::getValues(Gpio::RequestHandle handle,
                            const QList<quint32>& offsets,
                            QList<Gpio::Value>& values)
{
    if(_requests.contains(handle) == false) {
        setErrorText(QString("No such request handle: %1").arg(handle));
        return false;
    }

    const Request& request = _requests[handle];
    QList<Gpio::Value> result;
    for(quint32 offset : offsets) {
        if(request.offsets.contains(offset) == false) {
            setErrorText(QString("Offset %1 is not held by this request").arg(offset));
            return false;
        }
        result.append(request.values.value(offset, Gpio::Value::Inactive));
    }

    values = result;
    return true;
}
```

Build the whole list before assigning, so a rejected offset leaves the caller's list untouched as the Doxygen promises.

- [ ] **Step 5: Run the MockBackend tests**

```bash
cmake --build build --target tst_mockbackend -j 32 && ./build/tests/tst_mockbackend
```

Expected: PASS, all cases.

- [ ] **Step 6: Commit**

```bash
git add include/Kanoop/pi/gpiobackend.h include/Kanoop/pi/mockbackend.h src/Kanoop/pi/mockbackend.cpp tests/tst_mockbackend.cpp
git commit -m "feat: add IGpioBackend::getValues() with a mock implementation"
```

- [ ] **Step 7: Write the failing OutputBank test**

In `tests/tst_outputbank.cpp`:

```cpp
void TestOutputBank::readValuesReflectsTheKernelNotTheCache()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    OutputBank bank(&backend, "test", { 5, 6 }, false);
    QVERIFY(bank.request());
    QVERIFY(bank.setValue(5, true));

    QMap<quint32, bool> values;
    QVERIFY(bank.readValues(values));
    QCOMPARE(values.count(), 2);
    QCOMPARE(values.value(5), true);
    QCOMPARE(values.value(6), false);
}

void TestOutputBank::readValuesFailsWhenNotRequested()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    OutputBank bank(&backend, "test", { 5 }, false);

    QMap<quint32, bool> values;
    QVERIFY(bank.readValues(values) == false);
}
```

- [ ] **Step 8: Run it and verify it fails**

```bash
cmake --build build --target tst_outputbank -j 32
```

Expected: compile failure, no member `readValues`.

- [ ] **Step 9: Implement OutputBank::readValues()**

Header, after `isActive()`:

```cpp
    /**
     * @brief Reads the present state of every line in the bank into @p values.
     *
     * Unlike isActive(), which returns the last value written, this queries the
     * backend. Use it wherever a disagreement between cache and hardware is the
     * thing being tested for.
     *
     * @return True on success.
     */
    bool readValues(QMap<quint32, bool>& values);
```

Implementation:

```cpp
bool OutputBank::readValues(QMap<quint32, bool>& values)
{
    if(isRequested() == false) {
        _errorText = "Lines are not requested";
        return false;
    }

    QList<Gpio::Value> read;
    if(_backend->getValues(_handle, _offsets, read) == false) {
        _errorText = _backend->errorText();
        return false;
    }

    QMap<quint32, bool> result;
    for(int i = 0; i < _offsets.count(); i++) {
        result.insert(_offsets.at(i), read.at(i) == Gpio::Value::Active);
    }

    values = result;
    return true;
}
```

- [ ] **Step 10: Run the OutputBank tests**

```bash
cmake --build build --target tst_outputbank -j 32 && ./build/tests/tst_outputbank
```

Expected: PASS.

- [ ] **Step 11: Write the failing InputPin test**

```cpp
void TestInputPin::isAssertedReadsTheLine()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    InputPin pin(&backend, "test", 25);
    pin.setActiveLow(true);
    QVERIFY(pin.request());

    bool ok = false;
    QCOMPARE(pin.isAsserted(&ok), false);
    QCOMPARE(ok, true);

    backend.setLineValue(25, Gpio::Value::Active);
    QCOMPARE(pin.isAsserted(&ok), true);
    QCOMPARE(ok, true);
}

void TestInputPin::isAssertedReportsFailureWhenNotRequested()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    InputPin pin(&backend, "test", 25);

    bool ok = true;
    QCOMPARE(pin.isAsserted(&ok), false);
    QCOMPARE(ok, false);
}
```

**MockBackend needs two new helpers, added in this task.** Verified against
`include/Kanoop/pi/mockbackend.h` at `daf25a9`, the existing surface is
`simulateEdge()`, `lineValue()`, `isRequested()`, `setValuesCallCount()`,
`lastSetOffsets()`, `lastInputRequest()`, `lastOutputRequest()` and
`setFailNextRequest()`. Missing, and required by Tasks 5, 7, 8 and 10:

```cpp
    /** @brief Sets the stored logical level of @p offset without emitting an edge. */
    void setLineValue(quint32 offset, Gpio::Value value) { _values.insert(offset, value); }

    /** @brief Resets the setValues() call counter. */
    void resetSetValuesCallCount() { _setValuesCallCount = 0; }
```

`setLineValue()` sets the level an input reports on the next `getValues()`
without emitting an edge — it is how a test simulates a button already held when
the process starts.

- [ ] **Step 12: Run it and verify it fails**

```bash
cmake --build build --target tst_inputpin -j 32
```

Expected: compile failure, no member `isAsserted`.

- [ ] **Step 13: Implement InputPin::isAsserted()**

Header, after `offset()`:

```cpp
    /**
     * @brief Returns the present logical level of the line.
     *
     * An edge-only input has no initial state, so a process that starts while the
     * input is already asserted never sees the edge that got it there. Call this
     * once after request() to establish the starting point.
     *
     * @param ok Set to true when the read succeeded, false otherwise. May be null.
     * @return True when the line is asserted.
     */
    bool isAsserted(bool* ok = nullptr);
```

Implementation:

```cpp
bool InputPin::isAsserted(bool* ok)
{
    if(ok != nullptr) {
        *ok = false;
    }

    if(isRequested() == false) {
        _errorText = "Line is not requested";
        return false;
    }

    QList<Gpio::Value> values;
    if(_backend->getValues(_handle, { _offset }, values) == false) {
        _errorText = _backend->errorText();
        return false;
    }

    if(ok != nullptr) {
        *ok = true;
    }
    return values.first() == Gpio::Value::Active;
}
```

No reference to `_activeLow`. The kernel already applied it — see spec §4.3.

- [ ] **Step 14: Implement LibGpiodBackend::getValues()**

Header declaration matching the other overrides, then in `src/Kanoop/pi/libgpiodbackend.cpp`:

```cpp
bool LibGpiodBackend::getValues(Gpio::RequestHandle handle,
                                const QList<quint32>& offsets,
                                QList<Gpio::Value>& values)
{
    if(_requests.contains(handle) == false) {
        setErrorText(QString("No such request handle: %1").arg(handle));
        return false;
    }

    gpiod_line_request* request = _requests.value(handle).request;
    if(request == nullptr) {
        setErrorText("Request has no line request");
        return false;
    }

    QList<unsigned int> rawOffsets;
    for(quint32 offset : offsets) {
        rawOffsets.append(offset);
    }

    QList<gpiod_line_value> rawValues;
    rawValues.resize(offsets.count());

    if(gpiod_line_request_get_values_subset(request,
                                            static_cast<size_t>(rawOffsets.count()),
                                            rawOffsets.constData(),
                                            rawValues.data()) != 0) {
        setErrorText(QString("gpiod_line_request_get_values_subset failed: %1").arg(strerror(errno)));
        return false;
    }

    QList<Gpio::Value> result;
    for(gpiod_line_value raw : rawValues) {
        if(raw == GPIOD_LINE_VALUE_ERROR) {
            setErrorText("Kernel reported GPIOD_LINE_VALUE_ERROR");
            return false;
        }
        result.append(raw == GPIOD_LINE_VALUE_ACTIVE ? Gpio::Value::Active : Gpio::Value::Inactive);
    }

    values = result;
    return true;
}
```

Signature verified against libgpiod 2.2.5 `/usr/include/gpiod.h:1042`:

```c
int gpiod_line_request_get_values_subset(struct gpiod_line_request *request,
                                         size_t num_values,
                                         const unsigned int *offsets,
                                         enum gpiod_line_value *values);
```

Returns `0` on success and `-1` on failure, and writes `GPIOD_LINE_VALUE_ERROR` (`-1`) into any element it could not read.

- [ ] **Step 15: Run the full suite**

```bash
cmake --build build -j 32 && ctest --test-dir build --output-on-failure
```

Expected: every suite passes. The `LibGpiodBackend` success path stays untested on a development host — that is what spec §2.9 bring-up covers.

- [ ] **Step 16: Commit**

```bash
git add -u
git commit -m "feat: add line read-back to OutputBank and InputPin"
```

- [ ] **Step 17: Bump the submodule pointer in the superproject**

```bash
cd ~/src/punak/irrigation
git -C KanoopPiQt log --oneline -1
git add KanoopPiQt
git commit -m "build: bump KanoopPiQt for GPIO read-back"
```

The superproject consumes `KanoopPiQt` as a submodule pinned to a SHA. Without this bump the daemon builds against the old interface and Task 5 fails to compile.

---

### Task 2: Daemon skeleton, settings and logging

Produces a binary that starts, reads its INI, logs, handles SIGINT/SIGTERM and exits cleanly. Nothing else in this plan can be run until this exists.

**Files:**
- Create: `IrrigationD/CMakeLists.txt`
- Create: `IrrigationD/src/main.cpp`
- Create: `IrrigationD/src/irrigationsettings.h`, `IrrigationD/src/irrigationsettings.cpp`
- Create: `IrrigationD/src/irrigationdaemon.h`, `IrrigationD/src/irrigationdaemon.cpp`
- Create: `IrrigationD/tests/CMakeLists.txt`
- Create: `IrrigationD/tests/tst_settings.cpp`
- Modify: `CMakeLists.txt` (superproject — add `add_subdirectory(IrrigationD)`)

**Interfaces:**
- Produces:
  - `class IrrigationSettings : public AppSettings` with `QMap<int,quint32> zoneGpioMap() const`, `QString chipLabel() const`, `int maxZoneSeconds() const`, `QString bindAddress() const`, `int listenPort() const`, `QString databasePath() const`, `quint32 stopButtonOffset() const`, `bool zoneActiveLow() const`
  - `class IrrigationDaemon : public QObject` with `bool start()`, `void stop()`

- [ ] **Step 1: Write the failing settings test**

`IrrigationD/tests/tst_settings.cpp`:

```cpp
#include <QTest>
#include <QTemporaryDir>
#include <QSettings>

#include "irrigationsettings.h"

class TestSettings : public QObject
{
    Q_OBJECT
private slots:
    void zoneMapParsesEightEntries();
    void zoneMapRejectsMalformedEntries();
    void defaultsAreSafe();
};

void TestSettings::zoneMapParsesEightEntries()
{
    QTemporaryDir dir;
    QString path = dir.filePath("test.ini");
    {
        QSettings ini(path, QSettings::IniFormat);
        ini.setValue("gpio/zones", "1=5,2=6,3=13,4=16,5=19,6=20,7=21,8=26");
    }

    IrrigationSettings settings(path);
    QMap<int, quint32> map = settings.zoneGpioMap();

    QCOMPARE(map.count(), 8);
    QCOMPARE(map.value(1), 5u);
    QCOMPARE(map.value(4), 16u);
    QCOMPARE(map.value(5), 19u);
    QCOMPARE(map.value(8), 26u);
}

void TestSettings::zoneMapRejectsMalformedEntries()
{
    QTemporaryDir dir;
    QString path = dir.filePath("test.ini");
    {
        QSettings ini(path, QSettings::IniFormat);
        ini.setValue("gpio/zones", "1=5,rubbish,3=13,4=");
    }

    IrrigationSettings settings(path);
    QMap<int, quint32> map = settings.zoneGpioMap();

    QCOMPARE(map.count(), 2);
    QCOMPARE(map.value(1), 5u);
    QCOMPARE(map.value(3), 13u);
}

void TestSettings::defaultsAreSafe()
{
    QTemporaryDir dir;
    IrrigationSettings settings(dir.filePath("absent.ini"));

    QCOMPARE(settings.maxZoneSeconds(), 3600);
    QCOMPARE(settings.bindAddress(), QString("127.0.0.1"));
    QCOMPARE(settings.listenPort(), 8080);
    QCOMPARE(settings.zoneActiveLow(), true);
    QVERIFY(settings.zoneGpioMap().isEmpty());
}

QTEST_MAIN(TestSettings)
#include "tst_settings.moc"
```

The malformed-entry case is the one that matters. A typo in the zone map must drop that entry and keep the rest, never shift the remaining zones up — a silently renumbered map waters the wrong beds.

- [ ] **Step 2: Write the build files**

`IrrigationD/CMakeLists.txt`:

```cmake
set(PROJ irrigationd)

set(IRRIGATION_DB_VERSION 1.0.0)

find_package(Qt6 REQUIRED COMPONENTS Core Network HttpServer Sql)

qt_standard_project_setup()

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

qt_add_executable(${PROJ}
    src/main.cpp
    src/irrigationsettings.h  src/irrigationsettings.cpp
    src/irrigationdaemon.h    src/irrigationdaemon.cpp
)

target_compile_definitions(${PROJ} PRIVATE
    IRRIGATION_DB_VERSION=${IRRIGATION_DB_VERSION}
)

target_include_directories(${PROJ} PRIVATE src)

target_link_libraries(${PROJ} PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::HttpServer
    Qt6::Sql
    KanoopCommonQt
    KanoopDatabaseQt
    KanoopPiQt
)

target_compile_options(${PROJ} PRIVATE -Wextra -Wall -Werror)

install(TARGETS ${PROJ} RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
```

`IrrigationD/tests/CMakeLists.txt`:

```cmake
find_package(Qt6 REQUIRED COMPONENTS Test)

function(irrigation_add_test name)
    qt_add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_link_libraries(${name} PRIVATE
        Qt6::Core Qt6::Network Qt6::HttpServer Qt6::Sql Qt6::Test
        KanoopCommonQt KanoopDatabaseQt KanoopPiQt
    )
    target_compile_options(${name} PRIVATE -Wextra -Wall -Werror)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

irrigation_add_test(tst_settings
    tst_settings.cpp
    ../src/irrigationsettings.cpp
)
```

Each test target compiles the sources it needs directly rather than linking a library, because `irrigationd` is an executable. Later tasks extend this file with one `irrigation_add_test` call each.

Add to the superproject `CMakeLists.txt`, after the three `add_subdirectory` lines:

```cmake
add_subdirectory(IrrigationD)
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
cd ~/src/punak/irrigation
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --target tst_settings -j 32
```

Expected: failure — `irrigationsettings.h` does not exist.

- [ ] **Step 4: Write IrrigationSettings**

`IrrigationD/src/irrigationsettings.h`:

```cpp
#ifndef IRRIGATIONSETTINGS_H
#define IRRIGATIONSETTINGS_H

#include <QMap>
#include <QSettings>
#include <QString>

/**
 * @brief INI-backed daemon configuration.
 *
 * The zone-to-GPIO map lives here rather than in the database because it
 * describes the wiring of one particular box; changing it must not require a
 * schema migration.
 */
class IrrigationSettings
{
public:
    /** @brief Constructs settings backed by the INI file at @p path. */
    explicit IrrigationSettings(const QString& path);

    /** @brief Returns the zone number to GPIO line offset map. Malformed entries are dropped. */
    QMap<int, quint32> zoneGpioMap() const;

    /** @brief Returns the kernel label of the GPIO chip to open. */
    QString chipLabel() const;

    /** @brief Returns true when driving a zone line active energises a low-trigger relay. */
    bool zoneActiveLow() const;

    /** @brief Returns the GPIO line offset of the stop button. */
    quint32 stopButtonOffset() const;

    /** @brief Returns the ceiling applied to any requested zone duration, in seconds. */
    int maxZoneSeconds() const;

    /** @brief Returns the address the control server binds to. */
    QString bindAddress() const;

    /** @brief Returns the port the control server listens on. */
    int listenPort() const;

    /** @brief Returns the path of the SQLite database file. */
    QString databasePath() const;

private:
    mutable QSettings _settings;
};

#endif // IRRIGATIONSETTINGS_H
```

Not an `AppSettings` subclass. `AppSettings` constructs its own `QSettings` from the organisation and application name, which makes it untestable against a temporary file. This class holds a `QSettings` built from an explicit path instead. `AppSettings` remains the right base for anything that wants the recent-files and last-directory machinery; the daemon wants none of it.

`IrrigationD/src/irrigationsettings.cpp`:

```cpp
#include "irrigationsettings.h"

IrrigationSettings::IrrigationSettings(const QString& path) :
    _settings(path, QSettings::IniFormat)
{
}

QMap<int, quint32> IrrigationSettings::zoneGpioMap() const
{
    QMap<int, quint32> result;

    QString raw = _settings.value("gpio/zones").toString();
    const QStringList pairs = raw.split(',', Qt::SkipEmptyParts);
    for(const QString& pair : pairs) {
        QStringList parts = pair.split('=');
        if(parts.count() != 2) {
            continue;
        }

        bool zoneOk = false;
        bool offsetOk = false;
        int zone = parts.at(0).trimmed().toInt(&zoneOk);
        uint offset = parts.at(1).trimmed().toUInt(&offsetOk);
        if(zoneOk == false || offsetOk == false) {
            continue;
        }

        result.insert(zone, offset);
    }

    return result;
}

QString IrrigationSettings::chipLabel() const
{
    return _settings.value("gpio/chipLabel", "pinctrl-bcm2711").toString();
}

bool IrrigationSettings::zoneActiveLow() const
{
    return _settings.value("gpio/zoneActiveLow", true).toBool();
}

quint32 IrrigationSettings::stopButtonOffset() const
{
    return _settings.value("gpio/stopButtonOffset", 25).toUInt();
}

int IrrigationSettings::maxZoneSeconds() const
{
    return _settings.value("limits/maxZoneSeconds", 3600).toInt();
}

QString IrrigationSettings::bindAddress() const
{
    return _settings.value("server/bindAddress", "127.0.0.1").toString();
}

int IrrigationSettings::listenPort() const
{
    return _settings.value("server/listenPort", 8080).toInt();
}

QString IrrigationSettings::databasePath() const
{
    return _settings.value("database/path", "/var/lib/irrigationd/irrigation.db").toString();
}
```

- [ ] **Step 5: Run the settings test**

```bash
cmake --build build --target tst_settings -j 32 && ./build/IrrigationD/tests/tst_settings
```

Expected: PASS, three cases.

- [ ] **Step 6: Write IrrigationDaemon as a shell**

`IrrigationD/src/irrigationdaemon.h`:

```cpp
#ifndef IRRIGATIONDAEMON_H
#define IRRIGATIONDAEMON_H

#include <QObject>

#include <Kanoop/utility/loggingbaseclass.h>

#include "irrigationsettings.h"

/**
 * @brief Owns the lifetime of every daemon component.
 *
 * @warning Construction order is a hardware contract, not a preference.
 *          ZoneController is constructed and drives all eight lines
 *          de-energised before the scheduler or the HTTP server exist, so
 *          nothing can ask for water before the valves are known shut.
 */
class IrrigationDaemon : public QObject,
                         public LoggingBaseClass
{
    Q_OBJECT
public:
    /** @brief Constructs the daemon against the settings at @p settingsPath. */
    explicit IrrigationDaemon(const QString& settingsPath, QObject* parent = nullptr);

    /** @brief Destructor. Stops anything still running. */
    virtual ~IrrigationDaemon();

    /** @brief Brings up every component in order. @return True when the daemon is serving. */
    bool start();

    /** @brief Tears every component down in reverse order. */
    void stop();

    /** @brief Returns the text of the most recent failure. */
    QString errorText() const { return _errorText; }

private:
    IrrigationSettings _settings;
    QString _errorText;
    bool _running = false;
};

#endif // IRRIGATIONDAEMON_H
```

`IrrigationD/src/irrigationdaemon.cpp`:

```cpp
#include "irrigationdaemon.h"

IrrigationDaemon::IrrigationDaemon(const QString& settingsPath, QObject* parent) :
    QObject(parent),
    LoggingBaseClass("daemon"),
    _settings(settingsPath)
{
}

IrrigationDaemon::~IrrigationDaemon()
{
    stop();
}

bool IrrigationDaemon::start()
{
    logText(LVL_INFO, "Starting irrigationd");
    _running = true;
    return true;
}

void IrrigationDaemon::stop()
{
    if(_running == false) {
        return;
    }

    logText(LVL_INFO, "Stopping irrigationd");
    _running = false;
}

#include "moc_irrigationdaemon.cpp"
```

Later tasks fill `start()` and `stop()` in. The shell exists now so `main.cpp` has something to construct.

- [ ] **Step 7: Write main.cpp**

```cpp
#include <csignal>
#include <unistd.h>

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QSocketNotifier>

#include <Kanoop/log.h>

#include "irrigationdaemon.h"

namespace
{
    int signalFds[2] = { -1, -1 };

    void posixSignalHandler(int)
    {
        char byte = 1;
        ssize_t written = ::write(signalFds[0], &byte, sizeof(byte));
        Q_UNUSED(written)
    }
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("irrigationd");
    QCoreApplication::setApplicationVersion("1.0.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("Irrigation controller daemon");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption configOption(
        QStringList() << "c" << "config",
        "Path to the INI configuration file.",
        "path",
        "/etc/irrigationd.ini");
    parser.addOption(configOption);

    QCommandLineOption verboseOption(
        QStringList() << "v" << "verbose",
        "Log at debug level.");
    parser.addOption(verboseOption);

    parser.process(app);

    Log::setFlags(Log::Standard);
    Log::setLevel(parser.isSet(verboseOption) ? Log::LogLevel::Debug : Log::LogLevel::Info);
    Log::systemLog()->openLog();

    if(::socketpair(AF_UNIX, SOCK_STREAM, 0, signalFds) != 0) {
        Log::logText(LVL_ERROR, "Failed to create signal socketpair");
        return 1;
    }

    QSocketNotifier notifier(signalFds[1], QSocketNotifier::Read);
    QObject::connect(&notifier, &QSocketNotifier::activated, &app, [&notifier, &app]()
    {
        notifier.setEnabled(false);
        char byte = 0;
        ssize_t got = ::read(signalFds[1], &byte, sizeof(byte));
        Q_UNUSED(got)
        Log::logText(LVL_INFO, "Signal received, shutting down");
        app.quit();
    });

    ::signal(SIGINT, posixSignalHandler);
    ::signal(SIGTERM, posixSignalHandler);

    IrrigationDaemon daemon(parser.value(configOption));
    if(daemon.start() == false) {
        Log::logText(LVL_ERROR, QString("Failed to start: %1").arg(daemon.errorText()));
        return 1;
    }

    int result = app.exec();
    daemon.stop();
    return result;
}
```

⚠ The signal handler writes one byte to a socketpair and returns. Everything else happens on the event loop. A POSIX signal handler may call almost nothing — `write()` is on the async-signal-safe list, `QCoreApplication::quit()` is emphatically not, and calling Qt from a handler produces a deadlock that reproduces once a month.

Add `#include <sys/socket.h>` for `socketpair`.

The logging API, verified against `KanoopCommonQt/include/Kanoop/log.h` and
`loggingtypes.h`:

- `LVL_INFO` is a macro expanding to `__FILE__, __LINE__, Log::LogLevel::Info`, so
  `Log::logText(LVL_INFO, text)` fills the first three parameters of
  `Log::logText(const char*, int, LogLevel, const QString&)`. There is no overload
  taking a category name as the first argument.
- `Log::Standard` is `LineNumbers | Timestamp | Level | Console`.
- Classes deriving `LoggingBaseClass` call the protected member `logText(LVL_INFO, text)`
  instead, which tags the message with the category passed to the constructor.

Add `src/main.cpp` to the executable's source list if it is not already there.

- [ ] **Step 8: Build and run the binary**

```bash
cmake --build build --target irrigationd -j 32
./build/IrrigationD/irrigationd --help
./build/IrrigationD/irrigationd -c /tmp/absent.ini &
sleep 1
kill -TERM %1
wait
```

Expected: `--help` prints the options; the run logs "Starting irrigationd", then on SIGTERM logs "Signal received, shutting down" and "Stopping irrigationd", and exits 0.

- [ ] **Step 9: Commit**

```bash
git add IrrigationD CMakeLists.txt
git commit -m "feat: add the irrigationd skeleton, settings and signal handling"
```

---

### Task 3: Database schema, migrations and data source

Produces a database that creates itself from scratch, carries its own version, and migrates forward. Every later task stores something, so this comes before all of them.

**Files:**
- Create: `IrrigationD/src/database/irrigationdatasource.h`, `.cpp`
- Create: `IrrigationD/src/database/schema.sql`
- Create: `IrrigationD/src/database/migrate/irrigation/1.0.0/01-initial.sql`
- Create: `IrrigationD/src/database/irrigation.qrc`
- Create: `IrrigationD/tests/tst_datasource.cpp`
- Modify: `IrrigationD/CMakeLists.txt`, `IrrigationD/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `DataSource` from `KanoopDatabaseQt` (`openConnection()`, `closeConnection()`, `executeQuery()`, `prepareQuery()`, `setConnectionName()`, `setCredentials()`, `errorText()`)
- Produces: `class IrrigationDataSource : public DataSource` with `bool open()`, `QString compiledDatabaseVersion() const`, `QString migrationScriptDirectory() const`

⚠ `EpcDataSource` lives in `libEpcCommonQt`, which this project does not consume. Its migration flow is the *reference*, reimplemented here against `DataSource` directly. Read `meta-qt-mains/.claude/docs/db-migration-architecture.md` before starting — particularly the catastrophic-failure path.

- [ ] **Step 1: Write the schema**

`IrrigationD/src/database/schema.sql`:

```sql
CREATE TABLE info (
    id                      INTEGER PRIMARY KEY,
    sw_version              TEXT NOT NULL
);

CREATE TABLE zones (
    id                      INTEGER PRIMARY KEY AUTOINCREMENT,
    number                  INTEGER NOT NULL UNIQUE,
    name                    TEXT NOT NULL DEFAULT '',
    enabled                 INTEGER NOT NULL DEFAULT 1
);

CREATE TABLE programs (
    id                      INTEGER PRIMARY KEY AUTOINCREMENT,
    name                    TEXT NOT NULL,
    enabled                 INTEGER NOT NULL DEFAULT 1,
    day_mode                TEXT NOT NULL DEFAULT 'DaysOfWeek',
    dow_mask                INTEGER NOT NULL DEFAULT 0,
    interval_days           INTEGER,
    anchor_date             TEXT
);

CREATE TABLE program_start_times (
    id                      INTEGER PRIMARY KEY AUTOINCREMENT,
    program_id              INTEGER NOT NULL,
    minutes_after_midnight  INTEGER NOT NULL,
    timezone                TEXT NOT NULL,
    FOREIGN KEY (program_id) REFERENCES programs(id) ON DELETE CASCADE
);

CREATE TABLE program_zones (
    id                      INTEGER PRIMARY KEY AUTOINCREMENT,
    program_id              INTEGER NOT NULL,
    zone_id                 INTEGER NOT NULL,
    sequence                INTEGER NOT NULL,
    duration_seconds        INTEGER NOT NULL,
    FOREIGN KEY (program_id) REFERENCES programs(id) ON DELETE CASCADE,
    FOREIGN KEY (zone_id)    REFERENCES zones(id)    ON DELETE CASCADE
);

CREATE TABLE fired_instants (
    id                      INTEGER PRIMARY KEY AUTOINCREMENT,
    program_id              INTEGER NOT NULL,
    start_time_id           INTEGER NOT NULL,
    scheduled_at_utc        TEXT NOT NULL,
    outcome                 TEXT NOT NULL,
    UNIQUE (program_id, start_time_id, scheduled_at_utc)
);

CREATE TABLE settings (
    key                     TEXT PRIMARY KEY,
    value                   TEXT NOT NULL
);

CREATE INDEX idx_program_zones_program  ON program_zones(program_id, sequence);
CREATE INDEX idx_start_times_program    ON program_start_times(program_id);
CREATE INDEX idx_fired_scheduled        ON fired_instants(scheduled_at_utc);

INSERT INTO zones (number, name, enabled) VALUES
    (1, 'Zone 1', 1), (2, 'Zone 2', 1), (3, 'Zone 3', 1), (4, 'Zone 4', 1),
    (5, 'Zone 5', 1), (6, 'Zone 6', 1), (7, 'Zone 7', 1), (8, 'Zone 8', 1);

INSERT INTO settings (key, value) VALUES
    ('rain_delay_until', ''),
    ('master_enabled',   '1'),
    ('max_zone_seconds', '3600'),
    ('log_level',        'info');
```

The `UNIQUE (program_id, start_time_id, scheduled_at_utc)` constraint is what makes firing idempotent across restarts and backward clock steps. It is the single most important line in this file: without it a daemon restarted at the wrong moment re-runs a program that already ran.

Copy the same content to `migrate/irrigation/1.0.0/01-initial.sql`, minus the `INSERT INTO info` handling — the migration framework writes the version itself.

- [ ] **Step 2: Write the resource file**

`IrrigationD/src/database/irrigation.qrc`:

```xml
<RCC>
    <qresource prefix="/database">
        <file>schema.sql</file>
        <file>migrate/irrigation/1.0.0/01-initial.sql</file>
    </qresource>
</RCC>
```

⚠ Every new migration script must be added here. A script that is not in the `.qrc` is not packaged, and `migrate()` skips it **silently** — you get a database that looks healthy until the first query against the missing column.

- [ ] **Step 3: Write the failing data source test**

`IrrigationD/tests/tst_datasource.cpp`:

```cpp
#include <QTest>
#include <QTemporaryDir>
#include <QSqlQuery>
#include <QSqlError>

#include "database/irrigationdatasource.h"

class TestDataSource : public QObject
{
    Q_OBJECT
private slots:
    void createsSchemaOnFirstOpen();
    void stampsTheCompiledVersion();
    void seedsEightZones();
    void reopenDoesNotRecreate();
    void firedInstantsRejectDuplicates();
};

void TestDataSource::createsSchemaOnFirstOpen()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY2(source.open(), qPrintable(source.errorText()));

    QSqlQuery query(QSqlDatabase::database(source.connectionName()));
    QVERIFY(query.exec("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"));

    QStringList tables;
    while(query.next()) {
        tables.append(query.value(0).toString());
    }

    QVERIFY(tables.contains("zones"));
    QVERIFY(tables.contains("programs"));
    QVERIFY(tables.contains("program_start_times"));
    QVERIFY(tables.contains("program_zones"));
    QVERIFY(tables.contains("fired_instants"));
    QVERIFY(tables.contains("settings"));
    QVERIFY(tables.contains("info"));
}

void TestDataSource::stampsTheCompiledVersion()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    QSqlQuery query(QSqlDatabase::database(source.connectionName()));
    QVERIFY(query.exec("SELECT sw_version FROM info WHERE id = 1"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), source.compiledDatabaseVersion());
}

void TestDataSource::seedsEightZones()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    QSqlQuery query(QSqlDatabase::database(source.connectionName()));
    QVERIFY(query.exec("SELECT COUNT(*) FROM zones"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 8);
}

void TestDataSource::reopenDoesNotRecreate()
{
    QTemporaryDir dir;
    QString path = dir.filePath("irrigation.db");

    {
        IrrigationDataSource source(path);
        QVERIFY(source.open());
        QSqlQuery query(QSqlDatabase::database(source.connectionName()));
        QVERIFY(query.exec("UPDATE zones SET name = 'Front lawn' WHERE number = 1"));
    }

    IrrigationDataSource source(path);
    QVERIFY(source.open());
    QSqlQuery query(QSqlDatabase::database(source.connectionName()));
    QVERIFY(query.exec("SELECT name FROM zones WHERE number = 1"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), QString("Front lawn"));
}

void TestDataSource::firedInstantsRejectDuplicates()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    QSqlQuery query(QSqlDatabase::database(source.connectionName()));
    QVERIFY(query.exec("INSERT INTO fired_instants (program_id, start_time_id, scheduled_at_utc, outcome) "
                       "VALUES (1, 1, '2026-09-12T13:00:00Z', 'ran')"));
    QVERIFY(query.exec("INSERT INTO fired_instants (program_id, start_time_id, scheduled_at_utc, outcome) "
                       "VALUES (1, 1, '2026-09-12T13:00:00Z', 'ran')") == false);
}

QTEST_MAIN(TestDataSource)
#include "tst_datasource.moc"
```

`reopenDoesNotRecreate` is the discrimination test for the whole task. Delete the "does this file already exist" branch from the implementation and it fails, where every other test still passes.

- [ ] **Step 4: Run it and verify it fails**

```bash
cmake --build build --target tst_datasource -j 32
```

Expected: failure — `database/irrigationdatasource.h` does not exist.

- [ ] **Step 5: Write IrrigationDataSource**

`DataSource::openConnection()` already does most of this. Read it before writing
anything — `KanoopDatabaseQt/src/database/datasource.cpp`. It checks driver
availability, generates a connection name, creates the directory, calls
`createSqliteDatabase()` when the file is absent and `createOnOpenFailure()` is
set, opens, enables foreign keys, and then calls the virtual `migrate()`. Three
extension points are all this class needs to override:

| Virtual | Purpose |
|---|---|
| `QString createSql() const` | Full create-from-scratch SQL. Called only when the file does not exist. |
| `bool executePostCreateScripts()` | Runs after creation. Stamps the compiled version into `info`. |
| `bool migrate()` | Runs on every open. Walks the version directories. |

`IrrigationD/src/database/irrigationdatasource.h`:

```cpp
#ifndef IRRIGATIONDATASOURCE_H
#define IRRIGATIONDATASOURCE_H

#include <QVersionNumber>

#include <Kanoop/database/datasource.h>

/**
 * @brief SQLite data source for the irrigation controller.
 *
 * Creates the schema from :/database/schema.sql on first open and runs any
 * pending scripts under :/database/migrate/irrigation on every subsequent open.
 *
 * @warning A migration that fails renames the database to
 *          <file>.<utc>.backup and recreates it empty. Every migration must be
 *          tested against a populated database before it ships; there is no
 *          second chance and the user is not told.
 */
class IrrigationDataSource : public DataSource
{
    Q_OBJECT
public:
    /** @brief Constructs a data source for the SQLite file at @p path. */
    explicit IrrigationDataSource(const QString& path);

    /** @brief Opens the database, creating or migrating it as required. @return True on success. */
    bool open() { return openConnection(); }

    /** @brief Returns the schema version compiled into this binary. */
    QString compiledDatabaseVersion() const { return QT_STRINGIFY(IRRIGATION_DB_VERSION); }

    /** @brief Returns the Qt resource path holding the migration script directories. */
    QString migrationScriptDirectory() const { return ":/database/migrate/irrigation"; }

protected:
    /** @brief Returns the full create-from-scratch schema. */
    virtual QString createSql() const override;

    /** @brief Stamps the compiled version into the info table. @return True on success. */
    virtual bool executePostCreateScripts() override;

    /** @brief Applies every migration between the stored and compiled versions. @return True on success. */
    virtual bool migrate() override;

private:
    bool applyDurabilityPragmas();
    bool readStoredVersion(QVersionNumber& version);
    bool writeStoredVersion(const QVersionNumber& version);
    bool applyScriptResource(const QString& resourcePath);
    bool recreateAndReopen(const QString& reason);

    static QString readResource(const QString& path);
    static QList<QVersionNumber> sortedMigrationVersions(const QString& directory);

    QString _path;
};

#endif // IRRIGATIONDATASOURCE_H
```

`QVersionNumber` is Qt's own. `SemanticVersion`, which the EPC reference uses,
lives in `libEpcCommonQt` and this project does not link it.

`IrrigationD/src/database/irrigationdatasource.cpp`:

```cpp
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>

#include <Kanoop/database/sqlparser.h>

#include "irrigationdatasource.h"

IrrigationDataSource::IrrigationDataSource(const QString& path) :
    DataSource(DatabaseCredentials(path)),
    _path(path)
{
    // The single-argument DatabaseCredentials constructor sets the engine to
    // SQLENG_SQLITE and the schema to the file path.
    setCreateOnOpenFailure(true);
}

QString IrrigationDataSource::readResource(const QString& path)
{
    QFile file(path);
    if(file.open(QIODevice::ReadOnly) == false) {
        return QString();
    }
    QString result = QString::fromUtf8(file.readAll());
    file.close();
    return result;
}

QString IrrigationDataSource::createSql() const
{
    return readResource(":/database/schema.sql");
}

bool IrrigationDataSource::executePostCreateScripts()
{
    return applyDurabilityPragmas() && writeStoredVersion(QVersionNumber::fromString(compiledDatabaseVersion()));
}

bool IrrigationDataSource::applyDurabilityPragmas()
{
    // The device runs from an unswitched outdoor outlet and will lose power with a
    // transaction open. WAL plus synchronous=FULL is what survives that.
    const QStringList pragmas = {
        "PRAGMA journal_mode=WAL",
        "PRAGMA synchronous=FULL"
    };

    return executeMultiple(pragmas);
}

bool IrrigationDataSource::readStoredVersion(QVersionNumber& version)
{
    bool success = false;
    QSqlQuery query = executeQuery("SELECT sw_version FROM info WHERE id = 1", &success);
    if(success == false || query.next() == false) {
        return false;
    }

    version = QVersionNumber::fromString(query.value(0).toString());
    return version.isNull() == false;
}

bool IrrigationDataSource::writeStoredVersion(const QVersionNumber& version)
{
    bool success = false;
    QSqlQuery query = prepareQuery(
        "INSERT INTO info (id, sw_version) VALUES (1, :version) "
        "ON CONFLICT(id) DO UPDATE SET sw_version = excluded.sw_version",
        &success);
    if(success == false) {
        return false;
    }

    query.bindValue(":version", version.toString());
    return executeQuery(query);
}

bool IrrigationDataSource::applyScriptResource(const QString& resourcePath)
{
    const QString sql = readResource(resourcePath);
    if(sql.isEmpty()) {
        return false;
    }

    SqlParser parser(sql);
    if(parser.isValid() == false) {
        return false;
    }

    return executeMultiple(parser.statements());
}

QList<QVersionNumber> IrrigationDataSource::sortedMigrationVersions(const QString& directory)
{
    QList<QVersionNumber> result;

    const QStringList entries = QDir(directory).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for(const QString& entry : entries) {
        QVersionNumber version = QVersionNumber::fromString(entry);
        if(version.isNull() == false) {
            result.append(version);
        }
    }

    std::sort(result.begin(), result.end());
    return result;
}

bool IrrigationDataSource::migrate()
{
    applyDurabilityPragmas();

    QVersionNumber stored;
    if(readStoredVersion(stored) == false) {
        // A database predating the info table. Treat it as version zero so every
        // migration runs.
        executeMultiple({ "CREATE TABLE IF NOT EXISTS info (id INTEGER PRIMARY KEY, sw_version TEXT NOT NULL)" });
        stored = QVersionNumber(0, 0, 0);
        writeStoredVersion(stored);
    }

    const QVersionNumber compiled = QVersionNumber::fromString(compiledDatabaseVersion());
    const QList<QVersionNumber> versions = sortedMigrationVersions(migrationScriptDirectory());

    int applied = 0;
    for(const QVersionNumber& version : versions) {
        if(version <= stored || version > compiled) {
            continue;
        }

        const QString directory = QString("%1/%2").arg(migrationScriptDirectory(), version.toString());
        const QStringList scripts = QDir(directory).entryList(QDir::Files, QDir::Name);
        for(const QString& script : scripts) {
            const QString resource = QString("%1/%2").arg(directory, script);
            if(applyScriptResource(resource) == false) {
                return recreateAndReopen(QString("migration %1 failed").arg(resource));
            }
            applied++;
        }
    }

    if(applied > 0) {
        return writeStoredVersion(compiled);
    }

    return true;
}

bool IrrigationDataSource::recreateAndReopen(const QString& reason)
{
    logText(LVL_ERROR, QString("%1 - recreating the database").arg(reason));

    closeConnection();

    const QString backup = QString("%1.%2.backup")
                               .arg(_path, QDateTime::currentDateTimeUtc().toString("yyyyMMddHHmmss"));
    if(QFile::rename(_path, backup) == false) {
        logText(LVL_ERROR, QString("Failed to move the database aside to %1").arg(backup));
        return false;
    }

    logText(LVL_WARNING, QString("Previous database preserved at %1").arg(backup));

    // openConnection() sees an absent file and recreates from createSql(), then
    // calls this method again. The recursion terminates because the fresh database
    // is stamped at the compiled version and no migration is selected.
    return openConnection();
}

#include "moc_irrigationdatasource.cpp"
```

⚠ `recreateAndReopen()` calls `openConnection()`, which calls `migrate()`, which
can call `recreateAndReopen()`. It terminates because the recreated file is
stamped at the compiled version by `executePostCreateScripts()`, so the version
walk selects nothing. **Any change that lets a fresh database select a migration
turns this into infinite recursion that eats the user's data once per loop.**
If you change the version comparison, add a guard flag.

- [ ] **Step 6: Wire the sources into the build**

Add to the `qt_add_executable` list in `IrrigationD/CMakeLists.txt`:

```cmake
    src/database/irrigationdatasource.h  src/database/irrigationdatasource.cpp
    src/database/irrigation.qrc
```

Add to `IrrigationD/tests/CMakeLists.txt`:

```cmake
irrigation_add_test(tst_datasource
    tst_datasource.cpp
    ../src/database/irrigationdatasource.cpp
    ../src/database/irrigation.qrc
)
```

- [ ] **Step 7: Run the data source tests**

```bash
cmake --build build --target tst_datasource -j 32 && ./build/IrrigationD/tests/tst_datasource
```

Expected: PASS, five cases.

⚠ If every test fails with `QSqlDatabase: QSQLITE driver not loaded`, qtbase was built without the SQLite driver. On the development host install `qt6-qtbase-mysql`/`sqlite` packages; in Yocto add `PACKAGECONFIG:append:pn-qtbase = " sql-sqlite"`. The SDK at `/opt/poky/5.2.4` was verified to carry `plugins/sqldrivers/libqsqlite.so`, so a cross build is fine.

- [ ] **Step 8: Commit**

```bash
git add IrrigationD
git commit -m "feat: add the irrigation schema, migrations and data source"
```

---

### Task 4: Domain models and repository methods

Value types for every table plus the queries that load and store them. Tasks 6, 7 and 9 all consume these.

**Files:**
- Create: `IrrigationD/src/model/zone.h`, `.cpp`
- Create: `IrrigationD/src/model/program.h`, `.cpp`
- Create: `IrrigationD/src/model/programstarttime.h`, `.cpp`
- Create: `IrrigationD/src/model/programzone.h`, `.cpp`
- Create: `IrrigationD/src/model/firedinstant.h`, `.cpp`
- Modify: `IrrigationD/src/database/irrigationdatasource.h`, `.cpp`
- Create: `IrrigationD/tests/tst_repository.cpp`

**Interfaces:**
- Produces:
  - `class Zone` — `int id`, `int number`, `QString name`, `bool enabled`; `ZoneList` is `QList<Zone>`
  - `class Program` — `int id`, `QString name`, `bool enabled`, `Program::DayMode dayMode`, `int dowMask`, `int intervalDays`, `QDate anchorDate`; `ProgramList`
  - `class ProgramStartTime` — `int id`, `int programId`, `int minutesAfterMidnight`, `QString timezone`; `ProgramStartTimeList`
  - `class ProgramZone` — `int id`, `int programId`, `int zoneId`, `int sequence`, `int durationSeconds`; `ProgramZoneList`
  - `class FiredInstant` — `int id`, `int programId`, `int startTimeId`, `QDateTime scheduledAtUtc`, `FiredInstant::Outcome outcome`
  - On `IrrigationDataSource`:
    - `ZoneList allZones()`
    - `bool updateZone(const Zone& zone)`
    - `ProgramList enabledPrograms()`
    - `ProgramList allPrograms()`
    - `bool insertProgram(Program& program)` / `bool updateProgram(const Program& program)` / `bool deleteProgram(int programId)`
    - `ProgramStartTimeList startTimesFor(int programId)`
    - `bool insertStartTime(ProgramStartTime& startTime)` — assigns `startTime.id` on success
    - `ProgramZoneList zonesFor(int programId)` — **ordered by `sequence`**, not by `id`
    - `bool insertProgramZone(ProgramZone& programZone)` — assigns `programZone.id` on success
    - `bool recordFiring(const FiredInstant& instant)`
    - `bool hasFired(int programId, int startTimeId, const QDateTime& scheduledAtUtc)`
    - `bool pruneFiredInstantsOlderThan(const QDateTime& cutoffUtc)`
    - `QString settingValue(const QString& key)` / `bool setSettingValue(const QString& key, const QString& value)`
    - `QSqlQuery rawQuery(const QString& sql, bool* ok)` — public passthrough to the protected `executeQuery`, so tests can assert on row counts without befriending the class

  Both insert methods take a non-const reference and write the generated primary
  key back into it. Tasks 6, 7 and 10 all build fixtures by inserting a program,
  then its start times and zones keyed on the id they just received; an insert
  that discarded the id would make every one of those fixtures reference row 0.

- [ ] **Step 1: Write the model headers**

Each model is a plain value type with in-class initializers and no Qt parent. `IrrigationD/src/model/program.h` is the only one carrying enums:

```cpp
#ifndef PROGRAM_H
#define PROGRAM_H

#include <QDate>
#include <QList>
#include <QString>

/** @brief A watering program: a day rule, a set of start times and an ordered zone list. */
class Program
{
public:
    /** @brief How the program decides whether today is a watering day. */
    enum class DayMode
    {
        DaysOfWeek,
        Odd,
        Even,
        EveryNDays
    };

    /** @brief Parses @p value into a DayMode. Returns DaysOfWeek when unrecognised. */
    static DayMode dayModeFromString(const QString& value);

    /** @brief Returns the storage string for @p value. */
    static QString dayModeToString(DayMode value);

    int id = 0;
    QString name;
    bool enabled = true;
    DayMode dayMode = DayMode::DaysOfWeek;
    int dowMask = 0;
    int intervalDays = 0;
    QDate anchorDate;

    /** @brief Returns true when this program came from the database. */
    bool isValid() const { return id > 0; }
};

typedef QList<Program> ProgramList;

#endif // PROGRAM_H
```

`dowMask` is a bitmask where bit 0 is Monday, matching `QDate::dayOfWeek()` minus one. State that in the Doxygen, because Sunday-first is the other plausible reading and picking the wrong one waters on the wrong days without ever failing a test that does not check a specific weekday.

`FiredInstant::Outcome` is `Ran`, `SkippedBusy`, `SkippedRain`, `Missed`, with `outcomeToString` / `outcomeFromString` producing `ran`, `skipped_busy`, `skipped_rain`, `missed` exactly as the schema's CHECK-free text column expects.

- [ ] **Step 2: Write the failing repository test**

`IrrigationD/tests/tst_repository.cpp`. The cases that matter:

```cpp
void TestRepository::recordFiringIsIdempotent()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program program;
    program.name = "Morning";
    QVERIFY(source.insertProgram(program));
    QVERIFY(program.id > 0);

    FiredInstant instant;
    instant.programId = program.id;
    instant.startTimeId = 1;
    instant.scheduledAtUtc = QDateTime(QDate(2026, 9, 12), QTime(13, 0), QTimeZone::UTC);
    instant.outcome = FiredInstant::Outcome::Ran;

    QVERIFY(source.hasFired(instant.programId, instant.startTimeId, instant.scheduledAtUtc) == false);
    QVERIFY(source.recordFiring(instant));
    QVERIFY(source.hasFired(instant.programId, instant.startTimeId, instant.scheduledAtUtc));

    // A second record of the same instant must not create a second row.
    source.recordFiring(instant);

    bool ok = false;
    QSqlQuery query = source.rawQuery("SELECT COUNT(*) FROM fired_instants", &ok);
    QVERIFY(ok);
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 1);
}

void TestRepository::insertProgramAssignsTheId()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program program;
    program.name = "Evening";
    program.dayMode = Program::DayMode::EveryNDays;
    program.intervalDays = 3;
    program.anchorDate = QDate(2026, 4, 1);

    QVERIFY(source.insertProgram(program));
    QVERIFY(program.id > 0);

    ProgramList all = source.allPrograms();
    QCOMPARE(all.count(), 1);
    QCOMPARE(all.first().name, QString("Evening"));
    QCOMPARE(all.first().intervalDays, 3);
    QCOMPARE(all.first().anchorDate, QDate(2026, 4, 1));
}

void TestRepository::deleteProgramCascades()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program program;
    program.name = "Doomed";
    QVERIFY(source.insertProgram(program));

    ProgramStartTime start;
    start.programId = program.id;
    start.minutesAfterMidnight = 360;
    start.timezone = "America/Los_Angeles";
    QVERIFY(source.insertStartTime(start));

    QCOMPARE(source.startTimesFor(program.id).count(), 1);
    QVERIFY(source.deleteProgram(program.id));
    QCOMPARE(source.startTimesFor(program.id).count(), 0);
}

void TestRepository::prunePreservesRecentRows()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program program;
    program.name = "Morning";
    QVERIFY(source.insertProgram(program));

    FiredInstant old;
    old.programId = program.id;
    old.startTimeId = 1;
    old.scheduledAtUtc = QDateTime(QDate(2026, 1, 1), QTime(6, 0), QTimeZone::UTC);
    old.outcome = FiredInstant::Outcome::Ran;
    QVERIFY(source.recordFiring(old));

    FiredInstant recent = old;
    recent.startTimeId = 2;
    recent.scheduledAtUtc = QDateTime(QDate(2026, 9, 1), QTime(6, 0), QTimeZone::UTC);
    QVERIFY(source.recordFiring(recent));

    QVERIFY(source.pruneFiredInstantsOlderThan(QDateTime(QDate(2026, 6, 1), QTime(0, 0), QTimeZone::UTC)));

    QVERIFY(source.hasFired(program.id, 1, old.scheduledAtUtc) == false);
    QVERIFY(source.hasFired(program.id, 2, recent.scheduledAtUtc));
}
```

`deleteProgramCascades` is the discrimination test for `PRAGMA foreign_keys=ON`. `DataSource::openConnection()` enables it for SQLite; delete that call and only this test fails.

- [ ] **Step 3: Run it and verify it fails**

```bash
cmake --build build --target tst_repository -j 32
```

Expected: failure — the model headers do not exist.

- [ ] **Step 4: Implement the models and repository methods**

Store every `QDateTime` as ISO-8601 UTC:

```cpp
bool IrrigationDataSource::recordFiring(const FiredInstant& instant)
{
    bool success = false;
    QSqlQuery query = prepareQuery(
        "INSERT OR IGNORE INTO fired_instants "
        "(program_id, start_time_id, scheduled_at_utc, outcome) "
        "VALUES (:programId, :startTimeId, :scheduledAt, :outcome)",
        &success);
    if(success == false) {
        return false;
    }

    query.bindValue(":programId",   instant.programId);
    query.bindValue(":startTimeId", instant.startTimeId);
    query.bindValue(":scheduledAt", instant.scheduledAtUtc.toUTC().toString(Qt::ISODate));
    query.bindValue(":outcome",     FiredInstant::outcomeToString(instant.outcome));

    return executeQuery(query);
}
```

`INSERT OR IGNORE` plus the `UNIQUE` constraint is what makes a re-fire a no-op rather than an error. Both halves are required: drop the constraint and duplicates insert; drop `OR IGNORE` and a legitimate retry after a crash returns failure.

`toString(Qt::ISODate)` on a UTC `QDateTime` produces `2026-09-12T13:00:00Z`, which compares correctly as text because the format is fixed-width and zero-padded. Every write must call `.toUTC()` first — a local-zone `QDateTime` serialises with an offset suffix and sorts wrong.

- [ ] **Step 5: Run the repository tests**

```bash
cmake --build build --target tst_repository -j 32 && ./build/IrrigationD/tests/tst_repository
```

Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add IrrigationD
git commit -m "feat: add the domain models and repository queries"
```

---

### Task 5: ZoneController

The only component that touches GPIO, and the one that decides whether your yard floods. Four invariants, each with its own test.

**Files:**
- Create: `IrrigationD/src/zonecontroller.h`, `.cpp`
- Create: `IrrigationD/tests/tst_zonecontroller.cpp`

**Interfaces:**
- Consumes: `IGpioBackend`, `OutputBank`, `Gpio` from `KanoopPiQt`; `IrrigationSettings` from Task 2; `OutputBank::readValues()` from Task 1
- Produces:
  - `ZoneController(IGpioBackend* backend, const QMap<int,quint32>& zoneGpioMap, bool activeLow, int maxZoneSeconds, QObject* parent = nullptr)`
  - `bool begin()` — requests the bank and drives everything inactive
  - `bool openZone(int zoneNumber, int seconds)`
  - `void allOff()`
  - `int openZoneNumber() const` — zero when none
  - `int secondsRemaining() const`
  - `void setWatchdogInterval(const TimeSpan& value)`
  - signals: `void zoneOpened(int zoneNumber, int seconds)`, `void zoneClosed(int zoneNumber)`, `void watchdogTripped(int zoneNumber)`

- [ ] **Step 1: Write the failing invariant tests**

```cpp
#include <QTest>
#include <QSignalSpy>

#include <Kanoop/pi/mockbackend.h>

#include "zonecontroller.h"

static QMap<int, quint32> eightZones()
{
    return { {1,5}, {2,6}, {3,13}, {4,16}, {5,19}, {6,20}, {7,21}, {8,26} };
}

class TestZoneController : public QObject
{
    Q_OBJECT
private slots:
    void beginDrivesEveryLineInactive();
    void openingAZoneClosesTheOpenOneAtomically();
    void durationIsClampedToTheCeiling();
    void durationBelowOneIsRejected();
    void zoneClosesWhenItsTimerExpires();
    void watchdogClosesAZonePastItsDeadline();
    void allOffClosesEverything();
    void unknownZoneNumberIsRejected();
};

void TestZoneController::beginDrivesEveryLineInactive()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QCOMPARE(controller.openZoneNumber(), 0);
    for(quint32 offset : eightZones().values()) {
        QCOMPARE(backend.lineValue(offset), Gpio::Value::Inactive);
    }
}

void TestZoneController::openingAZoneClosesTheOpenOneAtomically()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(3, 60));
    QCOMPARE(backend.lineValue(13), Gpio::Value::Active);

    backend.resetSetValuesCallCount();
    QVERIFY(controller.openZone(5, 60));

    QCOMPARE(backend.lineValue(13), Gpio::Value::Inactive);
    QCOMPARE(backend.lineValue(19), Gpio::Value::Active);

    // Both transitions in ONE write. Two writes means a window where both valves
    // are open, and on a 40 VA transformer that is a brownout.
    QCOMPARE(backend.setValuesCallCount(), 1);
}

void TestZoneController::durationIsClampedToTheCeiling()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 120);
    QVERIFY(controller.begin());

    QSignalSpy spy(&controller, &ZoneController::zoneOpened);
    QVERIFY(controller.openZone(1, 99999));

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(1).toInt(), 120);
    QVERIFY(controller.secondsRemaining() <= 120);
}

void TestZoneController::durationBelowOneIsRejected()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(1, 0) == false);
    QVERIFY(controller.openZone(1, -5) == false);
    QCOMPARE(controller.openZoneNumber(), 0);
}

void TestZoneController::zoneClosesWhenItsTimerExpires()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QSignalSpy spy(&controller, &ZoneController::zoneClosed);
    QVERIFY(controller.openZone(1, 1));

    QVERIFY(spy.wait(3000));
    QCOMPARE(spy.first().at(0).toInt(), 1);
    QCOMPARE(controller.openZoneNumber(), 0);
    QCOMPARE(backend.lineValue(5), Gpio::Value::Inactive);
}

void TestZoneController::watchdogClosesAZonePastItsDeadline()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    controller.setWatchdogInterval(TimeSpan::fromMilliseconds(100));
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(1, 1));

    // Simulate the close timer never firing: stop it behind the controller's back
    // and let the watchdog be the only thing that can save the zone.
    controller.disableCloseTimerForTest();

    QSignalSpy spy(&controller, &ZoneController::watchdogTripped);
    QVERIFY(spy.wait(5000));
    QCOMPARE(backend.lineValue(5), Gpio::Value::Inactive);
    QCOMPARE(controller.openZoneNumber(), 0);
}

void TestZoneController::allOffClosesEverything()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(7, 600));

    controller.allOff();

    QCOMPARE(controller.openZoneNumber(), 0);
    for(quint32 offset : eightZones().values()) {
        QCOMPARE(backend.lineValue(offset), Gpio::Value::Inactive);
    }
}

void TestZoneController::unknownZoneNumberIsRejected()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(0, 60) == false);
    QVERIFY(controller.openZone(9, 60) == false);
    QCOMPARE(controller.openZoneNumber(), 0);
}
```

`lineValue()`, `setValuesCallCount()` and `resetSetValuesCallCount()` all exist
on `MockBackend` after Task 1. Do not re-add them.

`disableCloseTimerForTest()` is a deliberate test seam on `ZoneController`. The
watchdog exists precisely for the case where the close path failed, and there is
no way to exercise it without breaking that path. Name it for what it is rather
than hiding it behind `#ifdef`.

- [ ] **Step 2: Run it and verify it fails**

```bash
cmake --build build --target tst_zonecontroller -j 32
```

Expected: failure — `zonecontroller.h` does not exist.

- [ ] **Step 3: Write the header**

```cpp
#ifndef ZONECONTROLLER_H
#define ZONECONTROLLER_H

#include <QDateTime>
#include <QMap>
#include <QObject>
#include <QTimer>

#include <Kanoop/timespan.h>
#include <Kanoop/utility/loggingbaseclass.h>
#include <Kanoop/pi/outputbank.h>

/**
 * @brief Sole owner of the valve outputs.
 *
 * Enforces four invariants regardless of caller:
 *  1. Mutual exclusion — opening a zone closes any open zone in the same write.
 *  2. No open without a deadline — there is no overload that opens indefinitely.
 *  3. Duration clamp — requests are clamped to the configured ceiling.
 *  4. Watchdog — a periodic tick reads the lines back and closes the bank if a
 *     zone is open past its deadline.
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
    /**
     * @brief Constructs a controller over @p zoneGpioMap.
     * @param backend The GPIO backend. Must already have an open chip.
     * @param zoneGpioMap Zone number to line offset.
     * @param activeLow Whether the valve lines are active-low.
     * @param maxZoneSeconds The ceiling applied to every requested duration.
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

    /** @brief Opens @p zoneNumber for @p seconds, closing any open zone. @return True on success. */
    bool openZone(int zoneNumber, int seconds);

    /** @brief Closes every zone. Callable from any component; always takes precedence. */
    void allOff();

    /** @brief Returns the open zone number, or zero when none is open. */
    int openZoneNumber() const { return _openZone; }

    /** @brief Returns the seconds remaining on the open zone, or zero. */
    int secondsRemaining() const;

    /** @brief Sets how often the watchdog verifies line state against the deadline. */
    void setWatchdogInterval(const TimeSpan& value);

    /** @brief Stops the close timer without closing the zone. Test seam for the watchdog. */
    void disableCloseTimerForTest() { _closeTimer.stop(); }

    /** @brief Runs the close path immediately. Test seam so sequences do not wait on wall time. */
    void expireCloseTimerForTest() { _closeTimer.stop(); onCloseTimer(); }

    /** @brief Returns the text of the most recent failure. */
    QString errorText() const { return _errorText; }

signals:
    /** @brief Emitted after @p zoneNumber has been driven active for @p seconds. */
    void zoneOpened(int zoneNumber, int seconds);

    /** @brief Emitted after @p zoneNumber has been driven inactive. */
    void zoneClosed(int zoneNumber);

    /** @brief Emitted when the watchdog found @p zoneNumber open past its deadline. */
    void watchdogTripped(int zoneNumber);

private slots:
    void onCloseTimer();
    void onWatchdogTimer();

private:
    bool writeExclusive(int zoneNumber);

    IGpioBackend* _backend = nullptr;
    QMap<int, quint32> _zoneGpioMap;
    int _maxZoneSeconds = 3600;
    OutputBank* _bank = nullptr;
    QTimer _closeTimer;
    QTimer _watchdogTimer;
    int _openZone = 0;
    QDateTime _deadlineUtc;
    QString _errorText;
};

#endif // ZONECONTROLLER_H
```

- [ ] **Step 4: Implement it**

The two methods that carry the invariants:

```cpp
bool ZoneController::writeExclusive(int zoneNumber)
{
    QMap<quint32, bool> values;
    for(auto it = _zoneGpioMap.constBegin(); it != _zoneGpioMap.constEnd(); ++it) {
        values.insert(it.value(), it.key() == zoneNumber);
    }

    if(_bank->setValues(values) == false) {
        _errorText = _bank->errorText();
        return false;
    }

    return true;
}
```

Every transition writes the whole bank. Passing zero closes everything. There is
no code path that sets one line without clearing the others, which is what makes
invariant 1 structural rather than a rule somebody has to remember.

```cpp
bool ZoneController::openZone(int zoneNumber, int seconds)
{
    if(_zoneGpioMap.contains(zoneNumber) == false) {
        _errorText = QString("Unknown zone %1").arg(zoneNumber);
        return false;
    }

    if(seconds < 1) {
        _errorText = QString("Duration %1 is not positive").arg(seconds);
        return false;
    }

    const int clamped = qMin(seconds, _maxZoneSeconds);
    const int previous = _openZone;

    if(writeExclusive(zoneNumber) == false) {
        return false;
    }

    _openZone = zoneNumber;
    _deadlineUtc = QDateTime::currentDateTimeUtc().addSecs(clamped);

    _closeTimer.setSingleShot(true);
    _closeTimer.start(clamped * 1000);

    if(previous != 0 && previous != zoneNumber) {
        emit zoneClosed(previous);
    }
    emit zoneOpened(zoneNumber, clamped);

    return true;
}
```

The deadline is set before the timer starts, so a watchdog tick that lands
between the two sees a consistent state.

```cpp
void ZoneController::onWatchdogTimer()
{
    QMap<quint32, bool> actual;
    if(_bank->readValues(actual) == false) {
        logText(LVL_ERROR, QString("Watchdog could not read the bank: %1").arg(_bank->errorText()));
        allOff();
        return;
    }

    int energised = 0;
    for(bool value : actual) {
        if(value) {
            energised++;
        }
    }

    const bool pastDeadline = _openZone != 0
                              && _deadlineUtc.isValid()
                              && QDateTime::currentDateTimeUtc() > _deadlineUtc;

    if(energised > 1 || pastDeadline || (energised > 0 && _openZone == 0)) {
        const int trippedOn = _openZone;
        logText(LVL_ERROR, QString("Watchdog tripped: %1 line(s) energised, open zone %2")
                               .arg(energised).arg(_openZone));
        allOff();
        emit watchdogTripped(trippedOn);
    }
}
```

⚠ The watchdog reads through `OutputBank::readValues()`, which queries the
backend. Changing it to `isActive()` verifies the cache against itself and the
invariant becomes decorative — that is exactly the bug spec §5.1 records, and
the reason Task 1 exists.

Three conditions trip it, not one: more than one line energised, a zone past its
deadline, or any line energised while the controller believes nothing is open.
The third catches a write that landed after `allOff()` returned.

Watchdog interval defaults to one second; `setWatchdogInterval()` exists so the
test does not wait.

- [ ] **Step 5: Run the tests**

```bash
cmake --build build --target tst_zonecontroller -j 32 && ./build/IrrigationD/tests/tst_zonecontroller
```

Expected: PASS, eight cases.

- [ ] **Step 6: Commit**

```bash
git add IrrigationD
git commit -m "feat: add ZoneController and its four safety invariants"
```

---

### Task 6: Clock seam and Scheduler

Resolves enabled programs into due UTC instants. The subtlest task in this plan: everything in it is a date-arithmetic bug waiting to happen, which is why the clock is injected and the tests simulate months in milliseconds.

**Files:**
- Create: `IrrigationD/src/iclock.h`
- Create: `IrrigationD/src/scheduler.h`, `.cpp`
- Create: `IrrigationD/tests/tst_scheduler.cpp`

**Interfaces:**
- Consumes: `IrrigationDataSource`, `Program`, `ProgramStartTime`, `FiredInstant` from Tasks 3 and 4
- Produces:
  - `class IClock` — `virtual QDateTime nowUtc() const = 0`
  - `class SystemClock : public IClock`
  - `class TestClock : public IClock` — `void setNowUtc(const QDateTime&)`, `void advance(qint64 seconds)`
  - `Scheduler(IrrigationDataSource* source, IClock* clock, QObject* parent = nullptr)`
  - `void start()` / `void stop()` / `void tick()`
  - `static bool isWateringDay(const Program& program, const QDate& localDate)`
  - `static QDateTime resolveToUtc(const ProgramStartTime& startTime, const QDate& localDate, bool* valid)`
  - signal: `void programDue(int programId, int startTimeId, const QDateTime& scheduledAtUtc)`

- [ ] **Step 1: Write the clock seam**

`IrrigationD/src/iclock.h`:

```cpp
#ifndef ICLOCK_H
#define ICLOCK_H

#include <QDateTime>

/** @brief Source of the current instant. Injected so schedules can be tested without waiting. */
class IClock
{
public:
    virtual ~IClock() {}

    /** @brief Returns the current instant in UTC. */
    virtual QDateTime nowUtc() const = 0;
};

/** @brief Clock backed by the system time. */
class SystemClock : public IClock
{
public:
    virtual QDateTime nowUtc() const override { return QDateTime::currentDateTimeUtc(); }
};

/** @brief Clock the test drives by hand. */
class TestClock : public IClock
{
public:
    /** @brief Constructs a clock reading @p start. */
    explicit TestClock(const QDateTime& start) : _now(start.toUTC()) {}

    virtual QDateTime nowUtc() const override { return _now; }

    /** @brief Sets the instant this clock reports. */
    void setNowUtc(const QDateTime& value) { _now = value.toUTC(); }

    /** @brief Moves the clock forward by @p seconds. */
    void advance(qint64 seconds) { _now = _now.addSecs(seconds); }

private:
    QDateTime _now;
};

#endif // ICLOCK_H
```

- [ ] **Step 2: Write the failing scheduler tests**

The cases that carry real risk:

```cpp
void TestScheduler::dayOfWeekMaskSelectsTheRightDays()
{
    Program program;
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = (1 << 0) | (1 << 2);   // Monday and Wednesday

    QVERIFY(Scheduler::isWateringDay(program, QDate(2026, 9, 14)));   // Monday
    QVERIFY(Scheduler::isWateringDay(program, QDate(2026, 9, 15)) == false);
    QVERIFY(Scheduler::isWateringDay(program, QDate(2026, 9, 16)));   // Wednesday
    QVERIFY(Scheduler::isWateringDay(program, QDate(2026, 9, 20)) == false); // Sunday
}

void TestScheduler::oddAndEvenUseTheDayOfMonth()
{
    Program odd;
    odd.dayMode = Program::DayMode::Odd;
    QVERIFY(Scheduler::isWateringDay(odd, QDate(2026, 9, 15)));
    QVERIFY(Scheduler::isWateringDay(odd, QDate(2026, 9, 16)) == false);

    Program even;
    even.dayMode = Program::DayMode::Even;
    QVERIFY(Scheduler::isWateringDay(even, QDate(2026, 9, 16)));
    QVERIFY(Scheduler::isWateringDay(even, QDate(2026, 9, 15)) == false);
}

void TestScheduler::everyNDaysCountsFromTheAnchor()
{
    Program program;
    program.dayMode = Program::DayMode::EveryNDays;
    program.intervalDays = 3;
    program.anchorDate = QDate(2026, 9, 1);

    QVERIFY(Scheduler::isWateringDay(program, QDate(2026, 9, 1)));
    QVERIFY(Scheduler::isWateringDay(program, QDate(2026, 9, 2)) == false);
    QVERIFY(Scheduler::isWateringDay(program, QDate(2026, 9, 4)));
    QVERIFY(Scheduler::isWateringDay(program, QDate(2026, 9, 7)));

    // Dates before the anchor never water, rather than counting backwards.
    QVERIFY(Scheduler::isWateringDay(program, QDate(2026, 8, 29)) == false);
}

void TestScheduler::springForwardGapIsRejected()
{
    // 2026-03-08 02:30 America/Los_Angeles does not exist.
    ProgramStartTime startTime;
    startTime.minutesAfterMidnight = 150;
    startTime.timezone = "America/Los_Angeles";

    bool valid = true;
    QDateTime resolved = Scheduler::resolveToUtc(startTime, QDate(2026, 3, 8), &valid);
    QCOMPARE(valid, false);
    QVERIFY(resolved.isValid() == false);
}

void TestScheduler::fallBackRepeatPicksTheEarlierOffset()
{
    // 2026-11-01 01:30 America/Los_Angeles happens twice.
    ProgramStartTime startTime;
    startTime.minutesAfterMidnight = 90;
    startTime.timezone = "America/Los_Angeles";

    bool valid = false;
    QDateTime resolved = Scheduler::resolveToUtc(startTime, QDate(2026, 11, 1), &valid);
    QVERIFY(valid);

    // PDT is UTC-7, so the earlier occurrence is 08:30Z. The later is 09:30Z.
    QCOMPARE(resolved, QDateTime(QDate(2026, 11, 1), QTime(8, 30), QTimeZone::UTC));
}

void TestScheduler::aStartTimeDoesNotDriftAcrossDst()
{
    ProgramStartTime startTime;
    startTime.minutesAfterMidnight = 360;   // 06:00 local
    startTime.timezone = "America/Los_Angeles";

    bool valid = false;
    QDateTime winter = Scheduler::resolveToUtc(startTime, QDate(2026, 1, 15), &valid);
    QVERIFY(valid);
    QDateTime summer = Scheduler::resolveToUtc(startTime, QDate(2026, 7, 15), &valid);
    QVERIFY(valid);

    // Different UTC instants, same wall clock. Storing the rule in UTC would make
    // these identical and the program would run at 05:00 all summer.
    QCOMPARE(winter.time(), QTime(14, 0));
    QCOMPARE(summer.time(), QTime(13, 0));
}

void TestScheduler::firesOnceInsideTheGraceWindow()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program program;
    program.name = "Morning";
    program.dayMode = Program::DayMode::Odd;
    QVERIFY(source.insertProgram(program));

    ProgramStartTime startTime;
    startTime.programId = program.id;
    startTime.minutesAfterMidnight = 360;
    startTime.timezone = "UTC";
    QVERIFY(source.insertStartTime(startTime));

    TestClock clock(QDateTime(QDate(2026, 9, 15), QTime(6, 0, 30), QTimeZone::UTC));
    Scheduler scheduler(&source, &clock);

    QSignalSpy spy(&scheduler, &Scheduler::programDue);
    scheduler.tick();
    QCOMPARE(spy.count(), 1);

    // A second tick inside the same window must not fire again.
    clock.advance(30);
    scheduler.tick();
    QCOMPARE(spy.count(), 1);
}

void TestScheduler::missedOccurrenceIsRecordedAndNeverCaughtUp()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program program;
    program.name = "Morning";
    program.dayMode = Program::DayMode::Odd;
    QVERIFY(source.insertProgram(program));

    ProgramStartTime startTime;
    startTime.programId = program.id;
    startTime.minutesAfterMidnight = 360;
    startTime.timezone = "UTC";
    QVERIFY(source.insertStartTime(startTime));

    // The daemon was down and comes back an hour late.
    TestClock clock(QDateTime(QDate(2026, 9, 15), QTime(7, 0), QTimeZone::UTC));
    Scheduler scheduler(&source, &clock);

    QSignalSpy spy(&scheduler, &Scheduler::programDue);
    scheduler.tick();

    QCOMPARE(spy.count(), 0);
    QVERIFY(source.hasFired(program.id, startTime.id,
                            QDateTime(QDate(2026, 9, 15), QTime(6, 0), QTimeZone::UTC)));
}

void TestScheduler::rainDelaySuppressesFiring()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(source.setSettingValue("rain_delay_until", "2026-09-16T00:00:00Z"));

    Program program;
    program.name = "Morning";
    program.dayMode = Program::DayMode::Odd;
    QVERIFY(source.insertProgram(program));

    ProgramStartTime startTime;
    startTime.programId = program.id;
    startTime.minutesAfterMidnight = 360;
    startTime.timezone = "UTC";
    QVERIFY(source.insertStartTime(startTime));

    TestClock clock(QDateTime(QDate(2026, 9, 15), QTime(6, 0, 10), QTimeZone::UTC));
    Scheduler scheduler(&source, &clock);

    QSignalSpy spy(&scheduler, &Scheduler::programDue);
    scheduler.tick();

    QCOMPARE(spy.count(), 0);
    QVERIFY(source.hasFired(program.id, startTime.id,
                            QDateTime(QDate(2026, 9, 15), QTime(6, 0), QTimeZone::UTC)));
}
```

`aStartTimeDoesNotDriftAcrossDst` is the discrimination test for the entire
UTC-vs-wall-clock decision. Normalise the rule to UTC at write time and only
that test fails — every other one still passes, which is exactly how this bug
ships.

⚠ These tests need `tzdata` present. `QTimeZone` on a system with no zone
database silently falls back to UTC instead of raising, producing internally
consistent results that are all an hour wrong. Add an early assertion:

```cpp
void TestScheduler::initTestCase()
{
    QVERIFY2(QTimeZone("America/Los_Angeles").isValid(),
             "tzdata is missing; every DST assertion below would silently pass against UTC");
}
```

- [ ] **Step 3: Run it and verify it fails**

```bash
cmake --build build --target tst_scheduler -j 32
```

Expected: failure — `scheduler.h` does not exist.

- [ ] **Step 4: Implement the day rules**

```cpp
bool Scheduler::isWateringDay(const Program& program, const QDate& localDate)
{
    switch(program.dayMode)
    {
    case Program::DayMode::DaysOfWeek:
        // QDate::dayOfWeek() is 1..7 with Monday == 1; bit 0 is Monday.
        return (program.dowMask & (1 << (localDate.dayOfWeek() - 1))) != 0;

    case Program::DayMode::Odd:
        return (localDate.day() % 2) == 1;

    case Program::DayMode::Even:
        return (localDate.day() % 2) == 0;

    case Program::DayMode::EveryNDays:
        if(program.intervalDays < 1 || program.anchorDate.isValid() == false) {
            return false;
        }
        if(localDate < program.anchorDate) {
            return false;
        }
        return (program.anchorDate.daysTo(localDate) % program.intervalDays) == 0;
    }

    return false;
}
```

- [ ] **Step 5: Implement the time-zone resolution**

```cpp
QDateTime Scheduler::resolveToUtc(const ProgramStartTime& startTime,
                                  const QDate& localDate,
                                  bool* valid)
{
    if(valid != nullptr) {
        *valid = false;
    }

    QTimeZone zone(startTime.timezone.toUtf8());
    if(zone.isValid() == false) {
        return QDateTime();
    }

    const QTime localTime = QTime(0, 0).addSecs(startTime.minutesAfterMidnight * 60);

    // Spring forward: the local time does not exist. Reject rather than invent one.
    QDateTime rejected(localDate, localTime, zone, QDateTime::TransitionResolution::Reject);
    if(rejected.isValid() == false) {
        return QDateTime();
    }

    // Fall back: the local time happens twice. Run once, on the earlier offset.
    QDateTime resolved(localDate, localTime, zone, QDateTime::TransitionResolution::PreferBefore);
    if(resolved.isValid() == false) {
        return QDateTime();
    }

    if(valid != nullptr) {
        *valid = true;
    }
    return resolved.toUTC();
}
```

Both constructions are needed. `PreferBefore` alone still yields a valid
`QDateTime` inside a spring-forward gap by sliding to an adjacent instant, which
would run the program an hour early on exactly one day a year.

- [ ] **Step 6: Implement the tick**

```cpp
void Scheduler::tick()
{
    const QDateTime nowUtc = _clock->nowUtc();

    if(_source->settingValue("master_enabled") == "0") {
        return;
    }

    const QDateTime rainDelayUntil =
        QDateTime::fromString(_source->settingValue("rain_delay_until"), Qt::ISODate);
    const bool rainDelayed = rainDelayUntil.isValid() && nowUtc < rainDelayUntil.toUTC();

    const ProgramList programs = _source->enabledPrograms();
    for(const Program& program : programs) {
        const ProgramStartTimeList startTimes = _source->startTimesFor(program.id);
        for(const ProgramStartTime& startTime : startTimes) {
            QTimeZone zone(startTime.timezone.toUtf8());
            if(zone.isValid() == false) {
                continue;
            }

            const QDate localDate = nowUtc.toTimeZone(zone).date();
            if(isWateringDay(program, localDate) == false) {
                continue;
            }

            bool valid = false;
            const QDateTime scheduledUtc = resolveToUtc(startTime, localDate, &valid);
            if(valid == false) {
                recordOnce(program.id, startTime.id,
                           QDateTime(localDate, QTime(0, 0), QTimeZone::UTC)
                               .addSecs(startTime.minutesAfterMidnight * 60),
                           FiredInstant::Outcome::Missed);
                continue;
            }

            if(_source->hasFired(program.id, startTime.id, scheduledUtc)) {
                continue;
            }

            const qint64 lateBy = scheduledUtc.secsTo(nowUtc);
            if(lateBy < 0) {
                continue;
            }

            if(lateBy > GraceWindowSeconds) {
                // Never catch up. Watering at an arbitrary hour because the daemon
                // was down is worse than not watering.
                recordOnce(program.id, startTime.id, scheduledUtc, FiredInstant::Outcome::Missed);
                continue;
            }

            if(rainDelayed) {
                recordOnce(program.id, startTime.id, scheduledUtc, FiredInstant::Outcome::SkippedRain);
                continue;
            }

            recordOnce(program.id, startTime.id, scheduledUtc, FiredInstant::Outcome::Ran);
            emit programDue(program.id, startTime.id, scheduledUtc);
        }
    }
}
```

`static constexpr qint64 GraceWindowSeconds = 120;`

⚠ The firing is recorded **before** `programDue` is emitted. A crash between the
two loses one run; recording afterwards means a crash re-runs the program on
restart, and a scheduler that waters twice is worse than one that waters once.

`recordOnce()` builds a `FiredInstant` and calls `_source->recordFiring()`, whose
`INSERT OR IGNORE` makes a repeat harmless.

`start()` creates a one-second `QTimer` connected to `tick()`. `tick()` is public
so the tests drive it directly without a running event loop.

- [ ] **Step 7: Run the tests**

```bash
cmake --build build --target tst_scheduler -j 32 && ./build/IrrigationD/tests/tst_scheduler
```

Expected: PASS, nine cases.

- [ ] **Step 8: Commit**

```bash
git add IrrigationD
git commit -m "feat: add the clock seam and the scheduler"
```

---

### Task 7: ProgramRunner

Walks one program's ordered zone list. Timing authority stays with `ZoneController`; this class only decides what comes next.

**Files:**
- Create: `IrrigationD/src/programrunner.h`, `.cpp`
- Create: `IrrigationD/tests/tst_programrunner.cpp`

**Interfaces:**
- Consumes: `ZoneController` (Task 5), `IrrigationDataSource`, `ProgramZone` (Tasks 3–4)
- Produces:
  - `ProgramRunner(ZoneController* controller, IrrigationDataSource* source, QObject* parent = nullptr)`
  - `bool startProgram(int programId)`
  - `void abort()`
  - `bool isRunning() const`
  - `int runningProgramId() const`
  - signals: `void programStarted(int programId)`, `void programFinished(int programId)`, `void programAborted(int programId)`

- [ ] **Step 1: Write the failing tests**

```cpp
#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <Kanoop/pi/mockbackend.h>

#include "database/irrigationdatasource.h"
#include "programrunner.h"
#include "zonecontroller.h"

static QMap<int, quint32> eightZones()
{
    return { {1,5}, {2,6}, {3,13}, {4,16}, {5,19}, {6,20}, {7,21}, {8,26} };
}

class TestProgramRunner : public QObject
{
    Q_OBJECT
private:
    int buildProgram(IrrigationDataSource& source, const QList<int>& zoneNumbers, int seconds);

private slots:
    void walksZonesInSequenceOrder();
    void advancesOnZoneClosedNotOnATimer();
    void finishesAfterTheLastZone();
    void abortStopsTheSequenceAndClosesTheValve();
    void startingWhileRunningIsRejected();
    void aProgramWithNoZonesFinishesImmediately();
};

int TestProgramRunner::buildProgram(IrrigationDataSource& source,
                                    const QList<int>& zoneNumbers,
                                    int seconds)
{
    Program program;
    program.name = "Test";
    if(source.insertProgram(program) == false) {
        return 0;
    }

    const ZoneList zones = source.allZones();
    int sequence = 1;
    for(int number : zoneNumbers) {
        int zoneId = 0;
        for(const Zone& zone : zones) {
            if(zone.number == number) {
                zoneId = zone.id;
                break;
            }
        }

        ProgramZone entry;
        entry.programId = program.id;
        entry.zoneId = zoneId;
        entry.sequence = sequence++;
        entry.durationSeconds = seconds;
        if(source.insertProgramZone(entry) == false) {
            return 0;
        }
    }

    return program.id;
}

void TestProgramRunner::walksZonesInSequenceOrder()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    // Sequence 1,2,3 maps to zone numbers 3,1,2 - deliberately not ascending and
    // deliberately not the order the rows were inserted in by number.
    const int programId = buildProgram(source, { 3, 1, 2 }, 600);
    QVERIFY(programId > 0);

    ProgramRunner runner(&controller, &source);
    QObject::connect(&controller, &ZoneController::zoneClosed,
                     &runner, &ProgramRunner::onZoneClosed);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumber(), 3);

    controller.expireCloseTimerForTest();
    QCOMPARE(controller.openZoneNumber(), 1);

    controller.expireCloseTimerForTest();
    QCOMPARE(controller.openZoneNumber(), 2);
}

void TestProgramRunner::advancesOnZoneClosedNotOnATimer()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = buildProgram(source, { 1, 2 }, 600);
    ProgramRunner runner(&controller, &source);
    QObject::connect(&controller, &ZoneController::zoneClosed,
                     &runner, &ProgramRunner::onZoneClosed);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumber(), 1);

    // Ten minutes of wall time have not passed. The only thing that advances the
    // sequence is the controller saying the zone closed.
    QTest::qWait(50);
    QCOMPARE(controller.openZoneNumber(), 1);

    controller.expireCloseTimerForTest();
    QCOMPARE(controller.openZoneNumber(), 2);
}

void TestProgramRunner::finishesAfterTheLastZone()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = buildProgram(source, { 1, 2 }, 600);
    ProgramRunner runner(&controller, &source);
    QObject::connect(&controller, &ZoneController::zoneClosed,
                     &runner, &ProgramRunner::onZoneClosed);

    QSignalSpy spy(&runner, &ProgramRunner::programFinished);
    QVERIFY(runner.startProgram(programId));

    controller.expireCloseTimerForTest();
    controller.expireCloseTimerForTest();

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toInt(), programId);
    QCOMPARE(runner.isRunning(), false);
    QCOMPARE(controller.openZoneNumber(), 0);
}

void TestProgramRunner::abortStopsTheSequenceAndClosesTheValve()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = buildProgram(source, { 1, 2, 3 }, 600);
    ProgramRunner runner(&controller, &source);
    QObject::connect(&controller, &ZoneController::zoneClosed,
                     &runner, &ProgramRunner::onZoneClosed);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumber(), 1);

    runner.abort();
    controller.allOff();

    QCOMPARE(runner.isRunning(), false);
    QCOMPARE(controller.openZoneNumber(), 0);
    for(quint32 offset : eightZones().values()) {
        QCOMPARE(backend.lineValue(offset), Gpio::Value::Inactive);
    }

    // allOff() emitted zoneClosed. Without the isRunning guard in onZoneClosed the
    // runner treats its own abort as a completed zone and opens zone 2 - a program
    // that keeps watering after the stop button.
    QCOMPARE(controller.openZoneNumber(), 0);
}

void TestProgramRunner::startingWhileRunningIsRejected()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int first  = buildProgram(source, { 1 }, 600);
    const int second = buildProgram(source, { 5 }, 600);

    ProgramRunner runner(&controller, &source);
    QVERIFY(runner.startProgram(first));
    QVERIFY(runner.startProgram(second) == false);

    QCOMPARE(runner.runningProgramId(), first);
    QCOMPARE(controller.openZoneNumber(), 1);
}

void TestProgramRunner::aProgramWithNoZonesFinishesImmediately()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = buildProgram(source, {}, 600);
    QVERIFY(programId > 0);

    ProgramRunner runner(&controller, &source);
    QSignalSpy spy(&runner, &ProgramRunner::programFinished);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(runner.isRunning(), false);
    QCOMPARE(controller.openZoneNumber(), 0);
}

QTEST_MAIN(TestProgramRunner)
#include "tst_programrunner.moc"
```

`expireCloseTimerForTest()` runs the controller's close path synchronously, so a
six-zone sequence costs microseconds instead of an hour of wall time. It is a
seam on the component being driven, not on the component under test.

`abortStopsTheSequenceAndClosesTheValve` is the discrimination test. Delete the
`_running == false` guard in `onZoneClosed()` and only that case fails.

- [ ] **Step 2: Run it and verify it fails**

```bash
cmake --build build --target tst_programrunner -j 32
```

Expected: failure — `programrunner.h` does not exist.

- [ ] **Step 3: Implement it**

```cpp
bool ProgramRunner::startProgram(int programId)
{
    if(_running) {
        return false;
    }

    _zones = _source->zonesFor(programId);   // ordered by sequence
    _programId = programId;
    _index = -1;
    _running = true;

    emit programStarted(programId);

    return advance();
}

bool ProgramRunner::advance()
{
    _index++;

    if(_index >= _zones.count()) {
        const int finished = _programId;
        _running = false;
        _programId = 0;
        emit programFinished(finished);
        return true;
    }

    const ProgramZone& next = _zones.at(_index);
    const ZoneList zones = _source->allZones();

    int zoneNumber = 0;
    for(const Zone& zone : zones) {
        if(zone.id == next.zoneId) {
            zoneNumber = zone.number;
            break;
        }
    }

    if(zoneNumber == 0) {
        logText(LVL_ERROR, QString("Program %1 references unknown zone id %2")
                               .arg(_programId).arg(next.zoneId));
        return advance();
    }

    return _controller->openZone(zoneNumber, next.durationSeconds);
}

void ProgramRunner::onZoneClosed(int zoneNumber)
{
    Q_UNUSED(zoneNumber)

    if(_running == false) {
        return;
    }

    advance();
}
```

⚠ `onZoneClosed` must return immediately when `_running` is false. `abort()`
calls `ZoneController::allOff()`, which emits `zoneClosed` — without the guard
the runner treats its own abort as a completed zone and opens the next one. That
is a program that keeps watering after you press STOP.

`zonesFor()` must order by `sequence`, not by `id`. Write the `ORDER BY sequence`
into the query and assert it in Task 4's tests.

- [ ] **Step 4: Run the tests**

```bash
cmake --build build --target tst_programrunner -j 32 && ./build/IrrigationD/tests/tst_programrunner
```

Expected: PASS, six cases.

- [ ] **Step 5: Commit**

```bash
git add IrrigationD
git commit -m "feat: add ProgramRunner"
```

---

### Task 8: StopButton

Wraps an `InputPin` on the stop button. Small, and the one component whose failure mode is "the emergency stop does nothing".

**Files:**
- Create: `IrrigationD/src/stopbutton.h`, `.cpp`
- Create: `IrrigationD/tests/tst_stopbutton.cpp`

**Interfaces:**
- Consumes: `InputPin`, `Gpio` from `KanoopPiQt`; `InputPin::isAsserted()` from Task 1
- Produces:
  - `StopButton(IGpioBackend* backend, quint32 offset, QObject* parent = nullptr)`
  - `bool begin()`
  - `bool isHeld() const`
  - signal: `void pressed()`

- [ ] **Step 1: Write the failing tests**

```cpp
#include <QTest>
#include <QSignalSpy>

#include <Kanoop/pi/mockbackend.h>

#include "stopbutton.h"

class TestStopButton : public QObject
{
    Q_OBJECT
private slots:
    void pressEmitsPressed();
    void releaseDoesNotEmitPressed();
    void alreadyHeldAtStartupIsReported();
    void beginFailsWhenTheLineCannotBeRequested();
};

void TestStopButton::pressEmitsPressed()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    StopButton button(&backend, 25);
    QVERIFY(button.begin());

    QSignalSpy spy(&button, &StopButton::pressed);
    backend.simulateEdge(25, Gpio::Edge::Rising);

    QCOMPARE(spy.count(), 1);
}

void TestStopButton::releaseDoesNotEmitPressed()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    StopButton button(&backend, 25);
    QVERIFY(button.begin());

    QSignalSpy spy(&button, &StopButton::pressed);
    backend.simulateEdge(25, Gpio::Edge::Falling);

    QCOMPARE(spy.count(), 0);
}

void TestStopButton::alreadyHeldAtStartupIsReported()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    backend.setLineValue(25, Gpio::Value::Active);

    StopButton button(&backend, 25);
    QVERIFY(button.begin());

    QCOMPARE(button.isHeld(), true);
}

void TestStopButton::beginFailsWhenTheLineCannotBeRequested()
{
    MockBackend backend;
    // No chip opened.
    StopButton button(&backend, 25);
    QVERIFY(button.begin() == false);
}

QTEST_MAIN(TestStopButton)
#include "tst_stopbutton.moc"
```

⚠ `pressEmitsPressed` asserts on a **Rising** edge. The button is 1NO wired to
ground, so a press pulls the line physically LOW — which under `activeLow` the
kernel reports as a logical **rising** edge. Spec §4.3 states the convention.
This exact assertion was wrong once already and a passing test pinned the wrong
behaviour through six reviews. Inverting it in software cancels the kernel's
inversion and fires the emergency stop when you let go of the button.

`setLineValue()` and `simulateEdge()` both come from Task 1's additions to
`MockBackend`. Do not re-add them.

- [ ] **Step 2: Run it and verify it fails**

```bash
cmake --build build --target tst_stopbutton -j 32
```

Expected: failure — `stopbutton.h` does not exist.

- [ ] **Step 3: Implement it**

```cpp
bool StopButton::begin()
{
    _pin = new InputPin(_backend, "irrigationd-stop", _offset, this);
    _pin->setActiveLow(true);
    _pin->setBias(Gpio::Bias::PullUp);
    _pin->setEdge(Gpio::Edge::Both);
    _pin->setDebounce(TimeSpan::fromMilliseconds(20));

    if(_pin->request() == false) {
        _errorText = _pin->errorText();
        return false;
    }

    connect(_pin, &InputPin::asserted, this, &StopButton::pressed);

    // An edge-only input has no initial state. A daemon restarted under
    // Restart=always while the button is held would otherwise never know.
    bool ok = false;
    _held = _pin->isAsserted(&ok);
    if(ok == false) {
        _errorText = _pin->errorText();
        return false;
    }

    return true;
}
```

No `deasserted` connection. Release is not an event this daemon acts on.

- [ ] **Step 4: Run the tests and commit**

```bash
cmake --build build --target tst_stopbutton -j 32 && ./build/IrrigationD/tests/tst_stopbutton
git add IrrigationD
git commit -m "feat: add the stop button input"
```

---

### Task 9: IrrigationControlServer

`QHttpServer` on its own thread, with every route in spec §7. Talks to the valve owner only through signals.

**Files:**
- Create: `IrrigationD/src/irrigationcontrolserver.h`, `.cpp`
- Create: `IrrigationD/src/json/statusjson.h`, `.cpp`
- Create: `IrrigationD/src/json/programjson.h`, `.cpp`
- Create: `IrrigationD/tests/tst_controlserver.cpp`

**Interfaces:**
- Consumes: `AbstractThreadClass` from `KanoopCommonQt`; `IrrigationDataSource`; the models from Task 4
- Produces:
  - `IrrigationControlServer(const QString& databasePath)`
  - `void setBindAddress(const QString&)` / `void setListenPort(int)`
  - `void updateStatus(const ServerStatus& status)` — thread-safe setter, emits internally
  - signals into the daemon: `void manualZoneRunRequested(int zoneNumber, int seconds)`, `void programRunRequested(int programId)`, `void stopRequested()`
  - `ServerStatus` — `int runningZone`, `int secondsRemaining`, `QDateTime nextRunUtc`, `QString timezone`, `bool masterEnabled`, `QDateTime rainDelayUntilUtc`

- [ ] **Step 1: Write the failing tests**

Test the server through real HTTP against `127.0.0.1` on an ephemeral port, using `QNetworkAccessManager`. Handlers that only get called directly are not evidence the routes are wired.

```cpp
void TestControlServer::healthAnswers200()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    server.setBindAddress("127.0.0.1");
    server.setListenPort(0);
    QVERIFY(server.start(TimeSpan::fromSeconds(5)));
    QVERIFY(server.waitUntilReady(TimeSpan::fromSeconds(5)));

    QNetworkAccessManager manager;
    QNetworkReply* reply = manager.get(
        QNetworkRequest(QUrl(QString("http://127.0.0.1:%1/admin/health").arg(server.boundPort()))));

    QSignalSpy spy(reply, &QNetworkReply::finished);
    QVERIFY(spy.wait(5000));
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 200);

    server.stop();
}

// Posts to the running server and returns the reply, already finished.
static QNetworkReply* postJson(QNetworkAccessManager& manager,
                               int port,
                               const QString& path,
                               const QByteArray& body)
{
    QNetworkRequest request(QUrl(QString("http://127.0.0.1:%1%2").arg(port).arg(path)));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply* reply = manager.post(request, body);
    QSignalSpy spy(reply, &QNetworkReply::finished);
    spy.wait(5000);
    return reply;
}

static int statusCode(QNetworkReply* reply)
{
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

void TestControlServer::manualRunEmitsARequestRatherThanTouchingGpio()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    server.setBindAddress("127.0.0.1");
    server.setListenPort(0);
    QVERIFY(server.start(TimeSpan::fromSeconds(5)));
    QVERIFY(server.waitUntilReady(TimeSpan::fromSeconds(5)));

    QSignalSpy spy(&server, &IrrigationControlServer::manualZoneRunRequested);

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(),
                                    "/admin/zones/3/run", R"({"seconds": 60})");

    QCOMPARE(statusCode(reply), 202);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toInt(), 3);
    QCOMPARE(spy.first().at(1).toInt(), 60);

    server.stop();
}

void TestControlServer::statusReportsTheControllerTimezone()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    server.setBindAddress("127.0.0.1");
    server.setListenPort(0);
    QVERIFY(server.start(TimeSpan::fromSeconds(5)));
    QVERIFY(server.waitUntilReady(TimeSpan::fromSeconds(5)));

    ServerStatus status;
    status.runningZone = 4;
    status.secondsRemaining = 120;
    status.nextRunUtc = QDateTime(QDate(2026, 9, 13), QTime(13, 0), QTimeZone::UTC);
    status.timezone = "America/Los_Angeles";
    status.masterEnabled = true;
    server.updateStatus(status);

    QNetworkAccessManager manager;
    QNetworkReply* reply = manager.get(QNetworkRequest(
        QUrl(QString("http://127.0.0.1:%1/admin/status").arg(server.boundPort()))));
    QSignalSpy spy(reply, &QNetworkReply::finished);
    QVERIFY(spy.wait(5000));

    const QJsonObject object = QJsonDocument::fromJson(reply->readAll()).object();

    QCOMPARE(object.value("runningZone").toInt(), 4);
    QCOMPARE(object.value("secondsRemaining").toInt(), 120);

    // The browser renders in the CONTROLLER's zone, not its own, so the zone id
    // travels with the timestamps.
    QCOMPARE(object.value("timezone").toString(), QString("America/Los_Angeles"));
    QVERIFY(object.value("nextRunUtc").toString().endsWith("Z"));

    server.stop();
}

void TestControlServer::unknownZoneReturns404()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    server.setBindAddress("127.0.0.1");
    server.setListenPort(0);
    QVERIFY(server.start(TimeSpan::fromSeconds(5)));
    QVERIFY(server.waitUntilReady(TimeSpan::fromSeconds(5)));

    QSignalSpy spy(&server, &IrrigationControlServer::manualZoneRunRequested);

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(),
                                    "/admin/zones/99/run", R"({"seconds": 60})");

    QCOMPARE(statusCode(reply), 404);
    QCOMPARE(spy.count(), 0);

    server.stop();
}

void TestControlServer::malformedJsonReturns400()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    server.setBindAddress("127.0.0.1");
    server.setListenPort(0);
    QVERIFY(server.start(TimeSpan::fromSeconds(5)));
    QVERIFY(server.waitUntilReady(TimeSpan::fromSeconds(5)));

    QSignalSpy spy(&server, &IrrigationControlServer::manualZoneRunRequested);

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(),
                                    "/admin/zones/3/run", "not json");

    QCOMPARE(statusCode(reply), 400);
    QCOMPARE(spy.count(), 0);

    server.stop();
}
```

`server.start()` is `AbstractThreadClass::start()`, which reports that the
**thread** started, not that the listener bound. Add `bool waitUntilReady(const
TimeSpan&)` and `int boundPort() const` to this class — post-start readiness is a
separate query on the object, per the teardown contract in
`meta-qt-mains/.claude/docs/httpop-teardown-contract.md`.

- [ ] **Step 2: Run it and verify it fails**

```bash
cmake --build build --target tst_controlserver -j 32
```

Expected: failure — `irrigationcontrolserver.h` does not exist.

- [ ] **Step 3: Implement the threaded server**

```cpp
void IrrigationControlServer::threadStarted()
{
    logText(LVL_INFO, "Control server thread started");

    // Each thread needs its own named connection; QSqlDatabase connections cannot
    // be shared across threads.
    _source = new IrrigationDataSource(_databasePath);
    _source->setConnectionName(QString("irrigation-http-%1")
                                   .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if(_source->open() == false) {
        logText(LVL_ERROR, QString("Control server could not open the database: %1")
                               .arg(_source->errorText()));
        return;
    }

    _httpServer = new QHttpServer;

    // Every endpoint in spec section 7. QHttpServer binds <arg> path segments to
    // the leading parameters of the lambda, in order, before the request.
    _httpServer->route("/admin/health", QHttpServerRequest::Method::Get,
                       [this](const QHttpServerRequest& request)
    {
        return this->handleHealth(request);
    });

    _httpServer->route("/admin/version", QHttpServerRequest::Method::Get,
                       [this](const QHttpServerRequest& request)
    {
        return this->handleVersion(request);
    });

    _httpServer->route("/admin/status", QHttpServerRequest::Method::Get,
                       [this](const QHttpServerRequest& request)
    {
        return this->handleStatus(request);
    });

    _httpServer->route("/admin/zones", QHttpServerRequest::Method::Get,
                       [this](const QHttpServerRequest& request)
    {
        return this->handleZonesGet(request);
    });

    _httpServer->route("/admin/zones/<arg>", QHttpServerRequest::Method::Put,
                       [this](int zoneNumber, const QHttpServerRequest& request)
    {
        return this->handleZonePut(zoneNumber, request);
    });

    _httpServer->route("/admin/zones/<arg>/run", QHttpServerRequest::Method::Post,
                       [this](int zoneNumber, const QHttpServerRequest& request)
    {
        return this->handleZoneRun(zoneNumber, request);
    });

    _httpServer->route("/admin/programs", QHttpServerRequest::Method::Get,
                       [this](const QHttpServerRequest& request)
    {
        return this->handleProgramsGet(request);
    });

    _httpServer->route("/admin/programs", QHttpServerRequest::Method::Post,
                       [this](const QHttpServerRequest& request)
    {
        return this->handleProgramPost(request);
    });

    _httpServer->route("/admin/programs/<arg>", QHttpServerRequest::Method::Put,
                       [this](int programId, const QHttpServerRequest& request)
    {
        return this->handleProgramPut(programId, request);
    });

    _httpServer->route("/admin/programs/<arg>", QHttpServerRequest::Method::Delete,
                       [this](int programId, const QHttpServerRequest& request)
    {
        return this->handleProgramDelete(programId, request);
    });

    _httpServer->route("/admin/programs/<arg>/run", QHttpServerRequest::Method::Post,
                       [this](int programId, const QHttpServerRequest& request)
    {
        return this->handleProgramRun(programId, request);
    });

    _httpServer->route("/admin/stop", QHttpServerRequest::Method::Post,
                       [this](const QHttpServerRequest& request)
    {
        return this->handleStop(request);
    });

    _httpServer->route("/admin/settings", QHttpServerRequest::Method::Get,
                       [this](const QHttpServerRequest& request)
    {
        return this->handleSettingsGet(request);
    });

    _httpServer->route("/admin/settings", QHttpServerRequest::Method::Put,
                       [this](const QHttpServerRequest& request)
    {
        return this->handleSettingsPut(request);
    });

    _tcpServer = new QTcpServer;
    if(_tcpServer->listen(QHostAddress(_bindAddress), _listenPort) == false) {
        logText(LVL_ERROR, QString("Failed to listen on %1:%2 - %3")
                               .arg(_bindAddress).arg(_listenPort).arg(_tcpServer->errorString()));
        return;
    }

    if(_httpServer->bind(_tcpServer) == false) {
        logText(LVL_ERROR, "Failed to bind the HTTP server to the listening socket");
        return;
    }

    _boundPort = _tcpServer->serverPort();
    _ready = true;
    _readyEvent.set();
}
```

⚠ `QHttpServer` has **no `listen()`** in Qt 6.10. Verified against
`/opt/poky/5.2.4/sysroots/cortexa72-poky-linux/usr/include/QtHttpServer/qhttpserver.h`:
the only binding entry points are `QAbstractHttpServer::bind(QTcpServer*)` and
`bind(QLocalServer*)`. Every Qt 6.4-era example showing
`httpServer->listen(QHostAddress::Any, port)` will not compile.

Passing port `0` makes the kernel pick a free port, which is why the tests read
it back through `boundPort()` rather than hard-coding one and failing in CI.

```cpp
void IrrigationControlServer::threadFinished()
{
    logText(LVL_INFO, "Control server thread finished");

    delete _httpServer;
    _httpServer = nullptr;
    _tcpServer = nullptr;    // owned by _httpServer once bound

    delete _source;
    _source = nullptr;

    _ready = false;
}
```

⚠ Never destroy this object with `deleteLater()`. Its thread is gone by then and
the deferred delete is posted to a queue nothing will drain. The owner deletes
it directly after `stop()` returns.

- [ ] **Step 4: Implement the route handlers**

Every handler that changes valve state **emits and returns 202**. None of them
call `ZoneController`:

```cpp
QHttpServerResponse IrrigationControlServer::handleZoneRun(int zoneNumber,
                                                           const QHttpServerRequest& request)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(request.body(), &error);
    if(error.error != QJsonParseError::NoError) {
        return QHttpServerResponse(QJsonObject{{"error", "malformed JSON"}},
                                   QHttpServerResponder::StatusCode::BadRequest);
    }

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

    const int seconds = document.object().value("seconds").toInt(0);
    if(seconds < 1) {
        return QHttpServerResponse(QJsonObject{{"error", "seconds must be positive"}},
                                   QHttpServerResponder::StatusCode::BadRequest);
    }

    emit manualZoneRunRequested(zoneNumber, seconds);

    return QHttpServerResponse(QJsonObject{{"accepted", true}},
                               QHttpServerResponder::StatusCode::Accepted);
}
```

A route handler that called `_controller->openZone()` would be writing GPIO from
the HTTP thread while the scheduler writes it from the main thread. The signal
is what keeps the valves single-owner.

`/admin/status` serialises `ServerStatus`. It carries **both** the UTC instants
and the controller's IANA zone id, because the browser must render in the
controller's zone rather than its own — a phone that travels would otherwise show
schedules shifting.

`updateStatus()` is called from the main thread and must not write `_status`
directly. Emit a private signal and take the value on a slot, exactly as
`SomRestServer::setConfig()` does.

- [ ] **Step 5: Run the tests and commit**

```bash
cmake --build build --target tst_controlserver -j 32 && ./build/IrrigationD/tests/tst_controlserver
git add IrrigationD
git commit -m "feat: add the threaded REST control server"
```

---

### Task 10: Daemon assembly and the systemd unit

Wires every component together in the mandated order and ships the unit file.

**Files:**
- Modify: `IrrigationD/src/irrigationdaemon.h`, `.cpp`
- Create: `IrrigationD/systemd/irrigationd.service`
- Create: `IrrigationD/tests/tst_daemon.cpp`
- Modify: `IrrigationD/CMakeLists.txt`

- [ ] **Step 1: Write the failing assembly test**

```cpp
void TestDaemon::valvesAreClosedBeforeAnythingElseExists()
{
    QTemporaryDir dir;
    writeSettings(dir);

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    // Leave every line energised, as the bootloader might on a board whose
    // config.txt gpio= line is missing.
    for(quint32 offset : eightZones().values()) {
        backend.setLineValue(offset, Gpio::Value::Active);
    }

    IrrigationDaemon daemon(dir.filePath("irrigationd.ini"), &backend);
    QVERIFY2(daemon.start(), qPrintable(daemon.errorText()));

    for(quint32 offset : eightZones().values()) {
        QCOMPARE(backend.lineValue(offset), Gpio::Value::Inactive);
    }

    // The de-energising write must precede the first scheduler tick. The mock
    // numbers every write; the scheduler has not ticked, so its count is zero.
    QVERIFY(backend.setValuesCallCount() >= 1);
    QCOMPARE(daemon.schedulerTickCountForTest(), 0);

    daemon.stop();
}

void TestDaemon::stopRequestAbortsARunningProgram()
{
    QTemporaryDir dir;
    writeSettings(dir);

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    IrrigationDaemon daemon(dir.filePath("irrigationd.ini"), &backend);
    QVERIFY(daemon.start());

    const int programId = buildProgram(daemon.dataSourceForTest(), { 1, 2, 3 }, 600);
    QVERIFY(daemon.runnerForTest()->startProgram(programId));
    QCOMPARE(daemon.controllerForTest()->openZoneNumber(), 1);

    daemon.onStopPressed();

    QCOMPARE(daemon.runnerForTest()->isRunning(), false);
    QCOMPARE(daemon.controllerForTest()->openZoneNumber(), 0);
    for(quint32 offset : eightZones().values()) {
        QCOMPARE(backend.lineValue(offset), Gpio::Value::Inactive);
    }

    daemon.stop();
}

void TestDaemon::schedulerFiringStartsTheRunner()
{
    QTemporaryDir dir;
    writeSettings(dir);

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    IrrigationDaemon daemon(dir.filePath("irrigationd.ini"), &backend);
    QVERIFY(daemon.start());

    const int programId = buildProgram(daemon.dataSourceForTest(), { 4 }, 600);

    daemon.onProgramDue(programId, 1,
                        QDateTime(QDate(2026, 9, 15), QTime(6, 0), QTimeZone::UTC));

    QCOMPARE(daemon.runnerForTest()->isRunning(), true);
    QCOMPARE(daemon.controllerForTest()->openZoneNumber(), 4);

    daemon.stop();
}

void TestDaemon::aSecondProgramWhileBusyIsRecordedAsSkipped()
{
    QTemporaryDir dir;
    writeSettings(dir);

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    IrrigationDaemon daemon(dir.filePath("irrigationd.ini"), &backend);
    QVERIFY(daemon.start());

    IrrigationDataSource* source = daemon.dataSourceForTest();
    const int first  = buildProgram(*source, { 1 }, 600);
    const int second = buildProgram(*source, { 5 }, 600);

    const QDateTime scheduled(QDate(2026, 9, 15), QTime(6, 0), QTimeZone::UTC);

    daemon.onProgramDue(first, 1, scheduled);
    QCOMPARE(daemon.runnerForTest()->runningProgramId(), first);

    daemon.onProgramDue(second, 2, scheduled);

    // The busy program never starts, and the skip is on the record rather than
    // silently dropped.
    QCOMPARE(daemon.runnerForTest()->runningProgramId(), first);
    QCOMPARE(daemon.controllerForTest()->openZoneNumber(), 1);
    QVERIFY(source->hasFired(second, 2, scheduled));

    bool ok = false;
    QSqlQuery query = source->rawQuery(
        QString("SELECT outcome FROM fired_instants WHERE program_id = %1").arg(second), &ok);
    QVERIFY(ok);
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), QString("skipped_busy"));

    daemon.stop();
}
```

These tests need three things on `IrrigationDaemon` that production does not use:

- a second constructor `IrrigationDaemon(const QString& settingsPath, IGpioBackend* backend, QObject* parent = nullptr)` that adopts an externally owned backend instead of constructing a `LibGpiodBackend`. There is no GPIO chip on a development host, so without this seam none of the assembly is testable at all.
- accessors `IrrigationDataSource* dataSourceForTest()`, `ProgramRunner* runnerForTest()`, `ZoneController* controllerForTest()`
- `int schedulerTickCountForTest()`, incremented in `Scheduler::tick()`

`writeSettings(dir)` is a test helper writing an INI with the eight-zone map, a
mock chip label and a database path inside the temporary directory.
`buildProgram()` is the same helper as Task 7; lift it into a shared
`tests/testfixtures.h` when you write this task rather than copying it a third
time.

⚠ `onStopPressed()` and `onProgramDue()` are called directly by these tests, so
they must be public slots rather than private. That is a real widening of the
class's surface; it is justified because the alternative is asserting on daemon
assembly through a running HTTP server and a real GPIO chip, which tests nothing
about the wiring and everything about the environment.

- [ ] **Step 2: Implement start()**

```cpp
bool IrrigationDaemon::start()
{
    logText(LVL_INFO, "Starting irrigationd");

    // 1. Database.
    _source = new IrrigationDataSource(_settings.databasePath());
    if(_source->open() == false) {
        _errorText = _source->errorText();
        return false;
    }
    _source->pruneFiredInstantsOlderThan(QDateTime::currentDateTimeUtc().addDays(-90));

    // 2. GPIO, before anything can ask for water.
    _backend = new LibGpiodBackend(this);
    if(_backend->openChipByLabel(_settings.chipLabel()) == false) {
        _errorText = _backend->errorText();
        return false;
    }

    // 3. ZoneController drives all eight lines de-energised. Nothing above this
    //    line exists yet that could have opened one.
    _controller = new ZoneController(_backend,
                                     _settings.zoneGpioMap(),
                                     _settings.zoneActiveLow(),
                                     _settings.maxZoneSeconds(),
                                     this);
    if(_controller->begin() == false) {
        _errorText = _controller->errorText();
        return false;
    }

    // 4. Stop button.
    _stopButton = new StopButton(_backend, _settings.stopButtonOffset(), this);
    if(_stopButton->begin() == false) {
        logText(LVL_ERROR, QString("Stop button unavailable: %1").arg(_stopButton->errorText()));
    }
    else if(_stopButton->isHeld()) {
        logText(LVL_WARNING, "Stop button is held at startup; staying off");
    }

    // 5. Runner and scheduler.
    _runner = new ProgramRunner(_controller, _source, this);
    _clock = new SystemClock;
    _scheduler = new Scheduler(_source, _clock, this);

    // 6. Control server, last, on its own thread.
    _server = new IrrigationControlServer(_settings.databasePath());
    _server->setBindAddress(_settings.bindAddress());
    _server->setListenPort(_settings.listenPort());

    connectComponents();

    if(_server->start(TimeSpan::fromSeconds(10)) == false) {
        _errorText = "Control server thread failed to start";
        return false;
    }

    _scheduler->start();
    _running = true;
    return true;
}
```

⚠ The order in this function is a hardware contract. `ZoneController::begin()`
drives every line inactive; constructing the scheduler or the HTTP server before
it means a request can arrive while the valve state is whatever the bootloader
left. Adding a component means deciding where it goes relative to step 3, not
appending to the end.

`connectComponents()` wires:

```cpp
connect(_stopButton, &StopButton::pressed,           this, &IrrigationDaemon::onStopPressed);
connect(_server,     &IrrigationControlServer::stopRequested,
                                                     this, &IrrigationDaemon::onStopPressed);
connect(_server,     &IrrigationControlServer::manualZoneRunRequested,
                                                     this, &IrrigationDaemon::onManualZoneRun);
connect(_server,     &IrrigationControlServer::programRunRequested,
                                                     this, &IrrigationDaemon::onProgramRunRequested);
connect(_scheduler,  &Scheduler::programDue,         this, &IrrigationDaemon::onProgramDue);
connect(_controller, &ZoneController::zoneClosed,    _runner, &ProgramRunner::onZoneClosed);
connect(_controller, &ZoneController::watchdogTripped, this, &IrrigationDaemon::onWatchdogTripped);
```

No explicit connection types. The server lives on another thread and
`Qt::AutoConnection` routes across it correctly at emit time.

```cpp
void IrrigationDaemon::onStopPressed()
{
    logText(LVL_WARNING, "Stop requested");
    _runner->abort();
    _controller->allOff();
}

void IrrigationDaemon::onProgramDue(int programId, int startTimeId, const QDateTime& scheduledAtUtc)
{
    if(_runner->isRunning()) {
        FiredInstant instant;
        instant.programId = programId;
        instant.startTimeId = startTimeId;
        instant.scheduledAtUtc = scheduledAtUtc;
        instant.outcome = FiredInstant::Outcome::SkippedBusy;
        _source->recordFiring(instant);
        return;
    }

    _runner->startProgram(programId);
}
```

⚠ `abort()` is called before `allOff()`. Reverse them and `allOff()` emits
`zoneClosed`, the runner advances to the next zone, and the stop button opens a
valve.

- [ ] **Step 3: Implement stop() in reverse order**

```cpp
void IrrigationDaemon::stop()
{
    if(_running == false) {
        return;
    }

    _scheduler->stop();

    if(_server != nullptr) {
        _server->stop();      // blocks until the worker signals
        delete _server;       // never deleteLater(); its thread is gone
        _server = nullptr;
    }

    _runner->abort();
    _controller->allOff();

    delete _clock;
    _clock = nullptr;

    _running = false;
    logText(LVL_INFO, "Stopped irrigationd");
}
```

⚠ `allOff()` here is belt and braces, not the actual protection. The kernel
releases every requested line when this process exits and a released line
reverts to input. On a low-trigger relay board that opens all eight valves. The
external 10 kΩ pull-ups are what close them — see spec §4.4. Nothing in software
can substitute.

- [ ] **Step 4: Write the systemd unit**

`IrrigationD/systemd/irrigationd.service`:

```ini
[Unit]
Description=Irrigation controller
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/bin/irrigationd --config /etc/irrigationd.ini
Restart=always
RestartSec=5
User=root
StateDirectory=irrigationd
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

`Restart=always` is why `StopButton::begin()` reads the initial level: a restart
loop with the button held must not lose the fact that it is held.

Install it from CMake:

```cmake
install(FILES systemd/irrigationd.service DESTINATION lib/systemd/system)
```

- [ ] **Step 5: Build everything and run the full suite**

```bash
cmake --build build -j 32
ctest --test-dir build --output-on-failure
```

Expected: every suite passes.

- [ ] **Step 6: Cross-compile and deploy to the bench Pi**

```bash
source /opt/poky/5.2.4/environment-setup-cortexa72-poky-linux
cmake -S . -B build-arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$OECORE_NATIVE_SYSROOT/usr/share/cmake/OEToolchainConfig.cmake" \
  -DCMAKE_BUILD_TYPE=Debug -DIRRIGATION_USE_MOLD=OFF
cmake --build build-arm64 -j 32
rsync -a build-arm64/IrrigationD/irrigationd spunak@irrigation-dev.local:~/irrigation/bin/
```

Run it on the target with the Qt runtime already deployed there:

```bash
ssh spunak@irrigation-dev.local \
  'LD_LIBRARY_PATH=$HOME/irrigation/lib:/opt/qt6.10/lib QT_PLUGIN_PATH=/opt/qt6.10/plugins \
   ~/irrigation/bin/irrigationd -c ~/irrigation/irrigationd.ini -v'
```

- [ ] **Step 7: Commit**

```bash
git add IrrigationD
git commit -m "feat: assemble the daemon and add the systemd unit"
```

---

## Deferred to later plans

- **Web front end** (spec §8) — React 19 + Vite + TypeScript in `web/`. Its own plan.
- **Yocto layer** (spec §9) — `meta-rpi4-irrigation`, recipes, `config.txt` fragment, nginx. Its own plan.
- Weather-aware skip and runtime scaling; run history and reporting built on `fired_instants`; flow sensing; master valve output; read-only rootfs; `sd_notify` watchdog integration.

## Hardware bring-up

Spec §2.9 lists eight GPIO checks that must run on the target before this daemon
drives anything. They are not part of this plan and do not block it, but
**item 4 — running the self-destruct case under ASAN — gates trusting the edge
path**, and item 2 (STOP fires on press, not release) is the one behaviour the
in-memory backend certifies independently of the kernel.

---

## Spec coverage

| Spec | Requirement | Task |
|---|---|---|
| §5 layout | `main.cpp`, lifecycle owner, settings | 2 |
| §5.1 | Mutual exclusion in one atomic write | 5 |
| §5.1 | No open without a deadline | 5 |
| §5.1 | Duration clamp | 5 |
| §5.1 | Watchdog verifying against a **read-back**, not the cache | 1, 5 |
| §5.1 | `allOff()` callable from anywhere, always takes precedence | 5, 10 |
| §5.1 | Construction order: valves de-energised before scheduler or server exist | 10 |
| §5.2 | Valve owner and scheduler on the main event loop | 10 |
| §5.2 | Anything blocking or listening is an `AbstractThreadClass` | 9 |
| §5.2 | Route handlers emit rather than calling `ZoneController` | 9, 10 |
| §5.2 | Control server opens its own named DB connection | 9 |
| §5.2 | `start()` reports thread start, readiness is a separate query | 9 |
| §5.2 | Never `deleteLater()` an `AbstractThreadClass` | 9, 10 |
| §5.3 | Instants stored and transmitted as UTC | 4, 6 |
| §5.3 | Schedule rules stay local wall-clock plus IANA id | 3, 6 |
| §5.3 | `Reject` on spring forward, `PreferBefore` on fall back | 6 |
| §5.3 | `tzdata` required; missing zone db is a silent UTC fallback | 6 |
| §5.4 | One-second tick, day rules, rain delay, master enable | 6 |
| §5.4 | Every firing persisted; idempotent across restarts | 3, 4, 6 |
| §5.4 | Two-minute grace window, no catch-up | 6 |
| §5.4 | `DaysOfWeek`, `Odd`, `Even`, `EveryNDays` | 6 |
| §5.5 | Advanced by `zoneClosed()`, not its own timer | 7 |
| §5.5 | One runner at a time; `skipped_busy` recorded | 7, 10 |
| §5.5 | Manual run preempts | 10 |
| §5.6 | `InputPin` on BCM 25, pull-up, 20 ms debounce | 8 |
| §5.6 | Press calls `allOff()` and aborts the runner | 8, 10 |
| §6 | Every table, with the idempotency unique key | 3 |
| §6 | Versioned migrations, `.qrc` registration, backup-on-failure | 3 |
| §6 | 90-day prune at startup | 4, 10 |
| §6 | `journal_mode=WAL`, `synchronous=FULL` | 3 |
| §6 | Zone-to-GPIO map in INI, not the database | 2 |
| §7 | All fourteen endpoints | 9 |
| §7 | `/admin/status` carries the controller's timezone | 9 |
| §10 | `MockBackend` asserts exact line states | 5 |
| §10 | Injected clock for the scheduler | 6 |
| §10 | Dedicated tests for the four invariants | 5 |

### Deliberate departures from the spec

**1. `IrrigationSettings` does not derive `AppSettings`.** Spec §5 lists
`settings.{h,cpp}` as a `Kanoop::AppSettings` subclass. `AppSettings` builds its
own `QSettings` from the organisation and application name, which makes it
impossible to point at a temporary file, so every settings test would mutate the
developer's real config. This class holds a `QSettings` constructed from an
explicit path instead. Nothing in the daemon wants the recent-files or
last-directory machinery `AppSettings` exists to provide.

**2. `ProgramRunner` is tested against a real `ZoneController`, not a fake.**
Spec §10 says a fake. A real controller over a `MockBackend` is cheap, already
has its own tests, and cannot drift from the interface the runner calls — which a
hand-written fake does the first time a signature changes. The
`expireCloseTimerForTest()` seam is what makes it fast enough to prefer.

### Known gaps

- The `log_level` row in the `settings` table is written by the schema and read
  by nothing. Either wire it into `Log::setLevel()` at startup in Task 10 or drop
  the row; leaving a setting that silently does nothing is worse than either.
- `/admin/version` returns the compiled version only. There is no build-identity
  string (git SHA, build date) because nothing generates one yet.
