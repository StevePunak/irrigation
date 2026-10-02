#include <QTest>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>

#include <Kanoop/database/sqlparser.h>

#include "database/irrigationdatasource.h"

class TestDataSource : public QObject
{
    Q_OBJECT
private slots:
    void createsSchemaOnFirstOpen();
    void stampsTheCompiledVersion();
    void seedsEightZones();
    void reopenDoesNotRecreate();
    void firedInstantsRejectDuplicates();
    void migratesForwardFromEarlierVersion();
    void recreatesOnMigrationFailure();
    void seedsTheZoneCap();
    void migratesProgramZonesIntoOneZoneSteps();
};

void TestDataSource::createsSchemaOnFirstOpen()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY2(source.open(), qPrintable(source.errorText()));

    QSqlQuery query(QSqlDatabase::database(source.connectionName()));
    QVERIFY(query.exec("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"));

    QStringList tables;
    while(query.next()) {
        tables.append(query.value(0).toString());
    }

    QVERIFY(tables.contains("zones"));
    QVERIFY(tables.contains("programs"));
    QVERIFY(tables.contains("program_start_times"));
    QVERIFY(tables.contains("program_steps"));
    QVERIFY(tables.contains("program_step_zones"));
    QVERIFY(tables.contains("program_zones") == false);
    QVERIFY(tables.contains("fired_instants"));
    QVERIFY(tables.contains("settings"));
    QVERIFY(tables.contains("info"));
}

void TestDataSource::stampsTheCompiledVersion()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    QSqlQuery query(QSqlDatabase::database(source.connectionName()));
    QVERIFY(query.exec("SELECT sw_version FROM info WHERE id = 1"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), source.compiledDatabaseVersion());
}

void TestDataSource::seedsEightZones()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    QSqlQuery query(QSqlDatabase::database(source.connectionName()));
    QVERIFY(query.exec("SELECT COUNT(*) FROM zones"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 8);
}

void TestDataSource::reopenDoesNotRecreate()
{
    QTemporaryDir dir;
    QString path = dir.filePath("irrigation.db");

    {
        IrrigationDataSource source(path);
        QVERIFY(source.open());
        QSqlQuery query(QSqlDatabase::database(source.connectionName()));
        QVERIFY(query.exec("UPDATE zones SET name = 'Front lawn' WHERE number = 1"));
    }

    IrrigationDataSource source(path);
    QVERIFY(source.open());
    QSqlQuery query(QSqlDatabase::database(source.connectionName()));
    QVERIFY(query.exec("SELECT name FROM zones WHERE number = 1"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), QString("Front lawn"));
}

void TestDataSource::firedInstantsRejectDuplicates()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());

    QSqlQuery query(QSqlDatabase::database(source.connectionName()));
    QVERIFY(query.exec("INSERT INTO fired_instants (program_id, start_time_id, scheduled_at_utc, outcome) "
                       "VALUES (1, 1, '2026-09-12T13:00:00Z', 'ran')"));
    QVERIFY(query.exec("INSERT INTO fired_instants (program_id, start_time_id, scheduled_at_utc, outcome) "
                       "VALUES (1, 1, '2026-09-12T13:00:00Z', 'ran')") == false);
}

