#include "panelcontroller.h"

#include "panelformat.h"

PanelController::PanelController(IPanelHost* host, IClock* clock, QObject* parent) :
    QObject(parent),
    LoggingBaseClass("panel"),
    _host(host),
    _clock(clock),
    _timeZone(QTimeZone::UTC)
{
}

void PanelController::setInitialRunLine(bool low)
{
    _lineLow = low;
    _lowSinceMsecs = _clock->monotonicMsecs();
    _stuckLogged = false;
}

void PanelController::cancel()
{
    _mode = Mode::Idle;
    _selectedZone = 0;
    _panelZone = 0;
    _messageUntilMsecs = 0;
}

void PanelController::onRunLineChanged(bool low)
{
    if(low == false) {
        if(_stuckLogged == true) {
            logText(LVL_INFO, "The RUN line reads high again");
        }
        _lineLow = false;
        _stuckLogged = false;
        return;
    }

    if(_lineLow == true) {
        return;
    }

    _lineLow = true;
    _lowSinceMsecs = _clock->monotonicMsecs();
    press();
}

void PanelController::tick()
{
    const qint64 now = _clock->monotonicMsecs();
    PanelSnapshot snapshot = _host->panelSnapshot();

    if(_lineLow == true && _stuckLogged == false && now - _lowSinceMsecs > StuckLineMsecs) {
        logText(LVL_WARNING, QString("The RUN line has read low for more than %1 seconds and is stuck")
                                 .arg(StuckLineMsecs / 1000));
        _stuckLogged = true;
    }

    bool changed = endRunIfClosed(snapshot);
    if(_mode == Mode::Selecting) {
        if(snapshot.stopHeld == true || snapshot.faulted == true) {
            logText(LVL_INFO, "The selection was cancelled: STOP is held or the controller is faulted");
            _mode = Mode::Idle;
            _selectedZone = 0;
            changed = true;
        }
        else if(now - _lastPressMsecs >= CommitDelayMsecs) {
            commit(snapshot);
            changed = true;
        }
    }

    if(changed == true) {
        snapshot = _host->panelSnapshot();
        endRunIfClosed(snapshot);
        emit stateChanged();
    }
    publishFrame(snapshot);
}

void PanelController::press()
{
    PanelSnapshot snapshot = _host->panelSnapshot();
    if(snapshot.stopHeld == true || snapshot.faulted == true) {
        logText(LVL_INFO, "RUN ignored: STOP is held or the controller is faulted");
        publishFrame(snapshot);
        return;
    }

    endRunIfClosed(snapshot);
    _messageUntilMsecs = 0;

    switch(_mode) {
    case Mode::Idle:
        pressWhileIdle(snapshot);
        break;
    case Mode::Selecting:
        pressWhileSelecting(snapshot);
        break;
    case Mode::PanelRun:
        pressDuringRun(snapshot);
        break;
    }

    snapshot = _host->panelSnapshot();
    endRunIfClosed(snapshot);
    emit stateChanged();
    publishFrame(snapshot);
}

void PanelController::pressWhileIdle(const PanelSnapshot& snapshot)
{
    if(snapshot.masterEnabled == false) {
        logText(LVL_WARNING, "RUN refused: watering is turned off");
        showMessage(PanelFormat::masterOff());
        return;
    }
    if(snapshot.enabledZones.isEmpty() == true) {
        logText(LVL_WARNING, "RUN refused: no zone is enabled");
        showMessage(PanelFormat::noZones());
        return;
    }

    if(snapshot.openZones.isEmpty() == false) {
        logText(LVL_WARNING, "RUN takes over the running zones");
        _host->takeOverForPanel();
    }

    _mode = Mode::Selecting;
    _selectedZone = snapshot.enabledZones.first();
    _lastPressMsecs = _clock->monotonicMsecs();
}

void PanelController::pressWhileSelecting(const PanelSnapshot& snapshot)
{
    const int next = nextEnabledZone(snapshot.enabledZones, _selectedZone, true);
    if(next == 0) {
        logText(LVL_WARNING, "RUN refused: no zone is enabled");
        _mode = Mode::Idle;
        _selectedZone = 0;
        showMessage(PanelFormat::noZones());
        return;
    }

    _selectedZone = next;
    _lastPressMsecs = _clock->monotonicMsecs();
}

void PanelController::pressDuringRun(const PanelSnapshot& snapshot)
{
    const int next = nextEnabledZone(snapshot.enabledZones, _panelZone, false);
    if(next == 0) {
        const int closing = _panelZone;
        logText(LVL_INFO, QString("RUN closes zone %1, the last enabled zone; the panel run ends").arg(closing));
        _mode = Mode::Idle;
        _panelZone = 0;
        _host->closePanelZone(closing);
        return;
    }

    const RunRequest::Refusal refusal = _host->openPanelZone(next, _panelZone);
    if(refusal != RunRequest::Refusal::None) {
        showRefusal(refusal);
        return;
    }

    logText(LVL_INFO, QString("RUN moves the panel run from zone %1 to zone %2").arg(_panelZone).arg(next));
    _panelZone = next;
}

