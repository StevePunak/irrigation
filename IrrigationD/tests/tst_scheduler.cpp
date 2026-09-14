#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QSqlQuery>
#include <QDateTime>
#include <QTimeZone>

#include "database/irrigationdatasource.h"
#include "iclock.h"
#include "scheduler.h"

static QString outcomeFor(IrrigationDataSource& source, int programId, int startTimeId)
{
    bool ok = false;
    QSqlQuery row = source.rawQuery(
        QString("SELECT outcome FROM fired_instants WHERE program_id = %1 AND start_time_id = %2")
            .arg(programId).arg(startTimeId), &ok);
    if(ok == false || row.next() == false) {
        return QString();
    }
    return row.value(0).toString();
}

static int firedInstantCount(IrrigationDataSource& source, int programId, int startTimeId)
{
    bool ok = false;
    QSqlQuery count = source.rawQuery(
        QString("SELECT COUNT(*) FROM fired_instants WHERE program_id = %1 AND start_time_id = %2")
            .arg(programId).arg(startTimeId), &ok);
    if(ok == false || count.next() == false) {
        return -1;
    }
    return count.value(0).toInt();
}

class TestScheduler : public QObject
{
    Q_OBJECT
private:
    // Every real program/start-time pair in this file must have program.id != startTime.id:
    // SQLite hands both tables id 1 on a fresh insert, and that coincidence would hide a
    // swapped programId/startTimeId bind. One filler program and two filler start times push
    // the real ids apart before they are used.
    static bool offsetIdsSoProgramAndStartTimeDiffer(IrrigationDataSource& source);

private slots:
    void initTestCase();

    void dayOfWeekMaskSelectsTheRightDays();
    void oddAndEvenUseTheDayOfMonth();
    void everyNDaysCountsFromTheAnchor();
    void springForwardGapIsRejected();
    void fallBackRepeatPicksTheEarlierOffset();
    void fallBackRepeatInANegativeDstZonePicksTheEarlierOffset();
    void aStartTimeDoesNotDriftAcrossDst();
    void minutesAfterMidnightOutOfRangeIsRejected();

    void nextRunUtcFindsTheEarliestUpcomingStartTime();
    void nextRunUtcSkipsNonWateringDays();
    void nextRunUtcReturnsInvalidWhenNoDayEverMatchesWithinTheHorizon();
    void nextRunUtcFindsAHitExactlyAtTheHorizon();
    void nextRunUtcResolvesAcrossDst();
    void nextRunUtcReturnsInvalidForADisabledProgram();
    void nextRunUtcIncludesAnInstantExactlyNow();

    void firesOnceInsideTheGraceWindow();
    void tickDoesNotFireBeforeTheScheduledInstant();
    void graceWindowFiresAt120SecondsAndMissesAt121();
    void missedOccurrenceIsRecordedAndNeverCaughtUp();
    void rainDelaySuppressesFiring();
    void expiredRainDelayNoLongerSuppressesFiring();
    void graceWindowCrossesLocalMidnight();
    void graceWindowCrossesLocalMidnightAheadOfUtc();
    void graceWindowCrossesLocalMidnightBehindUtc();
    void unrecognisedZoneRecordsMissedInstant();
    void springForwardGapRecordsSlidMissedInstant();
    void outOfRangeMinutesNeverReachesTheDatabase();
    void emissionIsGatedOnRecordFiringSucceeding();
    void masterDisabledSkipsTheTick();
    void masterEnabledValuesAllRunNormally_data();
    void masterEnabledValuesAllRunNormally();
    void masterEnabledDefaultsToEnabledWhenAbsent();
};

bool TestScheduler::offsetIdsSoProgramAndStartTimeDiffer(IrrigationDataSource& source)
{
    Program filler;
    filler.name = "Filler";
    if(source.insertProgram(filler) == false) {
        return false;
    }

    ProgramStartTime fillerStart1;
    fillerStart1.programId = filler.id;
    fillerStart1.minutesAfterMidnight = 0;
    fillerStart1.timezone = "UTC";
    if(source.insertStartTime(fillerStart1) == false) {
        return false;
    }

    ProgramStartTime fillerStart2;
    fillerStart2.programId = filler.id;
    fillerStart2.minutesAfterMidnight = 1;
    fillerStart2.timezone = "UTC";
    return source.insertStartTime(fillerStart2);
}

void TestScheduler::initTestCase()
{
    QVERIFY2(QTimeZone("America/Los_Angeles").isValid(),
             "tzdata is missing; every DST assertion below would silently pass against UTC");
}

void TestScheduler::dayOfWeekMaskSelectsTheRightDays()
{
    Program program;
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = (1 << 0) | (1 << 2);   // Monday and Wednesday

    QVERIFY(Scheduler::isWateringDay(program, QDate(2026, 9, 14)));   // Monday
    QVERIFY(Scheduler::isWateringDay(program, QDate(2026, 9, 15)) == false);
    QVERIFY(Scheduler::isWateringDay(program, QDate(2026, 9, 16)));   // Wednesday
    QVERIFY(Scheduler::isWateringDay(program, QDate(2026, 9, 20)) == false); // Sunday
}

