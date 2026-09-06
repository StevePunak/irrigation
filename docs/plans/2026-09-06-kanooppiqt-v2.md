# KanoopPiQt v2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert KanoopPiQt from a qmake/pigpio library into a CMake Qt6 library providing testable digital GPIO on libgpiod v2.

**Architecture:** An `IGpioBackend` interface separates GPIO mechanics from the classes that use them. `MockBackend` implements it in memory so every consumer is unit-testable on a development host with no GPIO hardware. `LibGpiodBackend` implements it over libgpiod v2. `OutputBank`, `OutputPin` and `InputPin` are written against the interface and never touch libgpiod directly.

**Tech Stack:** Qt 6.10, CMake, libgpiod 2.x (2.2.5 on Fedora, 2.2.2 in Yocto walnascar), QTest.

**Spec:** `docs/design/2026-09-05-irrigation-design.md` (section 4)

**Repository:** `~/src/punak/KanoopPiQt` — its own git repo (`git@github.com:StevePunak/KanoopPiQt.git`), currently on `master` at `aad72ea`. All work in this plan happens there, on a branch. The `irrigation` superproject consumes it as a submodule in Task 7.

## Global Constraints

- `set(CMAKE_CXX_STANDARD 11)` — matches KanoopCommonQt, libEpcCommonQt and libEpcSimQt. Applications use 20; libraries do not.
- `target_compile_options(${PROJ} PRIVATE -Wextra -Wall -Werror)` — warnings are errors.
- Public headers live in `include/Kanoop/pi/`, sources in `src/`. Consumers include `<Kanoop/pi/outputbank.h>`.
- **Global namespace.** Kanoop libraries do not namespace their types: the include path carries the scoping, the class name does not. `MutexEvent`, `AbstractThreadClass`, `OutputBank`. Enum values are scoped inside a holder class instead.
- **Every `.cpp` whose header declares `Q_OBJECT` ends with its moc include**, path-prefixed relative to the include root, as the last line preceded by a blank line: `#include "Kanoop/pi/moc_outputbank.cpp"`. AUTOMOC's header search is non-recursive, so the prefix is required for a split `include/`+`src/` layout.
- **Split-layout libraries must suppress Qt metatype generation** or `moc --collect-json` fails on a clean build. Use `qt_add_library(... MANUAL_FINALIZATION ...)`, point `INTERFACE_QT_META_TYPES_BUILD_FILE` at an empty file, then `qt_finalize_target()`.
- Code style per `meta-qt-mains/.claude/docs/codestyle-cpp.md`: `_underscorePrefixed` members, `camelCase` methods, `PascalCase` classes, `if(` with no space, explicit `== false` rather than `!`, function opening brace on its own line, control-structure brace on the same line, `catch`/`else` on their own line.
- Doxygen on every public member. Single line: `/** @brief Summary. */`. Multi-line puts `@brief` on the line after `/**`.
- Commit messages are conventional commits and end with the standard trailer:
  `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`
- **Never `git push`.** Commits stay local.

---

### Task 1: Build skeleton, legacy cleanup, test harness

Converts the build and leaves a library that compiles to nothing useful but has a working test target. Nothing else can be tested until this exists.

**Files:**
- Create: `CMakeLists.txt`
- Create: `libKanoopPi.pc.in`
- Create: `include/Kanoop/pi/kanooppi.h`
- Create: `tests/CMakeLists.txt`
- Create: `tests/tst_smoke.cpp`
- Create: `legacy/README.md`
- Delete: `KanoopPiQt.pro`, `pigs.h`, `pigs.cpp`, `pigcommand.h`, `pigcommand.cpp`, `gpioreader.h`, `gpioreader.cpp`, `gpio.h`, `kanooppiqt.h`, `kanooppiqt.cpp`, `kanooppiqt_global.h`, `deploy.sh`
- Move: `i2c.h`, `i2c.cpp`, `devices/ads1115.{h,cpp}`, `devices/bmp280.{h,cpp}` → `legacy/`

**Interfaces:**
- Consumes: nothing.
- Produces: CMake target `KanoopPiQt`; export macro `KANOOPPI_EXPORT`; test helper function `add_kanooppi_test(name)`.

- [ ] **Step 1: Install the libgpiod development package**

```bash
sudo dnf install -y libgpiod-devel
pkg-config --modversion libgpiod
```

Expected: `2.2.5` or later. The 2.x API is required; the 1.x API shares almost no symbols with it.

- [ ] **Step 2: Create the branch**

```bash
cd ~/src/punak/KanoopPiQt
git checkout -b feature/libgpiod-v2
```

- [ ] **Step 3: Remove the pigpio-based sources and the qmake project**

```bash
cd ~/src/punak/KanoopPiQt
git rm -q pigs.h pigs.cpp pigcommand.h pigcommand.cpp \
          gpioreader.h gpioreader.cpp gpio.h \
          kanooppiqt.h kanooppiqt.cpp kanooppiqt_global.h \
          KanoopPiQt.pro deploy.sh
```

- [ ] **Step 4: Move the unported code to `legacy/`**

```bash
cd ~/src/punak/KanoopPiQt
mkdir -p legacy/devices
git mv i2c.h i2c.cpp legacy/
git mv devices/ads1115.h devices/ads1115.cpp devices/bmp280.h devices/bmp280.cpp legacy/devices/
rmdir devices
```

- [ ] **Step 5: Write `legacy/README.md`**

```markdown
# Unported code

These files predate the libgpiod v2 conversion and are excluded from the build.

- `i2c.{h,cpp}` — a stub over the I2C ioctls, never completed.
- `devices/ads1115.{h,cpp}` — ADS1115 ADC driver.
- `devices/bmp280.{h,cpp}` — BMP280 pressure/temperature driver.

Port them when a consuming project needs them. They were written against the
pigpio daemon's transport, which no longer exists in this library.
```

- [ ] **Step 6: Write `include/Kanoop/pi/kanooppi.h`**

```cpp
#ifndef KANOOPPI_H
#define KANOOPPI_H

#include <QtGlobal>

#if defined(KANOOPPI_LIBRARY)
#  define KANOOPPI_EXPORT Q_DECL_EXPORT
#else
#  define KANOOPPI_EXPORT Q_DECL_IMPORT
#endif

#endif // KANOOPPI_H
```

- [ ] **Step 7: Write `libKanoopPi.pc.in`**

```
prefix=@CMAKE_INSTALL_PREFIX@
exec_prefix=${prefix}
libdir=${prefix}/@CMAKE_INSTALL_LIBDIR@
includedir=${prefix}/@CMAKE_INSTALL_INCLUDEDIR@

Name: libKanoopPi
Description: Raspberry Pi GPIO for Qt
Version: @KANOOP_PI_VERSION@
Requires: libgpiod
Libs: -L${libdir} -lKanoopPiQt
Cflags: -I${includedir}
```

- [ ] **Step 8: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.16)
include(GNUInstallDirs)

set(PROJ KanoopPiQt)
project(${PROJ})

find_package(Qt6 REQUIRED COMPONENTS Core)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GPIOD REQUIRED IMPORTED_TARGET libgpiod>=2.0)
qt_standard_project_setup()

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(KANOOP_PI_VERSION "2.0.0")

