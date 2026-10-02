#include "programqueue.h"

#include "database/irrigationdatasource.h"
#include "programrunner.h"

ProgramQueue::ProgramQueue(ProgramRunner* runner, IrrigationDataSource* source, IClock* clock, QObject* parent) :
    QObject(parent),
    LoggingBaseClass("queue"),
    _runner(runner),
    _source(source),
    _clock(clock)
{
    connect(_runner, &ProgramRunner::programFinished, this, &ProgramQueue::onProgramEnded);
    connect(_runner, &ProgramRunner::programAborted, this, &ProgramQueue::onProgramEnded);
}

void ProgramQueue::enqueueScheduled(int programId, int startTimeId, const QDateTime& scheduledAtUtc)
{
    Entry entry;
    entry.programId = programId;
    entry.startTimeId = startTimeId;
    entry.scheduledAtUtc = scheduledAtUtc;
    entry.queuedAtUtc = _clock->nowUtc();

    if(contains(programId)) {
        logText(LVL_WARNING, QString("Program %1 came due while already queued").arg(programId));
        setOutcome(entry, FiredInstant::Outcome::SkippedDuplicate);
    }
    else if(_runner->isRunning() == false && _entries.isEmpty()) {
        start(entry);
    }
    else {
        logText(LVL_INFO, QString("Program %1 queued behind program %2")
                              .arg(programId).arg(_runner->runningProgramId()));
        _entries.append(entry);
    }
}

RunRequest::Refusal ProgramQueue::enqueueManual(int programId)
{
    if(_runner->runningProgramId() == programId || contains(programId)) {
        return RunRequest::Refusal::AlreadyQueued;
    }

    Entry entry;
    entry.programId = programId;
    entry.queuedAtUtc = _clock->nowUtc();

    if(_runner->isRunning() == false && _entries.isEmpty()) {
        return start(entry) == true ? RunRequest::Refusal::None : RunRequest::Refusal::Failed;
    }

    logText(LVL_INFO, QString("Manual run of program %1 queued behind program %2")
                          .arg(programId).arg(_runner->runningProgramId()));
    _entries.append(entry);
    return RunRequest::Refusal::None;
}

void ProgramQueue::dropAll(FiredInstant::Outcome outcome)
{
    const QList<Entry> dropped = _entries;
    _entries.clear();

    for(const Entry& entry : dropped) {
        if(entry.isScheduled()) {
            setOutcome(entry, outcome);
        }
    }

    if(dropped.isEmpty() == false) {
        logText(LVL_WARNING, QString("Dropped %1 queued programs as %2")
                                 .arg(dropped.count()).arg(FiredInstant::outcomeToString(outcome)));
    }
}

bool ProgramQueue::recordRestartDrops()
{
    int dropped = 0;
    if(_source->replaceFiringOutcomes(FiredInstant::Outcome::Queued, FiredInstant::Outcome::DroppedRestart, &dropped) == false) {
        logText(LVL_ERROR, "Failed to record the queued programs a restart dropped");
        return false;
    }

    if(dropped > 0) {
        logText(LVL_WARNING, QString("%1 queued programs were lost to a restart and recorded dropped_restart").arg(dropped));
    }
    return true;
}

void ProgramQueue::onProgramEnded(int programId)
{
    Q_UNUSED(programId)
    startNext();
}

bool ProgramQueue::start(const Entry& entry)
{
    const bool started = _runner->startProgram(entry.programId);
    if(started == false) {
        logText(LVL_ERROR, QString("Program %1 failed to start").arg(entry.programId));
    }

    if(entry.isScheduled()) {
        setOutcome(entry, started == true ? FiredInstant::Outcome::Ran : FiredInstant::Outcome::Failed);
    }

    return started;
}

void ProgramQueue::startNext()
{
    while(_runner->isRunning() == false && _entries.isEmpty() == false) {
        const Entry entry = _entries.takeFirst();

        if(_source->isMasterEnabled() == false) {
            logText(LVL_WARNING, QString("Dropping queued program %1: the master enable is off").arg(entry.programId));
            if(entry.isScheduled()) {
                setOutcome(entry, FiredInstant::Outcome::SkippedDisabled);
            }
        }
        else if(entry.isScheduled() && isRainDelayed()) {
            logText(LVL_WARNING, QString("Dropping queued program %1: a rain delay is in force").arg(entry.programId));
            setOutcome(entry, FiredInstant::Outcome::SkippedRain);
        }
        else {
            start(entry);
        }
    }
}

bool ProgramQueue::contains(int programId) const
{
    for(const Entry& entry : _entries) {
        if(entry.programId == programId) {
            return true;
        }
    }
    return false;
}

bool ProgramQueue::isRainDelayed()
{
    const QDateTime until = QDateTime::fromString(_source->settingValue("rain_delay_until"), Qt::ISODate);
    return until.isValid() && _clock->nowUtc() < until.toUTC();
}

void ProgramQueue::setOutcome(const Entry& entry, FiredInstant::Outcome outcome)
{
    if(_source->setFiringOutcome(entry.programId, entry.startTimeId, entry.scheduledAtUtc, outcome) == false) {
        logText(LVL_ERROR, QString("Failed to record program %1 at %2 as %3")
                               .arg(entry.programId)
                               .arg(entry.scheduledAtUtc.toUTC().toString(Qt::ISODate))
                               .arg(FiredInstant::outcomeToString(outcome)));
    }
}

#include "moc_programqueue.cpp"