void TestScheduler::oddAndEvenUseTheDayOfMonth()
{
    Program odd;
    odd.dayMode = Program::DayMode::Odd;
    QVERIFY(Scheduler::isWateringDay(odd, QDate(2026, 9, 15)));
    QVERIFY(Scheduler::isWateringDay(odd, QDate(2026, 9, 16)) == false);

    Program even;
    even.dayMode = Program::DayMode::Even;
    QVERIFY(Scheduler::isWateringDay(even, QDate(2026, 9, 16)));
    QVERIFY(Scheduler::isWateringDay(even, QDate(2026, 9, 15)) == false);
}

void TestScheduler::everyNDaysCountsFromTheAnchor()
{
    Program program;
    program.dayMode = Program::DayMode::EveryNDays;
    program.intervalDays = 3;
    program.anchorDate = QDate(2026, 9, 1);

    QVERIFY(Scheduler::isWateringDay(program, QDate(2026, 9, 1)));
    QVERIFY(Scheduler::isWateringDay(program, QDate(2026, 9, 2)) == false);
    QVERIFY(Scheduler::isWateringDay(program, QDate(2026, 9, 4)));
    QVERIFY(Scheduler::isWateringDay(program, QDate(2026, 9, 7)));

    // Dates before the anchor never water.
    QVERIFY(Scheduler::isWateringDay(program, QDate(2026, 8, 29)) == false);
}

void TestScheduler::springForwardGapIsRejected()
{
    // 2026-03-08 02:30 America/Los_Angeles does not exist.
    ProgramStartTime startTime;
    startTime.minutesAfterMidnight = 150;
    startTime.timezone = "America/Los_Angeles";

    bool valid = true;
    QDateTime resolved = Scheduler::resolveToUtc(startTime, QDate(2026, 3, 8), &valid);
    QCOMPARE(valid, false);
    QVERIFY(resolved.isValid() == false);
}

void TestScheduler::fallBackRepeatPicksTheEarlierOffset()
{
    // 2026-11-01 01:30 America/Los_Angeles occurs twice: 08:30Z (PDT) and 09:30Z (PST).
    ProgramStartTime startTime;
    startTime.minutesAfterMidnight = 90;
    startTime.timezone = "America/Los_Angeles";

    bool valid = false;
    const QDateTime resolved = Scheduler::resolveToUtc(startTime, QDate(2026, 11, 1), &valid);
    QVERIFY(valid);

    const QDateTime earlierOffset(QDate(2026, 11, 1), QTime(8, 30), QTimeZone::UTC);
    const QDateTime laterOffset(QDate(2026, 11, 1), QTime(9, 30), QTimeZone::UTC);
    QVERIFY(earlierOffset < laterOffset);
    QCOMPARE(resolved, earlierOffset);

    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(offsetIdsSoProgramAndStartTimeDiffer(source));

    Program program;
    program.name = "Fall back";
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = 1 << 6;   // Sunday only: 2026-11-01 is a Sunday, 2026-10-31 a Saturday
    QVERIFY(source.insertProgram(program));

    ProgramStartTime dbStartTime;
    dbStartTime.programId = program.id;
    dbStartTime.minutesAfterMidnight = 90;
    dbStartTime.timezone = "America/Los_Angeles";
    QVERIFY(source.insertStartTime(dbStartTime));
    QVERIFY(program.id != dbStartTime.id);

    TestClock clock(earlierOffset.addSecs(30));
    Scheduler scheduler(&source, &clock);

    QSignalSpy spy(&scheduler, &Scheduler::programDue);
    scheduler.tick();
    QCOMPARE(spy.count(), 1);

    // Still inside the same grace window: hasFired() blocks a second firing for the
    // same resolved instant.
    clock.advance(30);
    scheduler.tick();
    QCOMPARE(spy.count(), 1);

    // The second physical pass through the repeated local hour, an hour later:
    // resolveToUtc() yields the same earlier instant again, and hasFired() blocks it
    // before the grace-window check runs.
    clock.setNowUtc(laterOffset.addSecs(30));
    scheduler.tick();
    QCOMPARE(spy.count(), 1);

    QCOMPARE(firedInstantCount(source, program.id, dbStartTime.id), 1);
    QVERIFY(source.hasFired(program.id, dbStartTime.id, earlierOffset));
    QCOMPARE(outcomeFor(source, program.id, dbStartTime.id), QString("ran"));
}

