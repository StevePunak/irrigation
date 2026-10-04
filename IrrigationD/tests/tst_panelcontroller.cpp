#include <QTest>
#include <QPair>
#include <QSignalSpy>
#include <QStringList>
#include <QTimeZone>

#include <Kanoop/log.h>
#include <Kanoop/logconsumer.h>
#include <Kanoop/logentry.h>

#include "iclock.h"
#include "panelcontroller.h"
#include "panelformat.h"

/** @brief An IPanelHost whose state the test sets by hand. Opens and closes change that state the way the daemon would. */
class FakeHost : public IPanelHost
{
public:
    /** @brief Constructs a host with every zone 1 through 8 enabled and a ten-minute panel run time. */
    FakeHost()
    {
        state.enabledZones = { 1, 2, 3, 4, 5, 6, 7, 8 };
        state.runMinutes = 10;
    }

    /** @brief Returns the state the test has set. */
    virtual PanelSnapshot panelSnapshot() override
    {
        return state;
    }

    /**
     * @brief Records the call and applies it to state, unless nextRefusal is pending.
     *
     * closeReplacingZoneBeforeNextRefusal, when set, closes @p replacingZone before
     * returning the pending refusal, the one exception IPanelHost documents.
     */
    virtual RunRequest::Refusal openPanelZone(int zoneNumber, int replacingZone) override
    {
        opens.append(QPair<int, int>(zoneNumber, replacingZone));
        if(nextRefusal != RunRequest::Refusal::None) {
            const RunRequest::Refusal refusal = nextRefusal;
            nextRefusal = RunRequest::Refusal::None;
            if(closeReplacingZoneBeforeNextRefusal == true && replacingZone > 0) {
                closeReplacingZoneBeforeNextRefusal = false;
                close(replacingZone);
            }
            return refusal;
        }
        if(replacingZone > 0) {
            close(replacingZone);
        }
        open(zoneNumber, runSeconds);
        return RunRequest::Refusal::None;
    }

    /** @brief Records the call and closes the zone. */
    virtual void closePanelZone(int zoneNumber) override
    {
        closes.append(zoneNumber);
        close(zoneNumber);
    }

    /** @brief Records the call and clears every open zone, as an API STOP does. */
    virtual void takeOverForPanel() override
    {
        takeOvers++;
        state.openZones.clear();
    }

    /** @brief Opens @p zoneNumber for @p seconds, keeping openZones ascending by zone number. */
    void open(int zoneNumber, int seconds)
    {
        close(zoneNumber);
        PanelSnapshot::OpenZone entry;
        entry.zone = zoneNumber;
        entry.secondsRemaining = seconds;
        int index = 0;
        while(index < state.openZones.count() && state.openZones.at(index).zone < zoneNumber) {
            index++;
        }
        state.openZones.insert(index, entry);
    }

    /** @brief Removes @p zoneNumber from openZones, if present. */
    void close(int zoneNumber)
    {
        for(int i = 0; i < state.openZones.count(); i++) {
            if(state.openZones.at(i).zone == zoneNumber) {
                state.openZones.removeAt(i);
                return;
            }
        }
    }

    /** @brief Returns whether @p zoneNumber is currently open. */
    bool isOpen(int zoneNumber) const { return state.secondsRemaining(zoneNumber) >= 0; }

    /** @brief Reads "2/0 3/2": each open request as zone/replacing, in order. */
    QString openCalls() const
    {
        QStringList calls;
        for(const QPair<int, int>& call : opens) {
            calls.append(QString("%1/%2").arg(call.first).arg(call.second));
        }
        return calls.join(' ');
    }

    PanelSnapshot state;                                                       ///< The controller state the test edits directly.
    RunRequest::Refusal nextRefusal = RunRequest::Refusal::None;               ///< Refusal the next openPanelZone() call returns, then clears.
    bool closeReplacingZoneBeforeNextRefusal = false;                         ///< Closes the replaced zone before the pending refusal fires, then clears.
    QList<QPair<int, int>> opens;                                              ///< Every openPanelZone() call, in order.
    QList<int> closes;                                                         ///< Every closePanelZone() call, in order.
    int takeOvers = 0;                                                         ///< Number of takeOverForPanel() calls.
    int runSeconds = 600;                                                      ///< Seconds a successful open() grants.
};

