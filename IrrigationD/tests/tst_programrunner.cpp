#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <Kanoop/pi/mockbackend.h>

#include "database/irrigationdatasource.h"
#include "model/zone.h"
#include "programrunner.h"
#include "zonecontroller.h"

class RefusingBackend : public MockBackend
{
public:
    quint32 refusedOffset = 0;
    bool refuse = false;

    virtual bool setValues(Gpio::RequestHandle handle, const QList<quint32>& offsets, const QList<Gpio::Value>& values) override
    {
        for(int i = 0; refuse == true && i < offsets.count(); i++) {
            if(offsets.at(i) == refusedOffset && values.at(i) == Gpio::Value::Active) {
                setErrorText("injected refusal");
                return false;
            }
        }
        return MockBackend::setValues(handle, offsets, values);
    }
};

static QMap<int, quint32> eightZones()
{
    return { {1,5}, {2,6}, {3,12}, {4,13}, {5,16}, {6,19}, {7,20}, {8,21} };
}

struct StepSpec
{
    QList<int> zoneNumbers;
    int durationSeconds = 0;
    int sequence = 0;
};

class Bench
{
public:
    Bench() : source(dir.filePath("irrigation.db")) {}

    // Renumbers every zone id past its manifold number so a step that stored a zone
    // number where it should store an id names no zone at all.
    bool begin()
    {
        if(source.open() == false || backend.openChipByLabel("mock") == false) {
            return false;
        }
        bool ok = false;
        source.rawQuery("UPDATE zones SET id = id + 1000", &ok);
        return ok;
    }

    int zoneIdFor(int zoneNumber)
    {
        for(const Zone& zone : source.allZones()) {
            if(zone.number == zoneNumber) {
                return zone.id;
            }
        }
        return 0;
    }

    bool setZoneEnabled(int zoneNumber, bool enabled)
    {
        bool ok = false;
        source.rawQuery(QString("UPDATE zones SET enabled = %1 WHERE number = %2").arg(enabled ? 1 : 0).arg(zoneNumber), &ok);
        return ok;
    }

    // A zero sequence means the step's list position plus one.
    int buildProgram(const QList<StepSpec>& steps, const QString& name = "Test")
    {
        Program program;
        program.name = name;
        if(source.insertProgram(program) == false) {
            return 0;
        }

        for(int i = 0; i < steps.count(); i++) {
            ProgramStep step;
            step.programId = program.id;
            step.sequence = steps.at(i).sequence != 0 ? steps.at(i).sequence : i + 1;
            step.durationSeconds = steps.at(i).durationSeconds;
            for(int zoneNumber : steps.at(i).zoneNumbers) {
                step.zoneIds.append(zoneIdFor(zoneNumber));
            }
            if(source.insertProgramStep(step) == false) {
                return 0;
            }
        }
        return program.id;
    }

    QTemporaryDir dir;
    IrrigationDataSource source;
    RefusingBackend backend;
};

class TestProgramRunner : public QObject
{
    Q_OBJECT
private slots:
    void walksStepsInSequenceOrder();
    void opensEveryZoneOfAStepTogether();
    void aStepLargerThanTheCapRunsInWaves();
    void waitingZonesOpenInAscendingZoneNumber();
    void stepCompletesOnlyWhenEveryZoneHasOpenedAndClosed();
    void takesOverAManuallyOpenZoneAndResetsItsDeadline();
    void perZoneStopCountsAsFinishingTheZonesShare();
    void aManualZoneClosingFreesASlotForAWaitingZone();
    void allOffAbortsTheProgramAndOpensNothing();
    void watchdogTripAbortsTheProgram();
    void disabledZoneIsSkipped();
    void aStepOfOnlyDisabledZonesCompletesImmediately();
    void abortClosesOnlyTheProgramsZones();
    void firstOpenFailureReturnsFalseAndAborts();
    void openFailureMidProgramAborts();
    void fillSlotsOpensWaitingZonesAfterTheCapRises();
    void startingWhileRunningIsRejected();
    void aProgramWithNoStepsFinishesImmediately();
    void aZoneInTwoStepsWatersInEach();
    void reportsItsNameStepAndWaitingZones();
    void abortWhileIdleIsANoOp();
    void aStepZoneWhoseClosePendingWaitsRatherThanAborting();
    void aStepZoneRefusedByOpenZoneAbortsWhileAnotherZoneIsClosing();
};

