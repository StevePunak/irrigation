#include "zonecontroller.h"

#include <chrono>
#include <limits>

const TimeSpan ZoneController::RetryInterval           = TimeSpan::fromSeconds(1);
const TimeSpan ZoneController::DefaultWatchdogInterval = TimeSpan::fromSeconds(1);
const TimeSpan ZoneController::MinimumWatchdogInterval = TimeSpan::fromMilliseconds(50);

ZoneController::ZoneController(IGpioBackend* backend,
                               const QMap<int, quint32>& zoneGpioMap,
                               bool activeLow,
                               int maxZoneSeconds,
                               QObject* parent) :
    QObject(parent),
    LoggingBaseClass("zone"),
    _zoneGpioMap(zoneGpioMap),
    _hardMaxZoneSeconds(maxZoneSeconds),
    _maxZoneSeconds(maxZoneSeconds),
    _bank(new OutputBank(backend, QStringLiteral("irrigationd-zones"), zoneGpioMap.values(), activeLow, this)),
    _deadline(QDeadlineTimer::Forever)
{
    _closeTimer.setSingleShot(true);
    connect(&_closeTimer, &QTimer::timeout, this, &ZoneController::onCloseTimer);

    setWatchdogInterval(DefaultWatchdogInterval);
    connect(&_watchdogTimer, &QTimer::timeout, this, &ZoneController::onWatchdogTimer);
}

ZoneController::~ZoneController()
{
    allOff();
}

bool ZoneController::begin()
{
    if(_bank->request() == false) {
        _errorText = _bank->errorText();
        return false;
    }

    _watchdogTimer.start();

    if(writeExclusive(0) == false) {
        return false;
    }

    return true;
}

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

bool ZoneController::openZone(int zoneNumber, int seconds)
{
    if(_faulted == true) {
        _errorText = QString("Refused to open zone %1: the watchdog fault latch is set until a read-back matches the expected line state")
                         .arg(zoneNumber);
        return false;
    }

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

    const TimeSpan duration = TimeSpan::fromSeconds(clamped);
    _openZone = zoneNumber;
    _deadline = QDeadlineTimer(std::chrono::seconds(clamped));

    const qint64 timerMilliseconds = qMin<qint64>(static_cast<qint64>(duration.totalMilliseconds()),
                                                  std::numeric_limits<int>::max());
    _closeTimer.setSingleShot(true);
    _closeTimer.start(static_cast<int>(timerMilliseconds));

    if(previous != 0 && previous != zoneNumber) {
        emit zoneClosed(previous);
    }
    emit zoneOpened(zoneNumber, clamped);

    return true;
}

bool ZoneController::allOff()
{
    if(_bank->isRequested() == false) {
        _closeTimer.stop();
        _deadline = QDeadlineTimer(QDeadlineTimer::Forever);
        if(_openZone != 0) {
            const int zoneNumber = _openZone;
            _openZone = 0;
            emit zoneClosed(zoneNumber);
        }
        return true;
    }

    const int zoneNumber = _openZone;
    if(writeExclusive(0) == false) {
        logText(LVL_ERROR, QString("allOff failed: %1").arg(_errorText));
        if(zoneNumber != 0) {
            _closeTimer.start(static_cast<int>(RetryInterval.totalMilliseconds()));
        }
        return false;
    }

    _closeTimer.stop();
    _openZone = 0;
    _deadline = QDeadlineTimer(QDeadlineTimer::Forever);

    if(zoneNumber != 0) {
        emit zoneClosed(zoneNumber);
    }

    return true;
}

int ZoneController::secondsRemaining() const
{
    int result = 0;
    if(_openZone != 0 && _deadline.isForever() == false) {
        const qint64 secondsLeft =
            std::chrono::duration_cast<std::chrono::seconds>(_deadline.remainingTimeAsDuration()).count();
        result = secondsLeft > 0 ? static_cast<int>(secondsLeft) : 0;
    }
    return result;
}

void ZoneController::setMaxZoneSeconds(int value)
{
    _maxZoneSeconds = qMin(qMax(value, 1), _hardMaxZoneSeconds);
}

void ZoneController::setWatchdogInterval(const TimeSpan& value)
{
    const TimeSpan interval = TimeSpan::max(MinimumWatchdogInterval, value);
    _watchdogTimer.setInterval(static_cast<int>(interval.totalMilliseconds()));
}

void ZoneController::onCloseTimer()
{
    if(_openZone == 0) {
        return;
    }

    const int zoneNumber = _openZone;
    if(writeExclusive(0) == false) {
        logText(LVL_ERROR, QString("Failed to close zone %1: %2").arg(zoneNumber).arg(_errorText));
        _closeTimer.start(static_cast<int>(RetryInterval.totalMilliseconds()));
        return;
    }

    _openZone = 0;
    _deadline = QDeadlineTimer(QDeadlineTimer::Forever);

    emit zoneClosed(zoneNumber);
}

void ZoneController::onWatchdogTimer()
{
    QMap<quint32, bool> actual;
    if(_bank->readValues(actual) == false) {
        logText(LVL_ERROR, QString("Watchdog could not read the bank: %1").arg(_bank->errorText()));
        tripWatchdog();
    }
    else {
        QMap<quint32, bool> expected;
        for(auto it = _zoneGpioMap.constBegin(); it != _zoneGpioMap.constEnd(); ++it) {
            expected.insert(it.value(), it.key() == _openZone);
        }
        const bool mismatch = actual != expected;

        const bool pastDeadline = _openZone != 0
                                  && _deadline.isForever() == false
                                  && _deadline.hasExpired();

        if(mismatch || pastDeadline) {
            logText(LVL_ERROR, QString("Watchdog tripped: open zone %1").arg(_openZone));
            tripWatchdog();
        }
        else if(_faulted == true && _openZone == 0) {
            _faulted = false;
            logText(LVL_WARNING, "Watchdog read-back matches the expected line state; fault latch cleared");
        }
    }
}

void ZoneController::tripWatchdog()
{
    const int trippedOn = _openZone;

    // _faulted must be set before allOff(): allOff() emits zoneClosed synchronously,
    // and a ProgramRunner answers it by calling openZone() on its next zone.
    _faulted = true;

    if(allOff() == true) {
        emit watchdogTripped(trippedOn);
    }
    else {
        logText(LVL_ERROR, QString("Watchdog could not close the bank: %1").arg(_errorText));
    }
}

#include "moc_zonecontroller.cpp"
