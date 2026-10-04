#include <QTest>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimeZone>

#include <Kanoop/pi/mockbackend.h>

#include "database/irrigationdatasource.h"
#include "iclock.h"
#include "model/program.h"
#include "model/programstep.h"
#include "model/zone.h"
#include "panelcontroller.h"
#include "panelhost.h"
#include "programqueue.h"
#include "programrunner.h"
#include "stopbutton.h"
#include "zonecontroller.h"

namespace
{
const quint32 STOP_OFFSET = 25;
const QDateTime DueAt = QDateTime(QDate(2026, 10, 3), QTime(13, 0), QTimeZone::UTC);

QMap<int, quint32> eightZones()
{
    return { {1,5}, {2,6}, {3,12}, {4,13}, {5,16}, {6,19}, {7,20}, {8,21} };
}
}

/** Backend whose read can be told to fail, to trip the controller's watchdog fault latch on demand. */
class FaultBackend : public MockBackend
{
public:
    bool failRead = false;

    virtual bool getValues(Gpio::RequestHandle handle, const QList<quint32>& offsets, QList<Gpio::Value>& values) override
    {
        if(failRead == true) {
            setErrorText("injected read failure");
            return false;
        }
        return MockBackend::getValues(handle, offsets, values);
    }
};

/** The daemon's components on the in-memory backend, wired as threadStarted() wires them. */
class Rig
{
public:
    Rig() :
        source(dir.filePath("irrigation.db")),
        clock(DueAt),
        controller(&backend, eightZones(), true, 3600),
        stopButton(&backend, STOP_OFFSET),
        runner(&controller, &source),
        queue(&runner, &source, &clock),
        host(&controller, &runner, &queue, &source, &stopButton)
    {
    }

    bool begin()
    {
        return source.open() && backend.openChipByLabel("mock") && controller.begin() && stopButton.begin();
    }

    int buildProgram(const QString& name, const QList<int>& zoneNumbers, int seconds)
    {
        Program program;
        program.name = name;
        if(source.insertProgram(program) == false) {
            return 0;
        }

        ProgramStep step;
        step.programId = program.id;
        step.sequence = 1;
        step.durationSeconds = seconds;
        for(const Zone& zone : source.allZones()) {
            if(zoneNumbers.contains(zone.number) == true) {
                step.zoneIds.append(zone.id);
            }
        }
        return source.insertProgramStep(step) == true ? program.id : 0;
    }

    bool insertQueuedFiring(int programId, int startTimeId, const QDateTime& scheduledAtUtc)
    {
        FiredInstant instant;
        instant.programId = programId;
        instant.startTimeId = startTimeId;
        instant.scheduledAtUtc = scheduledAtUtc;
        instant.outcome = FiredInstant::Outcome::Queued;
        return source.recordFiring(instant);
    }

    QString outcomeFor(int programId, int startTimeId)
    {
        bool ok = false;
        QSqlQuery query = source.rawQuery(
            QString("SELECT outcome FROM fired_instants WHERE program_id = %1 AND start_time_id = %2")
                .arg(programId).arg(startTimeId), &ok);
        return ok == true && query.next() == true ? query.value(0).toString() : QString();
    }

    bool setZoneEnabled(int zoneNumber, bool enabled)
    {
        for(Zone zone : source.allZones()) {
            if(zone.number == zoneNumber) {
                zone.enabled = enabled;
                return source.updateZone(zone);
            }
        }
        return false;
    }

    // A press pulls the line low; activeLow inversion reports it as a logical Rising edge.
    void holdStop() { backend.simulateEdge(STOP_OFFSET, Gpio::Edge::Rising); }
    void releaseStop() { backend.simulateEdge(STOP_OFFSET, Gpio::Edge::Falling); }

    QTemporaryDir dir;
    IrrigationDataSource source;
    FaultBackend backend;
    TestClock clock;
    ZoneController controller;
    StopButton stopButton;
    ProgramRunner runner;
    ProgramQueue queue;
    PanelHost host;
};

