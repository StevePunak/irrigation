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
    void programZonesRoundTripInSequenceOrder();
    void startTimeRoundTripsAllFields();
    void updateZoneChangesNameAndEnabled();
    void updateZoneLandsCorrectlyWhenNumberDiffersFromId();
    void updateProgramChangesFields();
    void enabledProgramsExcludesDisabled();
    void settingValueRoundTrips();
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

void TestRepository::programZonesRoundTripInSequenceOrder()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    Program program;
    program.name = "Backyard";
    QVERIFY(source.insertProgram(program));

    // Insert out of sequence order; zonesFor() must still come back ordered by sequence.
    // zoneId/sequence are kept clear of programId (1 in a fresh database) and of each
    // other, so a transposed bind between any two of these int columns is observable.
    ProgramZone third;
    third.programId = program.id;
    third.zoneId = 6;
    third.sequence = 12;
    third.durationSeconds = 300;
    QVERIFY(source.insertProgramZone(third));
    QVERIFY(third.id > 0);

    ProgramZone first;
    first.programId = program.id;
    first.zoneId = 4;
    first.sequence = 10;
    first.durationSeconds = 600;
    QVERIFY(source.insertProgramZone(first));

    ProgramZone second;
    second.programId = program.id;
    second.zoneId = 5;
    second.sequence = 11;
    second.durationSeconds = 450;
    QVERIFY(source.insertProgramZone(second));

    ProgramZoneList zones = source.zonesFor(program.id);
    QCOMPARE(zones.count(), 3);
    QCOMPARE(zones.at(0).id, first.id);
    QCOMPARE(zones.at(0).programId, program.id);
    QCOMPARE(zones.at(0).zoneId, 4);
    QCOMPARE(zones.at(0).sequence, 10);
    QCOMPARE(zones.at(0).durationSeconds, 600);
    QCOMPARE(zones.at(1).id, second.id);
    QCOMPARE(zones.at(1).programId, program.id);
    QCOMPARE(zones.at(1).zoneId, 5);
    QCOMPARE(zones.at(1).sequence, 11);
    QCOMPARE(zones.at(1).durationSeconds, 450);
    QCOMPARE(zones.at(2).id, third.id);
    QCOMPARE(zones.at(2).programId, program.id);
    QCOMPARE(zones.at(2).zoneId, 6);
    QCOMPARE(zones.at(2).sequence, 12);
    QCOMPARE(zones.at(2).durationSeconds, 300);
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

QTEST_MAIN(TestRepository)
#include "tst_repository.moc"
