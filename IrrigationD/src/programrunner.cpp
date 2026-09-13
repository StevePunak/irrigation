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
}

bool ProgramRunner::startProgram(int programId)
{
    if(_running) {
        return false;
    }

    _zones = _source->zonesFor(programId);
    _programId = programId;
    _index = -1;
    _running = true;

    emit programStarted(programId);

    return advance();
}

void ProgramRunner::abort()
{
    if(_running == false) {
        return;
    }

    const int aborted = _programId;
    _running = false;
    _programId = 0;

    if(_controller->allOff() == false) {
        logText(LVL_ERROR, QString("Failed to close the valve aborting program %1: %2")
                                .arg(aborted).arg(_controller->errorText()));
    }

    emit programAborted(aborted);
}

bool ProgramRunner::advance()
{
    _index++;

    if(_index >= _zones.count()) {
        const int finished = _programId;
        _running = false;
        _programId = 0;
        emit programFinished(finished);
        return true;
    }

    const ProgramZone& next = _zones.at(_index);
    const ZoneList zones = _source->allZones();

    int zoneNumber = 0;
    for(const Zone& zone : zones) {
        if(zone.id == next.zoneId) {
            zoneNumber = zone.number;
            break;
        }
    }

    if(zoneNumber == 0) {
        logText(LVL_ERROR, QString("Program %1 references unknown zone id %2")
                               .arg(_programId).arg(next.zoneId));
        return advance();
    }

    return _controller->openZone(zoneNumber, next.durationSeconds);
}

void ProgramRunner::onZoneClosed(int zoneNumber)
{
    Q_UNUSED(zoneNumber)

    if(_running == false) {
        return;
    }

    advance();
}

#include "moc_programrunner.cpp"