/** @brief Wires a FakeHost, a TestClock and a PanelController together, UTC-faced, at 2026-10-03 18:42:00. */
class Rig
{
public:
    /** @brief Constructs the rig with the clock at 2026-10-03 18:42:00 UTC and the panel reading that zone. */
    Rig() :
        clock(QDateTime(QDate(2026, 10, 3), QTime(18, 42, 0), QTimeZone::UTC)),
        panel(&host, &clock)
    {
        panel.setTimeZone(QTimeZone(QTimeZone::UTC));
    }

    /** @brief One press and its release, as the RUN line reports them. */
    void press()
    {
        panel.onRunLineChanged(true);
        panel.onRunLineChanged(false);
    }

    /** @brief Lets @p msecs pass in the daemon's 100 ms ticks. */
    void wait(qint64 msecs)
    {
        for(qint64 elapsed = 0; elapsed < msecs; elapsed += 100) {
            clock.advanceMsecs(qMin<qint64>(100, msecs - elapsed));
            panel.tick();
        }
    }

    /** @brief Presses until @p zoneNumber is on offer, then waits out the commit. */
    void startRunOn(int zoneNumber)
    {
        press();
        for(int presses = 0; presses < 16 && panel.selectedZone() != zoneNumber; presses++) {
            press();
        }
        wait(PanelController::CommitDelayMsecs);
    }

    FakeHost host;          ///< The host the panel reads and drives.
    TestClock clock;        ///< The clock the panel reads and times intervals on.
    PanelController panel;  ///< The controller under test.
};

/** @brief Collects every log line written while it lives. */
class LogCapture
{
public:
    /** @brief Starts capturing every log entry written from this point. */
    LogCapture()
    {
        QObject::connect(&_consumer, &LogConsumer::logEntry, [this](const Log::LogEntry& entry) {
            _entries.append(entry);
        });
        Log::addConsumer(&_consumer);
    }

    /** @brief Stops capturing. */
    ~LogCapture()
    {
        Log::removeConsumer(&_consumer);
    }

    /** @brief Returns how many captured lines contain @p fragment. */
    int count(const QString& fragment) const
    {
        int result = 0;
        for(const Log::LogEntry& entry : _entries) {
            if(entry.unformattedText().contains(fragment) == true) {
                result++;
            }
        }
        return result;
    }

private:
    LogConsumer _consumer;
    QList<Log::LogEntry> _entries;
};

class TestPanelController : public QObject
{
    Q_OBJECT
private slots:
    void idleShowsTheClockWithABlinkingColon();
    void idleClockReadsTheControllerZone();
    void aFrameIsEmittedOnlyWhenItChanges();
    void firstPressShowsTheFirstEnabledZoneBlinkingWithTheRunMinutes();
    void eachPressMovesToTheNextEnabledZoneAndWraps();
    void disabledZonesAreNeverOffered();
    void theSelectionCommitsThreeSecondsAfterTheLastPress();
    void noEnabledZoneShowsNoneForTwoSeconds();
    void aPanelRunCountsDownInMinutesRoundedUp();
    void aPressDuringARunOpensTheNextEnabledZoneAtOnce();
    void aPressOnTheLastEnabledZoneClosesItAndEndsTheRun();
    void theRunEndsWithItsZoneAndNeverMovesOn();
    void aPressJustAfterThePanelZoneClosedStartsASelection();
    void aProgramReopeningThePanelZoneBetweenTicksIsTreatedAsTheSameRun();
    void duringARunOnlyThePanelZoneShows();
    void aPressWithOtherZonesOpenTakesOverAndShowsTheSelection();
    void aPressWithNothingOpenTakesNothingOver();
    void stopHeldShowsStopAndIgnoresPresses();
    void aFaultShowsErrAndIgnoresPresses();
    void stopHeldAndAFaultWinOverEverythingElse();
    void aPressWhileStopHeldOrFaultedLatchesLowSoReleasingItIgnoresTheHeldRun();
    void masterOffShowsOffForTwoSecondsAndStartsNothing();
    void masterOffAtTheCommitShowsOff();
    void aRefusalAtTheCapShowsFullAndStartsNothing();
    void aRefusedAdvanceKeepsTheCurrentZoneRunning();
    void aCapReachedAfterTheZoneAlreadyClosedEndsTheRun();
    void aPressDuringAPanelRunWhileAMessageIsShowingClearsItAndAdvances();
    void aRefusedPressTakesNothingOver();
    void aTakeOverHappensOnlyAfterTheIdleChecksPass();
    void aZoneDisabledBeforeTheCommitOpensNothing();
    void stopHeldDuringASelectionCancelsIt();
    void cancelEndsASelectionAndARun();
    void cancelDuringAPanelRunThenAPressStartsAFreshSelection();
    void otherZonesAlternateEveryTwoSecondsInZoneOrder();
    void aClosedShownZoneMovesOnAtOnce();
    void aClosedShownZoneMovesToTheNextZoneAboveItNotTheLowest();
    void aWallClockStepDoesNotMoveTheCommit();
    void aLineHeldLowIsLoggedOnceAsStuck();
    void aLowWithoutASeenHighIsNoPress();
    void aLineLowAtStartupGivesNoPress();
    void aLineLowAtStartupIsReportedStuckAfterTenSeconds();
    void aSeenHighClearsTheStuckState();
};

