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
    void latchStaysSetAcrossRepeatedFailedReadsAndClearsOnAMatchingOne();
    void watchdogTripsWhenTheOpenZonesLineDropsOut();
    void watchdogTripsWhenAForeignLineEnergisesWithAZoneOpen();
    void maxZoneSecondsNeverRisesPastTheHardCeiling();
    void durationOfExactlyOneSecondIsAccepted();
    void aSecondZoneOpensAlongsideTheFirstInOneWrite();
    void openPastTheCapIsRefusedAndClosesNothing();
    void reopeningAnOpenZoneResetsItsDeadlineWithoutTakingASlot();
    void loweringTheCapClosesNothingAndRefusesNewOpens();
    void maxConcurrentZonesIsBoundedOneToEight();
    void eachZoneClosesOnItsOwnDeadline();
    void secondsRemainingIsPerZone();
    void closeZoneClosesOnlyThatZoneWithReasonStopped();
    void aRetriedCloseKeepsItsReason();
    void allOffReportsEveryOpenZoneWithReasonAllOff();
    void watchdogTripReportsEveryOpenZoneWithReasonWatchdog();
    void watchdogComparesTheWholeExpectedSet();
    void aZoneOpenedFromAClosedSlotNeverReenergisesTheClosedZone();
    void aZoneOpenedFromAnAllOffSlotLeavesEveryClosedZoneOff();
    void aZoneClosingOnItsOwnTimerNeverTripsAFastWatchdog();
    void reopeningAZoneWhoseCloseIsPendingIsRefusedButAllOffWins();
    void watchdogClosesADueZoneWhoseCloseTimerIsStillArmed();
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

    QCOMPARE(controller.openZoneNumbers().value(0), 0);
    for(quint32 offset : eightZones().values()) {
        QCOMPARE(backend.lineValue(offset), Gpio::Value::Inactive);
    }
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
    QCOMPARE(controller.secondsRemaining(1), 119);
}

void TestZoneController::durationBelowOneIsRejected()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(1, 0) == false);
    QVERIFY(controller.openZone(1, -5) == false);
    QCOMPARE(controller.openZoneNumbers().value(0), 0);
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
    QCOMPARE(controller.openZoneNumbers().value(0), 0);
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

    controller.disableCloseTimerForTest(3);

    QSignalSpy spy(&controller, &ZoneController::watchdogTripped);
    QVERIFY(spy.wait(5000));
    QCOMPARE(backend.lineValue(12), Gpio::Value::Inactive);
    QCOMPARE(controller.openZoneNumbers().value(0), 0);
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
    QCOMPARE(spy.first().at(0).value<QList<int>>(), QList<int>({ 7 }));
    QCOMPARE(controller.openZoneNumbers().value(0), 0);
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
    QCOMPARE(controller.openZoneNumbers().value(0), 8);
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
    QCOMPARE(controller.openZoneNumbers().value(0), 0);
    QVERIFY(controller.closeTimerActiveForTest(7) == false);
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
    QCOMPARE(controller.openZoneNumbers().value(0), 0);
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
    QCOMPARE(controller.openZoneNumbers().value(0), 0);

    QVERIFY(controller.openZone(6, 823) == false);
    QVERIFY(controller.errorText().contains("fault", Qt::CaseInsensitive));
    QCOMPARE(backend.lineValue(19), Gpio::Value::Inactive);

    backend.failRead = false;
    controller.triggerWatchdogForTest();

    QVERIFY(controller.isFaulted() == false);
    QVERIFY(controller.openZone(6, 823));
    QCOMPARE(controller.openZoneNumbers().value(0), 6);
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
    controller.disableCloseTimerForTest(5);

    backend.setLineValue(16, Gpio::Value::Inactive);
    backend.setLineValue(6, Gpio::Value::Active);
    backend.setFailNextSetValues(true);

    QSignalSpy trippedSpy(&controller, &ZoneController::watchdogTripped);
    controller.triggerWatchdogForTest();

    QCOMPARE(trippedSpy.count(), 0);
    QVERIFY(controller.isFaulted());
    QCOMPARE(controller.openZoneNumbers().value(0), 5);
    QVERIFY(controller.closeTimerActiveForTest(5));
    QVERIFY(controller.openZone(1, 601) == false);

    backend.setLineValue(16, Gpio::Value::Active);
    backend.setLineValue(6, Gpio::Value::Inactive);
    controller.triggerWatchdogForTest();

    QVERIFY(controller.isFaulted());
    QVERIFY(controller.openZone(1, 601) == false);

    controller.expireCloseTimerForTest(5);

    QCOMPARE(controller.openZoneNumbers().value(0), 0);
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
    controller.expireCloseTimerForTest(3);
    QVERIFY(controller.closeTimerActiveForTest(3));
    QCOMPARE(controller.openZoneNumbers().value(0), 3);
    QCOMPARE(backend.lineValue(12), Gpio::Value::Active);

    backend.setFailNextSetValues(true);
    controller.expireCloseTimerForTest(3);
    QVERIFY(controller.closeTimerActiveForTest(3));
    QCOMPARE(controller.openZoneNumbers().value(0), 3);

    controller.expireCloseTimerForTest(3);
    QVERIFY(controller.closeTimerActiveForTest(3) == false);
    QCOMPARE(controller.openZoneNumbers().value(0), 0);
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
    controller.expireCloseTimerForTest(3);
    QCOMPARE(controller.openZoneNumbers().value(0), 3);

    QTRY_COMPARE_WITH_TIMEOUT(controller.openZoneNumbers().value(0), 0, 2500);
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
    QCOMPARE(controller.openZoneNumbers().value(0), 5);

    QTRY_COMPARE_WITH_TIMEOUT(controller.openZoneNumbers().value(0), 0, 1300);
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
    QCOMPARE(controller.openZoneNumbers().value(0), 0);
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
    QCOMPARE(controller.openZoneNumbers().value(0), 6);
}

