#include <QTest>
#include <QTemporaryDir>
#include <QSettings>

#include "irrigationsettings.h"

class TestSettings : public QObject
{
    Q_OBJECT
private slots:
    void zoneMapParsesEightEntries();
    void zoneMapRejectsMalformedEntries();
    void defaultsAreSafe();
};

void TestSettings::zoneMapParsesEightEntries()
{
    QTemporaryDir dir;
    QString path = dir.filePath("test.ini");
    {
        QSettings ini(path, QSettings::IniFormat);
        ini.setValue("gpio/zones", "1=5,2=6,3=13,4=16,5=19,6=20,7=21,8=26");
    }

    IrrigationSettings settings(path);
    QMap<int, quint32> map = settings.zoneGpioMap();

    QCOMPARE(map.count(), 8);
    QCOMPARE(map.value(1), 5u);
    QCOMPARE(map.value(4), 16u);
    QCOMPARE(map.value(5), 19u);
    QCOMPARE(map.value(8), 26u);
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

QTEST_MAIN(TestSettings)
#include "tst_settings.moc"