void TestScheduler::fallBackRepeatInANegativeDstZonePicksTheEarlierOffset()
{
    // Europe/Dublin models winter as its daylight-saving period: legal standard time is
    // IST (UTC+1) and winter clock is a negative offset from it. 2026-10-25 01:30 happens
    // twice, at 00:30Z and 01:30Z; PreferDaylightSaving would pick the LATER of the two here,
    // the opposite of what it picks in a positive-DST zone like America/Los_Angeles.
    ProgramStartTime startTime;
    startTime.minutesAfterMidnight = 90;
    startTime.timezone = "Europe/Dublin";

    bool valid = false;
    const QDateTime resolved = Scheduler::resolveToUtc(startTime, QDate(2026, 10, 25), &valid);
    QVERIFY(valid);

    const QDateTime earlierOffset(QDate(2026, 10, 25), QTime(0, 30), QTimeZone::UTC);
    const QDateTime laterOffset(QDate(2026, 10, 25), QTime(1, 30), QTimeZone::UTC);
    QVERIFY(earlierOffset < laterOffset);
    QCOMPARE(resolved, earlierOffset);
}

void TestScheduler::aStartTimeDoesNotDriftAcrossDst()
{
    ProgramStartTime startTime;
    startTime.minutesAfterMidnight = 360;   // 06:00 local
    startTime.timezone = "America/Los_Angeles";

    bool valid = false;
    QDateTime winter = Scheduler::resolveToUtc(startTime, QDate(2026, 1, 15), &valid);
    QVERIFY(valid);
    QDateTime summer = Scheduler::resolveToUtc(startTime, QDate(2026, 7, 15), &valid);
    QVERIFY(valid);

    // Winter is PST (UTC-8) and summer is PDT (UTC-7): the same 06:00 wall clock
    // resolves an hour apart in UTC.
    QCOMPARE(winter.time(), QTime(14, 0));
    QCOMPARE(summer.time(), QTime(13, 0));
}

void TestScheduler::minutesAfterMidnightOutOfRangeIsRejected()
{
    QVERIFY(Scheduler::isValidMinutesAfterMidnight(0));
    QVERIFY(Scheduler::isValidMinutesAfterMidnight(1439));
    QVERIFY(Scheduler::isValidMinutesAfterMidnight(1440) == false);
    QVERIFY(Scheduler::isValidMinutesAfterMidnight(-1) == false);

    ProgramStartTime startTime;
    startTime.timezone = "UTC";

    startTime.minutesAfterMidnight = 1500;
    bool valid = true;
    QDateTime resolved = Scheduler::resolveToUtc(startTime, QDate(2026, 9, 15), &valid);
    QCOMPARE(valid, false);
    QVERIFY(resolved.isValid() == false);

    startTime.minutesAfterMidnight = -1;
    valid = true;
    resolved = Scheduler::resolveToUtc(startTime, QDate(2026, 9, 15), &valid);
    QCOMPARE(valid, false);
    QVERIFY(resolved.isValid() == false);
}

void TestScheduler::nextRunUtcFindsTheEarliestUpcomingStartTime()
{
    Program program;
    program.enabled = true;
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = 0x7F;   // every day

    ProgramStartTime early;
    early.minutesAfterMidnight = 360;   // 06:00 UTC, already behind nowUtc today
    early.timezone = "UTC";

    ProgramStartTime late;
    late.minutesAfterMidnight = 900;    // 15:00 UTC, still ahead of nowUtc today
    late.timezone = "UTC";

    // late is listed first: its instant is the true earliest of the two.
    const ProgramStartTimeList startTimes = { late, early };
    const QDateTime nowUtc(QDate(2026, 9, 14), QTime(10, 0), QTimeZone::UTC);

    QCOMPARE(Scheduler::nextRunUtc(program, startTimes, nowUtc),
             QDateTime(QDate(2026, 9, 14), QTime(15, 0), QTimeZone::UTC));
}

void TestScheduler::nextRunUtcSkipsNonWateringDays()
{
    Program program;
    program.enabled = true;
    program.dayMode = Program::DayMode::EveryNDays;
    program.intervalDays = 5;
    program.anchorDate = QDate(2026, 9, 1);

    ProgramStartTime startTime;
    startTime.minutesAfterMidnight = 480;   // 08:00 UTC
    startTime.timezone = "UTC";

    // Anchored at the 1st with a 5-day interval: watering days are the 1st, 6th, 11th...
    const QDateTime nowUtc(QDate(2026, 9, 3), QTime(0, 0), QTimeZone::UTC);
    QCOMPARE(Scheduler::nextRunUtc(program, { startTime }, nowUtc),
             QDateTime(QDate(2026, 9, 6), QTime(8, 0), QTimeZone::UTC));
}