void TestZoneController::latchStaysSetAcrossRepeatedFailedReadsAndClearsOnAMatchingOne()
{
    FaultBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    controller.setWatchdogInterval(TimeSpan::fromSeconds(60));
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(8, 853));

    QSignalSpy trippedSpy(&controller, &ZoneController::watchdogTripped);
    backend.failRead = true;
    controller.triggerWatchdogForTest();
    QCOMPARE(trippedSpy.count(), 1);
    QCOMPARE(trippedSpy.at(0).at(0).value<QList<int>>(), QList<int>({ 8 }));
    QVERIFY(controller.isFaulted());
    QCOMPARE(controller.openZoneNumbers().value(0), 0);

    for(int i = 0; i < 3; i++) {
        controller.triggerWatchdogForTest();
        QVERIFY(controller.isFaulted());
        QCOMPARE(trippedSpy.count(), i + 2);
        QVERIFY(controller.openZone(7, 857) == false);
        QCOMPARE(backend.lineValue(20), Gpio::Value::Inactive);
    }

    backend.failRead = false;
    controller.triggerWatchdogForTest();

    QVERIFY(controller.isFaulted() == false);
    QCOMPARE(trippedSpy.count(), 4);
    QVERIFY(controller.openZone(7, 857));
    QCOMPARE(controller.openZoneNumbers().value(0), 7);
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
    QCOMPARE(trippedSpy.first().at(0).value<QList<int>>(), QList<int>({ 4 }));
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
    QCOMPARE(trippedSpy.first().at(0).value<QList<int>>(), QList<int>({ 4 }));
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
    QCOMPARE(controller.openZoneNumbers().value(0), 4);
}

void TestZoneController::aSecondZoneOpensAlongsideTheFirstInOneWrite()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(3, 60));
    backend.resetSetValuesCallCount();
    QVERIFY(controller.openZone(5, 60));

    QCOMPARE(backend.lineValue(12), Gpio::Value::Active);
    QCOMPARE(backend.lineValue(16), Gpio::Value::Active);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 3, 5 }));
    QCOMPARE(backend.setValuesCallCount(), 1);
}

void TestZoneController::openPastTheCapIsRefusedAndClosesNothing()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QCOMPARE(controller.maxConcurrentZones(), 2);

    QVERIFY(controller.openZone(1, 60));
    QVERIFY(controller.openZone(2, 60));

    QSignalSpy closed(&controller, &ZoneController::zoneClosed);
    QVERIFY(controller.hasSlotFor(3) == false);
    QVERIFY(controller.openZone(3, 60) == false);

    QVERIFY(controller.errorText().contains("2 zones already running"));
    QCOMPARE(backend.lineValue(12), Gpio::Value::Inactive);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1, 2 }));
    QCOMPARE(closed.count(), 0);
}

