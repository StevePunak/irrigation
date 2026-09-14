#include <QTest>
#include <QTemporaryDir>
#include <QSettings>
#include <QFile>

#include <Kanoop/log.h>
#include <Kanoop/logconsumer.h>
#include <Kanoop/logentry.h>

#include "irrigationsettings.h"

static void writeRawIni(const QString& path, const QString& content)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(content.toUtf8());
    file.close();
}

class TestSettings : public QObject
{
    Q_OBJECT
private slots:
    void zoneMapParsesEightEntries();
    void zoneMapRejectsMalformedEntries();
    void defaultsAreSafe();
    void zoneMapAcceptsUnquotedRawIniList();
    void zoneMapTrimsWhitespaceAroundZoneAndOffsetInRawIni();
    void zoneMapWarnsOnMalformedEntriesInRawIni();
    void zoneMapDuplicateZoneNumberKeepsLastOffsetAndWarnsInSummary();
    void zoneMapEmptyValueProducesEmptyMapWithNoWarnings();
    void zoneMapCleanRawIniProducesNoWarnings();
};

void TestSettings::zoneMapParsesEightEntries()
{
    QTemporaryDir dir;
    QString path = dir.filePath("test.ini");
    {
        QSettings ini(path, QSettings::IniFormat);
        ini.setValue("gpio/zones", "1=5,2=6,3=12,4=13,5=16,6=19,7=20,8=21");
    }

    IrrigationSettings settings(path);
    QMap<int, quint32> map = settings.zoneGpioMap();

    QCOMPARE(map.count(), 8);
    QCOMPARE(map.value(1), 5u);
    QCOMPARE(map.value(4), 13u);
    QCOMPARE(map.value(5), 16u);
    QCOMPARE(map.value(8), 21u);
}

void TestSettings::zoneMapRejectsMalformedEntries()
{
    QTemporaryDir dir;
    QString path = dir.filePath("test.ini");
    {
        QSettings ini(path, QSettings::IniFormat);
        ini.setValue("gpio/zones", "1=5,rubbish,3=13,4=");
    }

    IrrigationSettings settings(path);
    QMap<int, quint32> map = settings.zoneGpioMap();

    QCOMPARE(map.count(), 2);
    QCOMPARE(map.value(1), 5u);
    QCOMPARE(map.value(3), 13u);
}

void TestSettings::defaultsAreSafe()
{
    QTemporaryDir dir;
    IrrigationSettings settings(dir.filePath("absent.ini"));

    QCOMPARE(settings.maxZoneSeconds(), 3600);
    QCOMPARE(settings.bindAddress(), QString("127.0.0.1"));
    QCOMPARE(settings.listenPort(), 8080);
    QCOMPARE(settings.zoneActiveLow(), true);
    QVERIFY(settings.zoneGpioMap().isEmpty());
}

void TestSettings::zoneMapAcceptsUnquotedRawIniList()
{
    QTemporaryDir dir;
    QString path = dir.filePath("test.ini");
    writeRawIni(path, "[gpio]\nzones=11=101,22=102,33=103,44=104\n");

    IrrigationSettings settings(path);
    QMap<int, quint32> map = settings.zoneGpioMap();

    QCOMPARE(map.count(), 4);
    QCOMPARE(map.value(11), 101u);
    QCOMPARE(map.value(22), 102u);
    QCOMPARE(map.value(33), 103u);
    QCOMPARE(map.value(44), 104u);
}

void TestSettings::zoneMapTrimsWhitespaceAroundZoneAndOffsetInRawIni()
{
    QTemporaryDir dir;
    QString path = dir.filePath("test.ini");
    writeRawIni(path, "[gpio]\nzones= 91 = 910 ,92=920\n");

    IrrigationSettings settings(path);
    QMap<int, quint32> map = settings.zoneGpioMap();

    QCOMPARE(map.count(), 2);
    QCOMPARE(map.value(91), 910u);
    QCOMPARE(map.value(92), 920u);
}

