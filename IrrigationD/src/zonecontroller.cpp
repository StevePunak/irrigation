#include "zonecontroller.h"

#include <QStringList>

#include <chrono>
#include <limits>

namespace
{
    QString zoneListText(const QList<int>& zoneNumbers)
    {
        QStringList parts;
        for(int zoneNumber : zoneNumbers) {
            parts.append(QString::number(zoneNumber));
        }
        return parts.join(", ");
    }
}

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
    _bank(new OutputBank(backend, QStringLiteral("irrigationd-zones"), zoneGpioMap.values(), activeLow, this))
{
    for(auto it = _zoneGpioMap.constBegin(); it != _zoneGpioMap.constEnd(); ++it) {
        const int zoneNumber = it.key();
        QTimer* timer = new QTimer(this);
        timer->setSingleShot(true);
        // A coarse timer may fire after the zone's deadline, and the watchdog trips on
        // any zone it finds past its deadline.
        timer->setTimerType(Qt::PreciseTimer);
        connect(timer, &QTimer::timeout, this, [this, zoneNumber]()
        {
            onCloseTimer(zoneNumber);
        });
        _closeTimers.insert(zoneNumber, timer);
    }

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

    if(writeOpenSet(QList<int>()) == false) {
        return false;
    }

    return true;
}

