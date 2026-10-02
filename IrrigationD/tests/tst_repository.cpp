#include <QTest>
#include <QTemporaryDir>
#include <QSqlQuery>
#include <QDateTime>
#include <QTimeZone>

#include "database/irrigationdatasource.h"

class TestRepository : public QObject
{
    Q_OBJECT
private slots:
    void recordFiringIsIdempotent();
    void insertProgramAssignsTheId();
    void deleteProgramCascades();
    void prunePreservesRecentRows();
    void startTimeRoundTripsAllFields();
    void updateZoneChangesNameAndEnabled();
    void updateZoneLandsCorrectlyWhenNumberDiffersFromId();
    void updateProgramChangesFields();
    void updateProgramLeavesEveryOtherProgramIntact();
    void enabledProgramsExcludesDisabled();
    void settingValueRoundTrips();
    void setFiringOutcomeMatchesTheExactKeyOnly();
    void pruneFiredInstantsOlderThanIsExclusiveOfTheCutoff();
    void deleteStartTimeRemovesOnlyThatRow();
    void startTimesForReportsSuccessInAscendingIdOrder();
    void startTimesForReportsFailureWhenTheReadFails();
    void isMasterEnabledOnlyExactZeroDisables_data();
    void isMasterEnabledOnlyExactZeroDisables();
    void isMasterEnabledDefaultsToEnabledWhenAbsent();
};

void TestRepository::recordFiringIsIdempotent()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program program;
    program.name = "Morning";
    QVERIFY(source.insertProgram(program));
    QVERIFY(program.id > 0);

    FiredInstant instant;
    instant.programId = program.id;
    // Deliberately distinct from programId: equal values would let a swapped programId/startTimeId bind pass unnoticed.
    instant.startTimeId = 42;
    instant.scheduledAtUtc = QDateTime(QDate(2026, 9, 12), QTime(13, 0), QTimeZone::UTC);
    instant.outcome = FiredInstant::Outcome::Ran;

    QVERIFY(source.hasFired(instant.programId, instant.startTimeId, instant.scheduledAtUtc) == false);
    QVERIFY(source.recordFiring(instant));
    QVERIFY(source.hasFired(instant.programId, instant.startTimeId, instant.scheduledAtUtc));

    // A second record of the same instant must not create a second row.
    source.recordFiring(instant);

    bool ok = false;
    QSqlQuery query = source.rawQuery("SELECT COUNT(*) FROM fired_instants", &ok);
    QVERIFY(ok);
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 1);

    QSqlQuery row = source.rawQuery(
        QString("SELECT program_id, start_time_id, scheduled_at_utc, outcome FROM fired_instants WHERE program_id = %1")
            .arg(instant.programId),
        &ok);
    QVERIFY(ok);
    QVERIFY(row.next());
    QCOMPARE(row.value(0).toInt(), instant.programId);
    QCOMPARE(row.value(1).toInt(), instant.startTimeId);
    QCOMPARE(row.value(2).toString(), instant.scheduledAtUtc.toUTC().toString(Qt::ISODate));
    QCOMPARE(row.value(3).toString(), QString("ran"));
}

void TestRepository::insertProgramAssignsTheId()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program program;
    program.name = "Evening";
    program.dayMode = Program::DayMode::EveryNDays;
    program.intervalDays = 3;
    program.anchorDate = QDate(2026, 4, 1);

    QVERIFY(source.insertProgram(program));
    QVERIFY(program.id > 0);

    ProgramList all = source.allPrograms();
    QCOMPARE(all.count(), 1);
    QCOMPARE(all.first().name, QString("Evening"));
    QCOMPARE(all.first().dayMode, Program::DayMode::EveryNDays);
    QCOMPARE(all.first().intervalDays, 3);
    QCOMPARE(all.first().anchorDate, QDate(2026, 4, 1));
}

void TestRepository::deleteProgramCascades()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program program;
    program.name = "Doomed";
    QVERIFY(source.insertProgram(program));

    ProgramStartTime start;
    start.programId = program.id;
    start.minutesAfterMidnight = 360;
    start.timezone = "America/Los_Angeles";
    QVERIFY(source.insertStartTime(start));

    QCOMPARE(source.startTimesFor(program.id).count(), 1);
    QVERIFY(source.deleteProgram(program.id));
    QCOMPARE(source.startTimesFor(program.id).count(), 0);
}