void TestPanelController::idleShowsTheClockWithABlinkingColon()
{
    Rig rig;
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::clock(QTime(18, 42), true));
    rig.wait(500);
    QCOMPARE(rig.panel.frame(), PanelFormat::clock(QTime(18, 42), false));
    rig.wait(500);
    QCOMPARE(rig.panel.frame(), PanelFormat::clock(QTime(18, 42), true));
}

void TestPanelController::idleClockReadsTheControllerZone()
{
    Rig rig;
    rig.panel.setTimeZone(QTimeZone("America/Los_Angeles"));
    rig.clock.setNowUtc(QDateTime(QDate(2026, 10, 3), QTime(13, 5, 0), QTimeZone::UTC));
    rig.panel.tick();

    // 13:05 UTC is 06:05 PDT: " 6:05", leading digit blank.
    QCOMPARE(rig.panel.frame(), PanelFormat::clock(QTime(6, 5), true));
    QCOMPARE(static_cast<int>(rig.panel.frame().at(0)), 0);
}

void TestPanelController::aFrameIsEmittedOnlyWhenItChanges()
{
    Rig rig;
    QSignalSpy frames(&rig.panel, &PanelController::frameChanged);
    rig.panel.tick();
    rig.wait(400);
    QCOMPARE(frames.count(), 1);
    rig.wait(100);
    QCOMPARE(frames.count(), 2);
}

void TestPanelController::firstPressShowsTheFirstEnabledZoneBlinkingWithTheRunMinutes()
{
    Rig rig;
    rig.host.state.enabledZones = { 2, 3, 5 };
    rig.press();

    QCOMPARE(rig.panel.mode(), PanelController::Mode::Selecting);
    QCOMPARE(rig.panel.selectedZone(), 2);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 10, true));
    rig.wait(500);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 10, false));
    rig.wait(500);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 10, true));
    QVERIFY(rig.host.opens.isEmpty());
}

void TestPanelController::eachPressMovesToTheNextEnabledZoneAndWraps()
{
    Rig rig;
    rig.host.state.enabledZones = { 2, 3, 5 };
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 2);
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 3);
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 5);
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 2);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 10, true));
}

void TestPanelController::disabledZonesAreNeverOffered()
{
    Rig rig;
    rig.host.state.enabledZones = { 1, 4 };
    rig.host.state.runMinutes = 25;
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 1);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(1, 25, true));
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 4);
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 1);
}

void TestPanelController::theSelectionCommitsThreeSecondsAfterTheLastPress()
{
    Rig rig;
    rig.press();
    rig.wait(2000);
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 2);
    rig.wait(2900);
    QVERIFY(rig.host.opens.isEmpty());
    rig.wait(100);

    QCOMPARE(rig.host.openCalls(), QString("2/0"));
    QCOMPARE(rig.panel.mode(), PanelController::Mode::PanelRun);
    QCOMPARE(rig.panel.panelZone(), 2);
    QCOMPARE(rig.panel.selectedZone(), 0);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 10, true));
}

void TestPanelController::noEnabledZoneShowsNoneForTwoSeconds()
{
    Rig rig;
    rig.host.state.enabledZones.clear();
    rig.press();
    QCOMPARE(rig.panel.frame(), PanelFormat::noZones());
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    rig.wait(1900);
    QCOMPARE(rig.panel.frame(), PanelFormat::noZones());
    rig.wait(100);
    QCOMPARE(rig.panel.frame(), PanelFormat::clock(QTime(18, 42), true));
    QVERIFY(rig.host.opens.isEmpty());
}