void TestScheduler::nextRunUtcReturnsInvalidWhenNoDayEverMatchesWithinTheHorizon()
{
    Program program;
    program.enabled = true;
    program.dayMode = Program::DayMode::EveryNDays;
    program.intervalDays = 367;   // one more than the search horizon
    program.anchorDate = QDate(2026, 9, 14);

    ProgramStartTime startTime;
    startTime.minutesAfterMidnight = 0;
    startTime.timezone = "UTC";

    // nowUtc is past the anchor day's own instant; the next multiple of 367 falls
    // outside the 366-day search horizon.
    const QDateTime nowUtc(QDate(2026, 9, 14), QTime(12, 0), QTimeZone::UTC);
    QVERIFY(Scheduler::nextRunUtc(program, { startTime }, nowUtc).isValid() == false);
}

void TestScheduler::nextRunUtcFindsAHitExactlyAtTheHorizon()
{
    Program program;
    program.enabled = true;
    program.dayMode = Program::DayMode::EveryNDays;
    program.intervalDays = 366;   // exactly the search horizon
    program.anchorDate = QDate(2026, 9, 14);

    ProgramStartTime startTime;
    startTime.minutesAfterMidnight = 0;
    startTime.timezone = "UTC";

    // The only watering day within reach is 366 days after nowUtc's date, the last day
    // the horizon loop visits.
    const QDateTime nowUtc(QDate(2026, 9, 14), QTime(12, 0), QTimeZone::UTC);
    QCOMPARE(Scheduler::nextRunUtc(program, { startTime }, nowUtc),
             QDateTime(QDate(2027, 9, 15), QTime(0, 0), QTimeZone::UTC));
}

void TestScheduler::nextRunUtcResolvesAcrossDst()
{
    Program program;
    program.enabled = true;
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = 0x7F;

    ProgramStartTime startTime;
    startTime.minutesAfterMidnight = 360;   // 06:00 local
    startTime.timezone = "America/Los_Angeles";

    const QDateTime winterNow(QDate(2026, 1, 10), QTime(0, 0), QTimeZone::UTC);
    QCOMPARE(Scheduler::nextRunUtc(program, { startTime }, winterNow).time(), QTime(14, 0));

    const QDateTime summerNow(QDate(2026, 7, 10), QTime(0, 0), QTimeZone::UTC);
    QCOMPARE(Scheduler::nextRunUtc(program, { startTime }, summerNow).time(), QTime(13, 0));
}

void TestScheduler::nextRunUtcReturnsInvalidForADisabledProgram()
{
    Program program;
    program.enabled = false;
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = 0x7F;

    ProgramStartTime startTime;
    startTime.minutesAfterMidnight = 0;
    startTime.timezone = "UTC";

    const QDateTime nowUtc(QDate(2026, 9, 14), QTime(0, 0), QTimeZone::UTC);
    QVERIFY(Scheduler::nextRunUtc(program, { startTime }, nowUtc).isValid() == false);
}

void TestScheduler::nextRunUtcIncludesAnInstantExactlyNow()
{
    Program program;
    program.enabled = true;
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = 0x7F;

    ProgramStartTime startTime;
    startTime.minutesAfterMidnight = 600;   // 10:00 UTC
    startTime.timezone = "UTC";

    // nowUtc lands exactly on a start time's own instant: "at or after nowUtc" includes it.
    const QDateTime nowUtc(QDate(2026, 9, 14), QTime(10, 0), QTimeZone::UTC);
    QCOMPARE(Scheduler::nextRunUtc(program, { startTime }, nowUtc), nowUtc);
}

void TestScheduler::firesOnceInsideTheGraceWindow()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(offsetIdsSoProgramAndStartTimeDiffer(source));

    Program program;
    program.name = "Morning";
    program.dayMode = Program::DayMode::Odd;
    QVERIFY(source.insertProgram(program));

    ProgramStartTime startTime;
    startTime.programId = program.id;
    startTime.minutesAfterMidnight = 360;
    startTime.timezone = "UTC";
    QVERIFY(source.insertStartTime(startTime));
    QVERIFY(program.id != startTime.id);

    TestClock clock(QDateTime(QDate(2026, 9, 15), QTime(6, 0, 30), QTimeZone::UTC));
    Scheduler scheduler(&source, &clock);

    QSignalSpy spy(&scheduler, &Scheduler::programDue);
    scheduler.tick();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toInt(), program.id);
    QCOMPARE(spy.at(0).at(1).toInt(), startTime.id);
    QCOMPARE(spy.at(0).at(2).toDateTime(), QDateTime(QDate(2026, 9, 15), QTime(6, 0), QTimeZone::UTC));

    // A second tick inside the same window must not fire again.
    clock.advance(30);
    scheduler.tick();
    QCOMPARE(spy.count(), 1);

    QVERIFY(source.hasFired(program.id, startTime.id,
                             QDateTime(QDate(2026, 9, 15), QTime(6, 0), QTimeZone::UTC)));
    QCOMPARE(outcomeFor(source, program.id, startTime.id), QString("ran"));
}