void TestRepository::prunePreservesRecentRows()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program program;
    program.name = "Morning";
    QVERIFY(source.insertProgram(program));

    FiredInstant old;
    old.programId = program.id;
    // Deliberately distinct from programId: equal values would let a swapped programId/startTimeId bind pass unnoticed.
    old.startTimeId = 41;
    old.scheduledAtUtc = QDateTime(QDate(2026, 1, 1), QTime(6, 0), QTimeZone::UTC);
    old.outcome = FiredInstant::Outcome::Ran;
    QVERIFY(source.recordFiring(old));

    FiredInstant recent = old;
    recent.startTimeId = 42;
    recent.scheduledAtUtc = QDateTime(QDate(2026, 9, 1), QTime(6, 0), QTimeZone::UTC);
    QVERIFY(source.recordFiring(recent));

    QVERIFY(source.pruneFiredInstantsOlderThan(QDateTime(QDate(2026, 6, 1), QTime(0, 0), QTimeZone::UTC)));

    QVERIFY(source.hasFired(program.id, old.startTimeId, old.scheduledAtUtc) == false);
    QVERIFY(source.hasFired(program.id, recent.startTimeId, recent.scheduledAtUtc));
}

void TestRepository::startTimeRoundTripsAllFields()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program program;
    program.name = "Dawn";
    QVERIFY(source.insertProgram(program));

    ProgramStartTime start;
    start.programId = program.id;
    start.minutesAfterMidnight = 330;
    start.timezone = "America/Denver";
    QVERIFY(source.insertStartTime(start));
    QVERIFY(start.id > 0);

    ProgramStartTimeList startTimes = source.startTimesFor(program.id);
    QCOMPARE(startTimes.count(), 1);
    QCOMPARE(startTimes.first().id, start.id);
    QCOMPARE(startTimes.first().programId, program.id);
    QCOMPARE(startTimes.first().minutesAfterMidnight, 330);
    QCOMPARE(startTimes.first().timezone, QString("America/Denver"));
}

void TestRepository::updateZoneChangesNameAndEnabled()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    ZoneList zones = source.allZones();
    QCOMPARE(zones.count(), 8);

    Zone zone = zones.first();
    QCOMPARE(zone.number, 1);
    zone.name = "Front lawn";
    zone.enabled = false;
    QVERIFY(source.updateZone(zone));

    ZoneList updated = source.allZones();
    QCOMPARE(updated.first().number, 1);
    QCOMPARE(updated.first().name, QString("Front lawn"));
    QCOMPARE(updated.first().enabled, false);
    // The untouched zones must survive the update unchanged.
    QCOMPARE(updated.at(1).name, QString("Zone 2"));
    QCOMPARE(updated.at(1).enabled, true);
}

void TestRepository::updateZoneLandsCorrectlyWhenNumberDiffersFromId()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    // Every seeded zone has number == id, which would let a swapped :number/:id
    // bind in updateZone() go unnoticed. Break that equality before testing it.
    bool seeded = false;
    source.rawQuery("UPDATE zones SET number = 99 WHERE id = 1", &seeded);
    QVERIFY(seeded);

    Zone zone;
    zone.id = 1;
    zone.number = 99;
    zone.name = "Mismatched";
    zone.enabled = false;
    QVERIFY(source.updateZone(zone));

    bool ok = false;
    QSqlQuery row = source.rawQuery("SELECT id, number, name, enabled FROM zones WHERE id = 1", &ok);
    QVERIFY(ok);
    QVERIFY(row.next());
    QCOMPARE(row.value(0).toInt(), 1);
    QCOMPARE(row.value(1).toInt(), 99);
    QCOMPARE(row.value(2).toString(), QString("Mismatched"));
    QCOMPARE(row.value(3).toInt(), 0);
}