void TestPanelController::aPanelRunCountsDownInMinutesRoundedUp()
{
    Rig rig;
    rig.startRunOn(3);
    QCOMPARE(rig.panel.panelZone(), 3);

    rig.host.open(3, 661);
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(3, 12, true));
    rig.host.open(3, 660);
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(3, 11, true));
    rig.host.open(3, 1);
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(3, 1, true));
}

void TestPanelController::aPressDuringARunOpensTheNextEnabledZoneAtOnce()
{
    Rig rig;
    rig.startRunOn(1);
    rig.press();

    QCOMPARE(rig.host.openCalls(), QString("1/0 2/1"));
    QCOMPARE(rig.panel.mode(), PanelController::Mode::PanelRun);
    QCOMPARE(rig.panel.panelZone(), 2);
    QVERIFY(rig.host.isOpen(1) == false);
    QVERIFY(rig.host.isOpen(2));
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 10, true));
}

void TestPanelController::aPressOnTheLastEnabledZoneClosesItAndEndsTheRun()
{
    Rig rig;
    rig.host.state.enabledZones = { 1, 2 };
    rig.startRunOn(1);
    rig.press();
    QCOMPARE(rig.panel.panelZone(), 2);
    rig.press();

    QCOMPARE(rig.host.closes, QList<int>({ 2 }));
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    QCOMPARE(rig.panel.panelZone(), 0);
    QVERIFY(rig.host.state.openZones.isEmpty());
    QCOMPARE(rig.host.openCalls(), QString("1/0 2/1"));
    QCOMPARE(rig.panel.frame(), PanelFormat::clock(QTime(18, 42), true));
}

void TestPanelController::theRunEndsWithItsZoneAndNeverMovesOn()
{
    Rig rig;
    rig.startRunOn(1);
    rig.host.close(1);
    rig.panel.tick();

    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    QCOMPARE(rig.panel.panelZone(), 0);
    rig.wait(5000);
    QCOMPARE(rig.host.openCalls(), QString("1/0"));
    QCOMPARE(rig.panel.frame(), PanelFormat::clock(QTime(18, 42), true));
}

void TestPanelController::aPressJustAfterThePanelZoneClosedStartsASelection()
{
    Rig rig;
    rig.startRunOn(1);
    rig.host.close(1);
    rig.press();

    QCOMPARE(rig.host.openCalls(), QString("1/0"));
    QVERIFY(rig.host.closes.isEmpty());
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Selecting);
    QCOMPARE(rig.panel.selectedZone(), 1);
}

void TestPanelController::aProgramReopeningThePanelZoneBetweenTicksIsTreatedAsTheSameRun()
{
    Rig rig;
    rig.startRunOn(2);
    rig.host.close(2);
    rig.host.open(2, 1800);
    rig.panel.tick();

    QCOMPARE(rig.panel.mode(), PanelController::Mode::PanelRun);
    QCOMPARE(rig.panel.panelZone(), 2);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 30, true));
}

void TestPanelController::duringARunOnlyThePanelZoneShows()
{
    Rig rig;
    rig.startRunOn(1);
    rig.host.open(5, 300);
    rig.wait(4000);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(1, 10, true));

    rig.host.close(1);
    rig.panel.tick();
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(5, 5, true));
}

void TestPanelController::aPressWithOtherZonesOpenTakesOverAndShowsTheSelection()
{
    Rig rig;
    rig.host.open(4, 300);
    rig.host.open(6, 120);
    rig.press();

    QCOMPARE(rig.host.takeOvers, 1);
    QVERIFY(rig.host.state.openZones.isEmpty());
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Selecting);
    QCOMPARE(rig.panel.selectedZone(), 1);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(1, 10, true));
    QVERIFY(rig.host.opens.isEmpty());
}

void TestPanelController::aPressWithNothingOpenTakesNothingOver()
{
    Rig rig;
    rig.press();
    QCOMPARE(rig.host.takeOvers, 0);
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Selecting);
}

void TestPanelController::stopHeldShowsStopAndIgnoresPresses()
{
    Rig rig;
    rig.host.state.stopHeld = true;
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::stopHeld());

    rig.press();
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    QCOMPARE(rig.host.takeOvers, 0);
    QVERIFY(rig.host.opens.isEmpty());
    QCOMPARE(rig.panel.frame(), PanelFormat::stopHeld());
}