class TestPanelHost : public QObject
{
    Q_OBJECT
private slots:
    void theSnapshotListsOpenAndEnabledZonesAscending();
    void theRunTimeIsClampedByTheZoneCeiling();
    void aPanelZoneOpensForTheRunTime();
    void openPanelZoneRefusesOnAFaultBeforeStopMasterZoneOrCap();
    void refusalsComeInTheManualRunOrderAndOpenNothing();
    void aRefusedAdvanceLeavesTheReplacedZoneOpen();
    void anAdvanceSwapsZonesAtACapOfOne();
    void anAdvanceBesideAnotherZoneKeepsThatZone();
    void aWaitingProgramZoneTakesTheFreedSlot();
    void openPanelZoneAtAFullCapWhereTheCloseFailsRefusesAndOpensNothing();
    void openPanelZoneWithAFreeSlotWhereTheOpenFailsLeavesTheReplacedZoneOpen();
    void closePanelZoneClosesOnlyThatZone();
    void takeOverClearsAProgramAppZonesAndTheQueue();
};

void TestPanelHost::theSnapshotListsOpenAndEnabledZonesAscending()
{
    Rig rig;
    QVERIFY(rig.begin());
    QVERIFY(rig.setZoneEnabled(3, false));
    QVERIFY(rig.controller.openZone(5, 300));
    QVERIFY(rig.controller.openZone(2, 120));

    const PanelSnapshot snapshot = rig.host.panelSnapshot();
    QCOMPARE(snapshot.openZones.count(), 2);
    QCOMPARE(snapshot.openZones.at(0).zone, 2);
    QVERIFY(snapshot.openZones.at(0).secondsRemaining > 115 && snapshot.openZones.at(0).secondsRemaining <= 120);
    QCOMPARE(snapshot.openZones.at(1).zone, 5);
    QCOMPARE(snapshot.enabledZones, QList<int>({ 1, 2, 4, 5, 6, 7, 8 }));
    QCOMPARE(snapshot.runMinutes, PanelController::DefaultRunMinutes);
    QVERIFY(snapshot.stopHeld == false);
    QVERIFY(snapshot.faulted == false);
    QVERIFY(snapshot.masterEnabled);

    rig.holdStop();
    QVERIFY(rig.source.setSettingValue("master_enabled", "0"));
    const PanelSnapshot later = rig.host.panelSnapshot();
    QVERIFY(later.stopHeld);
    QVERIFY(later.masterEnabled == false);
}

void TestPanelHost::theRunTimeIsClampedByTheZoneCeiling()
{
    Rig rig;
    QVERIFY(rig.begin());

    rig.host.setRunMinutes(10);
    QCOMPARE(rig.host.runSeconds(), 600);
    rig.controller.setMaxZoneSeconds(290);
    QCOMPARE(rig.host.runSeconds(), 290);
    QCOMPARE(rig.host.panelSnapshot().runMinutes, 5);
}

void TestPanelHost::aPanelZoneOpensForTheRunTime()
{
    Rig rig;
    QVERIFY(rig.begin());
    rig.host.setRunMinutes(7);

    QCOMPARE(rig.host.openPanelZone(3, 0), RunRequest::Refusal::None);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 3 }));
    QVERIFY(rig.controller.secondsRemaining(3) > 415 && rig.controller.secondsRemaining(3) <= 420);
}

void TestPanelHost::openPanelZoneRefusesOnAFaultBeforeStopMasterZoneOrCap()
{
    Rig rig;
    QVERIFY(rig.begin());

    rig.backend.failRead = true;
    rig.controller.triggerWatchdogForTest();
    QVERIFY(rig.controller.isFaulted());

    QCOMPARE(rig.host.openPanelZone(1, 0), RunRequest::Refusal::Failed);

    rig.holdStop();
    QCOMPARE(rig.host.openPanelZone(1, 0), RunRequest::Refusal::Failed);
}

