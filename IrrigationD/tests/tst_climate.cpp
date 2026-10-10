#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>

#include <Kanoop/log.h>
#include <Kanoop/logconsumer.h>
#include <Kanoop/logentry.h>

#include "climatelogger.h"
#include "database/climatedatasource.h"
#include "iclimatesensor.h"
#include "iclock.h"
#include "irrigationsettings.h"
#include "sht30.h"

namespace
{
const QDateTime START = QDateTime(QDate(2026, 10, 9), QTime(18, 0, 0), QTimeZone::UTC);

QByteArray word(quint16 value)
{
    QByteArray result;
    result.append(static_cast<char>(value >> 8));
    result.append(static_cast<char>(value & 0xFF));
    return result;
}

QByteArray frameFor(quint16 rawTemperature, quint16 rawHumidity)
{
    const QByteArray temperature = word(rawTemperature);
    const QByteArray humidity = word(rawHumidity);
    return temperature + static_cast<char>(Sht30::crc8(temperature)) + humidity + static_cast<char>(Sht30::crc8(humidity));
}

bool writeIni(const QString& path, const QString& content)
{
    QFile file(path);
    if(file.open(QIODevice::WriteOnly | QIODevice::Truncate) == false) {
        return false;
    }
    return file.write(content.toUtf8()) == content.toUtf8().size();
}
}

/** Answers each start and read from its scripted results; a script that runs out succeeds. */
class FakeSensor : public IClimateSensor
{
public:
    virtual bool startMeasurement() override
    {
        starts++;
        return startResults.isEmpty() ? true : startResults.takeFirst();
    }

    virtual bool readMeasurement(double& temperatureCelsius, double& humidityPercent) override
    {
        if(readResults.isEmpty() == false && readResults.takeFirst() == false) {
            return false;
        }
        temperatureCelsius = temperature;
        humidityPercent = humidity;
        return true;
    }

    virtual QString errorText() const override { return "injected sensor failure"; }

    QList<bool> startResults;
    QList<bool> readResults;
    int starts = 0;
    double temperature = 31.5;
    double humidity = 52.5;
};

class TestClimate : public QObject
{
    Q_OBJECT
private slots:
    void crcMatchesTheDatasheetVector()
    {
        QCOMPARE(Sht30::crc8(word(0xBEEF)), static_cast<quint8>(0x92));
    }

    void decodeConvertsBothWords()
    {
        double temperature = 0;
        double humidity = 0;
        QVERIFY(Sht30::decode(frameFor(0x6666, 0x8000), temperature, humidity));
        QVERIFY(qAbs(temperature - 25.0) < 0.01);
        QVERIFY(qAbs(humidity - 50.0) < 0.01);
    }

    void decodeReachesBothEndsOfTheRange()
    {
        double temperature = 0;
        double humidity = 0;
        QVERIFY(Sht30::decode(frameFor(0x0000, 0x0000), temperature, humidity));
        QCOMPARE(temperature, -45.0);
        QCOMPARE(humidity, 0.0);
        QVERIFY(Sht30::decode(frameFor(0xFFFF, 0xFFFF), temperature, humidity));
        QCOMPARE(temperature, 130.0);
        QCOMPARE(humidity, 100.0);
    }

    void decodeRefusesABadTemperatureCrc()
    {
        QByteArray frame = frameFor(0x6666, 0x8000);
        frame[2] = static_cast<char>(frame.at(2) ^ 0x01);
        double temperature = 0;
        double humidity = 0;
        QCOMPARE(Sht30::decode(frame, temperature, humidity), false);
    }

    void decodeRefusesABadHumidityCrc()
    {
        QByteArray frame = frameFor(0x6666, 0x8000);
        frame[5] = static_cast<char>(frame.at(5) ^ 0x80);
        double temperature = 0;
        double humidity = 0;
        QCOMPARE(Sht30::decode(frame, temperature, humidity), false);
    }

    void decodeRefusesAShortFrame()
    {
        double temperature = 0;
        double humidity = 0;
        QCOMPARE(Sht30::decode(frameFor(0x6666, 0x8000).left(5), temperature, humidity), false);
    }

    void aSensorOnAMissingBusFailsWithoutThrowing()
    {
        Sht30 sensor(99, Sht30::DefaultAddress);
        QCOMPARE(sensor.startMeasurement(), false);
        QVERIFY(sensor.errorText().contains("/dev/i2c-99"));
        double temperature = 0;
        double humidity = 0;
        QCOMPARE(sensor.readMeasurement(temperature, humidity), false);
    }