void TestZoneController::reopeningAnOpenZoneResetsItsDeadlineWithoutTakingASlot()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(1, 60));
    QVERIFY(controller.openZone(2, 60));

    QSignalSpy opened(&controller, &ZoneController::zoneOpened);
    backend.resetSetValuesCallCount();

    QVERIFY(controller.hasSlotFor(2));
    QVERIFY(controller.openZone(2, 900));

    QCOMPARE(opened.count(), 1);
    QCOMPARE(opened.first().at(0).toInt(), 2);
    QCOMPARE(opened.first().at(1).toInt(), 900);
    QVERIFY(controller.secondsRemaining(2) > 60);
    QVERIFY(controller.secondsRemaining(1) <= 60);
    QCOMPARE(backend.setValuesCallCount(), 0);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1, 2 }));
}

void TestZoneController::loweringTheCapClosesNothingAndRefusesNewOpens()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    controller.setWatchdogInterval(TimeSpan::fromSeconds(60));
    QVERIFY(controller.begin());
    controller.setMaxConcurrentZones(3);
    QVERIFY(controller.openZone(1, 600));
    QVERIFY(controller.openZone(2, 600));
    QVERIFY(controller.openZone(3, 600));

    QSignalSpy closed(&controller, &ZoneController::zoneClosed);
    QSignalSpy tripped(&controller, &ZoneController::watchdogTripped);

    controller.setMaxConcurrentZones(1);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1, 2, 3 }));
    QVERIFY(controller.openZone(4, 60) == false);

    // Three lines asserted against a cap of 1, but the zones opened under a cap of 3.
    controller.triggerWatchdogForTest();
    QCOMPARE(tripped.count(), 0);
    QVERIFY(controller.isFaulted() == false);
    QCOMPARE(closed.count(), 0);

    QVERIFY(controller.closeZone(1));
    QVERIFY(controller.closeZone(2));
    QVERIFY(controller.openZone(4, 60) == false);
    QVERIFY(controller.closeZone(3));
    QVERIFY(controller.openZone(4, 60));
}

void TestZoneController::maxConcurrentZonesIsBoundedOneToEight()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QCOMPARE(controller.maxConcurrentZones(), ZoneController::DefaultMaxConcurrentZones);

    controller.setMaxConcurrentZones(0);
    QCOMPARE(controller.maxConcurrentZones(), 1);

    controller.setMaxConcurrentZones(9);
    QCOMPARE(controller.maxConcurrentZones(), 8);
}

void TestZoneController::eachZoneClosesOnItsOwnDeadline()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    controller.setWatchdogInterval(TimeSpan::fromSeconds(60));
    QVERIFY(controller.begin());

    QSignalSpy closed(&controller, &ZoneController::zoneClosed);
    QVERIFY(controller.openZone(6, 1));
    QVERIFY(controller.openZone(2, 3));

    QVERIFY(closed.wait(2500));
    QCOMPARE(closed.count(), 1);
    QCOMPARE(closed.at(0).at(0).toInt(), 6);
    QCOMPARE(closed.at(0).at(1).value<ZoneController::CloseReason>(), ZoneController::CloseReason::Deadline);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 2 }));
    QCOMPARE(backend.lineValue(19), Gpio::Value::Inactive);
    QCOMPARE(backend.lineValue(6), Gpio::Value::Active);

    QVERIFY(closed.wait(3500));
    QCOMPARE(closed.at(1).at(0).toInt(), 2);
    QVERIFY(controller.openZoneNumbers().isEmpty());
}

void TestZoneController::secondsRemainingIsPerZone()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(1, 100));
    QVERIFY(controller.openZone(2, 500));

    QVERIFY(controller.secondsRemaining(1) >= 98 && controller.secondsRemaining(1) <= 100);
    QVERIFY(controller.secondsRemaining(2) >= 498 && controller.secondsRemaining(2) <= 500);
    QCOMPARE(controller.secondsRemaining(3), 0);
}

void TestZoneController::closeZoneClosesOnlyThatZoneWithReasonStopped()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(3, 600));
    QVERIFY(controller.openZone(5, 600));

    QSignalSpy closed(&controller, &ZoneController::zoneClosed);
    backend.resetSetValuesCallCount();

    QVERIFY(controller.closeZone(3));

    QCOMPARE(closed.count(), 1);
    QCOMPARE(closed.first().at(0).toInt(), 3);
    QCOMPARE(closed.first().at(1).value<ZoneController::CloseReason>(), ZoneController::CloseReason::Stopped);
    QCOMPARE(backend.lineValue(12), Gpio::Value::Inactive);
    QCOMPARE(backend.lineValue(16), Gpio::Value::Active);
    QCOMPARE(backend.setValuesCallCount(), 1);
    QVERIFY(controller.closeTimerActiveForTest(3) == false);
    QVERIFY(controller.closeTimerActiveForTest(5));

    QVERIFY(controller.closeZone(3));
    QCOMPARE(closed.count(), 1);
}

