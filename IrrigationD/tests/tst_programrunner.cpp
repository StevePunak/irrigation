#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <Kanoop/pi/mockbackend.h>

#include "database/irrigationdatasource.h"
#include "model/zone.h"
#include "programrunner.h"
#include "zonecontroller.h"

class ReadFailBackend : public MockBackend
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

static QMap<int, quint32> eightZones()
{
    return { {1,5}, {2,6}, {3,13}, {4,16}, {5,19}, {6,20}, {7,21}, {8,26} };
}

static quint32 zoneOffsetFor(int zoneNumber)
{
    return eightZones().value(zoneNumber);
}

struct ZoneStep
{
    int zoneNumber;
    int sequence;
    int durationSeconds;
};

// Renumbers every zone id past its manifold number.
static bool detachZoneIdsFromNumbers(IrrigationDataSource& source)
{
    bool ok = false;
    source.rawQuery("UPDATE zones SET id = id + 1000", &ok);
    return ok;
}

static Zone zoneByNumber(IrrigationDataSource& source, int number)
{
    for(const Zone& zone : source.allZones()) {
        if(zone.number == number) {
            return zone;
        }
    }
    return Zone();
}

// Renumbers the program id past any zone sequence or index value reached in these
// tests, so a payload transposition between programId and _index cannot hide behind
// a coincidence where both are small integers.
static int buildProgram(IrrigationDataSource& source, const QList<ZoneStep>& steps)
{
    Program program;
    program.name = "Test";
    if(source.insertProgram(program) == false) {
        return 0;
    }

    bool shifted = false;
    source.rawQuery(QString("UPDATE programs SET id = id + 500 WHERE id = %1").arg(program.id), &shifted);
    if(shifted == false) {
        return 0;
    }
    program.id += 500;

    const ZoneList zones = source.allZones();
    for(const ZoneStep& step : steps) {
        int zoneId = 0;
        for(const Zone& zone : zones) {
            if(zone.number == step.zoneNumber) {
                zoneId = zone.id;
                break;
            }
        }

        ProgramZone entry;
        entry.programId = program.id;
        entry.zoneId = zoneId;
        entry.sequence = step.sequence;
        entry.durationSeconds = step.durationSeconds;
        if(source.insertProgramZone(entry) == false) {
            return 0;
        }
    }

    return program.id;
}

static int buildProgram(IrrigationDataSource& source, const QList<int>& zoneNumbers, const QList<int>& durationsSeconds)
{
    QList<ZoneStep> steps;
    for(int i = 0; i < zoneNumbers.count(); i++) {
        steps.append(ZoneStep{ zoneNumbers.at(i), i + 1, durationsSeconds.at(i) });
    }
    return buildProgram(source, steps);
}

class TestProgramRunner : public QObject
{
    Q_OBJECT
private slots:
    void walksZonesInSequenceOrder();
    void finishesAfterTheLastZone();
    void abortStopsTheSequenceAndClosesTheValve();
    void startingWhileRunningIsRejected();
    void aProgramWithNoZonesFinishesImmediately();
    void openFailureMidSequenceAbortsTheProgram();
    void manuallyOpenedZoneDoesNotCascadeWhenDisplaced();
    void watchdogMismatchTripAbortsTheProgram();
    void watchdogReadFailureTripAbortsTheProgram();
    void watchdogTripWhoseCloseFailsAbortsOnlyAfterTheRetryLands();
    void watchdogTripOnTheFinalZoneEmitsProgramFinished();
    void disabledZoneIsSkippedByARunningProgram();
    void programOfOnlyDisabledZonesFinishesImmediately();
    void firstZoneOpenFailureReturnsFalseAndAbortsTheProgram();
    void aRepeatedZoneNumberWatersEachOccurrenceSeparately();
    void watchdogTrippedSignalAbortsARunningProgram();
    void abortWhileIdleIsANoOp();
};