    void storeCreatesTheSchemaAndStampsItsVersion()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("climate.db");
        {
            ClimateDataSource store(path);
            QVERIFY(store.open());
        }
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "inspect");
        db.setDatabaseName(path);
        QVERIFY(db.open());
        {
            QSqlQuery query(db);
            QVERIFY(query.exec("SELECT sw_version FROM info WHERE id = 1"));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toString(), ClimateDataSource::schemaVersion());
            QVERIFY(query.exec("PRAGMA journal_mode"));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toString(), QString("wal"));
        }
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase("inspect");
    }

    void storeRoundTripsReadingsAcrossAReopen()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("climate.db");
        ClimateReadingList readings;
        for(int i = 0; i < 3; i++) {
            ClimateReading reading;
            reading.atUtc = START.addSecs(i * 5);
            reading.temperatureCelsius = 20.0 + i;
            reading.humidityPercent = 40.0 + i;
            readings.append(reading);
        }
        {
            ClimateDataSource store(path);
            QVERIFY(store.open());
            QVERIFY(store.insertReadings(readings));
        }

        ClimateDataSource store(path);
        QVERIFY(store.open());
        const ClimateReadingList stored = store.readingsBetween(START, START.addSecs(60));
        QCOMPARE(stored.count(), 3);
        for(int i = 0; i < 3; i++) {
            QCOMPARE(stored.at(i).atUtc, readings.at(i).atUtc);
            QCOMPARE(stored.at(i).temperatureCelsius, readings.at(i).temperatureCelsius);
            QCOMPARE(stored.at(i).humidityPercent, readings.at(i).humidityPercent);
        }
    }

    void storeBoundsReadingsHalfOpen()
    {
        QTemporaryDir dir;
        ClimateDataSource store(dir.filePath("climate.db"));
        QVERIFY(store.open());
        ClimateReading first;
        first.atUtc = START;
        ClimateReading second;
        second.atUtc = START.addSecs(5);
        QVERIFY(store.insertReadings({ first, second }));

        QCOMPARE(store.readingsBetween(START, START.addSecs(5)).count(), 1);
        QCOMPARE(store.readingsBetween(START.addSecs(5), START.addSecs(6)).count(), 1);
    }

    void storeAcceptsAnEmptyBatch()
    {
        QTemporaryDir dir;
        ClimateDataSource store(dir.filePath("climate.db"));
        QVERIFY(store.open());
        QVERIFY(store.insertReadings(ClimateReadingList()));
    }

    void loggerStampsACollectedReadingWithTheClock()
    {
        QTemporaryDir dir;
        ClimateDataSource store(dir.filePath("climate.db"));
        QVERIFY(store.open());
        FakeSensor sensor;
        TestClock clock(START);
        ClimateLogger logger(&sensor, &store, &clock, 5, 60);

        logger.sample();
        logger.collect();
        QCOMPARE(logger.pendingCount(), 1);

        logger.flush();
        QCOMPARE(logger.pendingCount(), 0);
        const ClimateReadingList stored = store.readingsBetween(START, START.addSecs(1));
        QCOMPARE(stored.count(), 1);
        QCOMPARE(stored.first().temperatureCelsius, 31.5);
        QCOMPARE(stored.first().humidityPercent, 52.5);
    }

    void loggerCollectsAfterTheMeasurementTime()
    {
        QTemporaryDir dir;
        ClimateDataSource store(dir.filePath("climate.db"));
        QVERIFY(store.open());
        FakeSensor sensor;
        TestClock clock(START);
        ClimateLogger logger(&sensor, &store, &clock, 5, 60);

        logger.sample();
        QCOMPARE(logger.pendingCount(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(logger.pendingCount(), 1, 1000);
    }

    void loggerWritesNoRowForAFailedStart()
    {
        QTemporaryDir dir;
        ClimateDataSource store(dir.filePath("climate.db"));
        QVERIFY(store.open());
        FakeSensor sensor;
        sensor.startResults = { false };
        TestClock clock(START);
        ClimateLogger logger(&sensor, &store, &clock, 5, 60);

        logger.sample();
        QTest::qWait(Sht30::MeasurementMilliseconds * 3);
        QCOMPARE(logger.pendingCount(), 0);
    }

    void loggerWritesNoRowForAFailedRead()
    {
        QTemporaryDir dir;
        ClimateDataSource store(dir.filePath("climate.db"));
        QVERIFY(store.open());
        FakeSensor sensor;
        sensor.readResults = { false };
        TestClock clock(START);
        ClimateLogger logger(&sensor, &store, &clock, 5, 60);

        logger.collect();
        QCOMPARE(logger.pendingCount(), 0);
    }

    void loggerLogsAFailureOnceAndTheRecoveryOnce()
    {
        QTemporaryDir dir;
        ClimateDataSource store(dir.filePath("climate.db"));
        QVERIFY(store.open());
        FakeSensor sensor;
        sensor.readResults = { false, false, false, true };
        TestClock clock(START);
        ClimateLogger logger(&sensor, &store, &clock, 5, 60);

        QStringList texts;
        LogConsumer consumer;
        connect(&consumer, &LogConsumer::logEntry, this, [&texts](const Log::LogEntry& entry) {
            texts.append(entry.unformattedText());
        });
        Log::addConsumer(&consumer);
        for(int i = 0; i < 4; i++) {
            logger.collect();
        }
        Log::removeConsumer(&consumer);

        QCOMPARE(texts.count(), 2);
        QCOMPARE(texts.at(0), QString("The climate sensor failed: injected sensor failure"));
        QCOMPARE(texts.at(1), QString("The climate sensor is reading again: 31.5 C, 52.5 %RH"));
    }

    void loggerKeepsReadingsAcrossAFailedCommit()
    {
        QTemporaryDir dir;
        ClimateDataSource store(dir.filePath("climate.db"));
        QVERIFY(store.open());
        FakeSensor sensor;
        TestClock clock(START);
        ClimateLogger logger(&sensor, &store, &clock, 5, 60);

        logger.collect();
        store.closeConnection();
        logger.flush();
        QCOMPARE(logger.pendingCount(), 1);

        QVERIFY(store.open());
        logger.flush();
        QCOMPARE(logger.pendingCount(), 0);
        QCOMPARE(store.readingsBetween(START, START.addSecs(1)).count(), 1);
    }

    void loggerDropsTheOldestBeyondThePendingCap()
    {
        QTemporaryDir dir;
        ClimateDataSource store(dir.filePath("climate.db"));
        QVERIFY(store.open());
        FakeSensor sensor;
        TestClock clock(START);
        ClimateLogger logger(&sensor, &store, &clock, 5, 60);

        store.closeConnection();
        for(int i = 0; i < ClimateLogger::MaximumPendingReadings + 3; i++) {
            logger.collect();
            clock.advance(5);
        }
        logger.flush();
        QCOMPARE(logger.pendingCount(), ClimateLogger::MaximumPendingReadings);

        QVERIFY(store.open());
        logger.flush();
        const ClimateReadingList stored = store.readingsBetween(START, START.addDays(1));
        QCOMPARE(stored.count(), ClimateLogger::MaximumPendingReadings);
        QCOMPARE(stored.first().atUtc, START.addSecs(3 * 5));
    }

    void loggerReportsTheLatestReadingUntilItGoesStale()
    {
        QTemporaryDir dir;
        ClimateDataSource store(dir.filePath("climate.db"));
        QVERIFY(store.open());
        FakeSensor sensor;
        TestClock clock(START);
        ClimateLogger logger(&sensor, &store, &clock, 5, 60);

        ClimateReading reading;
        QCOMPARE(logger.latestReading(reading, 30), false);

        logger.collect();
        clock.advance(30);
        QVERIFY(logger.latestReading(reading, 30));
        QCOMPARE(reading.temperatureCelsius, 31.5);
        QCOMPARE(reading.humidityPercent, 52.5);

        clock.advanceMsecs(1);
        QCOMPARE(logger.latestReading(reading, 30), false);
    }

    void loggerKeepsTheLastGoodReadingThroughAFailedRead()
    {
        QTemporaryDir dir;
        ClimateDataSource store(dir.filePath("climate.db"));
        QVERIFY(store.open());
        FakeSensor sensor;
        sensor.readResults = { true, false };
        TestClock clock(START);
        ClimateLogger logger(&sensor, &store, &clock, 5, 60);

        logger.collect();
        sensor.temperature = 99.0;
        logger.collect();

        ClimateReading reading;
        QVERIFY(logger.latestReading(reading, 30));
        QCOMPARE(reading.temperatureCelsius, 31.5);
    }

    void loggerCommitsPendingReadingsWhenDestroyed()
    {
        QTemporaryDir dir;
        ClimateDataSource store(dir.filePath("climate.db"));
        QVERIFY(store.open());
        FakeSensor sensor;
        TestClock clock(START);
        {
            ClimateLogger logger(&sensor, &store, &clock, 5, 60);
            logger.collect();
            logger.collect();
        }
        QCOMPARE(store.readingsBetween(START, START.addSecs(1)).count(), 2);
    }

    void settingsLeaveClimateOffWithoutABus()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("irrigationd.ini");
        QVERIFY(writeIni(path, "[database]\npath=/data/irrigationd/irrigation.db\n"));
        IrrigationSettings settings(path);
        QCOMPARE(settings.climateBus(), -1);
    }

    void settingsReadTheClimateSection()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("irrigationd.ini");
        QVERIFY(writeIni(path, "[database]\npath=/data/irrigationd/irrigation.db\n"
                               "[climate]\nbus=1\naddress=0x45\nintervalSeconds=10\n"));
        IrrigationSettings settings(path);
        QCOMPARE(settings.climateBus(), 1);
        QCOMPARE(settings.climateAddress(), static_cast<quint8>(0x45));
        QCOMPARE(settings.climateSampleSeconds(), 10);
        QCOMPARE(settings.climateDatabasePath(), QString("/data/irrigationd/climate.db"));
    }

    void settingsFallBackOnMalformedClimateValues()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("irrigationd.ini");
        QVERIFY(writeIni(path, "[climate]\nbus=one\naddress=0x99\nintervalSeconds=0\ndatabasePath=/tmp/c.db\n"));
        IrrigationSettings settings(path);
        QCOMPARE(settings.climateBus(), -1);
        QCOMPARE(settings.climateAddress(), Sht30::DefaultAddress);
        QCOMPARE(settings.climateSampleSeconds(), 5);
        QCOMPARE(settings.climateDatabasePath(), QString("/tmp/c.db"));
    }
};

QTEST_GUILESS_MAIN(TestClimate)

#include "tst_climate.moc"