file(GLOB_RECURSE KANOOP_PI_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE KANOOP_PI_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")

qt_add_library(${PROJ} MANUAL_FINALIZATION ${KANOOP_PI_SOURCES} ${KANOOP_PI_HEADERS})

target_compile_options(${PROJ} PRIVATE -Wextra -Wall -Werror)

set_target_properties(${PROJ} PROPERTIES VERSION "${KANOOP_PI_VERSION}")
set_target_properties(${PROJ} PROPERTIES SOVERSION 2)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/libKanoopPi.pc.in" "libKanoopPi.pc" @ONLY)

target_include_directories(${PROJ} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(${PROJ} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/Kanoop)
target_include_directories(${PROJ} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(${PROJ} PRIVATE Qt6::Core PkgConfig::GPIOD)

add_compile_definitions(KANOOPPI_LIBRARY)
add_compile_definitions(QT_DEPRECATED_WARNINGS)
add_compile_definitions(QT_DISABLE_DEPRECATED_BEFORE=0x060000)

if (DEFINED $ENV{KYBER_OUTPUT_PREFIX})
    set(KANOOP_INSTALL_PREFIX "$ENV{KYBER_OUTPUT_PREFIX}/")
endif()

set(_no_metatypes "${CMAKE_CURRENT_BINARY_DIR}/${PROJ}_no_metatypes.json")
file(TOUCH "${_no_metatypes}")
set_target_properties(${PROJ} PROPERTIES INTERFACE_QT_META_TYPES_BUILD_FILE "${_no_metatypes}")
qt_finalize_target(${PROJ})

install(TARGETS ${PROJ} LIBRARY DESTINATION "${KANOOP_INSTALL_PREFIX}${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/Kanoop" DESTINATION "${KANOOP_INSTALL_PREFIX}${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_INCLUDEDIR}" FILES_MATCHING PATTERN "*.h")

option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
```

- [ ] **Step 9: Write `tests/CMakeLists.txt`**

```cmake
find_package(Qt6 REQUIRED COMPONENTS Test)

function(add_kanooppi_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE
        Qt6::Test
        Qt6::Core
        KanoopPiQt
    )
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    )
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_kanooppi_test(tst_smoke)
```

- [ ] **Step 10: Write the failing test `tests/tst_smoke.cpp`**

```cpp
#include <QTest>

#include <Kanoop/pi/kanooppi.h>

class TstSmoke : public QObject
{
    Q_OBJECT

private slots:
    void exportMacroIsDefined()
    {
        QVERIFY(true);
    }
};

QTEST_MAIN(TstSmoke)
#include "tst_smoke.moc"
```

- [ ] **Step 11: Create an empty `src/` so the glob has something to find**

CMake's `GLOB_RECURSE` over an absent `src/` yields an empty source list and `qt_add_library` fails with no sources. Give it one translation unit.

Create `src/Kanoop/pi/kanooppi.cpp`:

```cpp
#include "Kanoop/pi/kanooppi.h"
```

- [ ] **Step 12: Configure and build**

```bash
cd ~/src/punak/KanoopPiQt
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

Expected: configure reports the libgpiod version found; build succeeds with no warnings.

- [ ] **Step 13: Run the test**

```bash
cd ~/src/punak/KanoopPiQt/build && ctest --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 1`.

- [ ] **Step 14: Commit**

```bash
cd ~/src/punak/KanoopPiQt
printf 'build/\n*.user\n' > .gitignore
git add -A
git commit -m "refactor: convert to CMake and retire the pigpio backend

Replaces the qmake project with the KanoopCommonQt CMake layout: split
include/ and src/ trees, MANUAL_FINALIZATION with metatype generation
suppressed, and a QTest target behind BUILD_TESTING.

The pigpio daemon client is removed. It does not function on Raspberry Pi 5,
whose GPIO moved to the RP1 southbridge. The I2C stub and the ADS1115 and
BMP280 drivers move to legacy/ pending a port.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Gpio types, IGpioBackend, MockBackend

The seam the whole library is built on. `MockBackend` is a first-class deliverable, not test scaffolding — every later task depends on it.

**Files:**
- Create: `include/Kanoop/pi/gpio.h`
- Create: `include/Kanoop/pi/gpiobackend.h`
- Create: `src/Kanoop/pi/gpiobackend.cpp`
- Create: `include/Kanoop/pi/mockbackend.h`
- Create: `src/Kanoop/pi/mockbackend.cpp`
- Modify: `tests/CMakeLists.txt`
- Create: `tests/tst_mockbackend.cpp`

**Interfaces:**
- Consumes: `KANOOPPI_EXPORT` from Task 1.
- Produces:
  - `Gpio::Value` — `Inactive = 0`, `Active = 1`
  - `Gpio::Bias` — `AsIs`, `Disabled`, `PullUp`, `PullDown`
  - `Gpio::Edge` — `None`, `Rising`, `Falling`, `Both`
  - `Gpio::RequestHandle` — `quint64`, `Gpio::InvalidRequest == 0`
  - `Gpio::OutputRequest` — `consumer`, `offsets`, `activeLow`, `initialValue`
  - `Gpio::InputRequest` — `consumer`, `offsets`, `activeLow`, `bias`, `edge`, `debounceMicroseconds`
  - `IGpioBackend` — `openChipByLabel(QString)`, `openChipByPath(QString)`, `closeChip()`, `isOpen()`, `requestOutputs(OutputRequest)`, `requestInputs(InputRequest)`, `release(RequestHandle)`, `setValues(RequestHandle, QList<quint32>, QList<Gpio::Value>)`, `errorText()`, signal `edgeEvent(quint32, Gpio::Edge)`
  - `MockBackend` — the above plus `simulateEdge(quint32, Gpio::Edge)`, `lineValue(quint32)`, `isRequested(quint32)`, `setValuesCallCount()`, `lastSetOffsets()`, `setFailNextRequest(bool)`

- [ ] **Step 1: Write the failing test `tests/tst_mockbackend.cpp`**

```cpp
#include <QSignalSpy>
#include <QTest>

#include <Kanoop/pi/mockbackend.h>

class TstMockBackend : public QObject
{
    Q_OBJECT

private slots:
    void requestFailsWhenChipIsClosed()
    {
        MockBackend backend;
        Gpio::OutputRequest request;
        request.offsets = QList<quint32>() << 5;

        QCOMPARE(backend.requestOutputs(request), Gpio::InvalidRequest);
        QVERIFY(backend.errorText().isEmpty() == false);
    }

    void requestRecordsInitialValue()
    {
        MockBackend backend;
        QVERIFY(backend.openChipByLabel(QString("test")));

        Gpio::OutputRequest request;
        request.offsets = QList<quint32>() << 5 << 6;
        request.initialValue = Gpio::Value::Active;

        Gpio::RequestHandle handle = backend.requestOutputs(request);
        QVERIFY(handle != Gpio::InvalidRequest);
        QCOMPARE(backend.lineValue(5), Gpio::Value::Active);
        QCOMPARE(backend.lineValue(6), Gpio::Value::Active);
        QVERIFY(backend.isRequested(5));
    }

    void secondRequestOfTheSameLineFails()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));

        Gpio::OutputRequest first;
        first.offsets = QList<quint32>() << 5;
        QVERIFY(backend.requestOutputs(first) != Gpio::InvalidRequest);

        Gpio::OutputRequest second;
        second.offsets = QList<quint32>() << 5;
        QCOMPARE(backend.requestOutputs(second), Gpio::InvalidRequest);
    }

    void setValuesRejectsForeignLines()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));

        Gpio::OutputRequest request;
        request.offsets = QList<quint32>() << 5;
        Gpio::RequestHandle handle = backend.requestOutputs(request);

        QList<quint32> offsets = QList<quint32>() << 9;
        QList<Gpio::Value> values = QList<Gpio::Value>() << Gpio::Value::Active;
        QCOMPARE(backend.setValues(handle, offsets, values), false);
    }

    void releaseFreesTheLines()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));

        Gpio::OutputRequest request;
        request.offsets = QList<quint32>() << 5;
        Gpio::RequestHandle handle = backend.requestOutputs(request);

        backend.release(handle);
        QCOMPARE(backend.isRequested(5), false);
    }

    void simulateEdgeEmitsTheSignal()
    {
        MockBackend backend;
        QSignalSpy spy(&backend, &IGpioBackend::edgeEvent);

        backend.simulateEdge(25, Gpio::Edge::Falling);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toUInt(), 25u);
        QCOMPARE(spy.at(0).at(1).value<Gpio::Edge>(), Gpio::Edge::Falling);
    }

    void failNextRequestIsOneShot()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));
        backend.setFailNextRequest(true);

        Gpio::OutputRequest request;
        request.offsets = QList<quint32>() << 5;

        QCOMPARE(backend.requestOutputs(request), Gpio::InvalidRequest);
        QVERIFY(backend.requestOutputs(request) != Gpio::InvalidRequest);
    }
};

QTEST_MAIN(TstMockBackend)
#include "tst_mockbackend.moc"
```

- [ ] **Step 2: Register the test**

Add to `tests/CMakeLists.txt`, after the `add_kanooppi_test(tst_smoke)` line:

```cmake
add_kanooppi_test(tst_mockbackend)
```

- [ ] **Step 3: Run the test to verify it fails**

```bash
cd ~/src/punak/KanoopPiQt && cmake --build build --parallel
```

Expected: `fatal error: Kanoop/pi/mockbackend.h: No such file or directory`.

- [ ] **Step 4: Write `include/Kanoop/pi/gpio.h`**

```cpp
#ifndef KANOOP_PI_GPIO_H
#define KANOOP_PI_GPIO_H

#include <QList>
#include <QString>

#include <Kanoop/pi/kanooppi.h>

/** @brief Scoping holder for GPIO enumerations and request descriptors. */
class KANOOPPI_EXPORT Gpio
{
public:
    /** @brief Logical line level, after any active-low inversion. */
    enum class Value
    {
        Inactive = 0,
        Active = 1
    };

    /** @brief Internal bias applied to an input line. */
    enum class Bias
    {
        AsIs,
        Disabled,
        PullUp,
        PullDown
    };

    /** @brief Edge detection selection for an input line. */
    enum class Edge
    {
        None,
        Rising,
        Falling,
        Both
    };

    /** @brief Opaque identifier for a set of requested lines. */
    typedef quint64 RequestHandle;

    /** @brief Value returned by a failed request. */
    static const RequestHandle InvalidRequest = 0;

    /** @brief Parameters for requesting a set of output lines. */
    class OutputRequest
    {
    public:
        QString consumer;
        QList<quint32> offsets;
        bool activeLow = false;
        Value initialValue = Value::Inactive;
    };

    /** @brief Parameters for requesting a set of input lines. */
    class InputRequest
    {
    public:
        QString consumer;
        QList<quint32> offsets;
        bool activeLow = false;
        Bias bias = Bias::AsIs;
        Edge edge = Edge::None;
        unsigned long debounceMicroseconds = 0;
    };
};

Q_DECLARE_METATYPE(Gpio::Value)
Q_DECLARE_METATYPE(Gpio::Bias)
Q_DECLARE_METATYPE(Gpio::Edge)

#endif // KANOOP_PI_GPIO_H
```

The three enumerations are declared to the meta-object system because
`QSignalSpy` stores signal arguments as `QVariant`; without the declaration
`spy.at(0).at(1).value<Gpio::Edge>()` silently yields a default-constructed
value and an edge assertion passes for the wrong reason.


- [ ] **Step 5: Write `include/Kanoop/pi/gpiobackend.h`**

```cpp
#ifndef KANOOP_PI_GPIOBACKEND_H
#define KANOOP_PI_GPIOBACKEND_H

#include <QObject>

#include <Kanoop/pi/gpio.h>

/**
 * @brief Abstract GPIO transport.
 *
 * Consumers of this library are written against this interface so they can be
 * exercised with MockBackend on a host with no GPIO hardware.
 */
class KANOOPPI_EXPORT IGpioBackend : public QObject
{
    Q_OBJECT
public:
    explicit IGpioBackend(QObject* parent = nullptr) :
        QObject(parent) {}

    virtual ~IGpioBackend() {}

    /** @brief Opens the chip whose kernel label matches @p label. @return True on success. */
    virtual bool openChipByLabel(const QString& label) = 0;

    /** @brief Opens the chip at character device @p path. @return True on success. */
    virtual bool openChipByPath(const QString& path) = 0;

    /** @brief Releases every request and closes the chip. */
    virtual void closeChip() = 0;

    /** @brief Returns true when a chip is open. */
    virtual bool isOpen() const = 0;

