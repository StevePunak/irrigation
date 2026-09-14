#include "scheduler.h"

#include <QTimeZone>

#include "database/irrigationdatasource.h"

const TimeSpan Scheduler::TickInterval = TimeSpan::fromSeconds(1);

Scheduler::Scheduler(IrrigationDataSource* source, IClock* clock, QObject* parent) :
    QObject(parent),
    LoggingBaseClass("scheduler"),
    _source(source),
    _clock(clock)
{
    _tickTimer.setInterval(static_cast<int>(TickInterval.totalMilliseconds()));
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

    if(isValidMinutesAfterMidnight(startTime.minutesAfterMidnight) == false) {
        return QDateTime();
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
    // sliding to an adjacent time.
    QDateTime resolved(localDate, localTime, zone, QDateTime::TransitionResolution::PreferBefore);
    if(resolved.isValid() == false) {
        return QDateTime();
    }

    if(valid != nullptr) {
        *valid = true;
    }
    return resolved.toUTC();
}

QDateTime Scheduler::missedInstantFor(const ProgramStartTime& startTime, const QDate& localDate)
{
    if(isValidMinutesAfterMidnight(startTime.minutesAfterMidnight) == false) {
        return QDateTime();
    }

    const QTime localTime = QTime(0, 0).addSecs(startTime.minutesAfterMidnight * 60);

    QTimeZone zone(startTime.timezone.toUtf8());
    if(zone.isValid()) {
        const QDateTime slid(localDate, localTime, zone, QDateTime::TransitionResolution::PreferBefore);
        if(slid.isValid()) {
            return slid.toUTC();
        }
    }

    return QDateTime(localDate, localTime, QTimeZone::UTC);
}

bool Scheduler::isValidMinutesAfterMidnight(int minutesAfterMidnight)
{
    return minutesAfterMidnight >= 0 && minutesAfterMidnight < 24 * 60;
}

QDateTime Scheduler::nextRunUtc(const Program& program,
                                const ProgramStartTimeList& startTimes,
                                const QDateTime& nowUtc)
{
    QDateTime earliest;
    if(program.enabled == false) {
        return earliest;
    }

    for(const ProgramStartTime& startTime : startTimes) {
        const QTimeZone zone(startTime.timezone.toUtf8());
        const QDate today = zone.isValid() ? nowUtc.toTimeZone(zone).date() : nowUtc.date();

        for(int offset = 0; offset <= HorizonDays; offset++) {
            const QDate candidate = today.addDays(offset);
            if(isWateringDay(program, candidate) == false) {
                continue;
            }

            bool valid = false;
            const QDateTime resolved = resolveToUtc(startTime, candidate, &valid);
            if(valid == false || resolved < nowUtc) {
                continue;
            }

            if(earliest.isValid() == false || resolved < earliest) {
                earliest = resolved;
            }
            break;
        }
    }

    return earliest;
}

void Scheduler::tick()
{
    const QDateTime nowUtc = _clock->nowUtc();

    if(_source->isMasterEnabled() == false) {
        return;
    }

    const QDateTime rainDelayUntil =
        QDateTime::fromString(_source->settingValue("rain_delay_until"), Qt::ISODate);
    const bool rainDelayed = rainDelayUntil.isValid() && nowUtc < rainDelayUntil.toUTC();

    const ProgramList programs = _source->enabledPrograms();
    for(const Program& program : programs) {
        const ProgramStartTimeList startTimes = _source->startTimesFor(program.id);
        for(const ProgramStartTime& startTime : startTimes) {
            const QTimeZone zone(startTime.timezone.toUtf8());
            const QDate today = zone.isValid() ? nowUtc.toTimeZone(zone).date() : nowUtc.date();

            // Yesterday's local date is included: a late-night start time can still be
            // inside the grace window shortly after local midnight has passed.
            const QDate candidateDates[] = { today.addDays(-1), today };
            for(const QDate& localDate : candidateDates) {
                if(isWateringDay(program, localDate) == false) {
                    continue;
                }

                bool valid = false;
                const QDateTime scheduledUtc = resolveToUtc(startTime, localDate, &valid);

                if(valid == false) {
                    const QDateTime missedInstant = missedInstantFor(startTime, localDate);
                    if(missedInstant.isValid() == false) {
                        continue;
                    }
                    if(_source->hasFired(program.id, startTime.id, missedInstant)) {
                        continue;
                    }
                    recordOnce(program.id, startTime.id, missedInstant, FiredInstant::Outcome::Missed);
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

                if(recordOnce(program.id, startTime.id, scheduledUtc, FiredInstant::Outcome::Ran)) {
                    emit programDue(program.id, startTime.id, scheduledUtc);
                }
            }
        }
    }
}

bool Scheduler::recordOnce(int programId, int startTimeId, const QDateTime& scheduledAtUtc, FiredInstant::Outcome outcome)
{
    FiredInstant instant;
    instant.programId = programId;
    instant.startTimeId = startTimeId;
    instant.scheduledAtUtc = scheduledAtUtc;
    instant.outcome = outcome;

    const bool recorded = _source->recordFiring(instant);
    if(recorded == false) {
        logText(LVL_ERROR, QString("Failed to record firing for program %1, start time %2, at %3")
                                .arg(programId).arg(startTimeId).arg(scheduledAtUtc.toString(Qt::ISODate)));
    }
    return recorded;
}

#include "moc_scheduler.cpp"