void TestProgramRunner::walksZonesInSequenceOrder()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(detachZoneIdsFromNumbers(source));

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    // Insertion order (2,7,5), sequence order (7,5,2) and zone-number order (2,5,7)
    // are pairwise different.
    const int programId = buildProgram(source, {
        ZoneStep{ 2, 3, 613 },
        ZoneStep{ 7, 1, 617 },
        ZoneStep{ 5, 2, 619 },
    });
    QVERIFY(programId > 0);

    ProgramRunner runner(&controller, &source);
    QSignalSpy started(&runner, &ProgramRunner::programStarted);
    QSignalSpy opened(&controller, &ZoneController::zoneOpened);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(started.count(), 1);
    QCOMPARE(started.first().at(0).toInt(), programId);
    QCOMPARE(controller.openZoneNumber(), 7);
    QCOMPARE(opened.count(), 1);
    QCOMPARE(opened.at(0).at(0).toInt(), 7);
    QCOMPARE(opened.at(0).at(1).toInt(), 617);

    controller.expireCloseTimerForTest();
    QCOMPARE(controller.openZoneNumber(), 5);
    QCOMPARE(opened.at(1).at(0).toInt(), 5);
    QCOMPARE(opened.at(1).at(1).toInt(), 619);

    controller.expireCloseTimerForTest();
    QCOMPARE(controller.openZoneNumber(), 2);
    QCOMPARE(opened.at(2).at(0).toInt(), 2);
    QCOMPARE(opened.at(2).at(1).toInt(), 613);
}

void TestProgramRunner::finishesAfterTheLastZone()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(detachZoneIdsFromNumbers(source));

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = buildProgram(source, { 6, 3 }, { 607, 631 });
    QVERIFY(programId > 0);

    ProgramRunner runner(&controller, &source);
    QSignalSpy spy(&runner, &ProgramRunner::programFinished);

    QVERIFY(runner.startProgram(programId));
    controller.expireCloseTimerForTest();
    controller.expireCloseTimerForTest();

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toInt(), programId);
    QCOMPARE(runner.isRunning(), false);
    QCOMPARE(controller.openZoneNumber(), 0);
}

void TestProgramRunner::abortStopsTheSequenceAndClosesTheValve()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(detachZoneIdsFromNumbers(source));

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = buildProgram(source, { 4, 2, 6 }, { 641, 643, 647 });
    QVERIFY(programId > 0);

    ProgramRunner runner(&controller, &source);
    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumber(), 4);

    runner.abort();

    // abort()'s own reentrant zoneClosed must not open the next zone.
    QCOMPARE(runner.isRunning(), false);
    QCOMPARE(controller.openZoneNumber(), 0);
    for(quint32 offset : eightZones().values()) {
        QCOMPARE(backend.lineValue(offset), Gpio::Value::Inactive);
    }
}

void TestProgramRunner::startingWhileRunningIsRejected()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(detachZoneIdsFromNumbers(source));

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int first  = buildProgram(source, { 2 }, { 653 });
    const int second = buildProgram(source, { 7 }, { 659 });
    QVERIFY(first > 0);
    QVERIFY(second > 0);
    QVERIFY(first != second);

    ProgramRunner runner(&controller, &source);
    QVERIFY(runner.startProgram(first));
    QVERIFY(runner.startProgram(second) == false);

    QCOMPARE(runner.runningProgramId(), first);
    QCOMPARE(controller.openZoneNumber(), 2);
}

void TestProgramRunner::aProgramWithNoZonesFinishesImmediately()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(detachZoneIdsFromNumbers(source));

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = buildProgram(source, {}, {});
    QVERIFY(programId > 0);

    ProgramRunner runner(&controller, &source);
    QSignalSpy spy(&runner, &ProgramRunner::programFinished);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toInt(), programId);
    QCOMPARE(runner.isRunning(), false);
    QCOMPARE(controller.openZoneNumber(), 0);
}

void TestProgramRunner::openFailureMidSequenceAbortsTheProgram()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(detachZoneIdsFromNumbers(source));

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = buildProgram(source, { 2, 7, 4 }, { 661, 673, 677 });
    QVERIFY(programId > 0);

    // Arms the write failure once zone 2 reports closed, so the write that opens
    // zone 7 is the one that fails.
    QObject::connect(&controller, &ZoneController::zoneClosed, [&backend](int zoneNumber) {
        if(zoneNumber == 2) {
            backend.setFailNextSetValues(true);
        }
    });

    ProgramRunner runner(&controller, &source);
    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumber(), 2);

    controller.expireCloseTimerForTest();

    QCOMPARE(aborted.count(), 1);
    QCOMPARE(aborted.first().at(0).toInt(), programId);
    QCOMPARE(runner.isRunning(), false);
    QCOMPARE(runner.runningProgramId(), 0);
    QCOMPARE(controller.openZoneNumber(), 0);
    QCOMPARE(backend.lineValue(zoneOffsetFor(7)), Gpio::Value::Inactive);
    QCOMPARE(backend.lineValue(zoneOffsetFor(4)), Gpio::Value::Inactive);

    const int recoveryProgramId = buildProgram(source, { 4 }, { 683 });
    QVERIFY(recoveryProgramId > 0);
    QVERIFY(runner.startProgram(recoveryProgramId));
    QCOMPARE(controller.openZoneNumber(), 4);
}