void TestRepository::updateProgramChangesFields()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program program;
    program.name = "Original";
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = 0x15;
    QVERIFY(source.insertProgram(program));

    program.name = "Renamed";
    program.enabled = false;
    program.dayMode = Program::DayMode::Odd;
    program.dowMask = 0x2A;
    program.intervalDays = 5;
    program.anchorDate = QDate(2026, 5, 1);
    QVERIFY(source.updateProgram(program));

    ProgramList all = source.allPrograms();
    QCOMPARE(all.count(), 1);
    QCOMPARE(all.first().name, QString("Renamed"));
    QCOMPARE(all.first().enabled, false);
    QCOMPARE(all.first().dayMode, Program::DayMode::Odd);
    QCOMPARE(all.first().dowMask, 0x2A);
    QCOMPARE(all.first().intervalDays, 5);
    QCOMPARE(all.first().anchorDate, QDate(2026, 5, 1));
}

void TestRepository::updateProgramLeavesEveryOtherProgramIntact()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program low;
    low.name = "Low";
    low.enabled = true;
    low.dayMode = Program::DayMode::DaysOfWeek;
    low.dowMask = 0x05;
    QVERIFY(source.insertProgram(low));

    Program middle;
    middle.name = "Middle";
    middle.dayMode = Program::DayMode::Odd;
    QVERIFY(source.insertProgram(middle));

    Program high;
    high.name = "High";
    high.enabled = false;
    high.dayMode = Program::DayMode::EveryNDays;
    high.intervalDays = 4;
    high.anchorDate = QDate(2027, 3, 4);
    QVERIFY(source.insertProgram(high));

    Program changed = middle;
    changed.name = "Middle Renamed";
    changed.enabled = false;
    changed.dayMode = Program::DayMode::Even;
    changed.dowMask = 0x2A;
    changed.intervalDays = 6;
    changed.anchorDate = QDate(2026, 7, 8);
    QVERIFY(source.updateProgram(changed));

    ProgramList all = source.allPrograms();
    QCOMPARE(all.count(), 3);

    QCOMPARE(all.at(0).id, low.id);
    QCOMPARE(all.at(0).name, QString("Low"));
    QCOMPARE(all.at(0).enabled, true);
    QCOMPARE(all.at(0).dayMode, Program::DayMode::DaysOfWeek);
    QCOMPARE(all.at(0).dowMask, 0x05);
    QCOMPARE(all.at(0).intervalDays, 0);
    QCOMPARE(all.at(0).anchorDate, QDate());

    QCOMPARE(all.at(1).id, middle.id);
    QCOMPARE(all.at(1).name, QString("Middle Renamed"));
    QCOMPARE(all.at(1).dayMode, Program::DayMode::Even);

    QCOMPARE(all.at(2).id, high.id);
    QCOMPARE(all.at(2).name, QString("High"));
    QCOMPARE(all.at(2).enabled, false);
    QCOMPARE(all.at(2).dayMode, Program::DayMode::EveryNDays);
    QCOMPARE(all.at(2).dowMask, 0);
    QCOMPARE(all.at(2).intervalDays, 4);
    QCOMPARE(all.at(2).anchorDate, QDate(2027, 3, 4));
}

void TestRepository::enabledProgramsExcludesDisabled()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program active;
    active.name = "Active";
    QVERIFY(source.insertProgram(active));

    Program disabled;
    disabled.name = "Disabled";
    disabled.enabled = false;
    QVERIFY(source.insertProgram(disabled));

    ProgramList enabled = source.enabledPrograms();
    QCOMPARE(enabled.count(), 1);
    QCOMPARE(enabled.first().name, QString("Active"));
}

void TestRepository::settingValueRoundTrips()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    // Seeded by schema.sql.
    QCOMPARE(source.settingValue("max_zone_seconds"), QString("3600"));

    QVERIFY(source.setSettingValue("max_zone_seconds", "1800"));
    QCOMPARE(source.settingValue("max_zone_seconds"), QString("1800"));

    QVERIFY(source.settingValue("no_such_key").isEmpty());
}

