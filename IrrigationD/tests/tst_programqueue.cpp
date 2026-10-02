#include <QTest>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimeZone>

#include <Kanoop/pi/mockbackend.h>

#include "database/irrigationdatasource.h"
#include "iclock.h"
#include "model/zone.h"
#include "programqueue.h"
#include "programrunner.h"
#include "zonecontroller.h"

static QMap<int, quint32> eightZones()
{
    return { {1,5}, {2,6}, {3,12}, {4,13}, {5,16}, {6,19}, {7,20}, {8,21} };
}

class Rig
{
public:
    Rig() :
        source(dir.filePath("irrigation.db")),
        clock(QDateTime(QDate(2026, 10, 1), QTime(13, 0), QTimeZone::UTC))
    {
    }

    bool begin()
    {
        return source.open() && backend.openChipByLabel("mock");
    }

    // One step of one zone for 600 s; the zone number doubles as the way a test finishes it.
    int buildProgram(const QString& name, int zoneNumber)
    {
        Program program;
        program.name = name;
        if(source.insertProgram(program) == false) {
            return 0;
        }

        ProgramStep step;
        step.programId = program.id;
        step.sequence = 1;
        step.durationSeconds = 600;
        for(const Zone& zone : source.allZones()) {
            if(zone.number == zoneNumber) {
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

    int firingCount()
    {
        bool ok = false;
        QSqlQuery query = source.rawQuery("SELECT COUNT(*) FROM fired_instants", &ok);
        return ok == true && query.next() == true ? query.value(0).toInt() : -1;
    }

    QTemporaryDir dir;
    IrrigationDataSource source;
    MockBackend backend;
    TestClock clock;
};

static const QDateTime DueAt = QDateTime(QDate(2026, 10, 1), QTime(13, 0), QTimeZone::UTC);

class TestProgramQueue : public QObject
{
    Q_OBJECT
private slots:
    void startsImmediatelyWhenIdleAndRecordsRan();
    void queuesBehindARunningProgramAndLeavesItQueued();
    void runsQueuedProgramsFirstInFirstOut();
    void aSecondDueTimeForAQueuedProgramIsSkippedDuplicate();
    void aRunningProgramMayHoldOneQueuedEntry();
    void manualRunStartsWhenIdleAndWritesNoFiring();
    void manualRunIsRefusedWhileRunningOrQueued();
    void rainDelaySetWhileWaitingSkipsAtDequeue();
    void masterDisabledWhileWaitingSkipsAtDequeue();
    void manualEntryIgnoresTheRainDelayAtDequeue();
    void dropAllRecordsDroppedStopForScheduledEntries();
    void dropAllBeforeAbortLeavesEveryValveClosed();
    void abortingWithAQueuedEntryStartsIt();
    void aDeletedQueueStartsNothingWhenTheRunnerAborts();
    void recordRestartDropsMarksOnlyQueuedFirings();
    void entriesReportHeadFirstWithQueueTimes();
};

void TestProgramQueue::startsImmediatelyWhenIdleAndRecordsRan()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int programId = rig.buildProgram("Front", 1);
    QVERIFY(rig.insertQueuedFiring(programId, 31, DueAt));

    queue.enqueueScheduled(programId, 31, DueAt);

    QCOMPARE(runner.runningProgramId(), programId);
    QCOMPARE(rig.outcomeFor(programId, 31), QString("ran"));
    QVERIFY(queue.entries().isEmpty());
}

void TestProgramQueue::queuesBehindARunningProgramAndLeavesItQueued()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int first = rig.buildProgram("Front", 1);
    const int second = rig.buildProgram("Back", 2);
    QVERIFY(rig.insertQueuedFiring(first, 31, DueAt));
    QVERIFY(rig.insertQueuedFiring(second, 32, DueAt));

    queue.enqueueScheduled(first, 31, DueAt);
    queue.enqueueScheduled(second, 32, DueAt);

    QCOMPARE(runner.runningProgramId(), first);
    QCOMPARE(queue.entries().count(), 1);
    QCOMPARE(queue.entries().first().programId, second);
    QCOMPARE(rig.outcomeFor(second, 32), QString("queued"));
    QVERIFY(controller.isOpen(2) == false);
}

void TestProgramQueue::runsQueuedProgramsFirstInFirstOut()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int a = rig.buildProgram("A", 1);
    const int b = rig.buildProgram("B", 2);
    const int c = rig.buildProgram("C", 3);
    for(int programId : { a, b, c }) {
        QVERIFY(rig.insertQueuedFiring(programId, programId + 100, DueAt));
        queue.enqueueScheduled(programId, programId + 100, DueAt);
    }

