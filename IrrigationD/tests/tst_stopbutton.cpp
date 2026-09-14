#include <QTest>
#include <QSignalSpy>

#include <Kanoop/pi/mockbackend.h>

#include "stopbutton.h"

namespace
{
// Off the production zone set {5,6,12,13,16,19,20,21} and off stopButtonOffset()'s
// own default (25), so a StopButton that ignores its configured offset fails loudly.
const quint32 STOP_OFFSET = 17;
}

class CountingBackend : public MockBackend
{
public:
    int requestCount = 0;

    virtual Gpio::RequestHandle requestInputs(const Gpio::InputRequest& request) override
    {
        requestCount++;
        return MockBackend::requestInputs(request);
    }
};

class FailingReadBackend : public MockBackend
{
public:
    bool failNextRead = false;

    virtual bool getValues(Gpio::RequestHandle handle, const QList<quint32>& offsets, QList<Gpio::Value>& values) override
    {
        if(failNextRead == true) {
            failNextRead = false;
            setErrorText("injected read failure");
            return false;
        }
        return MockBackend::getValues(handle, offsets, values);
    }
};

class TestStopButton : public QObject
{
    Q_OBJECT
private slots:
    void requestUsesActiveLowPullUpBothEdgesAnd20msDebounce();
    void pressEmitsPressedAndSetsHeld();
    void releaseAfterPressClearsHeldWithNoAdditionalEmit();
    void pressAgainAfterReleaseEmitsPressedTwice();
    void secondPressWithoutReleaseEmitsPressedAgain();
    void releaseWhileNotHeldEmitsNothing();
    void alreadyHeldAtStartupIsReportedWithoutEmittingPressed();
    void beginFailsWhenTheLineCannotBeRequested();
    void retryAfterFailedRequestSucceeds();
    void secondBeginAfterSuccessIsRefusedWithoutAllocating();
    void beginFailsWhenTheStartingReadFails();
    void retryAfterFailedStartingReadSucceeds();
};

void TestStopButton::requestUsesActiveLowPullUpBothEdgesAnd20msDebounce()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    StopButton button(&backend, STOP_OFFSET);
    QVERIFY(button.begin());

    Gpio::InputRequest request = backend.lastInputRequest();
    QCOMPARE(request.consumer, QString("irrigationd-stop"));
    QCOMPARE(request.offsets, QList<quint32>({STOP_OFFSET}));
    QCOMPARE(request.activeLow, true);
    QCOMPARE(request.bias, Gpio::Bias::PullUp);
    QCOMPARE(request.edge, Gpio::Edge::Both);
    QCOMPARE(request.debounceMicroseconds, 20000UL);
}

// A press pulls the line low; activeLow inversion reports it as a logical Rising edge.
void TestStopButton::pressEmitsPressedAndSetsHeld()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    StopButton button(&backend, STOP_OFFSET);
    QVERIFY(button.begin());
    QCOMPARE(backend.lastInputRequest().offsets, QList<quint32>({STOP_OFFSET}));

    QSignalSpy spy(&button, &StopButton::pressed);
    backend.simulateEdge(STOP_OFFSET, Gpio::Edge::Rising);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(button.isHeld(), true);
}

void TestStopButton::releaseAfterPressClearsHeldWithNoAdditionalEmit()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    StopButton button(&backend, STOP_OFFSET);
    QVERIFY(button.begin());
    QCOMPARE(backend.lastInputRequest().offsets, QList<quint32>({STOP_OFFSET}));

    QSignalSpy spy(&button, &StopButton::pressed);
    backend.simulateEdge(STOP_OFFSET, Gpio::Edge::Rising);
    backend.simulateEdge(STOP_OFFSET, Gpio::Edge::Falling);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(button.isHeld(), false);
}

void TestStopButton::pressAgainAfterReleaseEmitsPressedTwice()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    StopButton button(&backend, STOP_OFFSET);
    QVERIFY(button.begin());
    QCOMPARE(backend.lastInputRequest().offsets, QList<quint32>({STOP_OFFSET}));

    QSignalSpy spy(&button, &StopButton::pressed);
    backend.simulateEdge(STOP_OFFSET, Gpio::Edge::Rising);
    backend.simulateEdge(STOP_OFFSET, Gpio::Edge::Falling);
    backend.simulateEdge(STOP_OFFSET, Gpio::Edge::Rising);

    QCOMPARE(spy.count(), 2);
    QCOMPARE(button.isHeld(), true);
}

