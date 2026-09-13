#include "zonecontroller.h"

#include <limits>

ZoneController::ZoneController(IGpioBackend* backend,
                               const QMap<int, quint32>& zoneGpioMap,
                               bool activeLow,
                               int maxZoneSeconds,
                               QObject* parent) :
    QObject(parent),
    LoggingBaseClass("zone"),
    _zoneGpioMap(zoneGpioMap),
    _maxZoneSeconds(maxZoneSeconds),
    _bank(new OutputBank(backend, QStringLiteral("irrigationd-zones"), zoneGpioMap.values(), activeLow, this))
{
    _closeTimer.setSingleShot(true);
    connect(&_closeTimer, &QTimer::timeout, this, &ZoneController::onCloseTimer);

    _watchdogTimer.setInterval(1000);
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

    const qint64 timerMilliseconds = qMin<qint64>(static_cast<qint64>(clamped) * 1000,
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
            _closeTimer.start(RetryIntervalMilliseconds);
        }
        return false;
    }

    _closeTimer.stop();
    _openZone = 0;
    _deadlineUtc = QDateTime();

    if(zoneNumber != 0) {
        emit zoneClosed(zoneNumber);
    }

    return true;
}

int ZoneController::secondsRemaining() const
{
    if(_openZone == 0 || _deadlineUtc.isValid() == false) {
        return 0;
    }

    const qint64 secondsLeft = QDateTime::currentDateTimeUtc().secsTo(_deadlineUtc);
    return secondsLeft > 0 ? static_cast<int>(secondsLeft) : 0;
}

void ZoneController::setWatchdogInterval(const TimeSpan& value)
{
    const int milliseconds = qMax(50, static_cast<int>(value.totalMilliseconds()));
    _watchdogTimer.setInterval(milliseconds);
}

void ZoneController::onCloseTimer()
{
    if(_openZone == 0) {
        return;
    }

    const int zoneNumber = _openZone;
    if(writeExclusive(0) == false) {
        logText(LVL_ERROR, QString("Failed to close zone %1: %2").arg(zoneNumber).arg(_errorText));
        return;
    }

    _openZone = 0;
    _deadlineUtc = QDateTime();

    emit zoneClosed(zoneNumber);
}

void ZoneController::onWatchdogTimer()
{
    QMap<quint32, bool> actual;
    if(_bank->readValues(actual) == false) {
        logText(LVL_ERROR, QString("Watchdog could not read the bank: %1").arg(_bank->errorText()));
        allOff();
        return;
    }

    QMap<quint32, bool> expected;
    for(auto it = _zoneGpioMap.constBegin(); it != _zoneGpioMap.constEnd(); ++it) {
        expected.insert(it.value(), it.key() == _openZone);
    }
    const bool mismatch = actual != expected;

    const bool pastDeadline = _openZone != 0
                              && _deadlineUtc.isValid()
                              && QDateTime::currentDateTimeUtc() > _deadlineUtc;

    if(mismatch || pastDeadline) {
        const int trippedOn = _openZone;
        logText(LVL_ERROR, QString("Watchdog tripped: open zone %1").arg(_openZone));
        if(allOff() == true) {
            emit watchdogTripped(trippedOn);
        } else {
            logText(LVL_ERROR, QString("Watchdog could not close the bank: %1").arg(_errorText));
        }
    }
}

#include "moc_zonecontroller.cpp"