void TestPanelHost::refusalsComeInTheManualRunOrderAndOpenNothing()
{
    Rig rig;
    QVERIFY(rig.begin());
    QVERIFY(rig.controller.openZone(7, 300));
    QVERIFY(rig.controller.openZone(8, 300));
    QVERIFY(rig.setZoneEnabled(3, false));
    QVERIFY(rig.source.setSettingValue("master_enabled", "0"));
    rig.holdStop();

    QCOMPARE(rig.host.openPanelZone(3, 0), RunRequest::Refusal::StopHeld);
    rig.releaseStop();
    QCOMPARE(rig.host.openPanelZone(3, 0), RunRequest::Refusal::MasterDisabled);
    QVERIFY(rig.source.setSettingValue("master_enabled", "1"));
    QCOMPARE(rig.host.openPanelZone(3, 0), RunRequest::Refusal::ZoneDisabled);
    QVERIFY(rig.setZoneEnabled(3, true));
    QCOMPARE(rig.host.openPanelZone(3, 0), RunRequest::Refusal::CapReached);

    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 7, 8 }));
}

void TestPanelHost::aRefusedAdvanceLeavesTheReplacedZoneOpen()
{
    Rig rig;
    QVERIFY(rig.begin());
    QCOMPARE(rig.host.openPanelZone(1, 0), RunRequest::Refusal::None);

    QVERIFY(rig.source.setSettingValue("master_enabled", "0"));
    QCOMPARE(rig.host.openPanelZone(2, 1), RunRequest::Refusal::MasterDisabled);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 1 }));

    QVERIFY(rig.source.setSettingValue("master_enabled", "1"));
    QVERIFY(rig.setZoneEnabled(2, false));
    QCOMPARE(rig.host.openPanelZone(2, 1), RunRequest::Refusal::ZoneDisabled);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 1 }));

    QVERIFY(rig.setZoneEnabled(2, true));
    rig.holdStop();
    QCOMPARE(rig.host.openPanelZone(2, 1), RunRequest::Refusal::StopHeld);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 1 }));
}

void TestPanelHost::anAdvanceSwapsZonesAtACapOfOne()
{
    Rig rig;
    QVERIFY(rig.begin());
    rig.controller.setMaxConcurrentZones(1);

    QCOMPARE(rig.host.openPanelZone(1, 0), RunRequest::Refusal::None);
    QCOMPARE(rig.host.openPanelZone(2, 1), RunRequest::Refusal::None);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 2 }));
    QVERIFY(rig.controller.secondsRemaining(2) > 595);
}

void TestPanelHost::anAdvanceBesideAnotherZoneKeepsThatZone()
{
    // Declared before rig: reverse destruction order keeps this alive through Rig's
    // destructor, which closes every open zone and would otherwise invoke the connected
    // lambda below on a dangling capture.
    QStringList order;
    Rig rig;
    QVERIFY(rig.begin());
    QVERIFY(rig.controller.openZone(5, 300));

    // Cap 2, full with the app's zone 5 and the panel's zone 1.
    QCOMPARE(rig.host.openPanelZone(1, 0), RunRequest::Refusal::None);
    QCOMPARE(rig.host.openPanelZone(2, 1), RunRequest::Refusal::None);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 2, 5 }));

    rig.controller.setMaxConcurrentZones(3);

    connect(&rig.controller, &ZoneController::zoneOpened, &rig.controller,
            [&order](int zoneNumber, int) { order.append(QString("open:%1").arg(zoneNumber)); });
    connect(&rig.controller, &ZoneController::zoneClosed, &rig.controller,
            [&order](int zoneNumber, ZoneController::CloseReason) { order.append(QString("close:%1").arg(zoneNumber)); });

    QCOMPARE(rig.host.openPanelZone(3, 2), RunRequest::Refusal::None);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 3, 5 }));
    QCOMPARE(order, QStringList({ "open:3", "close:2" }));
}

