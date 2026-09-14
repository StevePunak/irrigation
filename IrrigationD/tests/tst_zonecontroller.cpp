#include <QTest>
#include <QSignalSpy>

#include <Kanoop/pi/mockbackend.h>

#include "zonecontroller.h"

static QMap<int, quint32> eightZones()
{
    return { {1,5}, {2,6}, {3,12}, {4,13}, {5,16}, {6,19}, {7,20}, {8,21} };
}

class FaultBackend : public MockBackend
{
public:
    bool failRead = false;
    int failWrites = 0;

    virtual bool getValues(Gpio::RequestHandle handle, const QList<quint32>& offsets, QList<Gpio::Value>& values) override
    {
        if(failRead == true) {
            setErrorText("injected read failure");
            return false;
        }
        return MockBackend::getValues(handle, offsets, values);
    }

    virtual bool setValues(Gpio::RequestHandle handle, const QList<quint32>& offsets, const QList<Gpio::Value>& values) override
    {
        if(failWrites > 0) {
            failWrites--;
            setErrorText("injected write failure");
            return false;
        }
        return MockBackend::setValues(handle, offsets, values);
    }
};

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
    void watchdogIsRunningEvenWhenBeginsInactiveWriteFails();
    void openZoneRefusedWhileFaultedThenAllowedAfterCleanReadBack();
    void mismatchTripAlsoLatchesAndBlocksOpen();
    void allOffFailingInsideTripLeavesLatchUntilRetriedCloseLands();
    void closeRetriesAfterTwoConsecutiveWriteFailuresThenSucceeds();
    void closeRetryLandsWithinTwoRetryIntervals();
    void allOffRetryLandsWithinOneRetryInterval();
    void latchStaysSetAcrossRepeatedDirtyTicksAndClearsOnACleanOne();
    void watchdogTripsWhenTheOpenZonesLineDropsOut();
    void watchdogTripsWhenAForeignLineEnergisesWithAZoneOpen();
    void maxZoneSecondsNeverRisesPastTheHardCeiling();
    void durationOfExactlyOneSecondIsAccepted();
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
    QCOMPARE(controller.secondsRemaining(), 119);
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

void TestZoneController::watchdogIsRunningEvenWhenBeginsInactiveWriteFails()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    controller.setWatchdogInterval(TimeSpan::fromMilliseconds(100));
    backend.setFailNextSetValues(true);
    QVERIFY(controller.begin() == false);

    // request() already reset every line to Inactive regardless of the failed write.
    backend.setLineValue(5, Gpio::Value::Active);

    QSignalSpy spy(&controller, &ZoneController::watchdogTripped);
    QVERIFY(spy.wait(2000));
}

void TestZoneController::openZoneRefusedWhileFaultedThenAllowedAfterCleanReadBack()
{
    FaultBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(2, 811));

    backend.failRead = true;
    QSignalSpy trippedSpy(&controller, &ZoneController::watchdogTripped);
    controller.triggerWatchdogForTest();

    QCOMPARE(trippedSpy.count(), 1);
    QVERIFY(controller.isFaulted());
    QCOMPARE(controller.openZoneNumber(), 0);

    QVERIFY(controller.openZone(6, 823) == false);
    QVERIFY(controller.errorText().contains("fault", Qt::CaseInsensitive));
    QCOMPARE(backend.lineValue(19), Gpio::Value::Inactive);

    backend.failRead = false;
    controller.triggerWatchdogForTest();

    QVERIFY(controller.isFaulted() == false);
    QVERIFY(controller.openZone(6, 823));
    QCOMPARE(controller.openZoneNumber(), 6);
}

void TestZoneController::mismatchTripAlsoLatchesAndBlocksOpen()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(4, 829));

    backend.setLineValue(13, Gpio::Value::Inactive);
    backend.setLineValue(20, Gpio::Value::Active);

    QSignalSpy trippedSpy(&controller, &ZoneController::watchdogTripped);
    controller.triggerWatchdogForTest();

    QCOMPARE(trippedSpy.count(), 1);
    QVERIFY(controller.isFaulted());
    QVERIFY(controller.openZone(1, 601) == false);
    QCOMPARE(backend.lineValue(5), Gpio::Value::Inactive);
}

void TestZoneController::allOffFailingInsideTripLeavesLatchUntilRetriedCloseLands()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(5, 839));
    controller.disableCloseTimerForTest();

    backend.setLineValue(16, Gpio::Value::Inactive);
    backend.setLineValue(6, Gpio::Value::Active);
    backend.setFailNextSetValues(true);

    QSignalSpy trippedSpy(&controller, &ZoneController::watchdogTripped);
    controller.triggerWatchdogForTest();

    QCOMPARE(trippedSpy.count(), 0);
    QVERIFY(controller.isFaulted());
    QCOMPARE(controller.openZoneNumber(), 5);
    QVERIFY(controller.closeTimerActiveForTest());
    QVERIFY(controller.openZone(1, 601) == false);

    backend.setLineValue(16, Gpio::Value::Active);
    backend.setLineValue(6, Gpio::Value::Inactive);
    controller.triggerWatchdogForTest();

    QVERIFY(controller.isFaulted());
    QVERIFY(controller.openZone(1, 601) == false);

    controller.expireCloseTimerForTest();

    QCOMPARE(controller.openZoneNumber(), 0);
    QCOMPARE(backend.lineValue(16), Gpio::Value::Inactive);
    QCOMPARE(backend.lineValue(6), Gpio::Value::Inactive);
    QVERIFY(controller.isFaulted());

    controller.triggerWatchdogForTest();
    QVERIFY(controller.isFaulted() == false);
}

