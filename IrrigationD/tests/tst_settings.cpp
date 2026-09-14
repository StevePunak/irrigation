#include <QTest>
#include <QTemporaryDir>
#include <QSettings>
#include <QFile>

#include <Kanoop/log.h>
#include <Kanoop/logconsumer.h>
#include <Kanoop/logentry.h>

#include "irrigationsettings.h"

static bool writeRawIni(const QString& path, const QString& content)
{
    QFile file(path);
    if(file.open(QIODevice::WriteOnly | QIODevice::Truncate) == false) {
        return false;
    }
    QByteArray bytes = content.toUtf8();
    bool ok = file.write(bytes) == bytes.size();
    file.close();
    return ok;
}

struct CapturedZoneMap
{
    QMap<int, quint32> map;
    QList<Log::LogEntry> entries;
};

static CapturedZoneMap parseZoneMapCapturingLog(const QString& path)
{
    CapturedZoneMap result;

    LogConsumer consumer;
    QObject::connect(&consumer, &LogConsumer::logEntry, [&result](const Log::LogEntry& entry) {
        result.entries.append(entry);
    });
    Log::addConsumer(&consumer);

    IrrigationSettings settings(path);
    result.map = settings.zoneGpioMap();

    Log::removeConsumer(&consumer);
    return result;
}

class TestSettings : public QObject
{
    Q_OBJECT
private slots:
    void zoneMapParsesEightEntries();
    void zoneMapRejectsMalformedEntries();
    void defaultsAreSafe();
    void zoneMapAcceptsUnquotedRawIniList();
    void zoneMapTrimsNonBreakingSpaceAroundZoneAndOffsetInRawIni();
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
    QCOMPARE(settings.stopButtonOffset(), 25u);
    QVERIFY(settings.zoneGpioMap().isEmpty());
}

void TestSettings::zoneMapAcceptsUnquotedRawIniList()
{
    QTemporaryDir dir;
    QString path = dir.filePath("test.ini");
    QVERIFY(writeRawIni(path, "[gpio]\nzones=11=101,22=102,33=103,44=104\n"));

    IrrigationSettings settings(path);
    QMap<int, quint32> map = settings.zoneGpioMap();

    QCOMPARE(map.count(), 4);
    QCOMPARE(map.value(11), 101u);
    QCOMPARE(map.value(22), 102u);
    QCOMPARE(map.value(33), 103u);
    QCOMPARE(map.value(44), 104u);
}

// QSettings passes U+00A0 through unquoted; toInt()/toUInt() accept ASCII space, tab
// and CR padding but reject U+00A0, so this exercises .trimmed() where ASCII whitespace
// cannot.
void TestSettings::zoneMapTrimsNonBreakingSpaceAroundZoneAndOffsetInRawIni()
{
    QTemporaryDir dir;
    QString path = dir.filePath("test.ini");
    QVERIFY(writeRawIni(path, QString::fromUtf8("[gpio]\nzones=61\xc2\xa0=\xc2\xa0" "610,62=620\n")));

    CapturedZoneMap result = parseZoneMapCapturingLog(path);

    QCOMPARE(result.map.count(), 2);
    QCOMPARE(result.map.value(61), 610u);
    QCOMPARE(result.map.value(62), 620u);
    QVERIFY(result.entries.isEmpty());
}

void TestSettings::zoneMapWarnsOnMalformedEntriesInRawIni()
{
    QTemporaryDir dir;
    QString path = dir.filePath("test.ini");
    QVERIFY(writeRawIni(path, "[gpio]\nzones=51=510,rubbish,abc=60,53=530,9=-1\n"));

    CapturedZoneMap result = parseZoneMapCapturingLog(path);

    QCOMPARE(result.map.count(), 2);
    QCOMPARE(result.map.value(51), 510u);
    QCOMPARE(result.map.value(53), 530u);
    QVERIFY(result.map.contains(9) == false);

    QCOMPARE(result.entries.count(), 4);
    QCOMPARE(result.entries.at(0).unformattedText(), QString("Skipping malformed zone entry \"rubbish\": expected zone=offset"));
    QCOMPARE(result.entries.at(1).unformattedText(), QString("Skipping malformed zone entry \"abc=60\": zone or offset is not numeric"));
    QCOMPARE(result.entries.at(2).unformattedText(), QString("Skipping malformed zone entry \"9=-1\": zone or offset is not numeric"));
    QCOMPARE(result.entries.at(3).unformattedText(), QString("Parsed 2 of 5 zone entries"));
    for(const Log::LogEntry& entry : result.entries) {
        QCOMPARE(entry.level(), Log::LogLevel::Warning);
    }
}

void TestSettings::zoneMapDuplicateZoneNumberKeepsLastOffsetAndWarnsInSummary()
{
    QTemporaryDir dir;
    QString path = dir.filePath("test.ini");
    QVERIFY(writeRawIni(path, "[gpio]\nzones=71=710,71=715,72=720\n"));

    CapturedZoneMap result = parseZoneMapCapturingLog(path);

    QCOMPARE(result.map.count(), 2);
    QCOMPARE(result.map.value(71), 715u);
    QCOMPARE(result.map.value(72), 720u);

    QCOMPARE(result.entries.count(), 1);
    QCOMPARE(result.entries.at(0).unformattedText(), QString("Parsed 2 of 3 zone entries"));
    QCOMPARE(result.entries.at(0).level(), Log::LogLevel::Warning);
}

void TestSettings::zoneMapEmptyValueProducesEmptyMapWithNoWarnings()
{
    QTemporaryDir dir;
    QString path = dir.filePath("test.ini");
    QVERIFY(writeRawIni(path, "[gpio]\nzones=\n"));

    CapturedZoneMap result = parseZoneMapCapturingLog(path);

    QVERIFY(result.map.isEmpty());
    QVERIFY(result.entries.isEmpty());
}

void TestSettings::zoneMapCleanRawIniProducesNoWarnings()
{
    QTemporaryDir dir;
    QString path = dir.filePath("test.ini");
    QVERIFY(writeRawIni(path, "[gpio]\nzones=81=810,82=820\n"));

    CapturedZoneMap result = parseZoneMapCapturingLog(path);

    QCOMPARE(result.map.count(), 2);
    QCOMPARE(result.map.value(81), 810u);
    QCOMPARE(result.map.value(82), 820u);
    QVERIFY(result.entries.isEmpty());
}

QTEST_MAIN(TestSettings)
#include "tst_settings.moc"