    QCOMPARE(runner.runningProgramId(), a);
    controller.expireCloseTimerForTest(1);
    QCOMPARE(runner.runningProgramId(), b);
    controller.expireCloseTimerForTest(2);
    QCOMPARE(runner.runningProgramId(), c);

    QCOMPARE(rig.outcomeFor(a, a + 100), QString("ran"));
    QCOMPARE(rig.outcomeFor(b, b + 100), QString("ran"));
    QCOMPARE(rig.outcomeFor(c, c + 100), QString("ran"));
}

void TestProgramQueue::aSecondDueTimeForAQueuedProgramIsSkippedDuplicate()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int running = rig.buildProgram("Running", 1);
    const int waiting = rig.buildProgram("Waiting", 2);
    QVERIFY(rig.insertQueuedFiring(running, 31, DueAt));
    QVERIFY(rig.insertQueuedFiring(waiting, 32, DueAt));
    QVERIFY(rig.insertQueuedFiring(waiting, 33, DueAt.addSecs(60)));

    queue.enqueueScheduled(running, 31, DueAt);
    queue.enqueueScheduled(waiting, 32, DueAt);
    queue.enqueueScheduled(waiting, 33, DueAt.addSecs(60));

    QCOMPARE(queue.entries().count(), 1);
    QCOMPARE(rig.outcomeFor(waiting, 32), QString("queued"));
    QCOMPARE(rig.outcomeFor(waiting, 33), QString("skipped_duplicate"));
}

void TestProgramQueue::aRunningProgramMayHoldOneQueuedEntry()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int programId = rig.buildProgram("Twice", 1);
    QVERIFY(rig.insertQueuedFiring(programId, 31, DueAt));
    QVERIFY(rig.insertQueuedFiring(programId, 32, DueAt.addSecs(300)));

    queue.enqueueScheduled(programId, 31, DueAt);
    queue.enqueueScheduled(programId, 32, DueAt.addSecs(300));

    QCOMPARE(queue.entries().count(), 1);
    QCOMPARE(rig.outcomeFor(programId, 32), QString("queued"));

    QSignalSpy started(&runner, &ProgramRunner::programStarted);
    controller.expireCloseTimerForTest(1);

    QCOMPARE(started.count(), 1);
    QCOMPARE(runner.runningProgramId(), programId);
    QCOMPARE(rig.outcomeFor(programId, 32), QString("ran"));
}

void TestProgramQueue::manualRunStartsWhenIdleAndWritesNoFiring()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int programId = rig.buildProgram("Manual", 4);

    QCOMPARE(queue.enqueueManual(programId), RunRequest::Refusal::None);
    QCOMPARE(runner.runningProgramId(), programId);
    QCOMPARE(rig.firingCount(), 0);
}

void TestProgramQueue::manualRunIsRefusedWhileRunningOrQueued()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int running = rig.buildProgram("Running", 1);
    const int waiting = rig.buildProgram("Waiting", 2);

    QCOMPARE(queue.enqueueManual(running), RunRequest::Refusal::None);
    QCOMPARE(queue.enqueueManual(running), RunRequest::Refusal::AlreadyQueued);
    QCOMPARE(queue.enqueueManual(waiting), RunRequest::Refusal::None);
    QCOMPARE(queue.enqueueManual(waiting), RunRequest::Refusal::AlreadyQueued);
    QCOMPARE(queue.entries().count(), 1);
}