void TestPanelController::aFaultShowsErrAndIgnoresPresses()
{
    Rig rig;
    rig.host.open(4, 300);
    rig.host.state.faulted = true;
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::fault());

    rig.press();
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    QCOMPARE(rig.host.takeOvers, 0);
    QVERIFY(rig.host.opens.isEmpty());
    QCOMPARE(rig.panel.frame(), PanelFormat::fault());
}

void TestPanelController::stopHeldAndAFaultWinOverEverythingElse()
{
    Rig rig;
    rig.startRunOn(1);
    rig.host.state.faulted = true;
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::fault());
    rig.host.state.stopHeld = true;
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::stopHeld());

    rig.host.state.stopHeld = false;
    rig.host.state.faulted = false;
    rig.host.close(1);
    rig.panel.tick();
    rig.host.state.enabledZones.clear();
    rig.press();
    QCOMPARE(rig.panel.frame(), PanelFormat::noZones());
    rig.host.state.stopHeld = true;
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::stopHeld());
}

void TestPanelController::aPressWhileStopHeldOrFaultedLatchesLowSoReleasingItIgnoresTheHeldRun()
{
    Rig stopCase;
    stopCase.host.state.stopHeld = true;
    stopCase.panel.onRunLineChanged(true);
    QCOMPARE(stopCase.panel.mode(), PanelController::Mode::Idle);
    stopCase.host.state.stopHeld = false;
    stopCase.wait(500);
    QCOMPARE(stopCase.panel.mode(), PanelController::Mode::Idle);
    QVERIFY(stopCase.host.opens.isEmpty());
    stopCase.panel.onRunLineChanged(false);
    stopCase.panel.onRunLineChanged(true);
    QCOMPARE(stopCase.panel.mode(), PanelController::Mode::Selecting);

    Rig faultCase;
    faultCase.host.state.faulted = true;
    faultCase.panel.onRunLineChanged(true);
    QCOMPARE(faultCase.panel.mode(), PanelController::Mode::Idle);
    faultCase.host.state.faulted = false;
    faultCase.wait(500);
    QCOMPARE(faultCase.panel.mode(), PanelController::Mode::Idle);
    QVERIFY(faultCase.host.opens.isEmpty());
    faultCase.panel.onRunLineChanged(false);
    faultCase.panel.onRunLineChanged(true);
    QCOMPARE(faultCase.panel.mode(), PanelController::Mode::Selecting);
}

void TestPanelController::masterOffShowsOffForTwoSecondsAndStartsNothing()
{
    Rig rig;
    rig.host.state.masterEnabled = false;
    rig.press();
    QCOMPARE(rig.panel.frame(), PanelFormat::masterOff());
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    QVERIFY(rig.host.opens.isEmpty());
    rig.wait(2000);
    QCOMPARE(rig.panel.frame(), PanelFormat::clock(QTime(18, 42), true));
}

void TestPanelController::masterOffAtTheCommitShowsOff()
{
    Rig rig;
    rig.press();
    rig.host.nextRefusal = RunRequest::Refusal::MasterDisabled;
    rig.wait(3000);
    QCOMPARE(rig.host.openCalls(), QString("1/0"));
    QCOMPARE(rig.panel.frame(), PanelFormat::masterOff());
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
}

void TestPanelController::aRefusalAtTheCapShowsFullAndStartsNothing()
{
    Rig rig;
    rig.press();
    rig.host.nextRefusal = RunRequest::Refusal::CapReached;
    rig.wait(3000);
    QCOMPARE(rig.panel.frame(), PanelFormat::full());
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    QCOMPARE(rig.panel.panelZone(), 0);
    rig.wait(2000);
    QCOMPARE(rig.panel.frame(), PanelFormat::clock(QTime(18, 42), true));
}

void TestPanelController::aRefusedAdvanceKeepsTheCurrentZoneRunning()
{
    Rig rig;
    rig.startRunOn(1);

    rig.host.nextRefusal = RunRequest::Refusal::CapReached;
    rig.press();
    QCOMPARE(rig.host.openCalls(), QString("1/0 2/1"));
    QCOMPARE(rig.panel.frame(), PanelFormat::full());
    QCOMPARE(rig.panel.mode(), PanelController::Mode::PanelRun);
    QCOMPARE(rig.panel.panelZone(), 1);
    QVERIFY(rig.host.isOpen(1));
    rig.wait(2000);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(1, 10, true));

    rig.host.nextRefusal = RunRequest::Refusal::MasterDisabled;
    rig.press();
    QCOMPARE(rig.panel.frame(), PanelFormat::masterOff());
    QCOMPARE(rig.panel.panelZone(), 1);
    QVERIFY(rig.host.isOpen(1));
}

