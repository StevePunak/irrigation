#include "programrunner.h"

#include "database/irrigationdatasource.h"
#include "model/program.h"
#include "model/zone.h"

#include <algorithm>

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

    _steps = _source->stepsFor(programId);
    _programName.clear();
    const ProgramList programs = _source->allPrograms();
    for(const Program& program : programs) {
        if(program.id == programId) {
            _programName = program.name;
            break;
        }
    }

    _programId = programId;
    _stepIndex = -1;
    _waiting.clear();
    _openZones.clear();
    _running = true;

    logText(LVL_INFO, QString("Program %1 '%2' starts with %3 steps").arg(programId).arg(_programName).arg(_steps.count()));
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

void ProgramRunner::fillSlots()
{
    if(_running == false || _waiting.isEmpty()) {
        return;
    }

    openWaiting();
}

bool ProgramRunner::advance()
{
    while(_running == true) {
        _stepIndex++;
        if(_stepIndex >= _steps.count()) {
            finish();
            return true;
        }

        loadStep();
        if(_waiting.isEmpty()) {
            logText(LVL_WARNING, QString("Program %1 step %2 has no enabled zone; moving on")
                                     .arg(_programId).arg(_stepIndex + 1));
            continue;
        }

        logText(LVL_INFO, QString("Program %1 starts step %2 of %3")
                              .arg(_programId).arg(_stepIndex + 1).arg(_steps.count()));
        return openWaiting();
    }

    return false;
}

void ProgramRunner::loadStep()
{
    _waiting.clear();
    _openZones.clear();

    const ProgramStep& step = _steps.at(_stepIndex);
    const ZoneList zones = _source->allZones();

    for(int zoneId : step.zoneIds) {
        int zoneNumber = 0;
        bool zoneEnabled = false;
        for(const Zone& zone : zones) {
            if(zone.id == zoneId) {
                zoneNumber = zone.number;
                zoneEnabled = zone.enabled;
                break;
            }
        }

        if(zoneNumber == 0) {
            logText(LVL_ERROR, QString("Program %1 references unknown zone id %2").arg(_programId).arg(zoneId));
        }
        else if(zoneEnabled == false) {
            logText(LVL_WARNING, QString("Program %1 skips disabled zone %2").arg(_programId).arg(zoneNumber));
        }
        else if(_waiting.contains(zoneNumber) == false) {
            _waiting.append(zoneNumber);
        }
    }

    std::sort(_waiting.begin(), _waiting.end());
}

bool ProgramRunner::openWaiting()
{
    const int duration = _steps.at(_stepIndex).durationSeconds;
    const QList<int> candidates = _waiting;
    QList<int> stillWaiting;

    for(int zoneNumber : candidates) {
        // hasSlotFor() is true for a zone whose close is pending a retry, and openZone() refuses it.
        if(_controller->isClosing(zoneNumber) == true || _controller->hasSlotFor(zoneNumber) == false) {
            stillWaiting.append(zoneNumber);
            continue;
        }

        if(_controller->openZone(zoneNumber, duration) == false) {
            logText(LVL_ERROR, QString("Program %1 failed to open zone %2: %3")
                                   .arg(_programId).arg(zoneNumber).arg(_controller->errorText()));
            stopRunning();
            return false;
        }

        _openZones.append(zoneNumber);
    }

    _waiting = stillWaiting;
    return true;
}

void ProgramRunner::finish()
{
    const int finished = _programId;

    _running = false;
    _programId = 0;
    _programName.clear();
    _stepIndex = -1;
    _steps.clear();
    _waiting.clear();
    _openZones.clear();

    logText(LVL_INFO, QString("Program %1 finished").arg(finished));
    emit programFinished(finished);
}

void ProgramRunner::stopRunning()
{
    const int stopped = _programId;
    const QList<int> owned = _openZones;

    // State is cleared before any close: closeZone() emits zoneClosed synchronously,
    // and onZoneClosed() would otherwise open waiting zones of the program being stopped.
    _running = false;
    _programId = 0;
    _programName.clear();
    _stepIndex = -1;
    _steps.clear();
    _waiting.clear();
    _openZones.clear();

    for(int zoneNumber : owned) {
        if(_controller->closeZone(zoneNumber) == false) {
            logText(LVL_ERROR, QString("Failed to close zone %1 stopping program %2: %3")
                                   .arg(zoneNumber).arg(stopped).arg(_controller->errorText()));
        }
    }

    logText(LVL_WARNING, QString("Program %1 aborted").arg(stopped));
    emit programAborted(stopped);
}

void ProgramRunner::onZoneClosed(int zoneNumber, ZoneController::CloseReason reason)
{
    if(_running == false) {
        return;
    }

    if(reason == ZoneController::CloseReason::AllOff || reason == ZoneController::CloseReason::Watchdog) {
        if(_openZones.contains(zoneNumber) || _waiting.contains(zoneNumber)) {
            logText(LVL_WARNING, QString("Zone %1 of program %2 was closed by an all-off; aborting")
                                     .arg(zoneNumber).arg(_programId));
            stopRunning();
        }
        return;
    }

    _openZones.removeAll(zoneNumber);

    if(openWaiting() == false) {
        return;
    }

    if(_waiting.isEmpty() && _openZones.isEmpty()) {
        advance();
    }
}

void ProgramRunner::onWatchdogTripped()
{
    if(_running == false) {
        return;
    }

    logText(LVL_ERROR, QString("Watchdog tripped while running program %1; aborting").arg(_programId));
    stopRunning();
}

#include "moc_programrunner.cpp"