void TestProgramRunner::walksStepsInSequenceOrder()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    // Insertion order (2, 7, 5), sequence order (7, 5, 2) and zone-number order (2, 5, 7)
    // are pairwise different.
    const int programId = bench.buildProgram({
        StepSpec{ { 2 }, 613, 3 },
        StepSpec{ { 7 }, 617, 1 },
        StepSpec{ { 5 }, 619, 2 },
    });
    QVERIFY(programId > 0);

    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy opened(&controller, &ZoneController::zoneOpened);
    QSignalSpy finished(&runner, &ProgramRunner::programFinished);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 7 }));
    QCOMPARE(opened.at(0).at(1).toInt(), 617);

    controller.expireCloseTimerForTest(7);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 5 }));
    QCOMPARE(opened.at(1).at(1).toInt(), 619);

    controller.expireCloseTimerForTest(5);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 2 }));
    QCOMPARE(opened.at(2).at(1).toInt(), 613);

    controller.expireCloseTimerForTest(2);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.first().at(0).toInt(), programId);
    QVERIFY(runner.isRunning() == false);
}

void TestProgramRunner::opensEveryZoneOfAStepTogether()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    controller.setMaxConcurrentZones(3);

    const int programId = bench.buildProgram({ StepSpec{ { 4, 1, 6 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy opened(&controller, &ZoneController::zoneOpened);

    QVERIFY(runner.startProgram(programId));

    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1, 4, 6 }));
    QCOMPARE(opened.count(), 3);
    for(const QList<QVariant>& arguments : opened) {
        QCOMPARE(arguments.at(1).toInt(), 600);
    }
    QVERIFY(runner.waitingZones().isEmpty());
}

void TestProgramRunner::aStepLargerThanTheCapRunsInWaves()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = bench.buildProgram({ StepSpec{ { 1, 2, 3 }, 300 } });
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy opened(&controller, &ZoneController::zoneOpened);
    QSignalSpy finished(&runner, &ProgramRunner::programFinished);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1, 2 }));
    QCOMPARE(runner.waitingZones(), QList<int>({ 3 }));

    controller.expireCloseTimerForTest(1);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 2, 3 }));
    QCOMPARE(opened.last().at(0).toInt(), 3);
    QCOMPARE(opened.last().at(1).toInt(), 300);
    QVERIFY(runner.waitingZones().isEmpty());

    controller.expireCloseTimerForTest(2);
    QCOMPARE(finished.count(), 0);
    controller.expireCloseTimerForTest(3);
    QCOMPARE(finished.count(), 1);
}

void TestProgramRunner::waitingZonesOpenInAscendingZoneNumber()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    controller.setMaxConcurrentZones(1);

    const int programId = bench.buildProgram({ StepSpec{ { 7, 2, 5 }, 120 } });
    ProgramRunner runner(&controller, &bench.source);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 2 }));
    QCOMPARE(runner.waitingZones(), QList<int>({ 5, 7 }));

    controller.expireCloseTimerForTest(2);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 5 }));

    controller.expireCloseTimerForTest(5);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 7 }));
}

void TestProgramRunner::stepCompletesOnlyWhenEveryZoneHasOpenedAndClosed()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = bench.buildProgram({ StepSpec{ { 1, 2 }, 300 }, StepSpec{ { 3 }, 200 } });
    ProgramRunner runner(&controller, &bench.source);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(runner.stepNumber(), 1);

    controller.expireCloseTimerForTest(1);
    QCOMPARE(runner.stepNumber(), 1);
    QVERIFY(controller.isOpen(3) == false);

    controller.expireCloseTimerForTest(2);
    QCOMPARE(runner.stepNumber(), 2);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 3 }));
}

void TestProgramRunner::takesOverAManuallyOpenZoneAndResetsItsDeadline()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(3, 30));

    const int programId = bench.buildProgram({ StepSpec{ { 3 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy opened(&controller, &ZoneController::zoneOpened);

    QVERIFY(runner.startProgram(programId));

    QCOMPARE(opened.count(), 1);
    QCOMPARE(opened.first().at(0).toInt(), 3);
    QCOMPARE(opened.first().at(1).toInt(), 600);
    QVERIFY(controller.secondsRemaining(3) > 30);
    QVERIFY(runner.ownsZone(3));
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 3 }));

    // The takeover took no slot: a second zone still fits under the cap of 2.
    QVERIFY(controller.openZone(8, 60));
    QVERIFY(runner.ownsZone(8) == false);
}