void TestProgramRunner::manuallyOpenedZoneDoesNotCascadeWhenDisplaced()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(detachZoneIdsFromNumbers(source));

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(5, 691));

    const int programId = buildProgram(source, { 3, 6 }, { 701, 709 });
    QVERIFY(programId > 0);

    ProgramRunner runner(&controller, &source);
    QSignalSpy finished(&runner, &ProgramRunner::programFinished);
    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);

    QVERIFY(runner.startProgram(programId));

    QCOMPARE(controller.openZoneNumber(), 3);
    QCOMPARE(runner.isRunning(), true);
    QCOMPARE(finished.count(), 0);
    QCOMPARE(aborted.count(), 0);
}

void TestProgramRunner::watchdogMismatchTripAbortsTheProgram()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(detachZoneIdsFromNumbers(source));

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = buildProgram(source, { 1, 8, 5 }, { 719, 727, 733 });
    QVERIFY(programId > 0);

    ProgramRunner runner(&controller, &source);
    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);
    QSignalSpy finished(&runner, &ProgramRunner::programFinished);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumber(), 1);

    QSignalSpy opened(&controller, &ZoneController::zoneOpened);
    backend.setLineValue(zoneOffsetFor(1), Gpio::Value::Inactive);
    backend.setLineValue(zoneOffsetFor(8), Gpio::Value::Active);

    controller.triggerWatchdogForTest();

    QCOMPARE(opened.count(), 0);
    QCOMPARE(aborted.count(), 1);
    QCOMPARE(aborted.first().at(0).toInt(), programId);
    QCOMPARE(finished.count(), 0);
    QCOMPARE(runner.isRunning(), false);
    QCOMPARE(controller.openZoneNumber(), 0);
    QCOMPARE(backend.lineValue(zoneOffsetFor(1)), Gpio::Value::Inactive);
    QCOMPARE(backend.lineValue(zoneOffsetFor(8)), Gpio::Value::Inactive);
    QCOMPARE(backend.lineValue(zoneOffsetFor(5)), Gpio::Value::Inactive);
}

void TestProgramRunner::watchdogReadFailureTripAbortsTheProgram()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(detachZoneIdsFromNumbers(source));

    ReadFailBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = buildProgram(source, { 6, 2, 8 }, { 739, 743, 751 });
    QVERIFY(programId > 0);

    ProgramRunner runner(&controller, &source);
    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumber(), 6);

    QSignalSpy opened(&controller, &ZoneController::zoneOpened);
    backend.failRead = true;
    controller.triggerWatchdogForTest();

    QCOMPARE(opened.count(), 0);
    QCOMPARE(aborted.count(), 1);
    QCOMPARE(aborted.first().at(0).toInt(), programId);
    QCOMPARE(runner.isRunning(), false);
    QCOMPARE(controller.openZoneNumber(), 0);
    QCOMPARE(backend.lineValue(zoneOffsetFor(2)), Gpio::Value::Inactive);
    QCOMPARE(backend.lineValue(zoneOffsetFor(8)), Gpio::Value::Inactive);
}

void TestProgramRunner::watchdogTripWhoseCloseFailsAbortsOnlyAfterTheRetryLands()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(detachZoneIdsFromNumbers(source));

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = buildProgram(source, { 4, 7, 1 }, { 757, 761, 769 });
    QVERIFY(programId > 0);

    ProgramRunner runner(&controller, &source);
    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);
    QSignalSpy tripped(&controller, &ZoneController::watchdogTripped);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumber(), 4);

    QSignalSpy opened(&controller, &ZoneController::zoneOpened);
    backend.setLineValue(zoneOffsetFor(4), Gpio::Value::Inactive);
    backend.setLineValue(zoneOffsetFor(7), Gpio::Value::Active);
    backend.setFailNextSetValues(true);

    controller.triggerWatchdogForTest();

    QCOMPARE(tripped.count(), 0);
    QCOMPARE(aborted.count(), 0);
    QCOMPARE(runner.isRunning(), true);
    QCOMPARE(controller.openZoneNumber(), 4);
    QVERIFY(controller.closeTimerActiveForTest());

    controller.expireCloseTimerForTest();

    QCOMPARE(opened.count(), 0);
    QCOMPARE(aborted.count(), 1);
    QCOMPARE(aborted.first().at(0).toInt(), programId);
    QCOMPARE(runner.isRunning(), false);
    QCOMPARE(controller.openZoneNumber(), 0);
    QCOMPARE(backend.lineValue(zoneOffsetFor(1)), Gpio::Value::Inactive);
}

