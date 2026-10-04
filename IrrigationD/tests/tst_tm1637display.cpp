#include <QTest>

#include <Kanoop/pi/mockbackend.h>

#include "tm1637display.h"

namespace
{
const quint32 CLOCK_OFFSET = 18;
const quint32 DATA_OFFSET = 27;

QByteArray bytes(quint8 first, quint8 second, quint8 third, quint8 fourth)
{
    QByteArray result;
    result.append(static_cast<char>(first));
    result.append(static_cast<char>(second));
    result.append(static_cast<char>(third));
    result.append(static_cast<char>(fourth));
    return result;
}
}

/** Records every successful line write in order. failAtWrite fails the write with that index once. */
class RecordingBackend : public MockBackend
{
public:
    class Write
    {
    public:
        quint32 offset = 0;
        bool high = false;
    };

    virtual bool setValues(Gpio::RequestHandle handle,
                           const QList<quint32>& offsets,
                           const QList<Gpio::Value>& values) override
    {
        if(failAtWrite >= 0 && writes.count() == failAtWrite) {
            failAtWrite = -1;
            setErrorText("injected write failure");
            return false;
        }
        if(MockBackend::setValues(handle, offsets, values) == false) {
            return false;
        }
        for(int i = 0; i < offsets.count(); i++) {
            Write write;
            write.offset = offsets.at(i);
            write.high = values.at(i) == Gpio::Value::Active;
            writes.append(write);
        }
        return true;
    }

    QList<Write> writes;
    int failAtWrite = -1;
};

/**
 * Replays line writes the way a TM1637 sees them, starting from both lines low. A start
 * is DIO falling while CLK is high; a stop is DIO rising while CLK is high. Each CLK
 * rising edge inside a frame samples DIO, eight data bits least significant first; the
 * ninth clock is the acknowledge, which runs from its rising edge to its falling edge.
 */
class Tm1637Trace
{
public:
    explicit Tm1637Trace(const QList<RecordingBackend::Write>& writes)
    {
        bool clock = false;
        bool data = false;
        bool inFrame = false;
        bool inAcknowledge = false;
        bool dataHighDuringAcknowledge = false;
        int bit = 0;
        quint8 value = 0;
        QByteArray frame;

        for(const RecordingBackend::Write& write : writes) {
            if(write.offset == CLOCK_OFFSET) {
                const bool rising = clock == false && write.high == true;
                const bool falling = clock == true && write.high == false;
                clock = write.high;
                if(inFrame == true && rising == true) {
                    if(bit < 8) {
                        if(data == true) {
                            value |= static_cast<quint8>(1 << bit);
                        }
                        bit++;
                    }
                    else {
                        inAcknowledge = true;
                        dataHighDuringAcknowledge = data;
                    }
                }
                if(inFrame == true && falling == true && inAcknowledge == true) {
                    acknowledgeClocks++;
                    if(dataHighDuringAcknowledge == true) {
                        acknowledgeClocksWithDataHigh++;
                    }
                    frame.append(static_cast<char>(value));
                    value = 0;
                    bit = 0;
                    inAcknowledge = false;
                }
            }
            else if(write.offset == DATA_OFFSET) {
                if(clock == true && data == true && write.high == false) {
                    inFrame = true;
                    frame.clear();
                    bit = 0;
                    value = 0;
                }
                else if(clock == true && data == false && write.high == true && inFrame == true) {
                    frames.append(frame);
                    inFrame = false;
                }
                data = write.high;
                if(inAcknowledge == true && data == true) {
                    dataHighDuringAcknowledge = true;
                }
            }
        }
    }

    QList<QByteArray> frames;
    int acknowledgeClocks = 0;
    int acknowledgeClocksWithDataHigh = 0;
};

class TestTm1637Display : public QObject
{
    Q_OBJECT
private slots:
    void beginRequestsBothLinesAsOutputsDrivenLow();
    void oneUpdateSendsTheDataAddressAndControlCommands();
    void everyByteGoesOutLeastSignificantBitFirst();
    void dataIsLowThroughEveryAcknowledgeClock();
    void bothLinesRestHighAfterAnUpdate();
    void onlyTheTwoDisplayLinesAreWritten();
    void showRefusesAnythingButFourBytes();
    void showBeforeBeginFails();
    void aFailedWriteFailsTheUpdateAndTheNextIsWhole();
    void beginFailsWhenTheLinesCannotBeRequested();
    void aSecondBeginIsRefused();
};

void TestTm1637Display::beginRequestsBothLinesAsOutputsDrivenLow()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);
    QVERIFY(display.begin());

    const Gpio::OutputRequest request = backend.lastOutputRequest();
    QCOMPARE(request.consumer, QString("irrigationd-display"));
    QCOMPARE(request.offsets, QList<quint32>({ CLOCK_OFFSET, DATA_OFFSET }));
    QCOMPARE(request.activeLow, false);
    QCOMPARE(request.initialValue, Gpio::Value::Inactive);
}