void TestSettings::zoneMapWarnsOnMalformedEntriesInRawIni()
{
    QTemporaryDir dir;
    QString path = dir.filePath("test.ini");
    writeRawIni(path, "[gpio]\nzones=51=510,rubbish,abc=60,53=530\n");

    LogConsumer consumer;
    QStringList messages;
    connect(&consumer, &LogConsumer::logEntry, [&messages](const Log::LogEntry& entry) {
        messages.append(entry.unformattedText());
    });
    Log::addConsumer(&consumer);

    IrrigationSettings settings(path);
    QMap<int, quint32> map = settings.zoneGpioMap();

    Log::removeConsumer(&consumer);

    QCOMPARE(map.count(), 2);
    QCOMPARE(map.value(51), 510u);
    QCOMPARE(map.value(53), 530u);

    QCOMPARE(messages.count(), 3);
    QCOMPARE(messages.at(0), QString("Skipping malformed zone entry \"rubbish\": expected zone=offset"));
    QCOMPARE(messages.at(1), QString("Skipping malformed zone entry \"abc=60\": zone or offset is not numeric"));
    QCOMPARE(messages.at(2), QString("Parsed 2 of 4 zone entries"));
}

void TestSettings::zoneMapDuplicateZoneNumberKeepsLastOffsetAndWarnsInSummary()
{
    QTemporaryDir dir;
    QString path = dir.filePath("test.ini");
    writeRawIni(path, "[gpio]\nzones=71=710,71=715,72=720\n");

    LogConsumer consumer;
    QStringList messages;
    connect(&consumer, &LogConsumer::logEntry, [&messages](const Log::LogEntry& entry) {
        messages.append(entry.unformattedText());
    });
    Log::addConsumer(&consumer);

    IrrigationSettings settings(path);
    QMap<int, quint32> map = settings.zoneGpioMap();

    Log::removeConsumer(&consumer);

    QCOMPARE(map.count(), 2);
    QCOMPARE(map.value(71), 715u);
    QCOMPARE(map.value(72), 720u);

    QCOMPARE(messages.count(), 1);
    QCOMPARE(messages.at(0), QString("Parsed 2 of 3 zone entries"));
}

void TestSettings::zoneMapEmptyValueProducesEmptyMapWithNoWarnings()
{
    QTemporaryDir dir;
    QString path = dir.filePath("test.ini");
    writeRawIni(path, "[gpio]\nzones=\n");

    LogConsumer consumer;
    QStringList messages;
    connect(&consumer, &LogConsumer::logEntry, [&messages](const Log::LogEntry& entry) {
        messages.append(entry.unformattedText());
    });
    Log::addConsumer(&consumer);

    IrrigationSettings settings(path);
    QMap<int, quint32> map = settings.zoneGpioMap();

    Log::removeConsumer(&consumer);

    QVERIFY(map.isEmpty());
    QVERIFY(messages.isEmpty());
}

void TestSettings::zoneMapCleanRawIniProducesNoWarnings()
{
    QTemporaryDir dir;
    QString path = dir.filePath("test.ini");
    writeRawIni(path, "[gpio]\nzones=81=810,82=820\n");

    LogConsumer consumer;
    QStringList messages;
    connect(&consumer, &LogConsumer::logEntry, [&messages](const Log::LogEntry& entry) {
        messages.append(entry.unformattedText());
    });
    Log::addConsumer(&consumer);

    IrrigationSettings settings(path);
    QMap<int, quint32> map = settings.zoneGpioMap();

    Log::removeConsumer(&consumer);

    QCOMPARE(map.count(), 2);
    QCOMPARE(map.value(81), 810u);
    QCOMPARE(map.value(82), 820u);
    QVERIFY(messages.isEmpty());
}

QTEST_MAIN(TestSettings)
#include "tst_settings.moc"