void TestProgramRunner::watchdogTripOnTheFinalZoneEmitsProgramFinished()
{
    {
        QTemporaryDir dir;
        IrrigationDataSource source(dir.filePath("irrigation.db"));
        QVERIFY(source.open());
        QVERIFY(detachZoneIdsFromNumbers(source));

        MockBackend backend;
        QVERIFY(backend.openChipByLabel("mock"));
        ZoneController controller(&backend, eightZones(), true, 3600);
        QVERIFY(controller.begin());

        const int programId = buildProgram(source, { 2, 5 }, { 773, 787 });
        QVERIFY(programId > 0);

        ProgramRunner runner(&controller, &source);
        QSignalSpy finished(&runner, &ProgramRunner::programFinished);
        QSignalSpy aborted(&runner, &ProgramRunner::programAborted);

        QVERIFY(runner.startProgram(programId));
        controller.expireCloseTimerForTest();
        QCOMPARE(controller.openZoneNumber(), 5);

        backend.setLineValue(zoneOffsetFor(5), Gpio::Value::Inactive);
        backend.setLineValue(zoneOffsetFor(2), Gpio::Value::Active);
        controller.triggerWatchdogForTest();

        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).toInt(), programId);
        QCOMPARE(aborted.count(), 0);
        QCOMPARE(runner.isRunning(), false);
        QCOMPARE(controller.openZoneNumber(), 0);
    }
    {
        QTemporaryDir dir;
        IrrigationDataSource source(dir.filePath("irrigation.db"));
        QVERIFY(source.open());
        QVERIFY(detachZoneIdsFromNumbers(source));

        Zone six = zoneByNumber(source, 6);
        six.enabled = false;
        QVERIFY(source.updateZone(six));

        MockBackend backend;
        QVERIFY(backend.openChipByLabel("mock"));
        ZoneController controller(&backend, eightZones(), true, 3600);
        QVERIFY(controller.begin());

        // Zone 6 follows zone 3 in the sequence but is disabled; zone 3 is the last
        // zone that will ever actually water.
        const int programId = buildProgram(source, { 3, 6 }, { 797, 809 });
        QVERIFY(programId > 0);

        ProgramRunner runner(&controller, &source);
        QSignalSpy finished(&runner, &ProgramRunner::programFinished);
        QSignalSpy aborted(&runner, &ProgramRunner::programAborted);

        QVERIFY(runner.startProgram(programId));
        QCOMPARE(controller.openZoneNumber(), 3);

        backend.setLineValue(zoneOffsetFor(3), Gpio::Value::Inactive);
        backend.setLineValue(zoneOffsetFor(8), Gpio::Value::Active);
        controller.triggerWatchdogForTest();

        QCOMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(0).toInt(), programId);
        QCOMPARE(aborted.count(), 0);
        QCOMPARE(runner.isRunning(), false);
        QCOMPARE(controller.openZoneNumber(), 0);
        QCOMPARE(backend.lineValue(zoneOffsetFor(6)), Gpio::Value::Inactive);
    }
}

void TestProgramRunner::disabledZoneIsSkippedByARunningProgram()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(detachZoneIdsFromNumbers(source));

    Zone four = zoneByNumber(source, 4);
    four.enabled = false;
    QVERIFY(source.updateZone(four));

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = buildProgram(source, { 2, 4, 7 }, { 821, 823, 827 });
    QVERIFY(programId > 0);

    ProgramRunner runner(&controller, &source);
    QSignalSpy finished(&runner, &ProgramRunner::programFinished);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumber(), 2);

    controller.expireCloseTimerForTest();

    QCOMPARE(controller.openZoneNumber(), 7);
    QCOMPARE(backend.lineValue(zoneOffsetFor(4)), Gpio::Value::Inactive);
    QCOMPARE(finished.count(), 0);
    QCOMPARE(runner.isRunning(), true);
}