void TestScheduler::tickDoesNotFireBeforeTheScheduledInstant()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(offsetIdsSoProgramAndStartTimeDiffer(source));

    Program program;
    program.name = "Not yet due";
    program.dayMode = Program::DayMode::Odd;
    QVERIFY(source.insertProgram(program));

    ProgramStartTime startTime;
    startTime.programId = program.id;
    startTime.minutesAfterMidnight = 360;   // 06:00
    startTime.timezone = "UTC";
    QVERIFY(source.insertStartTime(startTime));

    // One second before the instant.
    TestClock clock(QDateTime(QDate(2026, 9, 15), QTime(5, 59, 59), QTimeZone::UTC));
    Scheduler scheduler(&source, &clock);

    QSignalSpy spy(&scheduler, &Scheduler::programDue);
    scheduler.tick();
    QCOMPARE(spy.count(), 0);
    QCOMPARE(firedInstantCount(source, program.id, startTime.id), 0);

    // Advanced onto the instant.
    clock.advance(1);
    scheduler.tick();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(outcomeFor(source, program.id, startTime.id), QString("ran"));
}

void TestScheduler::graceWindowFiresAt120SecondsAndMissesAt121()
{
    {
        QTemporaryDir dir;
        IrrigationDataSource source(dir.filePath("irrigation.db"));
        QVERIFY(source.open());
        QVERIFY(offsetIdsSoProgramAndStartTimeDiffer(source));

        Program program;
        program.name = "Grace edge fires";
        program.dayMode = Program::DayMode::Odd;
        QVERIFY(source.insertProgram(program));

        ProgramStartTime startTime;
        startTime.programId = program.id;
        startTime.minutesAfterMidnight = 360;   // 06:00
        startTime.timezone = "UTC";
        QVERIFY(source.insertStartTime(startTime));

        // Exactly 120 s late.
        TestClock clock(QDateTime(QDate(2026, 9, 15), QTime(6, 2, 0), QTimeZone::UTC));
        Scheduler scheduler(&source, &clock);

        QSignalSpy spy(&scheduler, &Scheduler::programDue);
        scheduler.tick();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(outcomeFor(source, program.id, startTime.id), QString("ran"));
    }
    {
        QTemporaryDir dir;
        IrrigationDataSource source(dir.filePath("irrigation.db"));
        QVERIFY(source.open());
        QVERIFY(offsetIdsSoProgramAndStartTimeDiffer(source));

        Program program;
        program.name = "Grace edge misses";
        program.dayMode = Program::DayMode::Odd;
        QVERIFY(source.insertProgram(program));

        ProgramStartTime startTime;
        startTime.programId = program.id;
        startTime.minutesAfterMidnight = 360;   // 06:00
        startTime.timezone = "UTC";
        QVERIFY(source.insertStartTime(startTime));

        // Exactly 121 s late.
        TestClock clock(QDateTime(QDate(2026, 9, 15), QTime(6, 2, 1), QTimeZone::UTC));
        Scheduler scheduler(&source, &clock);

        QSignalSpy spy(&scheduler, &Scheduler::programDue);
        scheduler.tick();
        QCOMPARE(spy.count(), 0);
        QCOMPARE(outcomeFor(source, program.id, startTime.id), QString("missed"));
    }
}

void TestScheduler::missedOccurrenceIsRecordedAndNeverCaughtUp()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(offsetIdsSoProgramAndStartTimeDiffer(source));

    Program program;
    program.name = "Morning";
    program.dayMode = Program::DayMode::Odd;
    QVERIFY(source.insertProgram(program));

    ProgramStartTime startTime;
    startTime.programId = program.id;
    startTime.minutesAfterMidnight = 360;
    startTime.timezone = "UTC";
    QVERIFY(source.insertStartTime(startTime));

    // The daemon was down and comes back an hour late.
    TestClock clock(QDateTime(QDate(2026, 9, 15), QTime(7, 0), QTimeZone::UTC));
    Scheduler scheduler(&source, &clock);

    QSignalSpy spy(&scheduler, &Scheduler::programDue);
    scheduler.tick();
    QCOMPARE(spy.count(), 0);

    QVERIFY(source.hasFired(program.id, startTime.id,
                             QDateTime(QDate(2026, 9, 15), QTime(6, 0), QTimeZone::UTC)));
    QCOMPARE(outcomeFor(source, program.id, startTime.id), QString("missed"));

    clock.advance(3600);
    scheduler.tick();
    QCOMPARE(spy.count(), 0);
}