void PanelController::commit(const PanelSnapshot& snapshot)
{
    const int zoneNumber = _selectedZone;
    _mode = Mode::Idle;
    _selectedZone = 0;

    if(snapshot.enabledZones.contains(zoneNumber) == false) {
        logText(LVL_WARNING, QString("Zone %1 was disabled during the selection; nothing starts").arg(zoneNumber));
        return;
    }

    const RunRequest::Refusal refusal = _host->openPanelZone(zoneNumber, 0);
    if(refusal != RunRequest::Refusal::None) {
        showRefusal(refusal);
        return;
    }

    logText(LVL_INFO, QString("Zone %1 starts as a panel run").arg(zoneNumber));
    _mode = Mode::PanelRun;
    _panelZone = zoneNumber;
}

bool PanelController::endRunIfClosed(const PanelSnapshot& snapshot)
{
    if(_mode != Mode::PanelRun || snapshot.secondsRemaining(_panelZone) >= 0) {
        return false;
    }

    logText(LVL_INFO, QString("Zone %1 closed; the panel run ends").arg(_panelZone));
    _mode = Mode::Idle;
    _panelZone = 0;
    return true;
}

void PanelController::showRefusal(RunRequest::Refusal refusal)
{
    if(refusal == RunRequest::Refusal::CapReached) {
        showMessage(PanelFormat::full());
    }
    else if(refusal == RunRequest::Refusal::MasterDisabled) {
        showMessage(PanelFormat::masterOff());
    }
}

void PanelController::showMessage(const QByteArray& segments)
{
    _message = segments;
    _messageUntilMsecs = _clock->monotonicMsecs() + MessageMsecs;
}

void PanelController::publishFrame(const PanelSnapshot& snapshot)
{
    const QByteArray next = render(snapshot);
    if(next != _lastFrame) {
        _lastFrame = next;
        emit frameChanged(next);
    }
}

QByteArray PanelController::render(const PanelSnapshot& snapshot)
{
    const qint64 now = _clock->monotonicMsecs();
    QByteArray result;
    bool showingOthers = false;

    if(snapshot.stopHeld == true) {
        result = PanelFormat::stopHeld();
    }
    else if(snapshot.faulted == true) {
        result = PanelFormat::fault();
    }
    else if(now < _messageUntilMsecs) {
        result = _message;
    }
    else if(_mode == Mode::Selecting) {
        const bool zoneVisible = (now - _lastPressMsecs) % (2 * BlinkHalfPeriodMsecs) < BlinkHalfPeriodMsecs;
        result = PanelFormat::zoneMinutes(_selectedZone, snapshot.runMinutes, zoneVisible);
    }
    else if(_mode == Mode::PanelRun) {
        result = PanelFormat::zoneMinutes(_panelZone, PanelFormat::minutesLeft(snapshot.secondsRemaining(_panelZone)), true);
    }
    else if(snapshot.openZones.isEmpty() == false) {
        result = otherZonesFrame(snapshot, now);
        showingOthers = true;
    }
    else {
        const QTime local = _clock->nowUtc().toTimeZone(_timeZone).time();
        result = PanelFormat::clock(local, local.msec() < BlinkHalfPeriodMsecs);
    }

    if(showingOthers == false) {
        _shownZone = 0;
    }
    return result;
}

QByteArray PanelController::otherZonesFrame(const PanelSnapshot& snapshot, qint64 now)
{
    if(snapshot.secondsRemaining(_shownZone) < 0 || now - _shownSinceMsecs >= AlternateMsecs) {
        int next = snapshot.openZones.first().zone;
        for(const PanelSnapshot::OpenZone& open : snapshot.openZones) {
            if(open.zone > _shownZone) {
                next = open.zone;
                break;
            }
        }
        _shownZone = next;
        _shownSinceMsecs = now;
    }

    return PanelFormat::zoneMinutes(_shownZone, PanelFormat::minutesLeft(snapshot.secondsRemaining(_shownZone)), true);
}

int PanelController::nextEnabledZone(const QList<int>& enabledZones, int afterZone, bool wrap)
{
    for(int zoneNumber : enabledZones) {
        if(zoneNumber > afterZone) {
            return zoneNumber;
        }
    }
    return wrap == true && enabledZones.isEmpty() == false ? enabledZones.first() : 0;
}

#include "moc_panelcontroller.cpp"
