#include "panelhost.h"

#include "database/irrigationdatasource.h"
#include "panelcontroller.h"
#include "panelformat.h"
#include "programqueue.h"
#include "programrunner.h"
#include "stopbutton.h"
#include "zonecontroller.h"

#include <algorithm>

PanelHost::PanelHost(ZoneController* controller,
                     ProgramRunner* runner,
                     ProgramQueue* queue,
                     IrrigationDataSource* source,
                     StopButton* stopButton) :
    LoggingBaseClass("panel"),
    _controller(controller),
    _runner(runner),
    _queue(queue),
    _source(source),
    _stopButton(stopButton),
    _runMinutes(PanelController::DefaultRunMinutes)
{
}

void PanelHost::setRunMinutes(int value)
{
    _runMinutes = qBound(PanelController::MinimumRunMinutes, value, PanelController::MaximumRunMinutes);
}

int PanelHost::runSeconds() const
{
    return qMin(_runMinutes * 60, _controller->maxZoneSeconds());
}

bool PanelHost::clearController(QString* errorText)
{
    // dropAll() precedes abort(): an aborted program starts the next queued one.
    _queue->dropAll(FiredInstant::Outcome::DroppedStop);
    _runner->abort();

    const bool success = _controller->allOff();
    if(success == false && errorText != nullptr) {
        *errorText = _controller->errorText();
    }
    return success;
}

PanelSnapshot PanelHost::panelSnapshot()
{
    PanelSnapshot snapshot;

    // PanelController's other-zones rotation assumes ascending order; openZoneNumbers()
    // returns QMap keys, which are already sorted.
    const QList<int> openZones = _controller->openZoneNumbers();
    for(int zoneNumber : openZones) {
        PanelSnapshot::OpenZone open;
        open.zone = zoneNumber;
        open.secondsRemaining = _controller->secondsRemaining(zoneNumber);
        snapshot.openZones.append(open);
    }

    snapshot.enabledZones = enabledZoneNumbers();
    snapshot.runMinutes = PanelFormat::minutesLeft(runSeconds());
    snapshot.stopHeld = _stopButton->isHeld();
    snapshot.faulted = _controller->isFaulted();
    snapshot.masterEnabled = _source->isMasterEnabled();
    return snapshot;
}

RunRequest::Refusal PanelHost::openPanelZone(int zoneNumber, int replacingZone)
{
    const bool replacing = replacingZone > 0 && replacingZone != zoneNumber && _controller->isOpen(replacingZone);

    if(_controller->isFaulted() == true) {
        logText(LVL_ERROR, QString("Refused a panel run of zone %1: %2").arg(zoneNumber).arg(_controller->errorText()));
        return RunRequest::Refusal::Failed;
    }
    if(_stopButton->isHeld() == true) {
        logText(LVL_WARNING, QString("Refused a panel run of zone %1: the stop button is held").arg(zoneNumber));
        return RunRequest::Refusal::StopHeld;
    }
    if(_source->isMasterEnabled() == false) {
        logText(LVL_WARNING, QString("Refused a panel run of zone %1: the master enable is off").arg(zoneNumber));
        return RunRequest::Refusal::MasterDisabled;
    }
    if(enabledZoneNumbers().contains(zoneNumber) == false) {
        logText(LVL_WARNING, QString("Refused a panel run of zone %1: the zone is disabled or has no database row").arg(zoneNumber));
        return RunRequest::Refusal::ZoneDisabled;
    }
    if(_controller->hasSlotFor(zoneNumber) == false && replacing == false) {
        logText(LVL_WARNING, QString("Refused a panel run of zone %1: %2 zones already running")
                                 .arg(zoneNumber).arg(static_cast<int>(_controller->openZoneNumbers().count())));
        return RunRequest::Refusal::CapReached;
    }

    const int seconds = runSeconds();

    if(_controller->hasSlotFor(zoneNumber) == false) {
        // closeZone() emits zoneClosed, and ProgramRunner refills the freed slot from its
        // waiting zones before closeZone() returns, so the slot can be gone again below.
        if(_controller->closeZone(replacingZone) == false) {
            logText(LVL_ERROR, QString("Failed to close zone %1: %2").arg(replacingZone).arg(_controller->errorText()));
            return RunRequest::Refusal::Failed;
        }
        if(_controller->hasSlotFor(zoneNumber) == false) {
            logText(LVL_WARNING, QString("Refused a panel run of zone %1: a waiting program zone took the slot zone %2 freed")
                                     .arg(zoneNumber).arg(replacingZone));
            return RunRequest::Refusal::CapReached;
        }
    }

    if(_controller->openZone(zoneNumber, seconds) == false) {
        logText(LVL_ERROR, QString("Failed to open zone %1 for %2 seconds: %3")
                               .arg(zoneNumber).arg(seconds).arg(_controller->errorText()));
        return RunRequest::Refusal::Failed;
    }

    if(replacing == true && _controller->isOpen(replacingZone) == true && _controller->closeZone(replacingZone) == false) {
        logText(LVL_ERROR, QString("Failed to close zone %1: %2").arg(replacingZone).arg(_controller->errorText()));
    }

    logText(LVL_INFO, QString("The panel opened zone %1 for %2 seconds").arg(zoneNumber).arg(seconds));
    return RunRequest::Refusal::None;
}

void PanelHost::closePanelZone(int zoneNumber)
{
    logText(LVL_INFO, QString("The panel closes zone %1").arg(zoneNumber));
    if(_controller->closeZone(zoneNumber) == false) {
        logText(LVL_ERROR, QString("Failed to close zone %1: %2").arg(zoneNumber).arg(_controller->errorText()));
    }
}

void PanelHost::takeOverForPanel()
{
    logText(LVL_WARNING, "RUN took over: closing every zone, aborting the program and emptying the queue");

    QString errorText;
    if(clearController(&errorText) == false) {
        logText(LVL_ERROR, QString("Failed to close the zones: %1").arg(errorText));
    }
}

QList<int> PanelHost::enabledZoneNumbers()
{
    QList<int> result;
    const ZoneList zones = _source->allZones();
    for(const Zone& zone : zones) {
        if(zone.enabled == true) {
            result.append(zone.number);
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}