void TestScheduler::rainDelaySuppressesFiring()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(source.setSettingValue("rain_delay_until", "2026-09-16T00:00:00Z"));
    QVERIFY(offsetIdsSoProgramAndStartTimeDiffer(source));

    Program program;
    program.name = "Morning";
    program.dayMode = Program::DayMode::Odd;
    QVERIFY(source.insertProgram(program));

    ProgramStartTime startTime;
    startTime.programId = program.id;
    startTime.minutesAfterMidnight = 360;
    startTime.timezone = "UTC";
    QVERIFY(source.insertStartTime(startTime));

    TestClock clock(QDateTime(QDate(2026, 9, 15), QTime(6, 0, 10), QTimeZone::UTC));
    Scheduler scheduler(&source, &clock);

    QSignalSpy spy(&scheduler, &Scheduler::programDue);
    scheduler.tick();
    QCOMPARE(spy.count(), 0);

    QVERIFY(source.hasFired(program.id, startTime.id,
                             QDateTime(QDate(2026, 9, 15), QTime(6, 0), QTimeZone::UTC)));
    QCOMPARE(outcomeFor(source, program.id, startTime.id), QString("skipped_rain"));
}

void TestScheduler::expiredRainDelayNoLongerSuppressesFiring()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(source.setSettingValue("rain_delay_until", "2026-09-14T00:00:00Z"));   // earlier than the clock
    QVERIFY(offsetIdsSoProgramAndStartTimeDiffer(source));

    Program program;
    program.name = "Rain delay expired";
    program.dayMode = Program::DayMode::Odd;
    QVERIFY(source.insertProgram(program));

    ProgramStartTime startTime;
    startTime.programId = program.id;
    startTime.minutesAfterMidnight = 360;
    startTime.timezone = "UTC";
    QVERIFY(source.insertStartTime(startTime));

    TestClock clock(QDateTime(QDate(2026, 9, 15), QTime(6, 0, 10), QTimeZone::UTC));
    Scheduler scheduler(&source, &clock);

    QSignalSpy spy(&scheduler, &Scheduler::programDue);
    scheduler.tick();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(outcomeFor(source, program.id, startTime.id), QString("ran"));
}

void TestScheduler::graceWindowCrossesLocalMidnight()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(offsetIdsSoProgramAndStartTimeDiffer(source));

    Program program;
    program.name = "Late night";
    program.dayMode = Program::DayMode::Odd;
    QVERIFY(source.insertProgram(program));

    ProgramStartTime startTime;
    startTime.programId = program.id;
    startTime.minutesAfterMidnight = 1439;   // 23:59
    startTime.timezone = "UTC";
    QVERIFY(source.insertStartTime(startTime));

    // Sept 15 (odd, a watering day) at 23:59, checked just after local midnight rolls to the 16th.
    TestClock clock(QDateTime(QDate(2026, 9, 16), QTime(0, 0, 30), QTimeZone::UTC));
    Scheduler scheduler(&source, &clock);

    QSignalSpy spy(&scheduler, &Scheduler::programDue);
    scheduler.tick();
    QCOMPARE(spy.count(), 1);

    const QList<QVariant> args = spy.takeFirst();
    QCOMPARE(args.at(0).toInt(), program.id);
    QCOMPARE(args.at(1).toInt(), startTime.id);
    QCOMPARE(args.at(2).toDateTime(), QDateTime(QDate(2026, 9, 15), QTime(23, 59), QTimeZone::UTC));
    QCOMPARE(firedInstantCount(source, program.id, startTime.id), 1);
}

void TestScheduler::graceWindowCrossesLocalMidnightAheadOfUtc()
{
    // Pacific/Auckland is NZST (UTC+12, no DST) in July. nowUtc's own calendar date (the
    // 15th) is one day behind the zone's local date (the 16th) at this instant, so a "today"
    // that skips the zone conversion would evaluate the 14th/15th instead of the 15th/16th.
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(offsetIdsSoProgramAndStartTimeDiffer(source));

    Program program;
    program.name = "Ahead of UTC";
    program.dayMode = Program::DayMode::Even;
    QVERIFY(source.insertProgram(program));

    ProgramStartTime startTime;
    startTime.programId = program.id;
    startTime.minutesAfterMidnight = 479;   // 07:59 local, just before nowUtc's local time
    startTime.timezone = "Pacific/Auckland";
    QVERIFY(source.insertStartTime(startTime));

    // 2026-07-15 20:00:30 UTC is 2026-07-16 08:00:30 NZST.
    TestClock clock(QDateTime(QDate(2026, 7, 15), QTime(20, 0, 30), QTimeZone::UTC));
    Scheduler scheduler(&source, &clock);

    QSignalSpy spy(&scheduler, &Scheduler::programDue);
    scheduler.tick();
    QCOMPARE(spy.count(), 1);

    const QDateTime scheduledUtc(QDate(2026, 7, 15), QTime(19, 59), QTimeZone::UTC);
    QVERIFY(source.hasFired(program.id, startTime.id, scheduledUtc));
    QCOMPARE(outcomeFor(source, program.id, startTime.id), QString("ran"));
    QCOMPARE(firedInstantCount(source, program.id, startTime.id), 1);
}