    /** @brief Requests @p request as output lines. @return A handle, or Gpio::InvalidRequest on failure. */
    virtual Gpio::RequestHandle requestOutputs(const Gpio::OutputRequest& request) = 0;

    /** @brief Requests @p request as input lines. @return A handle, or Gpio::InvalidRequest on failure. */
    virtual Gpio::RequestHandle requestInputs(const Gpio::InputRequest& request) = 0;

    /** @brief Releases the lines held by @p handle. */
    virtual void release(Gpio::RequestHandle handle) = 0;

    /**
     * @brief Writes @p values to @p offsets in a single operation.
     * @return True on success.
     */
    virtual bool setValues(Gpio::RequestHandle handle,
                           const QList<quint32>& offsets,
                           const QList<Gpio::Value>& values) = 0;

    /** @brief Returns the text of the most recent failure. */
    QString errorText() const { return _errorText; }

signals:
    /** @brief Emitted when an edge is detected on a requested input line. */
    void edgeEvent(quint32 offset, Gpio::Edge edge);

protected:
    /** @brief Records @p value as the most recent failure text. */
    void setErrorText(const QString& value) { _errorText = value; }

private:
    QString _errorText;
};

#endif // KANOOP_PI_GPIOBACKEND_H
```

- [ ] **Step 6: Write `src/Kanoop/pi/gpiobackend.cpp`**

```cpp
#include "Kanoop/pi/gpiobackend.h"

#include "Kanoop/pi/moc_gpiobackend.cpp"
```

The moc include is the last line and is path-prefixed. AUTOMOC will not find the header from an unprefixed include in this layout.

- [ ] **Step 7: Write `include/Kanoop/pi/mockbackend.h`**

```cpp
#ifndef KANOOP_PI_MOCKBACKEND_H
#define KANOOP_PI_MOCKBACKEND_H

#include <QMap>

#include <Kanoop/pi/gpiobackend.h>

/**
 * @brief In-memory IGpioBackend for tests and for running a consumer on a host
 *        with no GPIO hardware.
 */
class KANOOPPI_EXPORT MockBackend : public IGpioBackend
{
    Q_OBJECT
public:
    explicit MockBackend(QObject* parent = nullptr);

    virtual bool openChipByLabel(const QString& label) override;
    virtual bool openChipByPath(const QString& path) override;
    virtual void closeChip() override;
    virtual bool isOpen() const override { return _open; }

    virtual Gpio::RequestHandle requestOutputs(const Gpio::OutputRequest& request) override;
    virtual Gpio::RequestHandle requestInputs(const Gpio::InputRequest& request) override;
    virtual void release(Gpio::RequestHandle handle) override;

    virtual bool setValues(Gpio::RequestHandle handle,
                           const QList<quint32>& offsets,
                           const QList<Gpio::Value>& values) override;

    /** @brief Emits edgeEvent() for @p offset as though hardware had detected @p edge. */
    void simulateEdge(quint32 offset, Gpio::Edge edge);

    /** @brief Returns the current logical value of @p offset. */
    Gpio::Value lineValue(quint32 offset) const { return _values.value(offset, Gpio::Value::Inactive); }

    /** @brief Returns true when @p offset belongs to a live request. */
    bool isRequested(quint32 offset) const { return _owners.contains(offset); }

    /** @brief Returns the number of setValues() calls since construction. */
    int setValuesCallCount() const { return _setValuesCallCount; }

    /** @brief Returns the offsets passed to the most recent setValues() call. */
    QList<quint32> lastSetOffsets() const { return _lastSetOffsets; }

    /** @brief Returns the label or path the chip was opened with. */
    QString openedWith() const { return _openedWith; }

    /** @brief Returns the request descriptor recorded for the most recent requestInputs(). */
    Gpio::InputRequest lastInputRequest() const { return _lastInputRequest; }

    /** @brief When set, the next request call fails and clears the flag. */
    void setFailNextRequest(bool value) { _failNextRequest = value; }

private:
    bool _open = false;
    bool _failNextRequest = false;
    QString _openedWith;
    Gpio::RequestHandle _nextHandle = 1;
    QMap<quint32, Gpio::Value> _values;
    QMap<quint32, Gpio::RequestHandle> _owners;
    Gpio::InputRequest _lastInputRequest;
    int _setValuesCallCount = 0;
    QList<quint32> _lastSetOffsets;
};

#endif // KANOOP_PI_MOCKBACKEND_H
```

- [ ] **Step 8: Write `src/Kanoop/pi/mockbackend.cpp`**

```cpp
#include "Kanoop/pi/mockbackend.h"

MockBackend::MockBackend(QObject* parent) :
    IGpioBackend(parent)
{
}

bool MockBackend::openChipByLabel(const QString& label)
{
    _open = true;
    _openedWith = label;
    return true;
}

bool MockBackend::openChipByPath(const QString& path)
{
    _open = true;
    _openedWith = path;
    return true;
}

void MockBackend::closeChip()
{
    _open = false;
    _owners.clear();
    _values.clear();
}

Gpio::RequestHandle MockBackend::requestOutputs(const Gpio::OutputRequest& request)
{
    if(_open == false) {
        setErrorText(QString("chip is not open"));
        return Gpio::InvalidRequest;
    }

    if(_failNextRequest == true) {
        _failNextRequest = false;
        setErrorText(QString("injected failure"));
        return Gpio::InvalidRequest;
    }

    for(quint32 offset : request.offsets) {
        if(_owners.contains(offset) == true) {
            setErrorText(QString("line %1 is already requested").arg(offset));
            return Gpio::InvalidRequest;
        }
    }

    Gpio::RequestHandle handle = _nextHandle++;
    for(quint32 offset : request.offsets) {
        _owners.insert(offset, handle);
        _values.insert(offset, request.initialValue);
    }
    return handle;
}

Gpio::RequestHandle MockBackend::requestInputs(const Gpio::InputRequest& request)
{
    if(_open == false) {
        setErrorText(QString("chip is not open"));
        return Gpio::InvalidRequest;
    }

    if(_failNextRequest == true) {
        _failNextRequest = false;
        setErrorText(QString("injected failure"));
        return Gpio::InvalidRequest;
    }

    Gpio::RequestHandle handle = _nextHandle++;
    for(quint32 offset : request.offsets) {
        _owners.insert(offset, handle);
    }
    _lastInputRequest = request;
    return handle;
}

void MockBackend::release(Gpio::RequestHandle handle)
{
    QList<quint32> offsets = _owners.keys(handle);
    for(quint32 offset : offsets) {
        _owners.remove(offset);
        _values.remove(offset);
    }
}

bool MockBackend::setValues(Gpio::RequestHandle handle,
                            const QList<quint32>& offsets,
                            const QList<Gpio::Value>& values)
{
    _setValuesCallCount++;
    _lastSetOffsets = offsets;

    if(offsets.count() != values.count()) {
        setErrorText(QString("offset and value counts differ"));
        return false;
    }

    for(int i = 0;i < offsets.count();i++) {
        if(_owners.value(offsets.at(i), Gpio::InvalidRequest) != handle) {
            setErrorText(QString("line %1 is not held by this request").arg(offsets.at(i)));
            return false;
        }
    }

    for(int i = 0;i < offsets.count();i++) {
        _values.insert(offsets.at(i), values.at(i));
    }
    return true;
}

void MockBackend::simulateEdge(quint32 offset, Gpio::Edge edge)
{
    emit edgeEvent(offset, edge);
}

#include "Kanoop/pi/moc_mockbackend.cpp"
```

- [ ] **Step 9: Build and run**

```bash
cd ~/src/punak/KanoopPiQt && cmake --build build --parallel && cd build && ctest --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 2`.

- [ ] **Step 10: Commit**

```bash
cd ~/src/punak/KanoopPiQt
git add -A
git commit -m "feat: add the GPIO backend interface and an in-memory mock

IGpioBackend is the seam every consumer in this library is written against.
MockBackend implements it in memory with inspection accessors and edge
injection, so consumers can be unit tested on a host with no GPIO hardware.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: OutputBank

The multi-line output primitive. Its atomic `setValues()` is what lets a caller close one line and open another with no interval where both are active.

**Files:**
- Create: `include/Kanoop/pi/outputbank.h`
- Create: `src/Kanoop/pi/outputbank.cpp`
- Modify: `tests/CMakeLists.txt`
- Create: `tests/tst_outputbank.cpp`

**Interfaces:**
- Consumes: `IGpioBackend`, `MockBackend`, `Gpio::*` from Task 2.
- Produces: `OutputBank(IGpioBackend*, QString consumer, QList<quint32> offsets, bool activeLow, QObject* parent = nullptr)`; `request()`, `release()`, `isRequested()`, `setValue(quint32, bool)`, `setValues(QMap<quint32,bool>)`, `allInactive()`, `isActive(quint32)`, `offsets()`, `errorText()`.

- [ ] **Step 1: Write the failing test `tests/tst_outputbank.cpp`**

