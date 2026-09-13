#include "scheduler.h"

#include <QTimeZone>

#include "database/irrigationdatasource.h"

Scheduler::Scheduler(IrrigationDataSource* source, IClock* clock, QObject* parent) :
    QObject(parent),
    _source(source),
    _clock(clock)
{
    _tickTimer.setInterval(1000);
    connect(&_tickTimer, &QTimer::timeout, this, &Scheduler::tick);
}

void Scheduler::start()
{
    _tickTimer.start();
}

void Scheduler::stop()
{
    _tickTimer.stop();
}

bool Scheduler::isWateringDay(const Program& program, const QDate& localDate)
{
    switch(program.dayMode) {
    case Program::DayMode::DaysOfWeek:
        // QDate::dayOfWeek() is 1..7 with Monday == 1; bit 0 is Monday.
        return (program.dowMask & (1 << (localDate.dayOfWeek() - 1))) != 0;

    case Program::DayMode::Odd:
        return (localDate.day() % 2) == 1;

    case Program::DayMode::Even:
        return (localDate.day() % 2) == 0;

    case Program::DayMode::EveryNDays:
        if(program.intervalDays < 1 || program.anchorDate.isValid() == false) {
            return false;
        }
        if(localDate < program.anchorDate) {
            return false;
        }
        return (program.anchorDate.daysTo(localDate) % program.intervalDays) == 0;
    }

    return false;
}

QDateTime Scheduler::resolveToUtc(const ProgramStartTime& startTime, const QDate& localDate, bool* valid)
{
    if(valid != nullptr) {
        *valid = false;
    }

    QTimeZone zone(startTime.timezone.toUtf8());
    if(zone.isValid() == false) {
        return QDateTime();
    }

    const QTime localTime = QTime(0, 0).addSecs(startTime.minutesAfterMidnight * 60);

    QDateTime rejected(localDate, localTime, zone, QDateTime::TransitionResolution::Reject);
    if(rejected.isValid() == false) {
        return QDateTime();
    }

    // PreferBefore alone still returns a valid instant inside a spring-forward gap by
    // sliding to an adjacent time; the Reject construction above is what detects the gap.
    // When the local time occurs twice (fall back), PreferBefore selects the earlier one.
    QDateTime resolved(localDate, localTime, zone, QDateTime::TransitionResolution::PreferBefore);
    if(resolved.isValid() == false) {
        return QDateTime();
    }

    if(valid != nullptr) {
        *valid = true;
    }
    return resolved.toUTC();
}

void Scheduler::tick()
{
    const QDateTime nowUtc = _clock->nowUtc();

    if(_source->settingValue("master_enabled") == "0") {
        return;
    }

    const QDateTime rainDelayUntil =
        QDateTime::fromString(_source->settingValue("rain_delay_until"), Qt::ISODate);
    const bool rainDelayed = rainDelayUntil.isValid() && nowUtc < rainDelayUntil.toUTC();

    const ProgramList programs = _source->enabledPrograms();
    for(const Program& program : programs) {
        const ProgramStartTimeList startTimes = _source->startTimesFor(program.id);
        for(const ProgramStartTime& startTime : startTimes) {
            QTimeZone zone(startTime.timezone.toUtf8());
            if(zone.isValid() == false) {
                continue;
            }

            const QDate localDate = nowUtc.toTimeZone(zone).date();
            if(isWateringDay(program, localDate) == false) {
                continue;
            }

            bool valid = false;
            const QDateTime scheduledUtc = resolveToUtc(startTime, localDate, &valid);
            if(valid == false) {
                recordOnce(program.id, startTime.id,
                           QDateTime(localDate, QTime(0, 0), QTimeZone::UTC)
                               .addSecs(startTime.minutesAfterMidnight * 60),
                           FiredInstant::Outcome::Missed);
                continue;
            }

            if(_source->hasFired(program.id, startTime.id, scheduledUtc)) {
                continue;
            }

            const qint64 lateBy = scheduledUtc.secsTo(nowUtc);
            if(lateBy < 0) {
                continue;
            }

            if(lateBy > GraceWindowSeconds) {
                recordOnce(program.id, startTime.id, scheduledUtc, FiredInstant::Outcome::Missed);
                continue;
            }

            if(rainDelayed) {
                recordOnce(program.id, startTime.id, scheduledUtc, FiredInstant::Outcome::SkippedRain);
                continue;
            }

            recordOnce(program.id, startTime.id, scheduledUtc, FiredInstant::Outcome::Ran);
            emit programDue(program.id, startTime.id, scheduledUtc);
        }
    }
}

void Scheduler::recordOnce(int programId, int startTimeId, const QDateTime& scheduledAtUtc, FiredInstant::Outcome outcome)
{
    FiredInstant instant;
    instant.programId = programId;
    instant.startTimeId = startTimeId;
    instant.scheduledAtUtc = scheduledAtUtc;
    instant.outcome = outcome;
    _source->recordFiring(instant);
}

#include "moc_scheduler.cpp"
