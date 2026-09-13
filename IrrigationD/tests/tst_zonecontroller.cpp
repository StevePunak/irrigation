#include <QTest>
#include <QSignalSpy>

#include <Kanoop/pi/mockbackend.h>

#include "zonecontroller.h"

static QMap<int, quint32> eightZones()
{
    return { {1,5}, {2,6}, {3,12}, {4,13}, {5,16}, {6,19}, {7,20}, {8,21} };
}

class TestZoneController : public QObject
{
    Q_OBJECT
private slots:
    void beginDrivesEveryLineInactive();
    void openingAZoneClosesTheOpenOneAtomically();
    void durationIsClampedToTheCeiling();
    void durationBelowOneIsRejected();
    void zoneClosesWhenItsTimerExpires();
    void watchdogClosesAZonePastItsDeadline();
    void watchdogTripsOnWrongLineEnergised();
    void watchdogDoesNotReportSuccessWhenCloseFails();
    void allOffClosesEverything();
    void unknownZoneNumberIsRejected();
};

void TestZoneController::beginDrivesEveryLineInactive()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    for(quint32 offset : eightZones().values()) {
        backend.setLineValue(offset, Gpio::Value::Active);
    }

    QVERIFY(controller.begin());

    QCOMPARE(controller.openZoneNumber(), 0);
    for(quint32 offset : eightZones().values()) {
        QCOMPARE(backend.lineValue(offset), Gpio::Value::Inactive);
    }
    QCOMPARE(backend.setValuesCallCount(), 1);
}

void TestZoneController::openingAZoneClosesTheOpenOneAtomically()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(3, 60));
    QCOMPARE(backend.lineValue(12), Gpio::Value::Active);

    backend.resetSetValuesCallCount();
    QVERIFY(controller.openZone(5, 60));

    QCOMPARE(backend.lineValue(12), Gpio::Value::Inactive);
    QCOMPARE(backend.lineValue(16), Gpio::Value::Active);

    // Both transitions in ONE write. Two writes means a window where both valves
    // are open, and on a 40 VA transformer that is a brownout.
    QCOMPARE(backend.setValuesCallCount(), 1);
}

void TestZoneController::durationIsClampedToTheCeiling()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 120);
    QVERIFY(controller.begin());

    QSignalSpy spy(&controller, &ZoneController::zoneOpened);
    QVERIFY(controller.openZone(1, 99999));

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(1).toInt(), 120);
    QVERIFY(controller.secondsRemaining() >= 119 && controller.secondsRemaining() <= 120);
}

void TestZoneController::durationBelowOneIsRejected()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(1, 0) == false);
    QVERIFY(controller.openZone(1, -5) == false);
    QCOMPARE(controller.openZoneNumber(), 0);
}

void TestZoneController::zoneClosesWhenItsTimerExpires()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    controller.setWatchdogInterval(TimeSpan::fromSeconds(60));
    QVERIFY(controller.begin());

    QSignalSpy closedSpy(&controller, &ZoneController::zoneClosed);
    QSignalSpy watchdogSpy(&controller, &ZoneController::watchdogTripped);
    QVERIFY(controller.openZone(6, 2));

    QVERIFY(closedSpy.wait(4000));
    QCOMPARE(closedSpy.first().at(0).toInt(), 6);
    QCOMPARE(controller.openZoneNumber(), 0);
    QCOMPARE(backend.lineValue(19), Gpio::Value::Inactive);
    QCOMPARE(watchdogSpy.count(), 0);
}

void TestZoneController::watchdogClosesAZonePastItsDeadline()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    controller.setWatchdogInterval(TimeSpan::fromMilliseconds(100));
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(3, 2));

    controller.disableCloseTimerForTest();

    QSignalSpy spy(&controller, &ZoneController::watchdogTripped);
    QVERIFY(spy.wait(5000));
    QCOMPARE(backend.lineValue(12), Gpio::Value::Inactive);
    QCOMPARE(controller.openZoneNumber(), 0);
}

void TestZoneController::watchdogTripsOnWrongLineEnergised()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    controller.setWatchdogInterval(TimeSpan::fromMilliseconds(100));
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(7, 600));

    backend.setLineValue(20, Gpio::Value::Inactive);
    backend.setLineValue(13, Gpio::Value::Active);

    QSignalSpy spy(&controller, &ZoneController::watchdogTripped);
    QVERIFY(spy.wait(5000));
    QCOMPARE(spy.first().at(0).toInt(), 7);
    QCOMPARE(controller.openZoneNumber(), 0);
}

void TestZoneController::watchdogDoesNotReportSuccessWhenCloseFails()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(8, 600));

    backend.setLineValue(12, Gpio::Value::Active);
    backend.setFailNextSetValues(true);

    QSignalSpy trippedSpy(&controller, &ZoneController::watchdogTripped);
    controller.triggerWatchdogForTest();

    QCOMPARE(trippedSpy.count(), 0);
    QCOMPARE(controller.openZoneNumber(), 8);
    QCOMPARE(backend.lineValue(12), Gpio::Value::Active);
    QCOMPARE(backend.lineValue(21), Gpio::Value::Active);
}

void TestZoneController::allOffClosesEverything()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(7, 600));

    QSignalSpy spy(&controller, &ZoneController::zoneClosed);
    QVERIFY(controller.allOff());

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toInt(), 7);
    QCOMPARE(controller.openZoneNumber(), 0);
    QVERIFY(controller.closeTimerActiveForTest() == false);
    for(quint32 offset : eightZones().values()) {
        QCOMPARE(backend.lineValue(offset), Gpio::Value::Inactive);
    }
}

void TestZoneController::unknownZoneNumberIsRejected()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(0, 60) == false);
    QVERIFY(controller.openZone(9, 60) == false);
    QCOMPARE(controller.openZoneNumber(), 0);
}

QTEST_MAIN(TestZoneController)
#include "tst_zonecontroller.moc"