void TestProgramQueue::rainDelaySetWhileWaitingSkipsAtDequeue()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int first = rig.buildProgram("First", 1);
    const int second = rig.buildProgram("Second", 2);
    QVERIFY(rig.insertQueuedFiring(first, 31, DueAt));
    QVERIFY(rig.insertQueuedFiring(second, 32, DueAt));
    queue.enqueueScheduled(first, 31, DueAt);
    queue.enqueueScheduled(second, 32, DueAt);

    QVERIFY(rig.source.setSettingValue("rain_delay_until", DueAt.addDays(1).toString(Qt::ISODate)));
    controller.expireCloseTimerForTest(1);

    QVERIFY(runner.isRunning() == false);
    QVERIFY(controller.openZoneNumbers().isEmpty());
    QCOMPARE(rig.outcomeFor(second, 32), QString("skipped_rain"));
    QVERIFY(queue.entries().isEmpty());
}

void TestProgramQueue::masterDisabledWhileWaitingSkipsAtDequeue()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int first = rig.buildProgram("First", 1);
    const int second = rig.buildProgram("Second", 2);
    QVERIFY(rig.insertQueuedFiring(first, 31, DueAt));
    QVERIFY(rig.insertQueuedFiring(second, 32, DueAt));
    queue.enqueueScheduled(first, 31, DueAt);
    queue.enqueueScheduled(second, 32, DueAt);

    QVERIFY(rig.source.setSettingValue("master_enabled", "0"));
    controller.expireCloseTimerForTest(1);

    QVERIFY(runner.isRunning() == false);
    QCOMPARE(rig.outcomeFor(second, 32), QString("skipped_disabled"));
}

void TestProgramQueue::manualEntryIgnoresTheRainDelayAtDequeue()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int first = rig.buildProgram("First", 1);
    const int second = rig.buildProgram("Second", 2);
    QCOMPARE(queue.enqueueManual(first), RunRequest::Refusal::None);
    QCOMPARE(queue.enqueueManual(second), RunRequest::Refusal::None);

    QVERIFY(rig.source.setSettingValue("rain_delay_until", DueAt.addDays(1).toString(Qt::ISODate)));
    controller.expireCloseTimerForTest(1);

    QCOMPARE(runner.runningProgramId(), second);
    QVERIFY(controller.isOpen(2));
}

void TestProgramQueue::dropAllRecordsDroppedStopForScheduledEntries()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int running = rig.buildProgram("Running", 1);
    const int scheduled = rig.buildProgram("Scheduled", 2);
    const int manual = rig.buildProgram("Manual", 3);
    QVERIFY(rig.insertQueuedFiring(running, 31, DueAt));
    QVERIFY(rig.insertQueuedFiring(scheduled, 32, DueAt));
    queue.enqueueScheduled(running, 31, DueAt);
    queue.enqueueScheduled(scheduled, 32, DueAt);
    QCOMPARE(queue.enqueueManual(manual), RunRequest::Refusal::None);
    QCOMPARE(queue.entries().count(), 2);

    queue.dropAll(FiredInstant::Outcome::DroppedStop);

    QVERIFY(queue.entries().isEmpty());
    QCOMPARE(rig.outcomeFor(scheduled, 32), QString("dropped_stop"));
    QCOMPARE(rig.outcomeFor(running, 31), QString("ran"));
    QCOMPARE(rig.firingCount(), 2);
}