```cpp
#include <QTest>

#include <Kanoop/pi/mockbackend.h>
#include <Kanoop/pi/outputbank.h>

class TstOutputBank : public QObject
{
    Q_OBJECT

private:
    static QList<quint32> zoneOffsets()
    {
        return QList<quint32>() << 5 << 6 << 13 << 16;
    }

private slots:
    void requestOpensAllLinesInactive()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));
        OutputBank bank(&backend, QString("test"), zoneOffsets(), true);

        QVERIFY(bank.request());
        QVERIFY(bank.isRequested());
        for(quint32 offset : zoneOffsets()) {
            QCOMPARE(bank.isActive(offset), false);
        }
    }

    void requestFailurePropagates()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));
        backend.setFailNextRequest(true);
        OutputBank bank(&backend, QString("test"), zoneOffsets(), true);

        QCOMPARE(bank.request(), false);
        QCOMPARE(bank.isRequested(), false);
        QVERIFY(bank.errorText().isEmpty() == false);
    }

    void setValueDrivesOneLine()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));
        OutputBank bank(&backend, QString("test"), zoneOffsets(), true);
        bank.request();

        QVERIFY(bank.setValue(13, true));
        QCOMPARE(bank.isActive(13), true);
        QCOMPARE(bank.isActive(5), false);
        QCOMPARE(backend.lineValue(13), Gpio::Value::Active);
    }

    void setValuesIsOneBackendCall()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));
        OutputBank bank(&backend, QString("test"), zoneOffsets(), true);
        bank.request();

        int before = backend.setValuesCallCount();

        QMap<quint32, bool> values;
        values.insert(6, false);
        values.insert(13, true);
        QVERIFY(bank.setValues(values));

        QCOMPARE(backend.setValuesCallCount(), before + 1);
        QCOMPARE(bank.isActive(13), true);
        QCOMPARE(bank.isActive(6), false);
    }

    void setValueRejectsAnUnownedOffset()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));
        OutputBank bank(&backend, QString("test"), zoneOffsets(), true);
        bank.request();

        QCOMPARE(bank.setValue(99, true), false);
        QVERIFY(bank.errorText().contains(QString("99")));
    }

    void setValueFailsBeforeRequest()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));
        OutputBank bank(&backend, QString("test"), zoneOffsets(), true);

        QCOMPARE(bank.setValue(5, true), false);
    }

    void allInactiveClearsEveryLineInOneCall()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));
        OutputBank bank(&backend, QString("test"), zoneOffsets(), true);
        bank.request();
        bank.setValue(5, true);
        bank.setValue(16, true);

        int before = backend.setValuesCallCount();
        QVERIFY(bank.allInactive());

        QCOMPARE(backend.setValuesCallCount(), before + 1);
        QCOMPARE(backend.lastSetOffsets().count(), zoneOffsets().count());
        for(quint32 offset : zoneOffsets()) {
            QCOMPARE(bank.isActive(offset), false);
        }
    }

    void activeLowIsPassedToTheBackendRatherThanInvertedHere()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));
        OutputBank bank(&backend, QString("test"), zoneOffsets(), true);
        bank.request();
        bank.setValue(5, true);

        QCOMPARE(backend.lineValue(5), Gpio::Value::Active);
    }

    void destructorReleasesTheLines()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));
        {
            OutputBank bank(&backend, QString("test"), zoneOffsets(), true);
            bank.request();
            QCOMPARE(backend.isRequested(5), true);
        }
        QCOMPARE(backend.isRequested(5), false);
    }
};

QTEST_MAIN(TstOutputBank)
#include "tst_outputbank.moc"
```

The `activeLowIsPassedToTheBackend...` test pins an important decision: `OutputBank` speaks in logical terms. `setValue(offset, true)` means "energise this load", and the kernel performs the electrical inversion because `activeLow` was set on the line request. Inverting in both places yields a bank that opens every valve when asked to close them.

- [ ] **Step 2: Register the test**

Add to `tests/CMakeLists.txt`:

```cmake
add_kanooppi_test(tst_outputbank)
```

- [ ] **Step 3: Run it to verify it fails**

```bash
cd ~/src/punak/KanoopPiQt && cmake --build build --parallel
```

Expected: `fatal error: Kanoop/pi/outputbank.h: No such file or directory`.

- [ ] **Step 4: Write `include/Kanoop/pi/outputbank.h`**

```cpp
#ifndef KANOOP_PI_OUTPUTBANK_H
#define KANOOP_PI_OUTPUTBANK_H

#include <QMap>
#include <QObject>

#include <Kanoop/pi/gpiobackend.h>

/**
 * @brief A set of output lines requested and driven together.
 *
 * setValues() writes every line in one backend operation, so a caller can
 * deactivate one line and activate another with no interval in which both are
 * active.
 *
 * Values are logical. setValue(offset, true) energises the load; the kernel
 * performs any electrical inversion because activeLow was set on the request.
 */
class KANOOPPI_EXPORT OutputBank : public QObject
{
    Q_OBJECT
public:
    OutputBank(IGpioBackend* backend,
               const QString& consumer,
               const QList<quint32>& offsets,
               bool activeLow,
               QObject* parent = nullptr);

    virtual ~OutputBank();

    /** @brief Requests the lines, all inactive. @return True on success. */
    bool request();

    /** @brief Releases the lines if held. */
    void release();

    /** @brief Returns true while the lines are held. */
    bool isRequested() const { return _handle != Gpio::InvalidRequest; }

    /** @brief Drives @p offset to @p active. @return True on success. */
    bool setValue(quint32 offset, bool active);

    /** @brief Drives every entry of @p values in a single operation. @return True on success. */
    bool setValues(const QMap<quint32, bool>& values);

    /** @brief Drives every line in the bank inactive in a single operation. @return True on success. */
    bool allInactive();

    /** @brief Returns the last value written to @p offset. */
    bool isActive(quint32 offset) const { return _active.value(offset, false); }

    /** @brief Returns the line offsets in this bank. */
    QList<quint32> offsets() const { return _offsets; }

    /** @brief Returns the text of the most recent failure. */
    QString errorText() const { return _errorText; }

private:
    bool ownsAll(const QList<quint32>& offsets);

    IGpioBackend* _backend;
    QString _consumer;
    QList<quint32> _offsets;
    bool _activeLow;
    Gpio::RequestHandle _handle = Gpio::InvalidRequest;
    QMap<quint32, bool> _active;
    QString _errorText;
};

#endif // KANOOP_PI_OUTPUTBANK_H
```

- [ ] **Step 5: Write `src/Kanoop/pi/outputbank.cpp`**

```cpp
#include "Kanoop/pi/outputbank.h"

OutputBank::OutputBank(IGpioBackend* backend,
                       const QString& consumer,
                       const QList<quint32>& offsets,
                       bool activeLow,
                       QObject* parent) :
    QObject(parent),
    _backend(backend), _consumer(consumer),
    _offsets(offsets), _activeLow(activeLow)
{
}

OutputBank::~OutputBank()
{
    release();
}

bool OutputBank::request()
{
    if(isRequested() == true) {
        return true;
    }

    Gpio::OutputRequest request;
    request.consumer = _consumer;
    request.offsets = _offsets;
    request.activeLow = _activeLow;
    request.initialValue = Gpio::Value::Inactive;

    _handle = _backend->requestOutputs(request);
    if(_handle == Gpio::InvalidRequest) {
        _errorText = _backend->errorText();
        return false;
    }

    for(quint32 offset : _offsets) {
        _active.insert(offset, false);
    }
    return true;
}

void OutputBank::release()
{
    if(isRequested() == false) {
        return;
    }

    _backend->release(_handle);
    _handle = Gpio::InvalidRequest;
    _active.clear();
}

bool OutputBank::setValue(quint32 offset, bool active)
{
    QMap<quint32, bool> values;
    values.insert(offset, active);
    return setValues(values);
}

bool OutputBank::setValues(const QMap<quint32, bool>& values)
{
    if(isRequested() == false) {
        _errorText = QString("bank is not requested");
        return false;
    }

    QList<quint32> offsets = values.keys();
    if(ownsAll(offsets) == false) {
        return false;
    }

    QList<Gpio::Value> lineValues;
    for(quint32 offset : offsets) {
        lineValues.append(values.value(offset) == true
                          ? Gpio::Value::Active
                          : Gpio::Value::Inactive);
    }

    if(_backend->setValues(_handle, offsets, lineValues) == false) {
        _errorText = _backend->errorText();
        return false;
    }

    for(quint32 offset : offsets) {
        _active.insert(offset, values.value(offset));
    }
    return true;
}

bool OutputBank::allInactive()
{
    QMap<quint32, bool> values;
    for(quint32 offset : _offsets) {
        values.insert(offset, false);
    }
    return setValues(values);
}

bool OutputBank::ownsAll(const QList<quint32>& offsets)
{
    for(quint32 offset : offsets) {
        if(_offsets.contains(offset) == false) {
            _errorText = QString("line %1 is not in this bank").arg(offset);
            return false;
        }
    }
    return true;
}

#include "Kanoop/pi/moc_outputbank.cpp"
```

- [ ] **Step 6: Build and run**

```bash
cd ~/src/punak/KanoopPiQt && cmake --build build --parallel && cd build && ctest --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 3`.

- [ ] **Step 7: Commit**

```bash
cd ~/src/punak/KanoopPiQt
git add -A
git commit -m "feat: add OutputBank for atomically driven output lines

Requests a set of lines together and writes them in one backend operation, so
a caller can deactivate one line and activate another with no interval in
which both are active.

Values are logical throughout. The kernel performs electrical inversion from
the activeLow flag on the request; the bank never inverts.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: OutputPin

Single-line convenience for consumers that do not need a bank.

**Files:**
- Create: `include/Kanoop/pi/outputpin.h`
- Create: `src/Kanoop/pi/outputpin.cpp`
- Modify: `tests/CMakeLists.txt`
- Create: `tests/tst_outputpin.cpp`

**Interfaces:**
- Consumes: `OutputBank` from Task 3.
- Produces: `OutputPin(IGpioBackend*, QString consumer, quint32 offset, bool activeLow, QObject* parent = nullptr)`; `request()`, `release()`, `isRequested()`, `setActive(bool)`, `toggle()`, `isActive()`, `offset()`, `errorText()`.

- [ ] **Step 1: Write the failing test `tests/tst_outputpin.cpp`**

```cpp
#include <QTest>

#include <Kanoop/pi/mockbackend.h>
#include <Kanoop/pi/outputpin.h>