void TestProgramRunner::perZoneStopCountsAsFinishingTheZonesShare()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = bench.buildProgram({ StepSpec{ { 1, 2 }, 600 }, StepSpec{ { 4 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);

    QVERIFY(runner.startProgram(programId));

    QVERIFY(controller.closeZone(1));
    QCOMPARE(runner.stepNumber(), 1);
    QVERIFY(runner.isRunning());

    QVERIFY(controller.closeZone(2));
    QCOMPARE(runner.stepNumber(), 2);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 4 }));
    QCOMPARE(aborted.count(), 0);
}

void TestProgramRunner::aManualZoneClosingFreesASlotForAWaitingZone()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(7, 600));
    QVERIFY(controller.openZone(8, 600));

    const int programId = bench.buildProgram({ StepSpec{ { 1 }, 300 } });
    ProgramRunner runner(&controller, &bench.source);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 7, 8 }));
    QCOMPARE(runner.waitingZones(), QList<int>({ 1 }));

    QVERIFY(controller.closeZone(8));
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1, 7 }));
    QVERIFY(runner.ownsZone(1));
    QVERIFY(runner.waitingZones().isEmpty());
}

void TestProgramRunner::allOffAbortsTheProgramAndOpensNothing()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(8, 600));
    const int programId = bench.buildProgram({ StepSpec{ { 1, 2 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1, 8 }));
    QCOMPARE(runner.waitingZones(), QList<int>({ 2 }));

    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);
    QSignalSpy opened(&controller, &ZoneController::zoneOpened);
    QVERIFY(controller.allOff());

    QCOMPARE(aborted.count(), 1);
    QCOMPARE(opened.count(), 0);
    QVERIFY(controller.openZoneNumbers().isEmpty());
    QVERIFY(runner.isRunning() == false);
}

void TestProgramRunner::watchdogTripAbortsTheProgram()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = bench.buildProgram({ StepSpec{ { 1 }, 600 }, StepSpec{ { 2 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);
    QVERIFY(runner.startProgram(programId));

    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);
    QSignalSpy finished(&runner, &ProgramRunner::programFinished);
    bench.backend.setLineValue(13, Gpio::Value::Active);
    controller.triggerWatchdogForTest();

    QCOMPARE(aborted.count(), 1);
    QCOMPARE(finished.count(), 0);
    QVERIFY(runner.isRunning() == false);
    QVERIFY(controller.openZoneNumbers().isEmpty());
}

void TestProgramRunner::disabledZoneIsSkipped()
{
    Bench bench;
    QVERIFY(bench.begin());
    QVERIFY(bench.setZoneEnabled(3, false));
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = bench.buildProgram({ StepSpec{ { 1, 3 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy finished(&runner, &ProgramRunner::programFinished);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1 }));
    QVERIFY(runner.waitingZones().isEmpty());

    controller.expireCloseTimerForTest(1);
    QCOMPARE(finished.count(), 1);
}

void TestProgramRunner::aStepOfOnlyDisabledZonesCompletesImmediately()
{
    Bench bench;
    QVERIFY(bench.begin());
    QVERIFY(bench.setZoneEnabled(3, false));
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = bench.buildProgram({ StepSpec{ { 3 }, 600 }, StepSpec{ { 5 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(runner.stepNumber(), 2);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 5 }));
}

void TestProgramRunner::abortClosesOnlyTheProgramsZones()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(8, 600));
    const int programId = bench.buildProgram({ StepSpec{ { 1 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);

    QVERIFY(runner.startProgram(programId));
    runner.abort();

    QCOMPARE(aborted.count(), 1);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 8 }));
}

void TestProgramRunner::firstOpenFailureReturnsFalseAndAborts()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = bench.buildProgram({ StepSpec{ { 1 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);

    bench.backend.refusedOffset = 5;
    bench.backend.refuse = true;
    QVERIFY(runner.startProgram(programId) == false);

    QCOMPARE(aborted.count(), 1);
    QVERIFY(runner.isRunning() == false);
    QVERIFY(controller.openZoneNumbers().isEmpty());
}

void TestProgramRunner::openFailureMidProgramAborts()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = bench.buildProgram({ StepSpec{ { 1 }, 600 }, StepSpec{ { 2 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);

    QVERIFY(runner.startProgram(programId));
    bench.backend.refusedOffset = 6;
    bench.backend.refuse = true;
    controller.expireCloseTimerForTest(1);

    QCOMPARE(aborted.count(), 1);
    QVERIFY(runner.isRunning() == false);
    QVERIFY(controller.openZoneNumbers().isEmpty());
}

void TestProgramRunner::fillSlotsOpensWaitingZonesAfterTheCapRises()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());
    controller.setMaxConcurrentZones(1);

    const int programId = bench.buildProgram({ StepSpec{ { 1, 2 }, 600 } });
    ProgramRunner runner(&controller, &bench.source);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(runner.waitingZones(), QList<int>({ 2 }));

    controller.setMaxConcurrentZones(2);
    runner.fillSlots();

    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1, 2 }));
    QVERIFY(runner.waitingZones().isEmpty());
}