void TestPanelController::aCapReachedAfterTheZoneAlreadyClosedEndsTheRun()
{
    Rig rig;
    rig.startRunOn(1);

    rig.host.nextRefusal = RunRequest::Refusal::CapReached;
    rig.host.closeReplacingZoneBeforeNextRefusal = true;
    rig.press();

    QCOMPARE(rig.host.openCalls(), QString("1/0 2/1"));
    QCOMPARE(rig.panel.frame(), PanelFormat::full());
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    QCOMPARE(rig.panel.panelZone(), 0);
    QVERIFY(rig.host.isOpen(1) == false);
}

void TestPanelController::aPressDuringAPanelRunWhileAMessageIsShowingClearsItAndAdvances()
{
    Rig rig;
    rig.startRunOn(1);

    rig.host.nextRefusal = RunRequest::Refusal::CapReached;
    rig.press();
    QCOMPARE(rig.panel.frame(), PanelFormat::full());

    rig.press();
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 10, true));
    QCOMPARE(rig.panel.panelZone(), 2);

    rig.host.nextRefusal = RunRequest::Refusal::MasterDisabled;
    rig.press();
    QCOMPARE(rig.panel.frame(), PanelFormat::masterOff());

    rig.press();
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(3, 10, true));
    QCOMPARE(rig.panel.panelZone(), 3);
}

void TestPanelController::aRefusedPressTakesNothingOver()
{
    Rig rig;
    rig.host.open(4, 300);

    rig.host.state.masterEnabled = false;
    rig.press();
    QCOMPARE(rig.panel.frame(), PanelFormat::masterOff());
    QCOMPARE(rig.host.takeOvers, 0);
    QVERIFY(rig.host.isOpen(4));

    rig.host.state.masterEnabled = true;
    rig.host.state.enabledZones.clear();
    rig.press();
    QCOMPARE(rig.panel.frame(), PanelFormat::noZones());
    QCOMPARE(rig.host.takeOvers, 0);
    QVERIFY(rig.host.isOpen(4));
}

void TestPanelController::aTakeOverHappensOnlyAfterTheIdleChecksPass()
{
    Rig rig;
    rig.host.open(4, 300);

    rig.host.state.masterEnabled = false;
    rig.press();
    QCOMPARE(rig.host.takeOvers, 0);

    rig.host.state.masterEnabled = true;
    rig.host.state.enabledZones.clear();
    rig.press();
    QCOMPARE(rig.host.takeOvers, 0);

    rig.host.state.enabledZones = { 1, 2, 3 };
    rig.press();
    QCOMPARE(rig.host.takeOvers, 1);
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Selecting);

    rig.press();
    QCOMPARE(rig.host.takeOvers, 1);
}

void TestPanelController::aZoneDisabledBeforeTheCommitOpensNothing()
{
    Rig rig;
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 1);
    rig.host.state.enabledZones = { 2, 3 };
    rig.wait(3000);
    QVERIFY(rig.host.opens.isEmpty());
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);

    rig.host.state.enabledZones = { 1, 2, 3 };
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 1);
    rig.host.state.enabledZones = { 3 };
    rig.press();
    QCOMPARE(rig.panel.selectedZone(), 3);
}

void TestPanelController::stopHeldDuringASelectionCancelsIt()
{
    Rig rig;
    rig.press();
    rig.host.state.stopHeld = true;
    rig.panel.tick();
    rig.host.state.stopHeld = false;
    rig.wait(3000);
    QVERIFY(rig.host.opens.isEmpty());
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
}

void TestPanelController::cancelEndsASelectionAndARun()
{
    Rig rig;
    rig.press();
    rig.panel.cancel();
    rig.wait(3000);
    QVERIFY(rig.host.opens.isEmpty());
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);

    rig.startRunOn(2);
    QCOMPARE(rig.panel.panelZone(), 2);
    rig.panel.cancel();
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    QCOMPARE(rig.panel.panelZone(), 0);
}

void TestPanelController::cancelDuringAPanelRunThenAPressStartsAFreshSelection()
{
    Rig rig;
    rig.startRunOn(3);
    QCOMPARE(rig.panel.panelZone(), 3);

    rig.panel.cancel();
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    QCOMPARE(rig.panel.panelZone(), 0);

    rig.press();
    QCOMPARE(rig.host.takeOvers, 1);
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Selecting);
    QCOMPARE(rig.panel.selectedZone(), 1);
}