class TstOutputPin : public QObject
{
    Q_OBJECT

private slots:
    void requestStartsInactive()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));
        OutputPin pin(&backend, QString("test"), 25, true);

        QVERIFY(pin.request());
        QCOMPARE(pin.isActive(), false);
        QCOMPARE(backend.lineValue(25), Gpio::Value::Inactive);
    }

    void setActiveDrivesTheLine()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));
        OutputPin pin(&backend, QString("test"), 25, true);
        pin.request();

        QVERIFY(pin.setActive(true));
        QCOMPARE(pin.isActive(), true);
        QCOMPARE(backend.lineValue(25), Gpio::Value::Active);
    }

    void toggleInvertsTheCurrentValue()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));
        OutputPin pin(&backend, QString("test"), 25, true);
        pin.request();

        QVERIFY(pin.toggle());
        QCOMPARE(pin.isActive(), true);
        QVERIFY(pin.toggle());
        QCOMPARE(pin.isActive(), false);
    }

    void offsetIsReported()
    {
        MockBackend backend;
        OutputPin pin(&backend, QString("test"), 25, true);
        QCOMPARE(pin.offset(), 25u);
    }

    void destructorReleasesTheLine()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));
        {
            OutputPin pin(&backend, QString("test"), 25, true);
            pin.request();
            QCOMPARE(backend.isRequested(25), true);
        }
        QCOMPARE(backend.isRequested(25), false);
    }
};

QTEST_MAIN(TstOutputPin)
#include "tst_outputpin.moc"
```

- [ ] **Step 2: Register the test**

```cmake
add_kanooppi_test(tst_outputpin)
```

- [ ] **Step 3: Run it to verify it fails**

```bash
cd ~/src/punak/KanoopPiQt && cmake --build build --parallel
```

Expected: `fatal error: Kanoop/pi/outputpin.h: No such file or directory`.

- [ ] **Step 4: Write `include/Kanoop/pi/outputpin.h`**

```cpp
#ifndef KANOOP_PI_OUTPUTPIN_H
#define KANOOP_PI_OUTPUTPIN_H

#include <Kanoop/pi/outputbank.h>

/**
 * @brief A single output line.
 *
 * @warning The kernel releases a requested line when the owning process exits,
 *          and a released line reverts to input. Hardware that must stay
 *          de-energised while no process holds the line needs an external
 *          pull resistor; there is no way to latch an output state across
 *          process exit.
 */
class KANOOPPI_EXPORT OutputPin : public QObject
{
    Q_OBJECT
public:
    OutputPin(IGpioBackend* backend,
              const QString& consumer,
              quint32 offset,
              bool activeLow,
              QObject* parent = nullptr);

    /** @brief Requests the line, inactive. @return True on success. */
    bool request() { return _bank.request(); }

    /** @brief Releases the line if held. */
    void release() { _bank.release(); }

    /** @brief Returns true while the line is held. */
    bool isRequested() const { return _bank.isRequested(); }

    /** @brief Drives the line to @p active. @return True on success. */
    bool setActive(bool active) { return _bank.setValue(_offset, active); }

    /** @brief Drives the line to the inverse of its current value. @return True on success. */
    bool toggle() { return setActive(isActive() == false); }

    /** @brief Returns the last value written. */
    bool isActive() const { return _bank.isActive(_offset); }

    /** @brief Returns the line offset. */
    quint32 offset() const { return _offset; }

    /** @brief Returns the text of the most recent failure. */
    QString errorText() const { return _bank.errorText(); }

private:
    quint32 _offset;
    OutputBank _bank;
};

#endif // KANOOP_PI_OUTPUTPIN_H
```

- [ ] **Step 5: Write `src/Kanoop/pi/outputpin.cpp`**

```cpp
#include "Kanoop/pi/outputpin.h"

OutputPin::OutputPin(IGpioBackend* backend,
                     const QString& consumer,
                     quint32 offset,
                     bool activeLow,
                     QObject* parent) :
    QObject(parent),
    _offset(offset),
    _bank(backend, consumer, QList<quint32>() << offset, activeLow)
{
}

#include "Kanoop/pi/moc_outputpin.cpp"
```

- [ ] **Step 6: Build and run**

```bash
cd ~/src/punak/KanoopPiQt && cmake --build build --parallel && cd build && ctest --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 4`.

- [ ] **Step 7: Commit**

```bash
cd ~/src/punak/KanoopPiQt
git add -A
git commit -m "feat: add OutputPin for single-line output

Wraps a one-line OutputBank. Carries the header warning that a released line
reverts to input, so hardware needing a defined idle state requires an
external pull resistor.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: InputPin

Edge-triggered input with kernel-side debounce, delivered as Qt signals.

**Files:**
- Create: `include/Kanoop/pi/inputpin.h`
- Create: `src/Kanoop/pi/inputpin.cpp`
- Modify: `tests/CMakeLists.txt`
- Create: `tests/tst_inputpin.cpp`

**Interfaces:**
- Consumes: `IGpioBackend`, `MockBackend`, `Gpio::*` from Task 2.
- Produces: `InputPin(IGpioBackend*, QString consumer, quint32 offset, QObject* parent = nullptr)`; `setBias(Gpio::Bias)`, `setEdge(Gpio::Edge)`, `setDebounce(const TimeSpan&)`, `setActiveLow(bool)`, `request()`, `release()`, `isRequested()`, `offset()`, `errorText()`; signals `edge(Gpio::Edge)`, `asserted()`, `deasserted()`.

`asserted()` fires on a falling edge when `activeLow` is set and on a rising edge otherwise, so a consumer wiring a button to ground connects to `asserted()` without reasoning about polarity.

- [ ] **Step 1: Write the failing test `tests/tst_inputpin.cpp`**

```cpp
#include <QSignalSpy>
#include <QTest>

#include <Kanoop/timespan.h>
#include <Kanoop/pi/inputpin.h>
#include <Kanoop/pi/mockbackend.h>

class TstInputPin : public QObject
{
    Q_OBJECT

private slots:
    void requestPassesBiasEdgeAndDebounce()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));

        InputPin pin(&backend, QString("test"), 25);
        pin.setBias(Gpio::Bias::PullUp);
        pin.setEdge(Gpio::Edge::Falling);
        pin.setDebounce(TimeSpan::fromMilliseconds(20));
        pin.setActiveLow(true);

        QVERIFY(pin.request());

        Gpio::InputRequest request = backend.lastInputRequest();
        QCOMPARE(request.bias, Gpio::Bias::PullUp);
        QCOMPARE(request.edge, Gpio::Edge::Falling);
        QCOMPARE(request.debounceMicroseconds, 20000ul);
        QCOMPARE(request.activeLow, true);
        QCOMPARE(request.offsets.count(), 1);
        QCOMPARE(request.offsets.at(0), 25u);
    }

    void requestFailurePropagates()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));
        backend.setFailNextRequest(true);

        InputPin pin(&backend, QString("test"), 25);
        QCOMPARE(pin.request(), false);
        QVERIFY(pin.errorText().isEmpty() == false);
    }

    void edgeOnThisOffsetIsForwarded()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));
        InputPin pin(&backend, QString("test"), 25);
        pin.request();

        QSignalSpy spy(&pin, &InputPin::edge);
        backend.simulateEdge(25, Gpio::Edge::Falling);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).value<Gpio::Edge>(), Gpio::Edge::Falling);
    }

    void edgeOnAnotherOffsetIsIgnored()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));
        InputPin pin(&backend, QString("test"), 25);
        pin.request();

        QSignalSpy spy(&pin, &InputPin::edge);
        backend.simulateEdge(26, Gpio::Edge::Falling);

        QCOMPARE(spy.count(), 0);
    }

    void activeLowMapsFallingToAsserted()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));
        InputPin pin(&backend, QString("test"), 25);
        pin.setActiveLow(true);
        pin.request();

        QSignalSpy asserted(&pin, &InputPin::asserted);
        QSignalSpy deasserted(&pin, &InputPin::deasserted);

        backend.simulateEdge(25, Gpio::Edge::Falling);
        QCOMPARE(asserted.count(), 1);
        QCOMPARE(deasserted.count(), 0);

        backend.simulateEdge(25, Gpio::Edge::Rising);
        QCOMPARE(deasserted.count(), 1);
    }

    void activeHighMapsRisingToAsserted()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));
        InputPin pin(&backend, QString("test"), 25);
        pin.setActiveLow(false);
        pin.request();

        QSignalSpy asserted(&pin, &InputPin::asserted);
        backend.simulateEdge(25, Gpio::Edge::Rising);

        QCOMPARE(asserted.count(), 1);
    }

    void destructorReleasesTheLine()
    {
        MockBackend backend;
        backend.openChipByLabel(QString("test"));
        {
            InputPin pin(&backend, QString("test"), 25);
            pin.request();
            QCOMPARE(backend.isRequested(25), true);
        }
        QCOMPARE(backend.isRequested(25), false);
    }
};

QTEST_MAIN(TstInputPin)
#include "tst_inputpin.moc"
```

- [ ] **Step 2: Register the test**

```cmake
add_kanooppi_test(tst_inputpin)
```

- [ ] **Step 3: Run it to verify it fails**

```bash
cd ~/src/punak/KanoopPiQt && cmake --build build --parallel
```

Expected: `fatal error: Kanoop/pi/inputpin.h: No such file or directory`.

- [ ] **Step 4: Add KanoopCommonQt to the build**

`TimeSpan` comes from KanoopCommonQt, so the library now depends on it. Standalone builds need it side by side; the superproject provides it in Task 7.

Modify `CMakeLists.txt`. Replace:

```cmake
target_link_libraries(${PROJ} PRIVATE Qt6::Core PkgConfig::GPIOD)
```

with:

```cmake
if(NOT TARGET KanoopCommonQt)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../KanoopCommonQt/CMakeLists.txt")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../KanoopCommonQt" KanoopCommonQt)
    else()
        message(FATAL_ERROR "KanoopCommonQt not found. Clone it beside this repo or build from the superproject.")
    endif()
endif()

target_link_libraries(${PROJ} PUBLIC KanoopCommonQt)
target_link_libraries(${PROJ} PRIVATE Qt6::Core PkgConfig::GPIOD)
```

`KanoopCommonQt` is `PUBLIC` because `TimeSpan` appears in the public `InputPin` signature.

Also add `KanoopCommonQt` to the test link list in `tests/CMakeLists.txt`:

```cmake
    target_link_libraries(${name} PRIVATE
        Qt6::Test
        Qt6::Core
        KanoopCommonQt
        KanoopPiQt
    )
```

