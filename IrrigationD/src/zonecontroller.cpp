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
        timer->setTimerType(Qt::PreciseTimer);
        connect(timer, &QTimer::timeout, this, [this, zoneNumber]()
        {
            onCloseTimer(zoneNumber);
        });
        _closeTimers.insert(zoneNumber, timer);
    }

    _bankRetryTimer.setSingleShot(true);
    _bankRetryTimer.setInterval(static_cast<int>(RetryInterval.totalMilliseconds()));
    connect(&_bankRetryTimer, &QTimer::timeout, this, &ZoneController::onBankRetryTimer);

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

QList<int> ZoneController::activeZoneNumbers() const
{
    QList<int> result;
    for(auto it = _open.constBegin(); it != _open.constEnd(); ++it) {
        if(it.value().closing == false) {
            result.append(it.key());
        }
    }
    return result;
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

    if(_bankClosePending == true) {
        _errorText = QString("Refused to open zone %1: closing every zone is pending a retry").arg(zoneNumber);
        return false;
    }

    if(_zoneGpioMap.contains(zoneNumber) == false) {
        _errorText = QString("Unknown zone %1").arg(zoneNumber);
        return false;
    }

    if(_open.contains(zoneNumber) == true && _open.value(zoneNumber).closing == true) {
        _errorText = QString("Refused to open zone %1: its close is pending a retry").arg(zoneNumber);
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
        QList<int> next = activeZoneNumbers();
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

    QList<int> remaining = activeZoneNumbers();
    remaining.removeAll(zoneNumber);
    if(_bank->isRequested() == true && writeOpenSet(remaining) == false) {
        logText(LVL_ERROR, QString("Failed to close zone %1: %2").arg(zoneNumber).arg(_errorText));
        OpenZone& zone = _open[zoneNumber];
        zone.closing = true;
        zone.pendingReason = reason;
        startCloseTimer(zoneNumber, RetryInterval);
        return false;
    }

    // The zone leaves _open before zoneClosed: a slot that opens a zone writes the bank
    // from the active zones in _open, and a zone still listed there as active is re-energised.
    _closeTimers.value(zoneNumber)->stop();
    _open.remove(zoneNumber);
    if(_open.isEmpty() == true) {
        _bankRetryTimer.stop();
        _bankClosePending = false;
    }

    emit zoneClosed(zoneNumber, reason);
    return true;
}

bool ZoneController::closeAll(CloseReason reason)
{
    const QList<int> closing = _open.keys();

    if(_bank->isRequested() == true && writeOpenSet(QList<int>()) == false) {
        logText(LVL_ERROR, QString("allOff failed: %1").arg(_errorText));
        if(closing.isEmpty() == false) {
            for(int zoneNumber : closing) {
                OpenZone& zone = _open[zoneNumber];
                zone.closing = true;
                zone.pendingReason = reason;
                _closeTimers.value(zoneNumber)->stop();
            }
            _bankClosePending = true;
            _bankCloseReason = reason;
            _bankRetryTimer.start();
        }
        return false;
    }

    _bankRetryTimer.stop();
    _bankClosePending = false;

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

bool ZoneController::isClosing(int zoneNumber) const
{
    return _open.contains(zoneNumber) && _open.value(zoneNumber).closing;
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
    if(_bankClosePending == true && _open.contains(zoneNumber) == true) {
        _bankRetryTimer.stop();
        onBankRetryTimer();
    }
    else {
        onCloseTimer(zoneNumber);
    }
}

bool ZoneController::closeTimerActiveForTest(int zoneNumber) const
{
    QTimer* timer = _closeTimers.value(zoneNumber, nullptr);
    if(timer != nullptr && timer->isActive()) {
        return true;
    }
    return _bankRetryTimer.isActive() && _open.contains(zoneNumber);
}

void ZoneController::onCloseTimer(int zoneNumber)
{
    if(_open.contains(zoneNumber) == false) {
        return;
    }

    closeOne(zoneNumber, _open.value(zoneNumber).pendingReason);
}

void ZoneController::onBankRetryTimer()
{
    if(_open.isEmpty() == true) {
        _bankClosePending = false;
        return;
    }

    closeAll(_bankCloseReason);
}

void ZoneController::onWatchdogTimer()
{
    QMap<quint32, bool> actual;
    if(_bank->readValues(actual) == false) {
        logText(LVL_ERROR, QString("Watchdog could not read the bank: %1").arg(_bank->errorText()));
        tripWatchdog();
        return;
    }

    bool mismatch = false;
    for(auto it = _zoneGpioMap.constBegin(); it != _zoneGpioMap.constEnd(); ++it) {
        const bool closing = _open.contains(it.key()) && _open.value(it.key()).closing;
        if(actual.contains(it.value()) == false) {
            mismatch = true;
        }
        else if(closing == false && actual.value(it.value()) != _open.contains(it.key())) {
            mismatch = true;
        }
    }
    if(actual.count() != _zoneGpioMap.count()) {
        mismatch = true;
    }

    int asserted = 0;
    for(bool value : actual) {
        if(value == true) {
            asserted++;
        }
    }

    int allowed = 0;
    QList<int> late;
    QList<int> due;
    for(auto it = _open.constBegin(); it != _open.constEnd(); ++it) {
        allowed = qMax(allowed, it.value().capAtOpen);
        if(it.value().deadline.hasExpired()) {
            if(it.value().closing == false && _closeTimers.value(it.key())->isActive()) {
                due.append(it.key());
            }
            else {
                late.append(it.key());
            }
        }
    }

    const bool overCount = asserted > allowed;
    const bool nothingOpen = _open.isEmpty();

    if(mismatch || overCount || late.isEmpty() == false) {
        logText(LVL_ERROR, QString("Watchdog tripped: open zones [%1], past deadline [%2], %3 lines asserted, %4 allowed")
                               .arg(zoneListText(_open.keys()), zoneListText(late))
                               .arg(asserted).arg(allowed));
        tripWatchdog();
        return;
    }

    for(int zoneNumber : due) {
        // A slot answering an earlier zone's zoneClosed may have closed or re-opened this one.
        const bool stillDue = _open.contains(zoneNumber)
                              && _open.value(zoneNumber).closing == false
                              && _open.value(zoneNumber).deadline.hasExpired();
        if(stillDue == true && closeOne(zoneNumber, CloseReason::Deadline) == false) {
            logText(LVL_ERROR, QString("Watchdog could not close zone %1 at its deadline").arg(zoneNumber));
            tripWatchdog();
            return;
        }
    }

    if(_faulted == true && nothingOpen == true) {
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