void TestZoneController::closeRetriesAfterTwoConsecutiveWriteFailuresThenSucceeds()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(3, 811));

    QSignalSpy closedSpy(&controller, &ZoneController::zoneClosed);

    backend.setFailNextSetValues(true);
    controller.expireCloseTimerForTest();
    QVERIFY(controller.closeTimerActiveForTest());
    QCOMPARE(controller.openZoneNumber(), 3);
    QCOMPARE(backend.lineValue(12), Gpio::Value::Active);

    backend.setFailNextSetValues(true);
    controller.expireCloseTimerForTest();
    QVERIFY(controller.closeTimerActiveForTest());
    QCOMPARE(controller.openZoneNumber(), 3);

    controller.expireCloseTimerForTest();
    QVERIFY(controller.closeTimerActiveForTest() == false);
    QCOMPARE(controller.openZoneNumber(), 0);
    QCOMPARE(backend.lineValue(12), Gpio::Value::Inactive);
    QCOMPARE(closedSpy.count(), 1);
    QCOMPARE(closedSpy.first().at(0).toInt(), 3);
}

void TestZoneController::closeRetryLandsWithinTwoRetryIntervals()
{
    FaultBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    controller.setWatchdogInterval(TimeSpan::fromSeconds(60));
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(3, 811));

    backend.failWrites = 2;
    controller.expireCloseTimerForTest();
    QCOMPARE(controller.openZoneNumber(), 3);

    QTRY_COMPARE_WITH_TIMEOUT(controller.openZoneNumber(), 0, 2500);
    QCOMPARE(backend.lineValue(12), Gpio::Value::Inactive);
}

void TestZoneController::allOffRetryLandsWithinOneRetryInterval()
{
    FaultBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    controller.setWatchdogInterval(TimeSpan::fromSeconds(60));
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(5, 839));

    backend.setLineValue(16, Gpio::Value::Inactive);
    backend.setLineValue(6, Gpio::Value::Active);
    backend.failWrites = 1;
    controller.triggerWatchdogForTest();
    QCOMPARE(controller.openZoneNumber(), 5);

    QTRY_COMPARE_WITH_TIMEOUT(controller.openZoneNumber(), 0, 1300);
}

void TestZoneController::latchStaysSetAcrossRepeatedDirtyTicksAndClearsOnACleanOne()
{
    FaultBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    controller.setWatchdogInterval(TimeSpan::fromSeconds(60));
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(2, 811));

    backend.failRead = true;
    controller.triggerWatchdogForTest();
    QVERIFY(controller.isFaulted());
    QCOMPARE(controller.openZoneNumber(), 0);
    backend.failRead = false;

    for(int i = 0; i < 3; i++) {
        backend.setLineValue(5, Gpio::Value::Active);
        controller.triggerWatchdogForTest();
        QVERIFY(controller.isFaulted());
        QVERIFY(controller.openZone(6, 823) == false);
    }

    controller.triggerWatchdogForTest();

    QVERIFY(controller.isFaulted() == false);
    QVERIFY(controller.openZone(6, 823));
    QCOMPARE(controller.openZoneNumber(), 6);
}

void TestZoneController::watchdogTripsWhenTheOpenZonesLineDropsOut()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(4, 829));

    backend.setLineValue(13, Gpio::Value::Inactive);

    QSignalSpy trippedSpy(&controller, &ZoneController::watchdogTripped);
    controller.triggerWatchdogForTest();

    QCOMPARE(trippedSpy.count(), 1);
    QCOMPARE(trippedSpy.first().at(0).toInt(), 4);
}

void TestZoneController::watchdogTripsWhenAForeignLineEnergisesWithAZoneOpen()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(4, 829));

    backend.setLineValue(20, Gpio::Value::Active);

    QSignalSpy trippedSpy(&controller, &ZoneController::watchdogTripped);
    controller.triggerWatchdogForTest();

    QCOMPARE(trippedSpy.count(), 1);
    QCOMPARE(trippedSpy.first().at(0).toInt(), 4);
}

void TestZoneController::maxZoneSecondsNeverRisesPastTheHardCeiling()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 120);
    controller.setMaxZoneSeconds(600);

    QCOMPARE(controller.maxZoneSeconds(), 120);
}

void TestZoneController::durationOfExactlyOneSecondIsAccepted()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(4, 1));
    QCOMPARE(controller.openZoneNumber(), 4);
}

QTEST_MAIN(TestZoneController)
#include "tst_zonecontroller.moc"