void TestStopButton::secondPressWithoutReleaseEmitsPressedAgain()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    StopButton button(&backend, STOP_OFFSET);
    QVERIFY(button.begin());

    QSignalSpy spy(&button, &StopButton::pressed);
    backend.simulateEdge(STOP_OFFSET, Gpio::Edge::Rising);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(button.isHeld(), true);

    backend.simulateEdge(STOP_OFFSET, Gpio::Edge::Rising);

    QCOMPARE(spy.count(), 2);
    QCOMPARE(button.isHeld(), true);
}

void TestStopButton::releaseWhileNotHeldEmitsNothing()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    StopButton button(&backend, STOP_OFFSET);
    QVERIFY(button.begin());
    QCOMPARE(backend.lastInputRequest().offsets, QList<quint32>({STOP_OFFSET}));

    QSignalSpy spy(&button, &StopButton::pressed);
    backend.simulateEdge(STOP_OFFSET, Gpio::Edge::Falling);

    QCOMPARE(spy.count(), 0);
    QCOMPARE(button.isHeld(), false);
}

void TestStopButton::alreadyHeldAtStartupIsReportedWithoutEmittingPressed()
{
    MockBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    backend.setLineValue(STOP_OFFSET, Gpio::Value::Active);

    StopButton button(&backend, STOP_OFFSET);
    QSignalSpy spy(&button, &StopButton::pressed);

    QVERIFY(button.begin());
    QCOMPARE(backend.lastInputRequest().offsets, QList<quint32>({STOP_OFFSET}));

    QCOMPARE(button.isHeld(), true);
    QCOMPARE(spy.count(), 0);
}

void TestStopButton::beginFailsWhenTheLineCannotBeRequested()
{
    MockBackend backend;
    // No chip opened.
    StopButton button(&backend, STOP_OFFSET);

    QVERIFY(button.begin() == false);
    QCOMPARE(button.isHeld(), false);
    QCOMPARE(button.errorText(), QString("chip is not open"));
}

void TestStopButton::retryAfterFailedRequestSucceeds()
{
    CountingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    backend.setFailNextRequest(true);

    StopButton button(&backend, STOP_OFFSET);
    QVERIFY(button.begin() == false);
    QCOMPARE(button.errorText(), QString("injected failure"));
    QCOMPARE(backend.requestCount, 1);

    QVERIFY(button.begin());
    QCOMPARE(backend.requestCount, 2);
    QCOMPARE(backend.lastInputRequest().offsets, QList<quint32>({STOP_OFFSET}));

    QSignalSpy spy(&button, &StopButton::pressed);
    backend.simulateEdge(STOP_OFFSET, Gpio::Edge::Rising);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(button.isHeld(), true);
}

void TestStopButton::secondBeginAfterSuccessIsRefusedWithoutAllocating()
{
    CountingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));

    StopButton button(&backend, STOP_OFFSET);
    QVERIFY(button.begin());
    QCOMPARE(backend.requestCount, 1);
    QCOMPARE(backend.lastInputRequest().offsets, QList<quint32>({STOP_OFFSET}));

    QVERIFY(button.begin() == false);
    QCOMPARE(backend.requestCount, 1);
    QCOMPARE(button.errorText(), QString("The stop button line is already requested"));

    QSignalSpy spy(&button, &StopButton::pressed);
    backend.simulateEdge(STOP_OFFSET, Gpio::Edge::Rising);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(button.isHeld(), true);
}

void TestStopButton::beginFailsWhenTheStartingReadFails()
{
    FailingReadBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    backend.failNextRead = true;

    StopButton button(&backend, STOP_OFFSET);
    QSignalSpy spy(&button, &StopButton::pressed);

    QVERIFY(button.begin() == false);
    QCOMPARE(button.errorText(), QString("injected read failure"));
    QCOMPARE(button.isHeld(), false);

    backend.simulateEdge(STOP_OFFSET, Gpio::Edge::Rising);
    QCOMPARE(spy.count(), 0);
}

void TestStopButton::retryAfterFailedStartingReadSucceeds()
{
    FailingReadBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    backend.failNextRead = true;

    StopButton button(&backend, STOP_OFFSET);
    QVERIFY(button.begin() == false);

    QVERIFY(button.begin());
    QCOMPARE(backend.lastInputRequest().offsets, QList<quint32>({STOP_OFFSET}));

    QSignalSpy spy(&button, &StopButton::pressed);
    backend.simulateEdge(STOP_OFFSET, Gpio::Edge::Rising);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(button.isHeld(), true);
}

QTEST_MAIN(TestStopButton)
#include "tst_stopbutton.moc"