- [ ] **Step 5: Write `include/Kanoop/pi/inputpin.h`**

```cpp
#ifndef KANOOP_PI_INPUTPIN_H
#define KANOOP_PI_INPUTPIN_H

#include <Kanoop/timespan.h>

#include <Kanoop/pi/gpiobackend.h>

/**
 * @brief A single edge-triggered input line.
 *
 * Debounce is applied by the kernel from the period given to setDebounce(),
 * so no timer is needed in the consumer.
 */
class KANOOPPI_EXPORT InputPin : public QObject
{
    Q_OBJECT
public:
    InputPin(IGpioBackend* backend,
             const QString& consumer,
             quint32 offset,
             QObject* parent = nullptr);

    virtual ~InputPin();

    /** @brief Sets the internal bias applied when the line is requested. */
    void setBias(Gpio::Bias value) { _bias = value; }

    /** @brief Sets which edges are detected. */
    void setEdge(Gpio::Edge value) { _edge = value; }

    /** @brief Sets the kernel debounce period. */
    void setDebounce(const TimeSpan& value) { _debounce = value; }

    /** @brief Sets whether a low level is the asserted state. */
    void setActiveLow(bool value) { _activeLow = value; }

    /** @brief Requests the line. @return True on success. */
    bool request();

    /** @brief Releases the line if held. */
    void release();

    /** @brief Returns true while the line is held. */
    bool isRequested() const { return _handle != Gpio::InvalidRequest; }

    /** @brief Returns the line offset. */
    quint32 offset() const { return _offset; }

    /** @brief Returns the text of the most recent failure. */
    QString errorText() const { return _errorText; }

signals:
    /** @brief Emitted for every detected edge on this line. */
    void edge(Gpio::Edge edge);

    /** @brief Emitted when the line enters its asserted state. */
    void asserted();

    /** @brief Emitted when the line leaves its asserted state. */
    void deasserted();

private slots:
    void onBackendEdgeEvent(quint32 offset, Gpio::Edge edge);

private:
    IGpioBackend* _backend;
    QString _consumer;
    quint32 _offset;
    Gpio::Bias _bias = Gpio::Bias::AsIs;
    Gpio::Edge _edge = Gpio::Edge::Both;
    TimeSpan _debounce;
    bool _activeLow = false;
    Gpio::RequestHandle _handle = Gpio::InvalidRequest;
    QString _errorText;
};

#endif // KANOOP_PI_INPUTPIN_H
```

- [ ] **Step 6: Write `src/Kanoop/pi/inputpin.cpp`**

```cpp
#include "Kanoop/pi/inputpin.h"

InputPin::InputPin(IGpioBackend* backend,
                   const QString& consumer,
                   quint32 offset,
                   QObject* parent) :
    QObject(parent),
    _backend(backend), _consumer(consumer), _offset(offset)
{
    connect(_backend, &IGpioBackend::edgeEvent, this, &InputPin::onBackendEdgeEvent);
}

InputPin::~InputPin()
{
    release();
}

bool InputPin::request()
{
    if(isRequested() == true) {
        return true;
    }

    Gpio::InputRequest request;
    request.consumer = _consumer;
    request.offsets = QList<quint32>() << _offset;
    request.activeLow = _activeLow;
    request.bias = _bias;
    request.edge = _edge;
    request.debounceMicroseconds = static_cast<unsigned long>(_debounce.totalMicroseconds());

    _handle = _backend->requestInputs(request);
    if(_handle == Gpio::InvalidRequest) {
        _errorText = _backend->errorText();
        return false;
    }
    return true;
}

void InputPin::release()
{
    if(isRequested() == false) {
        return;
    }

    _backend->release(_handle);
    _handle = Gpio::InvalidRequest;
}

void InputPin::onBackendEdgeEvent(quint32 offset, Gpio::Edge edgeType)
{
    if(offset != _offset) {
        return;
    }

    emit edge(edgeType);

    Gpio::Edge assertingEdge = _activeLow == true
                               ? Gpio::Edge::Falling
                               : Gpio::Edge::Rising;
    if(edgeType == assertingEdge) {
        emit asserted();
    }
    else if(edgeType == Gpio::Edge::Falling || edgeType == Gpio::Edge::Rising) {
        emit deasserted();
    }
}

#include "Kanoop/pi/moc_inputpin.cpp"
```

- [ ] **Step 7: Confirm the TimeSpan accessor name**

`TimeSpan::totalMicroseconds()` is used above. Verify it exists:

```bash
grep -n "totalMicroseconds\|totalMilliseconds\|fromMilliseconds" \
  ~/src/punak/KanoopPiQt/../KanoopCommonQt/include/Kanoop/timespan.h
```

If the accessor is named differently, use the real name in both `inputpin.cpp` and the test's `debounceMicroseconds` assertion. If no microsecond accessor exists, use `totalMilliseconds() * 1000`.

- [ ] **Step 8: Build and run**

```bash
cd ~/src/punak/KanoopPiQt && cmake --build build --parallel && cd build && ctest --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 5`.

- [ ] **Step 9: Commit**

```bash
cd ~/src/punak/KanoopPiQt
git add -A
git commit -m "feat: add InputPin with kernel debounce and polarity mapping

Edges arrive as Qt signals. asserted() and deasserted() resolve activeLow so a
consumer wiring a button to ground never reasons about edge direction.

Adds KanoopCommonQt as a public dependency; TimeSpan appears in the
setDebounce signature.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 6: LibGpiodBackend

The real backend. Its tests skip when no GPIO character device is present, so the suite still passes on a development host.

**Files:**
- Create: `include/Kanoop/pi/libgpiodbackend.h`
- Create: `src/Kanoop/pi/libgpiodbackend.cpp`
- Modify: `tests/CMakeLists.txt`
- Create: `tests/tst_libgpiodbackend.cpp`

**Interfaces:**
- Consumes: `IGpioBackend`, `Gpio::*` from Task 2.
- Produces: `LibGpiodBackend(QObject* parent = nullptr)`; the full `IGpioBackend` surface; plus `static QStringList chipPaths()` and `static QString labelOf(const QString& path)`.

**libgpiod v2 symbols used** — verified against `include/gpiod.h` on the `v2.2.x` branch:

```c
struct gpiod_chip *gpiod_chip_open(const char *path);
void gpiod_chip_close(struct gpiod_chip *chip);
struct gpiod_chip_info *gpiod_chip_get_info(struct gpiod_chip *chip);
const char *gpiod_chip_info_get_label(struct gpiod_chip_info *info);
void gpiod_chip_info_free(struct gpiod_chip_info *info);
struct gpiod_line_request *gpiod_chip_request_lines(struct gpiod_chip *chip,
                                                    struct gpiod_request_config *req_cfg,
                                                    struct gpiod_line_config *line_cfg);
struct gpiod_line_settings *gpiod_line_settings_new(void);
void gpiod_line_settings_free(struct gpiod_line_settings *settings);
int  gpiod_line_settings_set_direction(struct gpiod_line_settings *, enum gpiod_line_direction);
int  gpiod_line_settings_set_output_value(struct gpiod_line_settings *, enum gpiod_line_value);
int  gpiod_line_settings_set_bias(struct gpiod_line_settings *, enum gpiod_line_bias);
int  gpiod_line_settings_set_edge_detection(struct gpiod_line_settings *, enum gpiod_line_edge);
void gpiod_line_settings_set_active_low(struct gpiod_line_settings *, bool);
int  gpiod_line_settings_set_debounce_period_us(struct gpiod_line_settings *, unsigned long);
struct gpiod_line_config *gpiod_line_config_new(void);
void gpiod_line_config_free(struct gpiod_line_config *config);
int  gpiod_line_config_add_line_settings(struct gpiod_line_config *, const unsigned int *offsets,
                                         size_t num_offsets, struct gpiod_line_settings *);
struct gpiod_request_config *gpiod_request_config_new(void);
void gpiod_request_config_free(struct gpiod_request_config *config);
void gpiod_request_config_set_consumer(struct gpiod_request_config *, const char *consumer);
void gpiod_line_request_release(struct gpiod_line_request *request);
int  gpiod_line_request_set_values_subset(struct gpiod_line_request *, size_t num_values,
                                          const unsigned int *offsets, const enum gpiod_line_value *values);
int  gpiod_line_request_get_fd(struct gpiod_line_request *request);
int  gpiod_line_request_read_edge_events(struct gpiod_line_request *, struct gpiod_edge_event_buffer *,
                                         size_t max_events);
struct gpiod_edge_event_buffer *gpiod_edge_event_buffer_new(size_t capacity);
void gpiod_edge_event_buffer_free(struct gpiod_edge_event_buffer *buffer);
struct gpiod_edge_event *gpiod_edge_event_buffer_get_event(struct gpiod_edge_event_buffer *, unsigned long index);
enum gpiod_edge_event_type gpiod_edge_event_get_event_type(struct gpiod_edge_event *event);
unsigned int gpiod_edge_event_get_line_offset(struct gpiod_edge_event *event);
```

Note that `gpiod_line_settings_set_active_low` returns `void`, unlike its siblings. Assigning its result does not compile.

- [ ] **Step 1: Write the failing test `tests/tst_libgpiodbackend.cpp`**

```cpp
#include <QDir>
#include <QTest>

#include <Kanoop/pi/libgpiodbackend.h>

class TstLibGpiodBackend : public QObject
{
    Q_OBJECT

private:
    static bool haveHardware()
    {
        return LibGpiodBackend::chipPaths().isEmpty() == false;
    }

private slots:
    void chipPathsEnumeratesDevNodes()
    {
        QStringList paths = LibGpiodBackend::chipPaths();
        for(const QString& path : paths) {
            QVERIFY(path.startsWith(QString("/dev/gpiochip")));
        }
    }

