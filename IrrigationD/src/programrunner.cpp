#include "programrunner.h"

#include "database/irrigationdatasource.h"
#include "model/zone.h"
#include "zonecontroller.h"

ProgramRunner::ProgramRunner(ZoneController* controller, IrrigationDataSource* source, QObject* parent) :
    QObject(parent),
    LoggingBaseClass("program"),
    _controller(controller),
    _source(source)
{
    connect(_controller, &ZoneController::zoneClosed, this, &ProgramRunner::onZoneClosed);
    connect(_controller, &ZoneController::watchdogTripped, this, &ProgramRunner::onWatchdogTripped);
}

bool ProgramRunner::startProgram(int programId)
{
    if(_running) {
        return false;
    }

    _zones = _source->zonesFor(programId);
    _programId = programId;
    _index = -1;
    _expectedZone = 0;
    _running = true;

    emit programStarted(programId);

    return advance();
}

void ProgramRunner::abort()
{
    if(_running == false) {
        return;
    }

    stopRunning();
}

void ProgramRunner::stopRunning()
{
    const int stopped = _programId;

    // _running must already be false before allOff() runs: allOff() emits zoneClosed
    // synchronously and onZoneClosed() gates advancing on _running.
    _running = false;
    _programId = 0;
    _expectedZone = 0;

    if(_controller->allOff() == false) {
        logText(LVL_ERROR, QString("Failed to close the valve stopping program %1: %2")
                                .arg(stopped).arg(_controller->errorText()));
    }

    emit programAborted(stopped);
}

bool ProgramRunner::advance()
{
    _index++;

    if(_index >= _zones.count()) {
        const int finished = _programId;
        _running = false;
        _programId = 0;
        _expectedZone = 0;
        emit programFinished(finished);
        return true;
    }

    const ProgramZone next = _zones.at(_index);
    const ZoneList zones = _source->allZones();

    int zoneNumber = 0;
    bool zoneEnabled = false;
    for(const Zone& zone : zones) {
        if(zone.id == next.zoneId) {
            zoneNumber = zone.number;
            zoneEnabled = zone.enabled;
            break;
        }
    }

    if(zoneNumber == 0) {
        logText(LVL_ERROR, QString("Program %1 references unknown zone id %2")
                               .arg(_programId).arg(next.zoneId));
        return advance();
    }

    if(zoneEnabled == false) {
        logText(LVL_WARNING, QString("Program %1 skips disabled zone %2")
                                 .arg(_programId).arg(zoneNumber));
        return advance();
    }

    if(_controller->openZone(zoneNumber, next.durationSeconds) == false) {
        logText(LVL_ERROR, QString("Program %1 failed to open zone %2: %3")
                               .arg(_programId).arg(zoneNumber).arg(_controller->errorText()));
        stopRunning();
        return false;
    }

    _expectedZone = zoneNumber;
    return true;
}

void ProgramRunner::onZoneClosed(int zoneNumber)
{
    if(_running == false || zoneNumber != _expectedZone) {
        return;
    }

    _expectedZone = 0;
    advance();
}

void ProgramRunner::onWatchdogTripped(int zoneNumber)
{
    Q_UNUSED(zoneNumber)

    if(_running == false) {
        return;
    }

    logText(LVL_ERROR, QString("Watchdog tripped while running program %1; aborting").arg(_programId));
    stopRunning();
}

#include "moc_programrunner.cpp"