void TestRepository::setFiringOutcomeMatchesTheExactKeyOnly()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program program;
    program.name = "Morning";
    QVERIFY(source.insertProgram(program));

    Program otherProgram;
    otherProgram.name = "Evening";
    QVERIFY(source.insertProgram(otherProgram));
    QVERIFY(program.id != otherProgram.id);

    FiredInstant first;
    first.programId = program.id;
    // programId, startTimeId and the instant are each varied one at a time across the rows
    // below, so dropping any single WHERE term collides with a neighbour.
    first.startTimeId = 41;
    first.scheduledAtUtc = QDateTime(QDate(2026, 9, 12), QTime(6, 0), QTimeZone::UTC);
    first.outcome = FiredInstant::Outcome::Ran;
    QVERIFY(source.recordFiring(first));

    // Shares program and start time with the target row; only the instant differs.
    FiredInstant sameKeyDifferentInstant = first;
    sameKeyDifferentInstant.scheduledAtUtc = QDateTime(QDate(2026, 9, 13), QTime(6, 0), QTimeZone::UTC);
    QVERIFY(source.recordFiring(sameKeyDifferentInstant));

    // Shares program and instant with the target row; only start_time_id differs.
    FiredInstant differentStartTimeSameInstant = first;
    differentStartTimeSameInstant.startTimeId = 42;
    QVERIFY(source.recordFiring(differentStartTimeSameInstant));

    // Shares start time and instant with the target row; only program_id differs.
    FiredInstant differentProgramSameKey = first;
    differentProgramSameKey.programId = otherProgram.id;
    QVERIFY(source.recordFiring(differentProgramSameKey));

    QVERIFY(source.setFiringOutcome(first.programId, first.startTimeId, first.scheduledAtUtc,
                                     FiredInstant::Outcome::SkippedStop));

    bool ok = false;
    QSqlQuery firstRow = source.rawQuery(
        QString("SELECT outcome FROM fired_instants WHERE program_id = %1 AND start_time_id = %2 AND scheduled_at_utc = '%3'")
            .arg(first.programId).arg(first.startTimeId).arg(first.scheduledAtUtc.toUTC().toString(Qt::ISODate)), &ok);
    QVERIFY(ok);
    QVERIFY(firstRow.next());
    QCOMPARE(firstRow.value(0).toString(), QString("skipped_stop"));

    QSqlQuery sameKeyRow = source.rawQuery(
        QString("SELECT outcome FROM fired_instants WHERE program_id = %1 AND start_time_id = %2 AND scheduled_at_utc = '%3'")
            .arg(sameKeyDifferentInstant.programId).arg(sameKeyDifferentInstant.startTimeId)
            .arg(sameKeyDifferentInstant.scheduledAtUtc.toUTC().toString(Qt::ISODate)), &ok);
    QVERIFY(ok);
    QVERIFY(sameKeyRow.next());
    QCOMPARE(sameKeyRow.value(0).toString(), QString("ran"));

    QSqlQuery differentStartTimeRow = source.rawQuery(
        QString("SELECT outcome FROM fired_instants WHERE program_id = %1 AND start_time_id = %2 AND scheduled_at_utc = '%3'")
            .arg(differentStartTimeSameInstant.programId).arg(differentStartTimeSameInstant.startTimeId)
            .arg(differentStartTimeSameInstant.scheduledAtUtc.toUTC().toString(Qt::ISODate)), &ok);
    QVERIFY(ok);
    QVERIFY(differentStartTimeRow.next());
    QCOMPARE(differentStartTimeRow.value(0).toString(), QString("ran"));

    QSqlQuery differentProgramRow = source.rawQuery(
        QString("SELECT outcome FROM fired_instants WHERE program_id = %1 AND start_time_id = %2 AND scheduled_at_utc = '%3'")
            .arg(differentProgramSameKey.programId).arg(differentProgramSameKey.startTimeId)
            .arg(differentProgramSameKey.scheduledAtUtc.toUTC().toString(Qt::ISODate)), &ok);
    QVERIFY(ok);
    QVERIFY(differentProgramRow.next());
    QCOMPARE(differentProgramRow.value(0).toString(), QString("ran"));

    // No row matches this instant: zero rows changed.
    QVERIFY(source.setFiringOutcome(first.programId, first.startTimeId,
                                     QDateTime(QDate(2026, 9, 14), QTime(6, 0), QTimeZone::UTC),
                                     FiredInstant::Outcome::Failed) == false);
}