void TestDataSource::migratesForwardFromEarlierVersion()
{
    QTemporaryDir dir;
    QString path = dir.filePath("irrigation.db");

    {
        QSqlDatabase seed = QSqlDatabase::addDatabase("QSQLITE", "seed-behind-connection");
        seed.setDatabaseName(path);
        QVERIFY(seed.open());
        QSqlQuery query(seed);
        QVERIFY(query.exec("CREATE TABLE info (id INTEGER PRIMARY KEY, sw_version TEXT NOT NULL)"));
        QVERIFY(query.exec("INSERT INTO info (id, sw_version) VALUES (1, '0.9.0')"));
        seed.close();
    }
    QSqlDatabase::removeDatabase("seed-behind-connection");

    IrrigationDataSource source(path);
    QVERIFY2(source.open(), qPrintable(source.errorText()));

    QSqlQuery query(QSqlDatabase::database(source.connectionName()));
    QVERIFY(query.exec("SELECT COUNT(*) FROM zones"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 8);

    QVERIFY(query.exec("SELECT sw_version FROM info WHERE id = 1"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), source.compiledDatabaseVersion());
}

void TestDataSource::recreatesOnMigrationFailure()
{
    QTemporaryDir dir;
    QString path = dir.filePath("irrigation.db");

    {
        QSqlDatabase seed = QSqlDatabase::addDatabase("QSQLITE", "seed-failure-connection");
        seed.setDatabaseName(path);
        QVERIFY(seed.open());
        QSqlQuery query(seed);
        QVERIFY(query.exec("CREATE TABLE info (id INTEGER PRIMARY KEY, sw_version TEXT NOT NULL)"));
        QVERIFY(query.exec("INSERT INTO info (id, sw_version) VALUES (1, '0.9.0')"));
        QVERIFY(query.exec("CREATE TABLE zones (id INTEGER PRIMARY KEY)"));
        seed.close();
    }
    QSqlDatabase::removeDatabase("seed-failure-connection");

    IrrigationDataSource source(path);
    QVERIFY2(source.open(), qPrintable(source.errorText()));

    QFileInfo dbInfo(path);
    QStringList backups = QDir(dbInfo.absolutePath())
                               .entryList(QStringList() << dbInfo.fileName() + ".*.backup", QDir::Files);
    QCOMPARE(backups.count(), 1);

    QSqlQuery query(QSqlDatabase::database(source.connectionName()));
    QVERIFY(query.exec("SELECT COUNT(*) FROM zones"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 8);

    QVERIFY(query.exec("SELECT sw_version FROM info WHERE id = 1"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), source.compiledDatabaseVersion());
}

void TestDataSource::seedsTheZoneCap()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QCOMPARE(source.settingValue("max_concurrent_zones"), QString("2"));
}

void TestDataSource::migratesProgramZonesIntoOneZoneSteps()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("irrigation.db");

    {
        QSqlDatabase seed = QSqlDatabase::addDatabase("QSQLITE", "seed-steps-connection");
        seed.setDatabaseName(path);
        QVERIFY(seed.open());
        QSqlQuery query(seed);

        QFile initial(":/database/migrate/irrigation/1.0.0/01-initial.sql");
        QVERIFY(initial.open(QIODevice::ReadOnly));
        SqlParser parser(QString::fromUtf8(initial.readAll()));
        QVERIFY(parser.isValid());
        for(const QString& statement : parser.statements()) {
            QVERIFY2(query.exec(statement), qPrintable(query.lastError().text()));
        }

        QVERIFY(query.exec("CREATE TABLE info (id INTEGER PRIMARY KEY, sw_version TEXT NOT NULL)"));
        QVERIFY(query.exec("INSERT INTO info (id, sw_version) VALUES (1, '1.0.0')"));
        QVERIFY(query.exec("INSERT INTO programs (id, name) VALUES (40, 'Front'), (41, 'Back')"));
        // Ids, zones, sequences and durations all differ, and row 71 carries sequence 1,
        // so program 40's steps come back in the reverse of their id order.
        QVERIFY(query.exec("INSERT INTO program_zones (id, program_id, zone_id, sequence, duration_seconds) VALUES "
                           "(70, 40, 3, 2, 300), (71, 40, 6, 1, 600), (72, 41, 8, 1, 900)"));
        seed.close();
    }
    QSqlDatabase::removeDatabase("seed-steps-connection");

    IrrigationDataSource source(path);
    QVERIFY2(source.open(), qPrintable(source.errorText()));

    const QFileInfo dbInfo(path);
    QVERIFY(QDir(dbInfo.absolutePath())
                .entryList(QStringList() << dbInfo.fileName() + ".*.backup", QDir::Files).isEmpty());

    const ProgramStepList front = source.stepsFor(40);
    QCOMPARE(front.count(), 2);
    QCOMPARE(front.at(0).id, 71);
    QCOMPARE(front.at(0).sequence, 1);
    QCOMPARE(front.at(0).durationSeconds, 600);
    QCOMPARE(front.at(0).zoneIds, QList<int>({ 6 }));
    QCOMPARE(front.at(1).id, 70);
    QCOMPARE(front.at(1).sequence, 2);
    QCOMPARE(front.at(1).durationSeconds, 300);
    QCOMPARE(front.at(1).zoneIds, QList<int>({ 3 }));

    const ProgramStepList back = source.stepsFor(41);
    QCOMPARE(back.count(), 1);
    QCOMPARE(back.at(0).id, 72);
    QCOMPARE(back.at(0).durationSeconds, 900);
    QCOMPARE(back.at(0).zoneIds, QList<int>({ 8 }));

    QSqlQuery query(QSqlDatabase::database(source.connectionName()));
    QVERIFY(query.exec("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'program_zones'"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 0);

    QCOMPARE(source.settingValue("max_concurrent_zones"), QString("2"));

    QVERIFY(query.exec("SELECT sw_version FROM info WHERE id = 1"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), source.compiledDatabaseVersion());

    ProgramStep added;
    added.programId = 41;
    added.sequence = 2;
    added.durationSeconds = 60;
    added.zoneIds = { 1 };
    QVERIFY(source.insertProgramStep(added));
    QVERIFY(added.id > 72);

    QVERIFY(source.deleteProgram(40));
    QVERIFY(query.exec("SELECT COUNT(*) FROM program_step_zones WHERE step_id IN (70, 71)"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 0);
}

QTEST_MAIN(TestDataSource)
#include "tst_datasource.moc"