void TestZoneController::aRetriedCloseKeepsItsReason()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(3, 600));

    QSignalSpy closed(&controller, &ZoneController::zoneClosed);
    backend.setFailNextSetValues(true);
    QVERIFY(controller.closeZone(3) == false);
    QVERIFY(controller.closeTimerActiveForTest(3));
    QCOMPARE(closed.count(), 0);

    controller.expireCloseTimerForTest(3);

    QCOMPARE(closed.count(), 1);
    QCOMPARE(closed.first().at(1).value<ZoneController::CloseReason>(), ZoneController::CloseReason::Stopped);
    QCOMPARE(backend.lineValue(12), Gpio::Value::Inactive);
}

void TestZoneController::allOffReportsEveryOpenZoneWithReasonAllOff()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(4, 600));
    QVERIFY(controller.openZone(7, 600));

    QSignalSpy closed(&controller, &ZoneController::zoneClosed);
    backend.resetSetValuesCallCount();
    QVERIFY(controller.allOff());

    QCOMPARE(closed.count(), 2);
    QCOMPARE(closed.at(0).at(0).toInt(), 4);
    QCOMPARE(closed.at(1).at(0).toInt(), 7);
    QCOMPARE(closed.at(0).at(1).value<ZoneController::CloseReason>(), ZoneController::CloseReason::AllOff);
    QCOMPARE(closed.at(1).at(1).value<ZoneController::CloseReason>(), ZoneController::CloseReason::AllOff);
    QCOMPARE(backend.setValuesCallCount(), 1);
    for(quint32 offset : eightZones().values()) {
        QCOMPARE(backend.lineValue(offset), Gpio::Value::Inactive);
    }
}

void TestZoneController::watchdogTripReportsEveryOpenZoneWithReasonWatchdog()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(2, 600));
    QVERIFY(controller.openZone(8, 600));

    backend.setLineValue(13, Gpio::Value::Active);

    QSignalSpy closed(&controller, &ZoneController::zoneClosed);
    QSignalSpy tripped(&controller, &ZoneController::watchdogTripped);
    controller.triggerWatchdogForTest();

    QCOMPARE(tripped.count(), 1);
    QCOMPARE(tripped.first().at(0).value<QList<int>>(), QList<int>({ 2, 8 }));
    QCOMPARE(closed.count(), 2);
    QCOMPARE(closed.at(0).at(1).value<ZoneController::CloseReason>(), ZoneController::CloseReason::Watchdog);
    QCOMPARE(closed.at(1).at(1).value<ZoneController::CloseReason>(), ZoneController::CloseReason::Watchdog);
    QVERIFY(controller.isFaulted());
}

void TestZoneController::watchdogComparesTheWholeExpectedSet()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(2, 600));
    QVERIFY(controller.openZone(8, 600));

    QSignalSpy tripped(&controller, &ZoneController::watchdogTripped);
    controller.triggerWatchdogForTest();
    QCOMPARE(tripped.count(), 0);

    // Zone 2's line still reads active; only zone 8's has dropped out.
    backend.setLineValue(21, Gpio::Value::Inactive);
    controller.triggerWatchdogForTest();

    QCOMPARE(tripped.count(), 1);
    QCOMPARE(backend.lineValue(6), Gpio::Value::Inactive);
    QVERIFY(controller.openZoneNumbers().isEmpty());
}

void TestZoneController::aZoneOpenedFromAClosedSlotNeverReenergisesTheClosedZone()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(1, 600));
    QVERIFY(controller.openZone(2, 600));

    connect(&controller, &ZoneController::zoneClosed, &controller,
            [&controller](int zoneNumber, ZoneController::CloseReason reason)
    {
        if(zoneNumber == 1 && reason == ZoneController::CloseReason::Deadline) {
            QVERIFY(controller.openZone(3, 600));
        }
    });

    controller.expireCloseTimerForTest(1);

    QCOMPARE(backend.lineValue(5), Gpio::Value::Inactive);
    QCOMPARE(backend.lineValue(6), Gpio::Value::Active);
    QCOMPARE(backend.lineValue(12), Gpio::Value::Active);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 2, 3 }));
}

