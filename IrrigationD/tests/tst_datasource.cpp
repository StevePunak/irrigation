#include <QTest>
#include <QTemporaryDir>
#include <QSqlQuery>
#include <QSqlError>

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
    QVERIFY(tables.contains("program_zones"));
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

QTEST_MAIN(TestDataSource)
#include "tst_datasource.moc"