    void openByPathFailsForAMissingDevice()
    {
        LibGpiodBackend backend;
        QCOMPARE(backend.openChipByPath(QString("/dev/gpiochip-does-not-exist")), false);
        QVERIFY(backend.errorText().isEmpty() == false);
        QCOMPARE(backend.isOpen(), false);
    }

    void openByLabelFailsForAnUnknownLabel()
    {
        LibGpiodBackend backend;
        QCOMPARE(backend.openChipByLabel(QString("no-such-label")), false);
        QCOMPARE(backend.isOpen(), false);
    }

    void requestFailsBeforeOpen()
    {
        LibGpiodBackend backend;
        Gpio::OutputRequest request;
        request.consumer = QString("tst");
        request.offsets = QList<quint32>() << 5;

        QCOMPARE(backend.requestOutputs(request), Gpio::InvalidRequest);
    }

    void openByLabelSucceedsOnHardware()
    {
        if(haveHardware() == false) {
            QSKIP("no GPIO character device on this host");
        }

        QString path = LibGpiodBackend::chipPaths().at(0);
        QString label = LibGpiodBackend::labelOf(path);
        QVERIFY(label.isEmpty() == false);

        LibGpiodBackend backend;
        QVERIFY(backend.openChipByLabel(label));
        QCOMPARE(backend.isOpen(), true);
        backend.closeChip();
        QCOMPARE(backend.isOpen(), false);
    }
};

QTEST_MAIN(TstLibGpiodBackend)
#include "tst_libgpiodbackend.moc"
```

- [ ] **Step 2: Register the test**

```cmake
add_kanooppi_test(tst_libgpiodbackend)
```

- [ ] **Step 3: Run it to verify it fails**

```bash
cd ~/src/punak/KanoopPiQt && cmake --build build --parallel
```

Expected: `fatal error: Kanoop/pi/libgpiodbackend.h: No such file or directory`.

- [ ] **Step 4: Write `include/Kanoop/pi/libgpiodbackend.h`**

```cpp
#ifndef KANOOP_PI_LIBGPIODBACKEND_H
#define KANOOP_PI_LIBGPIODBACKEND_H

#include <QMap>
#include <QStringList>

#include <Kanoop/pi/gpiobackend.h>

class QSocketNotifier;

struct gpiod_chip;
struct gpiod_line_request;
struct gpiod_edge_event_buffer;

/** @brief IGpioBackend implemented over libgpiod v2. */
class KANOOPPI_EXPORT LibGpiodBackend : public IGpioBackend
{
    Q_OBJECT
public:
    explicit LibGpiodBackend(QObject* parent = nullptr);
    virtual ~LibGpiodBackend();

    virtual bool openChipByLabel(const QString& label) override;
    virtual bool openChipByPath(const QString& path) override;
    virtual void closeChip() override;
    virtual bool isOpen() const override { return _chip != nullptr; }

    virtual Gpio::RequestHandle requestOutputs(const Gpio::OutputRequest& request) override;
    virtual Gpio::RequestHandle requestInputs(const Gpio::InputRequest& request) override;
    virtual void release(Gpio::RequestHandle handle) override;

    virtual bool setValues(Gpio::RequestHandle handle,
                           const QList<quint32>& offsets,
                           const QList<Gpio::Value>& values) override;

    /** @brief Returns every /dev/gpiochipN path present on the system, sorted. */
    static QStringList chipPaths();

    /** @brief Returns the kernel label of the chip at @p path, or an empty string. */
    static QString labelOf(const QString& path);

private slots:
    void onEventFdActivated(int fd);

private:
    class Request
    {
    public:
        gpiod_line_request* request = nullptr;
        gpiod_edge_event_buffer* eventBuffer = nullptr;
        QSocketNotifier* notifier = nullptr;
    };

    void releaseAll();

    gpiod_chip* _chip = nullptr;
    Gpio::RequestHandle _nextHandle = 1;
    QMap<Gpio::RequestHandle, Request> _requests;
    QMap<int, Gpio::RequestHandle> _handleByFd;
};

#endif // KANOOP_PI_LIBGPIODBACKEND_H
```

- [ ] **Step 5: Write `src/Kanoop/pi/libgpiodbackend.cpp`**

```cpp
#include "Kanoop/pi/libgpiodbackend.h"

#include <QDir>
#include <QSocketNotifier>

#include <gpiod.h>

namespace
{

enum gpiod_line_value toLineValue(Gpio::Value value)
{
    return value == Gpio::Value::Active
           ? GPIOD_LINE_VALUE_ACTIVE
           : GPIOD_LINE_VALUE_INACTIVE;
}

enum gpiod_line_bias toBias(Gpio::Bias bias)
{
    switch(bias) {
    case Gpio::Bias::Disabled:  return GPIOD_LINE_BIAS_DISABLED;
    case Gpio::Bias::PullUp:    return GPIOD_LINE_BIAS_PULL_UP;
    case Gpio::Bias::PullDown:  return GPIOD_LINE_BIAS_PULL_DOWN;
    case Gpio::Bias::AsIs:      break;
    }
    return GPIOD_LINE_BIAS_AS_IS;
}

enum gpiod_line_edge toEdge(Gpio::Edge edge)
{
    switch(edge) {
    case Gpio::Edge::Rising:    return GPIOD_LINE_EDGE_RISING;
    case Gpio::Edge::Falling:   return GPIOD_LINE_EDGE_FALLING;
    case Gpio::Edge::Both:      return GPIOD_LINE_EDGE_BOTH;
    case Gpio::Edge::None:      break;
    }
    return GPIOD_LINE_EDGE_NONE;
}

}

LibGpiodBackend::LibGpiodBackend(QObject* parent) :
    IGpioBackend(parent)
{
}

LibGpiodBackend::~LibGpiodBackend()
{
    closeChip();
}

QStringList LibGpiodBackend::chipPaths()
{
    QStringList result;
    QDir dev(QString("/dev"));
    QStringList names = dev.entryList(QStringList() << QString("gpiochip*"), QDir::System);
    for(const QString& name : names) {
        result.append(dev.absoluteFilePath(name));
    }
    result.sort();
    return result;
}

QString LibGpiodBackend::labelOf(const QString& path)
{
    QString result;
    gpiod_chip* chip = gpiod_chip_open(qPrintable(path));
    if(chip == nullptr) {
        return result;
    }

    gpiod_chip_info* info = gpiod_chip_get_info(chip);
    if(info != nullptr) {
        result = QString(gpiod_chip_info_get_label(info));
        gpiod_chip_info_free(info);
    }
    gpiod_chip_close(chip);
    return result;
}

bool LibGpiodBackend::openChipByPath(const QString& path)
{
    closeChip();

    _chip = gpiod_chip_open(qPrintable(path));
    if(_chip == nullptr) {
        setErrorText(QString("cannot open %1: %2").arg(path).arg(strerror(errno)));
        return false;
    }
    return true;
}

bool LibGpiodBackend::openChipByLabel(const QString& label)
{
    QStringList paths = chipPaths();
    for(const QString& path : paths) {
        if(labelOf(path) == label) {
            return openChipByPath(path);
        }
    }

    setErrorText(QString("no GPIO chip with label '%1'").arg(label));
    return false;
}

void LibGpiodBackend::closeChip()
{
    releaseAll();

    if(_chip != nullptr) {
        gpiod_chip_close(_chip);
        _chip = nullptr;
    }
}

void LibGpiodBackend::releaseAll()
{
    QList<Gpio::RequestHandle> handles = _requests.keys();
    for(Gpio::RequestHandle handle : handles) {
        release(handle);
    }
}

Gpio::RequestHandle LibGpiodBackend::requestOutputs(const Gpio::OutputRequest& request)
{
    if(isOpen() == false) {
        setErrorText(QString("chip is not open"));
        return Gpio::InvalidRequest;
    }

    gpiod_line_settings* settings = gpiod_line_settings_new();
    gpiod_line_config* lineConfig = gpiod_line_config_new();
    gpiod_request_config* reqConfig = gpiod_request_config_new();
    gpiod_line_request* lineRequest = nullptr;

    if(settings != nullptr && lineConfig != nullptr && reqConfig != nullptr) {
        gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
        gpiod_line_settings_set_output_value(settings, toLineValue(request.initialValue));
        gpiod_line_settings_set_active_low(settings, request.activeLow);

        QList<unsigned int> offsets;
        for(quint32 offset : request.offsets) {
            offsets.append(offset);
        }

        if(gpiod_line_config_add_line_settings(lineConfig, offsets.constData(),
                                               static_cast<size_t>(offsets.count()), settings) == 0) {
            gpiod_request_config_set_consumer(reqConfig, qPrintable(request.consumer));
            lineRequest = gpiod_chip_request_lines(_chip, reqConfig, lineConfig);
        }
    }

    if(reqConfig != nullptr) {
        gpiod_request_config_free(reqConfig);
    }
    if(lineConfig != nullptr) {
        gpiod_line_config_free(lineConfig);
    }
    if(settings != nullptr) {
        gpiod_line_settings_free(settings);
    }

    if(lineRequest == nullptr) {
        setErrorText(QString("request failed: %1").arg(strerror(errno)));
        return Gpio::InvalidRequest;
    }

    Request entry;
    entry.request = lineRequest;

    Gpio::RequestHandle handle = _nextHandle++;
    _requests.insert(handle, entry);
    return handle;
}