void TestScheduler::graceWindowCrossesLocalMidnightBehindUtc()
{
    // America/Los_Angeles is PST (UTC-8, no DST) in January. The watering days are Saturday
    // and Sunday only, so the 6th (Friday) is excluded and the 7th (Saturday) and 8th
    // (Sunday) are the only candidates a "today" of either the 7th or the 8th can reach.
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(offsetIdsSoProgramAndStartTimeDiffer(source));

    Program program;
    program.name = "Behind UTC";
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = (1 << 5) | (1 << 6);   // Saturday and Sunday
    QVERIFY(source.insertProgram(program));

    // 02:30 on 2026-03-08 (a Sunday) does not exist in this zone: the spring-forward gap.
    // Only a "today" that reaches the 8th can trigger the Missed row this test checks for.
    ProgramStartTime startTime;
    startTime.programId = program.id;
    startTime.minutesAfterMidnight = 150;
    startTime.timezone = "America/Los_Angeles";
    QVERIFY(source.insertStartTime(startTime));

    // 2026-03-08 03:00:00 UTC is 2026-03-07 19:00:00 PST: nowUtc's own calendar date (the
    // 8th) is one day ahead of the zone's local date (the 7th) at this instant.
    TestClock clock(QDateTime(QDate(2026, 3, 8), QTime(3, 0, 0), QTimeZone::UTC));
    Scheduler scheduler(&source, &clock);

    QSignalSpy spy(&scheduler, &Scheduler::programDue);
    scheduler.tick();
    QCOMPARE(spy.count(), 0);

    // A "today" taken from nowUtc's own date also reaches the 8th, recording an extra
    // Missed row for its nonexistent 02:30 on top of the 7th's genuine one.
    QCOMPARE(firedInstantCount(source, program.id, startTime.id), 1);
}

void TestScheduler::unrecognisedZoneRecordsMissedInstant()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(offsetIdsSoProgramAndStartTimeDiffer(source));

    Program program;
    program.name = "Bad zone";
    program.dayMode = Program::DayMode::Odd;
    QVERIFY(source.insertProgram(program));

    ProgramStartTime startTime;
    startTime.programId = program.id;
    startTime.minutesAfterMidnight = 370;
    startTime.timezone = "Not/AZone";
    QVERIFY(source.insertStartTime(startTime));

    TestClock clock(QDateTime(QDate(2026, 9, 15), QTime(6, 10, 0), QTimeZone::UTC));
    Scheduler scheduler(&source, &clock);

    QSignalSpy spy(&scheduler, &Scheduler::programDue);
    scheduler.tick();
    QCOMPARE(spy.count(), 0);

    const QDateTime missedInstant(QDate(2026, 9, 15), QTime(6, 10), QTimeZone::UTC);
    QVERIFY(source.hasFired(program.id, startTime.id, missedInstant));
    QCOMPARE(outcomeFor(source, program.id, startTime.id), QString("missed"));
}

void TestScheduler::springForwardGapRecordsSlidMissedInstant()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(offsetIdsSoProgramAndStartTimeDiffer(source));

    Program program;
    program.name = "Spring gap";
    program.dayMode = Program::DayMode::Even;
    QVERIFY(source.insertProgram(program));

    ProgramStartTime startTime;
    startTime.programId = program.id;
    startTime.minutesAfterMidnight = 150;   // 02:30 local, inside the 2026-03-08 spring-forward gap
    startTime.timezone = "America/Los_Angeles";
    QVERIFY(source.insertStartTime(startTime));

    // 11:00Z is already past the transition, so "today" in the zone resolves to the 8th.
    TestClock clock(QDateTime(QDate(2026, 3, 8), QTime(11, 0, 0), QTimeZone::UTC));
    Scheduler scheduler(&source, &clock);

    QSignalSpy spy(&scheduler, &Scheduler::programDue);
    scheduler.tick();
    QCOMPARE(spy.count(), 0);

    const QDateTime slidInstant(QDate(2026, 3, 8), QTime(9, 30), QTimeZone::UTC);
    const QDateTime naiveInstant(QDate(2026, 3, 8), QTime(2, 30), QTimeZone::UTC);
    QVERIFY(slidInstant != naiveInstant);

    QVERIFY(source.hasFired(program.id, startTime.id, slidInstant));
    QVERIFY(source.hasFired(program.id, startTime.id, naiveInstant) == false);
}

void TestScheduler::outOfRangeMinutesNeverReachesTheDatabase()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(offsetIdsSoProgramAndStartTimeDiffer(source));

    Program program;
    program.name = "Bad minutes";
    program.dayMode = Program::DayMode::Odd;
    QVERIFY(source.insertProgram(program));

    ProgramStartTime startTime;
    startTime.programId = program.id;
    startTime.minutesAfterMidnight = 1500;   // 25:00, wraps to 01:00 if the range guard is skipped
    startTime.timezone = "UTC";
    QVERIFY(source.insertStartTime(startTime));

    TestClock clock(QDateTime(QDate(2026, 9, 15), QTime(6, 0, 0), QTimeZone::UTC));
    Scheduler scheduler(&source, &clock);

    QSignalSpy spy(&scheduler, &Scheduler::programDue);
    scheduler.tick();
    QCOMPARE(spy.count(), 0);
    QCOMPARE(firedInstantCount(source, program.id, startTime.id), 0);
}

