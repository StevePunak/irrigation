#include "zonecontroller.h"

ZoneController::ZoneController(IGpioBackend* backend,
                               const QMap<int, quint32>& zoneGpioMap,
                               bool activeLow,
                               int maxZoneSeconds,
                               QObject* parent) :
    QObject(parent),
    LoggingBaseClass("zone"),
    _backend(backend),
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

    if(writeExclusive(0) == false) {
        return false;
    }

    _watchdogTimer.start();

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

    _closeTimer.setSingleShot(true);
    _closeTimer.start(clamped * 1000);

    if(previous != 0 && previous != zoneNumber) {
        emit zoneClosed(previous);
    }
    emit zoneOpened(zoneNumber, clamped);

    return true;
}

void ZoneController::allOff()
{
    _closeTimer.stop();

    if(_bank->isRequested() == false) {
        _openZone = 0;
        return;
    }

    const int zoneNumber = _openZone;
    if(writeExclusive(0) == false) {
        logText(LVL_ERROR, QString("allOff failed: %1").arg(_errorText));
        return;
    }

    _openZone = 0;
    _deadlineUtc = QDateTime();

    if(zoneNumber != 0) {
        emit zoneClosed(zoneNumber);
    }
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
    _watchdogTimer.setInterval(static_cast<int>(value.totalMilliseconds()));
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

#include "moc_zonecontroller.cpp"
