#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QSqlQuery>
#include <QDateTime>
#include <QTimeZone>

#include "database/irrigationdatasource.h"
#include "iclock.h"
#include "scheduler.h"

// Every real program/start-time pair below must have program.id != startTime.id: SQLite hands
// both tables id 1 on a fresh insert, and that coincidence would hide a swapped programId/
// startTimeId bind. Two filler rows in each table push the real ids apart before they are used.
static void offsetIdsSoProgramAndStartTimeDiffer(IrrigationDataSource& source)
{
    Program filler;
    filler.name = "Filler";
    QVERIFY(source.insertProgram(filler));

    ProgramStartTime fillerStart1;
    fillerStart1.programId = filler.id;
    fillerStart1.minutesAfterMidnight = 0;
    fillerStart1.timezone = "UTC";
    QVERIFY(source.insertStartTime(fillerStart1));

    ProgramStartTime fillerStart2;
    fillerStart2.programId = filler.id;
    fillerStart2.minutesAfterMidnight = 1;
    fillerStart2.timezone = "UTC";
    QVERIFY(source.insertStartTime(fillerStart2));
}

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
private slots:
    void initTestCase();

    void dayOfWeekMaskSelectsTheRightDays();
    void oddAndEvenUseTheDayOfMonth();
    void everyNDaysCountsFromTheAnchor();
    void springForwardGapIsRejected();
    void aStartTimeDoesNotDriftAcrossDst();
    void minutesAfterMidnightOutOfRangeIsRejected();

    void nextRunUtcFindsTheEarliestUpcomingStartTime();
    void nextRunUtcSkipsNonWateringDays();
    void nextRunUtcReturnsInvalidWhenNoDayEverMatchesWithinTheHorizon();
    void nextRunUtcResolvesAcrossDst();
    void nextRunUtcReturnsInvalidForADisabledProgram();

    void firesOnceInsideTheGraceWindow();
    void missedOccurrenceIsRecordedAndNeverCaughtUp();
    void rainDelaySuppressesFiring();
    void graceWindowCrossesLocalMidnight();
    void unrecognisedZoneRecordsMissedInstant();
    void springForwardGapRecordsSlidMissedInstant();
    void outOfRangeMinutesNeverReachesTheDatabase();
    void emissionIsGatedOnRecordFiringSucceeding();
    void masterDisabledSkipsTheTick();
    void masterEnabledValuesAllRunNormally_data();
    void masterEnabledValuesAllRunNormally();
    void masterEnabledDefaultsToEnabledWhenAbsent();
};

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

    // Different UTC instants, same wall clock. Storing the rule in UTC would make
    // these identical and the program would run at 05:00 all summer.
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
    program.dowMask = 0x7F;   // every day, so only the start-time ordering is under test

    ProgramStartTime early;
    early.minutesAfterMidnight = 360;   // 06:00 UTC, already behind nowUtc today
    early.timezone = "UTC";

    ProgramStartTime late;
    late.minutesAfterMidnight = 900;    // 15:00 UTC, still ahead of nowUtc today
    late.timezone = "UTC";

    const ProgramStartTimeList startTimes = { early, late };
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

void TestScheduler::firesOnceInsideTheGraceWindow()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    offsetIdsSoProgramAndStartTimeDiffer(source);

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

    // A second tick inside the same window must not fire again.
    clock.advance(30);
    scheduler.tick();
    QCOMPARE(spy.count(), 1);

    QVERIFY(source.hasFired(program.id, startTime.id,
                             QDateTime(QDate(2026, 9, 15), QTime(6, 0), QTimeZone::UTC)));
    QCOMPARE(outcomeFor(source, program.id, startTime.id), QString("ran"));
}

void TestScheduler::missedOccurrenceIsRecordedAndNeverCaughtUp()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    offsetIdsSoProgramAndStartTimeDiffer(source);

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
    offsetIdsSoProgramAndStartTimeDiffer(source);

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

void TestScheduler::graceWindowCrossesLocalMidnight()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    offsetIdsSoProgramAndStartTimeDiffer(source);

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
    QCOMPARE(args.at(2).toDateTime(), QDateTime(QDate(2026, 9, 15), QTime(23, 59), QTimeZone::UTC));
    QCOMPARE(firedInstantCount(source, program.id, startTime.id), 1);
}

void TestScheduler::unrecognisedZoneRecordsMissedInstant()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    offsetIdsSoProgramAndStartTimeDiffer(source);

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

    // A repeat tick must not add a second row for the same unresolved instant.
    clock.advance(1);
    scheduler.tick();
    QCOMPARE(firedInstantCount(source, program.id, startTime.id), 1);
}

void TestScheduler::springForwardGapRecordsSlidMissedInstant()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    offsetIdsSoProgramAndStartTimeDiffer(source);

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

    const QTimeZone zone("America/Los_Angeles");
    const QDateTime slidInstant =
        QDateTime(QDate(2026, 3, 8), QTime(2, 30), zone, QDateTime::TransitionResolution::PreferBefore).toUTC();
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
    offsetIdsSoProgramAndStartTimeDiffer(source);

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
    offsetIdsSoProgramAndStartTimeDiffer(source);

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
    offsetIdsSoProgramAndStartTimeDiffer(source);

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
    offsetIdsSoProgramAndStartTimeDiffer(source);

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

    offsetIdsSoProgramAndStartTimeDiffer(source);

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