void TestPanelController::otherZonesAlternateEveryTwoSecondsInZoneOrder()
{
    Rig rig;
    rig.host.open(6, 300);
    rig.host.open(2, 120);
    rig.host.open(4, 61);
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 2, true));
    rig.wait(1900);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 2, true));
    rig.wait(100);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(4, 2, true));
    rig.wait(2000);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(6, 5, true));
    rig.wait(2000);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 2, true));
}

void TestPanelController::aClosedShownZoneMovesOnAtOnce()
{
    Rig rig;
    rig.host.open(2, 120);
    rig.host.open(4, 61);
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 2, true));
    rig.host.close(2);
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(4, 2, true));
}

void TestPanelController::aClosedShownZoneMovesToTheNextZoneAboveItNotTheLowest()
{
    Rig rig;
    rig.host.open(2, 300);
    rig.host.open(4, 120);
    rig.host.open(6, 90);
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(2, 5, true));
    rig.wait(2000);
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(4, 2, true));

    rig.host.close(4);
    rig.panel.tick();
    QCOMPARE(rig.panel.frame(), PanelFormat::zoneMinutes(6, 2, true));
}

void TestPanelController::aWallClockStepDoesNotMoveTheCommit()
{
    Rig backward;
    backward.press();
    backward.clock.setNowUtc(backward.clock.nowUtc().addSecs(-3600));
    backward.wait(2900);
    QVERIFY(backward.host.opens.isEmpty());
    backward.wait(100);
    QCOMPARE(backward.host.openCalls(), QString("1/0"));

    Rig forward;
    forward.press();
    forward.clock.setNowUtc(forward.clock.nowUtc().addSecs(3600));
    forward.panel.tick();
    QVERIFY(forward.host.opens.isEmpty());
    forward.wait(3000);
    QCOMPARE(forward.host.openCalls(), QString("1/0"));
}

void TestPanelController::aLineHeldLowIsLoggedOnceAsStuck()
{
    LogCapture log;
    Rig rig;
    rig.panel.onRunLineChanged(true);
    rig.wait(10000);
    QVERIFY(rig.panel.isRunLineStuck() == false);
    rig.wait(100);
    QVERIFY(rig.panel.isRunLineStuck());
    QCOMPARE(log.count("stuck"), 1);
    rig.wait(20000);
    QCOMPARE(log.count("stuck"), 1);
}

void TestPanelController::aLowWithoutASeenHighIsNoPress()
{
    Rig rig;
    rig.panel.onRunLineChanged(true);
    QCOMPARE(rig.panel.selectedZone(), 1);
    rig.panel.onRunLineChanged(true);
    QCOMPARE(rig.panel.selectedZone(), 1);
    rig.panel.onRunLineChanged(false);
    rig.panel.onRunLineChanged(true);
    QCOMPARE(rig.panel.selectedZone(), 2);
}

void TestPanelController::aLineLowAtStartupGivesNoPress()
{
    Rig rig;
    rig.panel.setInitialRunLine(true);
    rig.panel.tick();
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    rig.panel.onRunLineChanged(true);
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Idle);
    rig.panel.onRunLineChanged(false);
    rig.panel.onRunLineChanged(true);
    QCOMPARE(rig.panel.mode(), PanelController::Mode::Selecting);
}

void TestPanelController::aLineLowAtStartupIsReportedStuckAfterTenSeconds()
{
    Rig rig;
    rig.panel.setInitialRunLine(true);
    rig.wait(10100);
    QVERIFY(rig.panel.isRunLineStuck());
    QVERIFY(rig.host.opens.isEmpty());
}

void TestPanelController::aSeenHighClearsTheStuckState()
{
    LogCapture log;
    Rig rig;
    rig.panel.onRunLineChanged(true);
    rig.wait(10100);
    QVERIFY(rig.panel.isRunLineStuck());

    rig.panel.onRunLineChanged(false);
    QVERIFY(rig.panel.isRunLineStuck() == false);
    rig.panel.onRunLineChanged(true);
    rig.wait(10100);
    QVERIFY(rig.panel.isRunLineStuck());
    QCOMPARE(log.count("stuck"), 2);
}

QTEST_MAIN(TestPanelController)
#include "tst_panelcontroller.moc"