void TestScheduler::emissionIsGatedOnRecordFiringSucceeding()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(offsetIdsSoProgramAndStartTimeDiffer(source));

    Program program;
    program.name = "Read-only";
    program.dayMode = Program::DayMode::Odd;
    QVERIFY(source.insertProgram(program));

    ProgramStartTime startTime;
    startTime.programId = program.id;
    startTime.minutesAfterMidnight = 360;
    startTime.timezone = "UTC";
    QVERIFY(source.insertStartTime(startTime));

    bool pragmaOk = false;
    source.rawQuery("PRAGMA query_only = ON", &pragmaOk);
    QVERIFY(pragmaOk);

    TestClock clock(QDateTime(QDate(2026, 9, 15), QTime(6, 0, 30), QTimeZone::UTC));
    Scheduler scheduler(&source, &clock);

    QSignalSpy spy(&scheduler, &Scheduler::programDue);
    scheduler.tick();

    QCOMPARE(spy.count(), 0);
    QVERIFY(source.hasFired(program.id, startTime.id,
                             QDateTime(QDate(2026, 9, 15), QTime(6, 0), QTimeZone::UTC)) == false);
}

void TestScheduler::masterDisabledSkipsTheTick()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(source.setSettingValue("master_enabled", "0"));
    QVERIFY(offsetIdsSoProgramAndStartTimeDiffer(source));

    Program program;
    program.name = "Disabled master";
    program.dayMode = Program::DayMode::Odd;
    QVERIFY(source.insertProgram(program));

    ProgramStartTime startTime;
    startTime.programId = program.id;
    startTime.minutesAfterMidnight = 360;
    startTime.timezone = "UTC";
    QVERIFY(source.insertStartTime(startTime));

    TestClock clock(QDateTime(QDate(2026, 9, 15), QTime(6, 0, 30), QTimeZone::UTC));
    Scheduler scheduler(&source, &clock);

    QSignalSpy spy(&scheduler, &Scheduler::programDue);
    scheduler.tick();
    QCOMPARE(spy.count(), 0);
    QCOMPARE(firedInstantCount(source, program.id, startTime.id), 0);
}

void TestScheduler::masterEnabledValuesAllRunNormally_data()
{
    QTest::addColumn<QString>("masterEnabledValue");

    QTest::newRow("explicit 1") << QString("1");
    QTest::newRow("empty string") << QString("");
    QTest::newRow("the word false") << QString("false");
}

void TestScheduler::masterEnabledValuesAllRunNormally()
{
    QFETCH(QString, masterEnabledValue);

    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(source.setSettingValue("master_enabled", masterEnabledValue));
    QVERIFY(offsetIdsSoProgramAndStartTimeDiffer(source));

    Program program;
    program.name = "Master enabled";
    program.dayMode = Program::DayMode::Odd;
    QVERIFY(source.insertProgram(program));

    ProgramStartTime startTime;
    startTime.programId = program.id;
    startTime.minutesAfterMidnight = 360;
    startTime.timezone = "UTC";
    QVERIFY(source.insertStartTime(startTime));

    TestClock clock(QDateTime(QDate(2026, 9, 15), QTime(6, 0, 30), QTimeZone::UTC));
    Scheduler scheduler(&source, &clock);

    QSignalSpy spy(&scheduler, &Scheduler::programDue);
    scheduler.tick();
    QCOMPARE(spy.count(), 1);
}

void TestScheduler::masterEnabledDefaultsToEnabledWhenAbsent()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    bool deleteOk = false;
    source.rawQuery("DELETE FROM settings WHERE key = 'master_enabled'", &deleteOk);
    QVERIFY(deleteOk);
    QVERIFY(source.settingValue("master_enabled").isEmpty());

    QVERIFY(offsetIdsSoProgramAndStartTimeDiffer(source));

    Program program;
    program.name = "No master row";
    program.dayMode = Program::DayMode::Odd;
    QVERIFY(source.insertProgram(program));

    ProgramStartTime startTime;
    startTime.programId = program.id;
    startTime.minutesAfterMidnight = 360;
    startTime.timezone = "UTC";
    QVERIFY(source.insertStartTime(startTime));

    TestClock clock(QDateTime(QDate(2026, 9, 15), QTime(6, 0, 30), QTimeZone::UTC));
    Scheduler scheduler(&source, &clock);

    QSignalSpy spy(&scheduler, &Scheduler::programDue);
    scheduler.tick();
    QCOMPARE(spy.count(), 1);
}

QTEST_MAIN(TestScheduler)
#include "tst_scheduler.moc"