void TestTm1637Display::oneUpdateSendsTheDataAddressAndControlCommands()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);
    display.setEdgeDelayMicroseconds(0);
    QVERIFY(display.begin());

    QVERIFY2(display.show(bytes(0x06, 0x5B | 0x80, 0x4F, 0x66)), qPrintable(display.errorText()));

    const Tm1637Trace trace(backend.writes);
    QCOMPARE(trace.frames.count(), 3);
    QCOMPARE(trace.frames.at(0), QByteArray(1, static_cast<char>(0x40)));
    QByteArray address(1, static_cast<char>(0xC0));
    address.append(bytes(0x06, 0x5B | 0x80, 0x4F, 0x66));
    QCOMPARE(trace.frames.at(1), address);
    QCOMPARE(trace.frames.at(2), QByteArray(1, static_cast<char>(0x88 | Tm1637Display::Brightness)));
    QCOMPARE(static_cast<quint8>(trace.frames.at(2).at(0)), quint8(0x8C));
}

void TestTm1637Display::everyByteGoesOutLeastSignificantBitFirst()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);
    display.setEdgeDelayMicroseconds(0);
    QVERIFY(display.begin());
    QVERIFY(display.show(bytes(0x01, 0x80, 0x00, 0xFF)));

    // The first data clock of the 0x01 byte must carry DIO high and the next seven low.
    const Tm1637Trace trace(backend.writes);
    QCOMPARE(trace.frames.at(1).mid(1), bytes(0x01, 0x80, 0x00, 0xFF));
}

void TestTm1637Display::dataIsLowThroughEveryAcknowledgeClock()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);
    display.setEdgeDelayMicroseconds(0);
    QVERIFY(display.begin());

    // Every segment byte ends on a 1 bit, so DIO is high when each acknowledge clock comes due.
    QVERIFY(display.show(bytes(0xFF, 0xFF, 0xFF, 0xFF)));

    const Tm1637Trace trace(backend.writes);
    // 0x40, then 0xC0 and four segment bytes, then 0x8C: seven bytes, seven acknowledge clocks.
    QCOMPARE(trace.acknowledgeClocks, 7);
    QCOMPARE(trace.acknowledgeClocksWithDataHigh, 0);
}

void TestTm1637Display::bothLinesRestHighAfterAnUpdate()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);
    display.setEdgeDelayMicroseconds(0);
    QVERIFY(display.begin());
    QCOMPARE(backend.lineValue(CLOCK_OFFSET), Gpio::Value::Inactive);
    QCOMPARE(backend.lineValue(DATA_OFFSET), Gpio::Value::Inactive);

    QVERIFY(display.show(bytes(0x40, 0x40, 0x40, 0x40)));
    QCOMPARE(backend.lineValue(CLOCK_OFFSET), Gpio::Value::Active);
    QCOMPARE(backend.lineValue(DATA_OFFSET), Gpio::Value::Active);
}

void TestTm1637Display::onlyTheTwoDisplayLinesAreWritten()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);
    display.setEdgeDelayMicroseconds(0);
    QVERIFY(display.begin());
    QVERIFY(display.show(bytes(0x40, 0x40, 0x40, 0x40)));

    QVERIFY(backend.writes.isEmpty() == false);
    for(const RecordingBackend::Write& write : backend.writes) {
        QVERIFY(write.offset == CLOCK_OFFSET || write.offset == DATA_OFFSET);
    }
}

void TestTm1637Display::showRefusesAnythingButFourBytes()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);
    display.setEdgeDelayMicroseconds(0);
    QVERIFY(display.begin());

    QVERIFY(display.show(QByteArray(3, '\0')) == false);
    QVERIFY(display.show(QByteArray(5, '\0')) == false);
    QVERIFY(display.errorText().contains("4 segment bytes"));
    QVERIFY(backend.writes.isEmpty());
}

void TestTm1637Display::showBeforeBeginFails()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);

    QVERIFY(display.show(bytes(0x40, 0x40, 0x40, 0x40)) == false);
    QVERIFY(display.errorText().isEmpty() == false);
    QVERIFY(backend.writes.isEmpty());
}

void TestTm1637Display::aFailedWriteFailsTheUpdateAndTheNextIsWhole()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);
    display.setEdgeDelayMicroseconds(0);
    QVERIFY(display.begin());

    // Write 40 is the first write of the 0xC0 address byte: 4 (data command start) +
    // 28 (data command byte) + 4 (data command stop) + 4 (address start) writes precede it.
    backend.failAtWrite = 40;
    QVERIFY(display.show(bytes(0x06, 0x06, 0x06, 0x06)) == false);
    QVERIFY(display.errorText().contains("injected write failure"));
    QCOMPARE(backend.writes.count(), 40);

    backend.writes.clear();
    QVERIFY(display.show(bytes(0x5B, 0x5B, 0x5B, 0x5B)));
    const Tm1637Trace trace(backend.writes);
    QCOMPARE(trace.frames.count(), 3);
    QCOMPARE(trace.frames.at(1).mid(1), bytes(0x5B, 0x5B, 0x5B, 0x5B));
}

void TestTm1637Display::beginFailsWhenTheLinesCannotBeRequested()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);

    backend.setFailNextRequest(true);
    QVERIFY(display.begin() == false);
    QVERIFY(display.errorText().isEmpty() == false);
    QVERIFY(display.show(bytes(0x40, 0x40, 0x40, 0x40)) == false);

    QVERIFY(display.begin());
}

void TestTm1637Display::aSecondBeginIsRefused()
{
    RecordingBackend backend;
    QVERIFY(backend.openChipByLabel("mock"));
    Tm1637Display display(&backend, CLOCK_OFFSET, DATA_OFFSET);
    QVERIFY(display.begin());
    QVERIFY(display.begin() == false);
    QVERIFY(display.errorText().contains("already"));
}

QTEST_MAIN(TestTm1637Display)
#include "tst_tm1637display.moc"