bool ZoneController::writeOpenSet(const QList<int>& zoneNumbers)
{
    QMap<quint32, bool> values;
    for(auto it = _zoneGpioMap.constBegin(); it != _zoneGpioMap.constEnd(); ++it) {
        values.insert(it.value(), zoneNumbers.contains(it.key()));
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

    const bool reopen = _open.contains(zoneNumber);
    if(reopen == false && _open.count() >= _maxConcurrentZones) {
        _errorText = QString("Refused to open zone %1: %2 zones already running")
                         .arg(zoneNumber).arg(_open.count());
        return false;
    }

    if(reopen == false) {
        QList<int> next = _open.keys();
        next.append(zoneNumber);
        if(writeOpenSet(next) == false) {
            return false;
        }
    }

    const int clamped = qMin(seconds, _maxZoneSeconds);
    OpenZone& zone = _open[zoneNumber];
    zone.deadline = QDeadlineTimer(std::chrono::seconds(clamped));
    zone.capAtOpen = qMax(zone.capAtOpen, _maxConcurrentZones);
    zone.pendingReason = CloseReason::Deadline;
    startCloseTimer(zoneNumber, TimeSpan::fromSeconds(clamped));

    emit zoneOpened(zoneNumber, clamped);

    return true;
}

bool ZoneController::closeZone(int zoneNumber)
{
    return closeOne(zoneNumber, CloseReason::Stopped);
}

bool ZoneController::allOff()
{
    return closeAll(CloseReason::AllOff);
}

bool ZoneController::closeOne(int zoneNumber, CloseReason reason)
{
    if(_open.contains(zoneNumber) == false) {
        return true;
    }

    QList<int> remaining = _open.keys();
    remaining.removeAll(zoneNumber);
    if(_bank->isRequested() == true && writeOpenSet(remaining) == false) {
        logText(LVL_ERROR, QString("Failed to close zone %1: %2").arg(zoneNumber).arg(_errorText));
        _open[zoneNumber].pendingReason = reason;
        startCloseTimer(zoneNumber, RetryInterval);
        return false;
    }

    // The zone leaves _open before zoneClosed: a slot that opens a zone writes the bank
    // from _open, and a zone still listed there is re-energised.
    _closeTimers.value(zoneNumber)->stop();
    _open.remove(zoneNumber);

    emit zoneClosed(zoneNumber, reason);
    return true;
}

bool ZoneController::closeAll(CloseReason reason)
{
    const QList<int> closing = _open.keys();

    if(_bank->isRequested() == true && writeOpenSet(QList<int>()) == false) {
        logText(LVL_ERROR, QString("allOff failed: %1").arg(_errorText));
        for(int zoneNumber : closing) {
            _open[zoneNumber].pendingReason = reason;
            startCloseTimer(zoneNumber, RetryInterval);
        }
        return false;
    }

    // Every closed zone leaves _open before the first zoneClosed: a slot that opens a
    // zone writes the bank from _open, and a zone still listed there is re-energised.
    for(int zoneNumber : closing) {
        _closeTimers.value(zoneNumber)->stop();
        _open.remove(zoneNumber);
    }

    for(int zoneNumber : closing) {
        emit zoneClosed(zoneNumber, reason);
    }

    return true;
}

void ZoneController::startCloseTimer(int zoneNumber, const TimeSpan& delay)
{
    const qint64 milliseconds = qMin<qint64>(static_cast<qint64>(delay.totalMilliseconds()),
                                             std::numeric_limits<int>::max());
    _closeTimers.value(zoneNumber)->start(static_cast<int>(milliseconds));
}

int ZoneController::secondsRemaining(int zoneNumber) const
{
    int result = 0;
    if(_open.contains(zoneNumber)) {
        const qint64 secondsLeft = std::chrono::duration_cast<std::chrono::seconds>(
                                       _open.value(zoneNumber).deadline.remainingTimeAsDuration()).count();
        result = secondsLeft > 0 ? static_cast<int>(secondsLeft) : 0;
    }
    return result;
}

void ZoneController::setMaxZoneSeconds(int value)
{
    _maxZoneSeconds = qMin(qMax(value, 1), _hardMaxZoneSeconds);
}

void ZoneController::setMaxConcurrentZones(int value)
{
    _maxConcurrentZones = qMin(qMax(value, 1), MaxConcurrentZonesCeiling);
}

void ZoneController::setWatchdogInterval(const TimeSpan& value)
{
    const TimeSpan interval = TimeSpan::max(MinimumWatchdogInterval, value);
    _watchdogTimer.setInterval(static_cast<int>(interval.totalMilliseconds()));
}

void ZoneController::disableCloseTimerForTest(int zoneNumber)
{
    QTimer* timer = _closeTimers.value(zoneNumber, nullptr);
    if(timer != nullptr) {
        timer->stop();
    }
}

void ZoneController::expireCloseTimerForTest(int zoneNumber)
{
    disableCloseTimerForTest(zoneNumber);
    onCloseTimer(zoneNumber);
}

bool ZoneController::closeTimerActiveForTest(int zoneNumber) const
{
    QTimer* timer = _closeTimers.value(zoneNumber, nullptr);
    return timer != nullptr && timer->isActive();
}

void ZoneController::onCloseTimer(int zoneNumber)
{
    if(_open.contains(zoneNumber) == false) {
        return;
    }

    closeOne(zoneNumber, _open.value(zoneNumber).pendingReason);
}

void ZoneController::onWatchdogTimer()
{
    QMap<quint32, bool> actual;
    if(_bank->readValues(actual) == false) {
        logText(LVL_ERROR, QString("Watchdog could not read the bank: %1").arg(_bank->errorText()));
        tripWatchdog();
        return;
    }

    QMap<quint32, bool> expected;
    for(auto it = _zoneGpioMap.constBegin(); it != _zoneGpioMap.constEnd(); ++it) {
        expected.insert(it.value(), _open.contains(it.key()));
    }

    int asserted = 0;
    for(bool value : actual) {
        if(value == true) {
            asserted++;
        }
    }

    int allowed = 0;
    QList<int> late;
    for(auto it = _open.constBegin(); it != _open.constEnd(); ++it) {
        allowed = qMax(allowed, it.value().capAtOpen);
        if(it.value().deadline.hasExpired()) {
            late.append(it.key());
        }
    }

    const bool mismatch = actual != expected;
    const bool overCount = asserted > allowed;

    if(mismatch || overCount || late.isEmpty() == false) {
        logText(LVL_ERROR, QString("Watchdog tripped: open zones [%1], past deadline [%2], %3 lines asserted, %4 allowed")
                               .arg(zoneListText(_open.keys()), zoneListText(late))
                               .arg(asserted).arg(allowed));
        tripWatchdog();
    }
    else if(_faulted == true && _open.isEmpty()) {
        _faulted = false;
        logText(LVL_WARNING, "Watchdog read-back matches the expected line state; fault latch cleared");
    }
}

void ZoneController::tripWatchdog()
{
    const QList<int> trippedOn = _open.keys();

    // _faulted must be set before closeAll(): closeAll() emits zoneClosed synchronously,
    // and a slot answering it may call openZone().
    _faulted = true;

    if(closeAll(CloseReason::Watchdog) == true) {
        emit watchdogTripped(trippedOn);
    }
    else {
        logText(LVL_ERROR, QString("Watchdog could not close the bank: %1").arg(_errorText));
    }
}

#include "moc_zonecontroller.cpp"