void TestZoneController::aZoneOpenedFromAnAllOffSlotLeavesEveryClosedZoneOff()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(1, 600));
    QVERIFY(controller.openZone(2, 600));

    bool reopened = false;
    connect(&controller, &ZoneController::zoneClosed, &controller,
            [&controller, &reopened](int zoneNumber, ZoneController::CloseReason reason)
    {
        if(zoneNumber == 1 && reason == ZoneController::CloseReason::AllOff) {
            reopened = controller.openZone(5, 600);
        }
    });

    QVERIFY(controller.allOff());

    QVERIFY(reopened);
    QCOMPARE(backend.lineValue(5), Gpio::Value::Inactive);
    QCOMPARE(backend.lineValue(6), Gpio::Value::Inactive);
    QCOMPARE(backend.lineValue(16), Gpio::Value::Active);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 5 }));
}

void TestZoneController::aZoneClosingOnItsOwnTimerNeverTripsAFastWatchdog()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    controller.setWatchdogInterval(TimeSpan::fromMilliseconds(50));
    QVERIFY(controller.begin());

    QSignalSpy closed(&controller, &ZoneController::zoneClosed);
    QSignalSpy tripped(&controller, &ZoneController::watchdogTripped);
    QVERIFY(controller.openZone(1, 1));
    QVERIFY(controller.openZone(2, 2));

    QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 2, 4000);
    QCOMPARE(tripped.count(), 0);
    QVERIFY(controller.isFaulted() == false);
    QCOMPARE(closed.at(0).at(1).value<ZoneController::CloseReason>(), ZoneController::CloseReason::Deadline);
    QCOMPARE(closed.at(1).at(1).value<ZoneController::CloseReason>(), ZoneController::CloseReason::Deadline);
}

void TestZoneController::reopeningAZoneWhoseCloseIsPendingIsRefusedButAllOffWins()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(3, 600));

    backend.setFailNextSetValues(true);
    QVERIFY(controller.closeZone(3) == false);
    QVERIFY(controller.isClosing(3));
    QCOMPARE(backend.lineValue(12), Gpio::Value::Active);

    backend.resetSetValuesCallCount();
    QVERIFY(controller.openZone(3, 600) == false);
    QVERIFY(controller.errorText().contains("pending a retry"));
    QCOMPARE(backend.setValuesCallCount(), 0);
    QCOMPARE(backend.lineValue(12), Gpio::Value::Active);

    QSignalSpy closed(&controller, &ZoneController::zoneClosed);
    QVERIFY(controller.allOff());

    QCOMPARE(closed.count(), 1);
    QCOMPARE(closed.first().at(0).toInt(), 3);
    QCOMPARE(closed.first().at(1).value<ZoneController::CloseReason>(), ZoneController::CloseReason::AllOff);
    QCOMPARE(backend.lineValue(12), Gpio::Value::Inactive);
    QVERIFY(controller.isClosing(3) == false);
    QVERIFY(controller.openZoneNumbers().isEmpty());
}

void TestZoneController::watchdogClosesADueZoneWhoseCloseTimerIsStillArmed()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    QVERIFY(controller.openZone(3, 1));
    QVERIFY(controller.closeTimerActiveForTest(3));

    // Advances real time past the deadline without spinning the event loop, so the
    // zone's own QTimer never gets a chance to fire: it is still armed when the
    // watchdog tick below finds the deadline already expired.
    QTest::qSleep(1100);
    QVERIFY(controller.closeTimerActiveForTest(3));

    QSignalSpy closed(&controller, &ZoneController::zoneClosed);
    QSignalSpy tripped(&controller, &ZoneController::watchdogTripped);
    controller.triggerWatchdogForTest();

    QCOMPARE(tripped.count(), 0);
    QCOMPARE(closed.count(), 1);
    QCOMPARE(closed.first().at(0).toInt(), 3);
    QCOMPARE(closed.first().at(1).value<ZoneController::CloseReason>(), ZoneController::CloseReason::Deadline);
    QCOMPARE(backend.lineValue(12), Gpio::Value::Inactive);
    QVERIFY(controller.isFaulted() == false);
}

QTEST_MAIN(TestZoneController)
#include "tst_zonecontroller.moc"