Gpio::RequestHandle LibGpiodBackend::requestInputs(const Gpio::InputRequest& request)
{
    if(isOpen() == false) {
        setErrorText(QString("chip is not open"));
        return Gpio::InvalidRequest;
    }

    gpiod_line_settings* settings = gpiod_line_settings_new();
    gpiod_line_config* lineConfig = gpiod_line_config_new();
    gpiod_request_config* reqConfig = gpiod_request_config_new();
    gpiod_line_request* lineRequest = nullptr;

    if(settings != nullptr && lineConfig != nullptr && reqConfig != nullptr) {
        gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
        gpiod_line_settings_set_bias(settings, toBias(request.bias));
        gpiod_line_settings_set_edge_detection(settings, toEdge(request.edge));
        gpiod_line_settings_set_active_low(settings, request.activeLow);
        if(request.debounceMicroseconds > 0) {
            gpiod_line_settings_set_debounce_period_us(settings, request.debounceMicroseconds);
        }

        QList<unsigned int> offsets;
        for(quint32 offset : request.offsets) {
            offsets.append(offset);
        }

        if(gpiod_line_config_add_line_settings(lineConfig, offsets.constData(),
                                               static_cast<size_t>(offsets.count()), settings) == 0) {
            gpiod_request_config_set_consumer(reqConfig, qPrintable(request.consumer));
            lineRequest = gpiod_chip_request_lines(_chip, reqConfig, lineConfig);
        }
    }

    if(reqConfig != nullptr) {
        gpiod_request_config_free(reqConfig);
    }
    if(lineConfig != nullptr) {
        gpiod_line_config_free(lineConfig);
    }
    if(settings != nullptr) {
        gpiod_line_settings_free(settings);
    }

    if(lineRequest == nullptr) {
        setErrorText(QString("request failed: %1").arg(strerror(errno)));
        return Gpio::InvalidRequest;
    }

    Request entry;
    entry.request = lineRequest;

    Gpio::RequestHandle handle = _nextHandle++;

    if(request.edge != Gpio::Edge::None) {
        entry.eventBuffer = gpiod_edge_event_buffer_new(16);
        int fd = gpiod_line_request_get_fd(lineRequest);
        entry.notifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
        connect(entry.notifier, &QSocketNotifier::activated,
                this, &LibGpiodBackend::onEventFdActivated);
        _handleByFd.insert(fd, handle);
        entry.notifier->setEnabled(true);
    }

    _requests.insert(handle, entry);
    return handle;
}

void LibGpiodBackend::release(Gpio::RequestHandle handle)
{
    if(_requests.contains(handle) == false) {
        return;
    }

    Request entry = _requests.take(handle);

    if(entry.notifier != nullptr) {
        _handleByFd.remove(static_cast<int>(entry.notifier->socket()));
        entry.notifier->setEnabled(false);
        delete entry.notifier;
    }
    if(entry.eventBuffer != nullptr) {
        gpiod_edge_event_buffer_free(entry.eventBuffer);
    }
    if(entry.request != nullptr) {
        gpiod_line_request_release(entry.request);
    }
}

bool LibGpiodBackend::setValues(Gpio::RequestHandle handle,
                                const QList<quint32>& offsets,
                                const QList<Gpio::Value>& values)
{
    if(_requests.contains(handle) == false) {
        setErrorText(QString("unknown request handle"));
        return false;
    }

    if(offsets.count() != values.count()) {
        setErrorText(QString("offset and value counts differ"));
        return false;
    }

    QList<unsigned int> rawOffsets;
    QList<enum gpiod_line_value> rawValues;
    for(int i = 0;i < offsets.count();i++) {
        rawOffsets.append(offsets.at(i));
        rawValues.append(toLineValue(values.at(i)));
    }

    gpiod_line_request* lineRequest = _requests.value(handle).request;
    if(gpiod_line_request_set_values_subset(lineRequest,
                                            static_cast<size_t>(rawOffsets.count()),
                                            rawOffsets.constData(),
                                            rawValues.constData()) != 0) {
        setErrorText(QString("set values failed: %1").arg(strerror(errno)));
        return false;
    }
    return true;
}

void LibGpiodBackend::onEventFdActivated(int fd)
{
    Gpio::RequestHandle handle = _handleByFd.value(fd, Gpio::InvalidRequest);
    if(handle == Gpio::InvalidRequest) {
        return;
    }

    Request entry = _requests.value(handle);
    if(entry.eventBuffer == nullptr) {
        return;
    }

    int count = gpiod_line_request_read_edge_events(entry.request, entry.eventBuffer, 16);
    for(int i = 0;i < count;i++) {
        gpiod_edge_event* event =
            gpiod_edge_event_buffer_get_event(entry.eventBuffer, static_cast<unsigned long>(i));
        if(event == nullptr) {
            continue;
        }

        Gpio::Edge edge = gpiod_edge_event_get_event_type(event) == GPIOD_EDGE_EVENT_RISING_EDGE
                          ? Gpio::Edge::Rising
                          : Gpio::Edge::Falling;
        emit edgeEvent(gpiod_edge_event_get_line_offset(event), edge);
    }
}

#include "Kanoop/pi/moc_libgpiodbackend.cpp"
```

`<cstring>` and `<cerrno>` are needed for `strerror` and `errno`. Add both to the include block if the compiler reports them missing.

- [ ] **Step 6: Build and run**

```bash
cd ~/src/punak/KanoopPiQt && cmake --build build --parallel && cd build && ctest --output-on-failure
```

Expected: `100% tests passed, 0 tests failed out of 6`. On a host with no GPIO device, `openByLabelSucceedsOnHardware` reports as skipped rather than failed.

- [ ] **Step 7: Commit**

```bash
cd ~/src/punak/KanoopPiQt
git add -A
git commit -m "feat: add the libgpiod v2 backend

Opens chips by kernel label rather than device index. Index lookup targets
different silicon between a Pi 4 and a Pi 5, whose GPIO moved to the RP1
southbridge and renumbered every chip on the system.

Edge events reach the Qt event loop through a QSocketNotifier on the request
file descriptor. Hardware-dependent tests skip when no GPIO character device
is present.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 7: Superproject wiring

Proves the library is consumable the way the daemon will consume it, and leaves the `irrigation` superproject able to build.

**Files:**
- Create: `~/src/punak/irrigation/CMakeLists.txt`
- Modify: `~/src/punak/irrigation/.gitignore`
- Create: `~/src/punak/irrigation/.gitmodules` (via `git submodule add`)

**Interfaces:**
- Consumes: the `KanoopPiQt` CMake target from Tasks 1-6.
- Produces: superproject targets `KanoopCommonQt`, `KanoopDatabaseQt`, `KanoopPiQt`.

- [ ] **Step 1: Add the submodules**

```bash
cd ~/src/punak/irrigation
git submodule add git@github.com:StevePunak/KanoopCommonQt.git KanoopCommonQt
git submodule add git@github.com:StevePunak/KanoopDatabaseQt.git KanoopDatabaseQt
git submodule add git@github.com:StevePunak/KanoopPiQt.git KanoopPiQt
```

If a URL is wrong, list the correct ones with:

```bash
git -C ~/mnt/seneca/spunak/src/epc/meta-qt-mains config --file .gitmodules --get-regexp url
```

- [ ] **Step 2: Point the KanoopPiQt submodule at the working branch**

The submodule clone lands on `master`, which does not yet have this work. Until `feature/libgpiod-v2` is merged:

```bash
cd ~/src/punak/irrigation/KanoopPiQt
git checkout feature/libgpiod-v2
```

- [ ] **Step 3: Write `~/src/punak/irrigation/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.16)

set(PROJ_MAINS_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

set(MAIN_PROJ irrigation)
project(${MAIN_PROJ} VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

option(IRRIGATION_USE_MOLD "Use mold linker on Linux if installed" ON)
if(IRRIGATION_USE_MOLD AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(MOLD_EXECUTABLE mold)
    if(MOLD_EXECUTABLE)
        add_link_options(-fuse-ld=mold)
        message(STATUS "Using mold linker: ${MOLD_EXECUTABLE}")
    endif()
endif()

file(WRITE "${CMAKE_SOURCE_DIR}/.clangd"
    "CompileFlags:\n"
    "  CompilationDatabase: ${CMAKE_BINARY_DIR}\n"
)

add_subdirectory(KanoopCommonQt   EXCLUDE_FROM_ALL)
add_subdirectory(KanoopDatabaseQt EXCLUDE_FROM_ALL)
add_subdirectory(KanoopPiQt       EXCLUDE_FROM_ALL)
```

`IrrigationD` is added here by the daemon plan; this task stops at the libraries.

- [ ] **Step 4: Configure and build the superproject**

```bash
cd ~/src/punak/irrigation
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

Expected: all three libraries build with no warnings. `EXCLUDE_FROM_ALL` means `--build` with no target builds nothing; build a library explicitly to confirm:

```bash
cmake --build build --target KanoopPiQt --parallel
```

- [ ] **Step 5: Run the KanoopPiQt suite from the superproject build**

```bash
cd ~/src/punak/irrigation/build && ctest --output-on-failure -R "tst_"
```

Expected: the six KanoopPiQt tests plus the KanoopCommonQt suite pass.

- [ ] **Step 6: Commit**

```bash
cd ~/src/punak/irrigation
printf 'build/\nnode_modules/\ndist/\n*.user\n.clangd\n' > .gitignore
git add -A
git commit -m "build: add the CMake superproject and Kanoop submodules

Adds KanoopCommonQt, KanoopDatabaseQt and KanoopPiQt as submodules and the
superproject that builds them, following meta-qt-mains. IrrigationD is added
by the daemon plan.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Deferred to later plans

- `IrrigationD` — `ZoneController`, `Scheduler`, `ProgramRunner`, `StopButton`, `Settings`, `IrrigationDataSource`, `IrrigationControlServer`
- `web/` — the React frontend
- `meta-rpi4-irrigation` — the Yocto layer, image, and `config.txt` fragment
- `legacy/` — porting `i2c` and the ADS1115 and BMP280 drivers

## Hardware bring-up, once the parts arrive

Not a code task, and it gates nothing in this plan. After wiring, before 24V is
connected:

1. Confirm the relay board's trigger polarity with a meter. The SunFounder
   datasheet contradicts itself; the pinout section is correct and the feature
   list is wrong.
2. Confirm `LibGpiodBackend::labelOf("/dev/gpiochip0")` reports
   `pinctrl-bcm2711` on the Pi 4B, and use that string in the daemon settings.
3. Run `tst_libgpiodbackend` on the target and confirm
   `openByLabelSucceedsOnHardware` runs rather than skipping.