void TestProgramQueue::dropAllBeforeAbortLeavesEveryValveClosed()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int running = rig.buildProgram("Running", 1);
    const int waiting = rig.buildProgram("Waiting", 2);
    QVERIFY(rig.insertQueuedFiring(running, 31, DueAt));
    QVERIFY(rig.insertQueuedFiring(waiting, 32, DueAt));
    queue.enqueueScheduled(running, 31, DueAt);
    queue.enqueueScheduled(waiting, 32, DueAt);

    queue.dropAll(FiredInstant::Outcome::DroppedStop);
    runner.abort();
    QVERIFY(controller.allOff());

    QVERIFY(runner.isRunning() == false);
    QVERIFY(controller.openZoneNumbers().isEmpty());
    QCOMPARE(rig.outcomeFor(waiting, 32), QString("dropped_stop"));
}

void TestProgramQueue::abortingWithAQueuedEntryStartsIt()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int running = rig.buildProgram("Running", 1);
    const int waiting = rig.buildProgram("Waiting", 2);
    QCOMPARE(queue.enqueueManual(running), RunRequest::Refusal::None);
    QCOMPARE(queue.enqueueManual(waiting), RunRequest::Refusal::None);

    runner.abort();

    QCOMPARE(runner.runningProgramId(), waiting);
    QVERIFY(controller.isOpen(2));
}

void TestProgramQueue::aDeletedQueueStartsNothingWhenTheRunnerAborts()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue* queue = new ProgramQueue(&runner, &rig.source, &rig.clock);

    const int running = rig.buildProgram("Running", 1);
    const int waiting = rig.buildProgram("Waiting", 2);
    QCOMPARE(queue->enqueueManual(running), RunRequest::Refusal::None);
    QCOMPARE(queue->enqueueManual(waiting), RunRequest::Refusal::None);

    delete queue;
    runner.abort();

    QVERIFY(runner.isRunning() == false);
    QVERIFY(controller.openZoneNumbers().isEmpty());
}

void TestProgramQueue::recordRestartDropsMarksOnlyQueuedFirings()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int programId = rig.buildProgram("Lost", 1);
    QVERIFY(rig.insertQueuedFiring(programId, 31, DueAt));
    QVERIFY(rig.insertQueuedFiring(programId, 32, DueAt.addSecs(60)));

    FiredInstant ran;
    ran.programId = programId;
    ran.startTimeId = 33;
    ran.scheduledAtUtc = DueAt.addSecs(-3600);
    ran.outcome = FiredInstant::Outcome::Ran;
    QVERIFY(rig.source.recordFiring(ran));

    QVERIFY(queue.recordRestartDrops());

    QCOMPARE(rig.outcomeFor(programId, 31), QString("dropped_restart"));
    QCOMPARE(rig.outcomeFor(programId, 32), QString("dropped_restart"));
    QCOMPARE(rig.outcomeFor(programId, 33), QString("ran"));
}

void TestProgramQueue::entriesReportHeadFirstWithQueueTimes()
{
    Rig rig;
    QVERIFY(rig.begin());
    ZoneController controller(&rig.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    ProgramRunner runner(&controller, &rig.source);
    ProgramQueue queue(&runner, &rig.source, &rig.clock);

    const int a = rig.buildProgram("A", 1);
    const int b = rig.buildProgram("B", 2);
    const int c = rig.buildProgram("C", 3);
    QCOMPARE(queue.enqueueManual(a), RunRequest::Refusal::None);
    rig.clock.advance(5);
    QCOMPARE(queue.enqueueManual(b), RunRequest::Refusal::None);
    rig.clock.advance(5);
    QCOMPARE(queue.enqueueManual(c), RunRequest::Refusal::None);

    const QList<ProgramQueue::Entry> entries = queue.entries();
    QCOMPARE(entries.count(), 2);
    QCOMPARE(entries.at(0).programId, b);
    QCOMPARE(entries.at(0).queuedAtUtc, DueAt.addSecs(5));
    QCOMPARE(entries.at(1).programId, c);
    QCOMPARE(entries.at(1).queuedAtUtc, DueAt.addSecs(10));
    QVERIFY(entries.at(0).isScheduled() == false);
}

QTEST_MAIN(TestProgramQueue)
#include "tst_programqueue.moc"