void TestRepository::pruneFiredInstantsOlderThanIsExclusiveOfTheCutoff()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program program;
    program.name = "Boundary";
    QVERIFY(source.insertProgram(program));

    const QDateTime cutoff(QDate(2026, 6, 1), QTime(0, 0), QTimeZone::UTC);

    FiredInstant atCutoff;
    atCutoff.programId = program.id;
    atCutoff.startTimeId = 41;
    atCutoff.scheduledAtUtc = cutoff;
    atCutoff.outcome = FiredInstant::Outcome::Ran;
    QVERIFY(source.recordFiring(atCutoff));

    FiredInstant beforeCutoff = atCutoff;
    beforeCutoff.startTimeId = 42;
    beforeCutoff.scheduledAtUtc = cutoff.addSecs(-1);
    QVERIFY(source.recordFiring(beforeCutoff));

    QVERIFY(source.pruneFiredInstantsOlderThan(cutoff));

    QVERIFY(source.hasFired(atCutoff.programId, atCutoff.startTimeId, atCutoff.scheduledAtUtc));
    QVERIFY(source.hasFired(beforeCutoff.programId, beforeCutoff.startTimeId, beforeCutoff.scheduledAtUtc) == false);
}

void TestRepository::deleteStartTimeRemovesOnlyThatRow()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program program;
    program.name = "Two starts";
    QVERIFY(source.insertProgram(program));

    ProgramStartTime keep;
    keep.programId = program.id;
    keep.minutesAfterMidnight = 300;
    keep.timezone = "UTC";
    QVERIFY(source.insertStartTime(keep));

    ProgramStartTime remove;
    remove.programId = program.id;
    remove.minutesAfterMidnight = 600;
    remove.timezone = "America/Denver";
    QVERIFY(source.insertStartTime(remove));

    QVERIFY(source.deleteStartTime(remove.id));

    ProgramStartTimeList remaining = source.startTimesFor(program.id);
    QCOMPARE(remaining.count(), 1);
    QCOMPARE(remaining.first().id, keep.id);
}

void TestRepository::startTimesForReportsSuccessInAscendingIdOrder()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program program;
    program.name = "Two starts";
    QVERIFY(source.insertProgram(program));

    ProgramStartTime firstInserted;
    firstInserted.programId = program.id;
    firstInserted.minutesAfterMidnight = 600;
    firstInserted.timezone = "America/Denver";
    QVERIFY(source.insertStartTime(firstInserted));

    ProgramStartTime secondInserted;
    secondInserted.programId = program.id;
    secondInserted.minutesAfterMidnight = 300;
    secondInserted.timezone = "UTC";
    QVERIFY(source.insertStartTime(secondInserted));

    QVERIFY(firstInserted.id < secondInserted.id);

    bool ok = false;
    ProgramStartTimeList startTimes = source.startTimesFor(program.id, &ok);
    QVERIFY(ok);
    QCOMPARE(startTimes.count(), 2);
    QCOMPARE(startTimes.at(0).id, firstInserted.id);
    QCOMPARE(startTimes.at(1).id, secondInserted.id);
}

void TestRepository::startTimesForReportsFailureWhenTheReadFails()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program program;
    program.name = "Doomed table";
    QVERIFY(source.insertProgram(program));

    bool dropOk = false;
    source.rawQuery("DROP TABLE program_start_times", &dropOk);
    QVERIFY(dropOk);

    bool ok = true;
    ProgramStartTimeList startTimes = source.startTimesFor(program.id, &ok);
    QVERIFY(ok == false);
    QVERIFY(startTimes.isEmpty());
}

void TestRepository::isMasterEnabledOnlyExactZeroDisables_data()
{
    QTest::addColumn<QString>("storedValue");
    QTest::addColumn<bool>("expectEnabled");

    QTest::newRow("exact zero disables") << QString("0") << false;
    QTest::newRow("explicit one enables") << QString("1") << true;
    QTest::newRow("empty string enables") << QString("") << true;
    QTest::newRow("the word false enables") << QString("false") << true;
}

void TestRepository::isMasterEnabledOnlyExactZeroDisables()
{
    QFETCH(QString, storedValue);
    QFETCH(bool, expectEnabled);

    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(source.setSettingValue("master_enabled", storedValue));

    QCOMPARE(source.isMasterEnabled(), expectEnabled);
}

void TestRepository::isMasterEnabledDefaultsToEnabledWhenAbsent()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    bool ok = false;
    source.rawQuery("DELETE FROM settings WHERE key = 'master_enabled'", &ok);
    QVERIFY(ok);
    QVERIFY(source.settingValue("master_enabled").isEmpty());

    QVERIFY(source.isMasterEnabled());
}

QTEST_MAIN(TestRepository)
#include "tst_repository.moc"