void TestProgramRunner::startingWhileRunningIsRejected()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int first = bench.buildProgram({ StepSpec{ { 1 }, 600 } }, "First");
    const int second = bench.buildProgram({ StepSpec{ { 2 }, 600 } }, "Second");
    ProgramRunner runner(&controller, &bench.source);

    QVERIFY(runner.startProgram(first));
    QVERIFY(runner.startProgram(second) == false);
    QCOMPARE(runner.runningProgramId(), first);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1 }));
}

void TestProgramRunner::aProgramWithNoStepsFinishesImmediately()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = bench.buildProgram({});
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy finished(&runner, &ProgramRunner::programFinished);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(finished.count(), 1);
    QVERIFY(runner.isRunning() == false);
}

void TestProgramRunner::aZoneInTwoStepsWatersInEach()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = bench.buildProgram({ StepSpec{ { 2 }, 600 }, StepSpec{ { 2 }, 300 } });
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy opened(&controller, &ZoneController::zoneOpened);

    QVERIFY(runner.startProgram(programId));
    controller.expireCloseTimerForTest(2);

    QCOMPARE(opened.count(), 2);
    QCOMPARE(opened.at(1).at(0).toInt(), 2);
    QCOMPARE(opened.at(1).at(1).toInt(), 300);
}

void TestProgramRunner::reportsItsNameStepAndWaitingZones()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(8, 600));
    const int programId = bench.buildProgram({ StepSpec{ { 5, 6, 7 }, 600 }, StepSpec{ { 1 }, 600 } }, "Morning Drip");
    ProgramRunner runner(&controller, &bench.source);

    QCOMPARE(runner.stepNumber(), 0);
    QCOMPARE(runner.stepCount(), 0);

    QVERIFY(runner.startProgram(programId));

    QCOMPARE(runner.runningProgramName(), QString("Morning Drip"));
    QCOMPARE(runner.stepNumber(), 1);
    QCOMPARE(runner.stepCount(), 2);
    QCOMPARE(runner.waitingZones(), QList<int>({ 6, 7 }));
    QVERIFY(runner.ownsZone(5));
    QVERIFY(runner.ownsZone(8) == false);
}

void TestProgramRunner::abortWhileIdleIsANoOp()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);
    runner.abort();
    QCOMPARE(aborted.count(), 0);
}

void TestProgramRunner::aStepZoneWhoseClosePendingWaitsRatherThanAborting()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(1, 600));
    bench.backend.setFailNextSetValues(true);
    QVERIFY(controller.closeZone(1) == false);
    QVERIFY(controller.isClosing(1));

    const int programId = bench.buildProgram({ StepSpec{ { 1, 2 }, 300 } });
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);

    QVERIFY(runner.startProgram(programId));

    QCOMPARE(runner.waitingZones(), QList<int>({ 1 }));
    QVERIFY(runner.ownsZone(2));
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1, 2 }));
    QCOMPARE(aborted.count(), 0);
    QVERIFY(runner.isRunning());
}

void TestProgramRunner::aStepZoneRefusedByOpenZoneAbortsWhileAnotherZoneIsClosing()
{
    Bench bench;
    QVERIFY(bench.begin());
    ZoneController controller(&bench.backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(1, 600));
    bench.backend.setFailNextSetValues(true);
    QVERIFY(controller.closeZone(1) == false);
    QVERIFY(controller.isClosing(1));

    const int programId = bench.buildProgram({ StepSpec{ { 1, 2 }, 300 } });
    ProgramRunner runner(&controller, &bench.source);
    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);

    bench.backend.refusedOffset = 6;
    bench.backend.refuse = true;
    QVERIFY(runner.startProgram(programId) == false);

    QCOMPARE(aborted.count(), 1);
    QVERIFY(runner.isRunning() == false);
    QCOMPARE(controller.openZoneNumbers(), QList<int>({ 1 }));
    QVERIFY(controller.isClosing(1));
    QCOMPARE(bench.backend.lineValue(6), Gpio::Value::Inactive);
}

QTEST_MAIN(TestProgramRunner)
#include "tst_programrunner.moc"