void TestPanelHost::aWaitingProgramZoneTakesTheFreedSlot()
{
    Rig rig;
    QVERIFY(rig.begin());
    QCOMPARE(rig.host.openPanelZone(1, 0), RunRequest::Refusal::None);

    const int program = rig.buildProgram("Pair", { 5, 6 }, 600);
    QVERIFY(program > 0);
    QCOMPARE(rig.queue.enqueueManual(program), RunRequest::Refusal::None);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 1, 5 }));
    QCOMPARE(rig.runner.waitingZones(), QList<int>({ 6 }));

    QCOMPARE(rig.host.openPanelZone(2, 1), RunRequest::Refusal::CapReached);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 5, 6 }));
}

void TestPanelHost::openPanelZoneAtAFullCapWhereTheCloseFailsRefusesAndOpensNothing()
{
    Rig rig;
    QVERIFY(rig.begin());
    rig.controller.setMaxConcurrentZones(1);
    QCOMPARE(rig.host.openPanelZone(1, 0), RunRequest::Refusal::None);

    rig.backend.setFailNextSetValues(true);
    QCOMPARE(rig.host.openPanelZone(2, 1), RunRequest::Refusal::Failed);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 1 }));
    QVERIFY(rig.controller.secondsRemaining(1) > 0);
}

void TestPanelHost::openPanelZoneWithAFreeSlotWhereTheOpenFailsLeavesTheReplacedZoneOpen()
{
    Rig rig;
    QVERIFY(rig.begin());
    QCOMPARE(rig.host.openPanelZone(1, 0), RunRequest::Refusal::None);

    rig.backend.setFailNextSetValues(true);
    QCOMPARE(rig.host.openPanelZone(2, 1), RunRequest::Refusal::Failed);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 1 }));
}

void TestPanelHost::closePanelZoneClosesOnlyThatZone()
{
    Rig rig;
    QVERIFY(rig.begin());
    QVERIFY(rig.controller.openZone(5, 300));
    QCOMPARE(rig.host.openPanelZone(1, 0), RunRequest::Refusal::None);

    rig.host.closePanelZone(1);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 5 }));
}

void TestPanelHost::takeOverClearsAProgramAppZonesAndTheQueue()
{
    Rig rig;
    QVERIFY(rig.begin());
    PanelController panel(&rig.host, &rig.clock);

    const int running = rig.buildProgram("Running", { 1 }, 600);
    const int waiting = rig.buildProgram("Waiting", { 2 }, 600);
    QVERIFY(rig.insertQueuedFiring(running, 31, DueAt));
    QVERIFY(rig.insertQueuedFiring(waiting, 32, DueAt));
    rig.queue.enqueueScheduled(running, 31, DueAt);
    rig.queue.enqueueScheduled(waiting, 32, DueAt);
    QVERIFY(rig.controller.openZone(7, 300));
    QVERIFY(rig.runner.isRunning());
    QCOMPARE(rig.queue.entries().count(), 1);
    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 1, 7 }));

    panel.onRunLineChanged(true);
    panel.onRunLineChanged(false);

    QVERIFY(rig.runner.isRunning() == false);
    QVERIFY(rig.queue.entries().isEmpty());
    QVERIFY(rig.controller.openZoneNumbers().isEmpty());
    QCOMPARE(rig.outcomeFor(waiting, 32), QString("dropped_stop"));
    QCOMPARE(rig.outcomeFor(running, 31), QString("ran"));
    QCOMPARE(panel.mode(), PanelController::Mode::Selecting);
    QCOMPARE(panel.selectedZone(), 1);

    for(int tick = 0; tick < 30; tick++) {
        rig.clock.advanceMsecs(100);
        panel.tick();
    }

    QCOMPARE(rig.controller.openZoneNumbers(), QList<int>({ 1 }));
    QCOMPARE(panel.panelZone(), 1);
    QVERIFY(rig.runner.isRunning() == false);
    QVERIFY(rig.queue.entries().isEmpty());
}

QTEST_MAIN(TestPanelHost)
#include "tst_panelhost.moc"
