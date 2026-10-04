#include <QTest>
#include <QSignalSpy>

#include <Kanoop/pi/mockbackend.h>

#include "runbutton.h"

namespace
{
// Off the production RUN offset (24) and every other configured line, so a RunButton
// that ignores its configured offset fails loudly.
const quint32 RUN_OFFSET = 26;
}

class TestRunButton : public QObject
{
    Q_OBJECT
private slots:
    void requestUsesActiveLowPullUpBothEdgesAnd20msDebounce();
    void aPressReportsLowAndAReleaseReportsHigh();
    void aLineLowAtStartupIsReadWithoutAChange();
    void beginFailsWhenTheLineCannotBeRequested();
    void aSecondBeginIsRefused();
};

void TestRunButton::requestUsesActiveLowPullUpBothEdgesAnd20msDebounce()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    RunButton button(&backend, RUN_OFFSET);
    QVERIFY(button.begin());

    const Gpio::InputRequest request = backend.lastInputRequest();
    QCOMPARE(request.consumer, QString("irrigationd-run"));
    QCOMPARE(request.offsets, QList<quint32>({ RUN_OFFSET }));
    QCOMPARE(request.activeLow, true);
    QCOMPARE(request.bias, Gpio::Bias::PullUp);
    QCOMPARE(request.edge, Gpio::Edge::Both);
    QCOMPARE(request.debounceMicroseconds, 20000UL);
}

// A press pulls the line low; activeLow inversion reports it as a logical Rising edge.
void TestRunButton::aPressReportsLowAndAReleaseReportsHigh()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    RunButton button(&backend, RUN_OFFSET);
    QVERIFY(button.begin());
    QSignalSpy spy(&button, &RunButton::lineChanged);

    backend.simulateEdge(RUN_OFFSET, Gpio::Edge::Rising);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toBool(), true);
    QVERIFY(button.isLow());

    backend.simulateEdge(RUN_OFFSET, Gpio::Edge::Falling);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.at(1).at(0).toBool(), false);
    QVERIFY(button.isLow() == false);
}

void TestRunButton::aLineLowAtStartupIsReadWithoutAChange()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    backend.setLineValue(RUN_OFFSET, Gpio::Value::Active);

    RunButton button(&backend, RUN_OFFSET);
    QSignalSpy spy(&button, &RunButton::lineChanged);
    QVERIFY(button.begin());

    QVERIFY(button.isLow());
    QCOMPARE(spy.count(), 0);
}

void TestRunButton::beginFailsWhenTheLineCannotBeRequested()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    RunButton button(&backend, RUN_OFFSET);

    backend.setFailNextRequest(true);
    QVERIFY(button.begin() == false);
    QVERIFY(button.errorText().isEmpty() == false);

    QVERIFY(button.begin());
}

void TestRunButton::aSecondBeginIsRefused()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    RunButton button(&backend, RUN_OFFSET);
    QVERIFY(button.begin());
    QVERIFY(button.begin() == false);
    QVERIFY(button.errorText().contains("already"));
}

QTEST_MAIN(TestRunButton)
#include "tst_runbutton.moc"