void TestProgramRunner::programOfOnlyDisabledZonesFinishesImmediately()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(detachZoneIdsFromNumbers(source));

    Zone one = zoneByNumber(source, 1);
    one.enabled = false;
    QVERIFY(source.updateZone(one));
    Zone eight = zoneByNumber(source, 8);
    eight.enabled = false;
    QVERIFY(source.updateZone(eight));

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = buildProgram(source, { 8, 1 }, { 829, 839 });
    QVERIFY(programId > 0);

    ProgramRunner runner(&controller, &source);
    QSignalSpy finished(&runner, &ProgramRunner::programFinished);

    QVERIFY(runner.startProgram(programId));

    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.first().at(0).toInt(), programId);
    QCOMPARE(runner.isRunning(), false);
    QCOMPARE(controller.openZoneNumber(), 0);
    QCOMPARE(backend.lineValue(zoneOffsetFor(8)), Gpio::Value::Inactive);
    QCOMPARE(backend.lineValue(zoneOffsetFor(1)), Gpio::Value::Inactive);
}

void TestProgramRunner::firstZoneOpenFailureReturnsFalseAndAbortsTheProgram()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(detachZoneIdsFromNumbers(source));

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = buildProgram(source, { 6, 3 }, { 607, 631 });
    QVERIFY(programId > 0);

    ProgramRunner runner(&controller, &source);
    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);
    backend.setFailNextSetValues(true);

    QVERIFY(runner.startProgram(programId) == false);

    QCOMPARE(aborted.count(), 1);
    QCOMPARE(aborted.first().at(0).toInt(), programId);
    QCOMPARE(runner.isRunning(), false);
    QCOMPARE(runner.runningProgramId(), 0);
    QCOMPARE(controller.openZoneNumber(), 0);
    QCOMPARE(backend.lineValue(zoneOffsetFor(6)), Gpio::Value::Inactive);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumber(), 6);
}

void TestProgramRunner::aRepeatedZoneNumberWatersEachOccurrenceSeparately()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(detachZoneIdsFromNumbers(source));

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = buildProgram(source, { 3, 3, 5 }, { 883, 887, 907 });
    QVERIFY(programId > 0);

    ProgramRunner runner(&controller, &source);
    QSignalSpy opened(&controller, &ZoneController::zoneOpened);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumber(), 3);
    QCOMPARE(opened.count(), 1);
    QCOMPARE(opened.at(0).at(1).toInt(), 883);

    controller.expireCloseTimerForTest();
    QCOMPARE(controller.openZoneNumber(), 3);
    QCOMPARE(opened.count(), 2);
    QCOMPARE(opened.at(1).at(1).toInt(), 887);

    controller.expireCloseTimerForTest();
    QCOMPARE(controller.openZoneNumber(), 5);
    QCOMPARE(opened.count(), 3);
    QCOMPARE(opened.at(2).at(1).toInt(), 907);
}

void TestProgramRunner::watchdogTrippedSignalAbortsARunningProgram()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(detachZoneIdsFromNumbers(source));

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    const int programId = buildProgram(source, { 4, 6 }, { 911, 919 });
    QVERIFY(programId > 0);

    ProgramRunner runner(&controller, &source);
    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);

    QVERIFY(runner.startProgram(programId));
    QCOMPARE(controller.openZoneNumber(), 4);

    emit controller.watchdogTripped(4);

    QCOMPARE(aborted.count(), 1);
    QCOMPARE(aborted.first().at(0).toInt(), programId);
    QCOMPARE(runner.isRunning(), false);
    QCOMPARE(controller.openZoneNumber(), 0);
}

void TestProgramRunner::abortWhileIdleIsANoOp()
{
    QTemporaryDir dir;
    IrrigationDataSource source(dir.filePath("irrigation.db"));
    QVERIFY(source.open());
    QVERIFY(detachZoneIdsFromNumbers(source));

    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    ZoneController controller(&backend, eightZones(), true, 3600);
    QVERIFY(controller.begin());

    QVERIFY(controller.openZone(2, 929));

    ProgramRunner runner(&controller, &source);
    QSignalSpy aborted(&runner, &ProgramRunner::programAborted);

    runner.abort();

    QCOMPARE(aborted.count(), 0);
    QCOMPARE(controller.openZoneNumber(), 2);
}

QTEST_MAIN(TestProgramRunner)
#include "tst_programrunner.moc"
