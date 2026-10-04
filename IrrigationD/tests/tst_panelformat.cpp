#include <QTest>

#include "panelformat.h"

namespace
{
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

class TestPanelFormat : public QObject
{
    Q_OBJECT
private slots:
    void clockBlanksTheLeadingDigitBeforeTen();
    void clockShowsBothHourDigitsFromTenToTwelve();
    void clockReadsMidnightAndNoonAsTwelve();
    void clockLightsTheColonOnlyWhenAsked();
    void zoneMinutesRightAlignsTheMinutes();
    void zoneMinutesBlanksTheZoneDigitWhenHidden();
    void zoneMinutesBoundsTheMinutesToTwoDigits();
    void minutesLeftRoundsUp_data();
    void minutesLeftRoundsUp();
    void everyGlyphUsedHasItsSegments_data();
    void everyGlyphUsedHasItsSegments();
    void textShowsACharacterWithoutAGlyphBlank();
};

void TestPanelFormat::clockBlanksTheLeadingDigitBeforeTen()
{
    // " 6:42" with the colon on digit 1.
    QCOMPARE(PanelFormat::clock(QTime(6, 42), true), bytes(0x00, 0x7D | 0x80, 0x66, 0x5B));
    QCOMPARE(PanelFormat::clock(QTime(18, 42), true), bytes(0x00, 0x7D | 0x80, 0x66, 0x5B));
}

void TestPanelFormat::clockShowsBothHourDigitsFromTenToTwelve()
{
    QCOMPARE(PanelFormat::clock(QTime(22, 5), true), bytes(0x06, 0x3F | 0x80, 0x3F, 0x6D));
    QCOMPARE(PanelFormat::clock(QTime(11, 59), true), bytes(0x06, 0x06 | 0x80, 0x6D, 0x6F));
}

void TestPanelFormat::clockReadsMidnightAndNoonAsTwelve()
{
    QCOMPARE(PanelFormat::clock(QTime(0, 30), true), bytes(0x06, 0x5B | 0x80, 0x4F, 0x3F));
    QCOMPARE(PanelFormat::clock(QTime(12, 0), true), bytes(0x06, 0x5B | 0x80, 0x3F, 0x3F));
}

void TestPanelFormat::clockLightsTheColonOnlyWhenAsked()
{
    const QByteArray lit = PanelFormat::clock(QTime(6, 42), true);
    const QByteArray dark = PanelFormat::clock(QTime(6, 42), false);
    QCOMPARE(static_cast<int>(static_cast<quint8>(lit.at(1)) & PanelFormat::Colon), static_cast<int>(PanelFormat::Colon));
    QCOMPARE(static_cast<int>(static_cast<quint8>(dark.at(1)) & PanelFormat::Colon), 0);
    QCOMPARE(dark, bytes(0x00, 0x7D, 0x66, 0x5B));
}

void TestPanelFormat::zoneMinutesRightAlignsTheMinutes()
{
    // "3 12", "3  1" and "1 10": digit 1 and the colon stay dark.
    QCOMPARE(PanelFormat::zoneMinutes(3, 12, true), bytes(0x4F, 0x00, 0x06, 0x5B));
    QCOMPARE(PanelFormat::zoneMinutes(3, 1, true), bytes(0x4F, 0x00, 0x00, 0x06));
    QCOMPARE(PanelFormat::zoneMinutes(1, 10, true), bytes(0x06, 0x00, 0x06, 0x3F));
}

void TestPanelFormat::zoneMinutesBlanksTheZoneDigitWhenHidden()
{
    QCOMPARE(PanelFormat::zoneMinutes(1, 10, false), bytes(0x00, 0x00, 0x06, 0x3F));
}

void TestPanelFormat::zoneMinutesBoundsTheMinutesToTwoDigits()
{
    QCOMPARE(PanelFormat::zoneMinutes(2, 250, true), bytes(0x5B, 0x00, 0x6F, 0x6F));
    QCOMPARE(PanelFormat::zoneMinutes(2, -4, true), bytes(0x5B, 0x00, 0x00, 0x3F));
}

void TestPanelFormat::minutesLeftRoundsUp_data()
{
    QTest::addColumn<int>("seconds");
    QTest::addColumn<int>("minutes");

    QTest::newRow("a negative count still shows a minute") << -5 << 1;
    QTest::newRow("zero seconds on an open zone shows a minute") << 0 << 1;
    QTest::newRow("one second") << 1 << 1;
    QTest::newRow("just under a minute") << 59 << 1;
    QTest::newRow("exactly a minute") << 60 << 1;
    QTest::newRow("one second over a minute") << 61 << 2;
    QTest::newRow("twelve minutes exactly") << 720 << 12;
    QTest::newRow("eleven minutes and one second") << 661 << 12;
    QTest::newRow("an hour") << 3600 << 60;
}

void TestPanelFormat::minutesLeftRoundsUp()
{
    QFETCH(int, seconds);
    QFETCH(int, minutes);
    QCOMPARE(PanelFormat::minutesLeft(seconds), minutes);
}

void TestPanelFormat::everyGlyphUsedHasItsSegments_data()
{
    QTest::addColumn<QByteArray>("actual");
    QTest::addColumn<QByteArray>("expected");

    QTest::newRow("StOP") << PanelFormat::stopHeld() << bytes(0x6D, 0x78, 0x3F, 0x73);
    QTest::newRow("Err, left-aligned") << PanelFormat::fault() << bytes(0x79, 0x50, 0x50, 0x00);
    QTest::newRow("OFF, left-aligned") << PanelFormat::masterOff() << bytes(0x3F, 0x71, 0x71, 0x00);
    QTest::newRow("FuLL") << PanelFormat::full() << bytes(0x71, 0x1C, 0x38, 0x38);
    QTest::newRow("nonE") << PanelFormat::noZones() << bytes(0x54, 0x5C, 0x54, 0x79);
    QTest::newRow("----") << PanelFormat::dashes() << bytes(0x40, 0x40, 0x40, 0x40);
    QTest::newRow("digits 0-3") << PanelFormat::text("0123") << bytes(0x3F, 0x06, 0x5B, 0x4F);
    QTest::newRow("digits 4-7") << PanelFormat::text("4567") << bytes(0x66, 0x6D, 0x7D, 0x07);
    QTest::newRow("digits 8-9") << PanelFormat::text("89") << bytes(0x7F, 0x6F, 0x00, 0x00);
}

void TestPanelFormat::everyGlyphUsedHasItsSegments()
{
    QFETCH(QByteArray, actual);
    QFETCH(QByteArray, expected);
    QCOMPARE(actual, expected);
}

void TestPanelFormat::textShowsACharacterWithoutAGlyphBlank()
{
    QCOMPARE(PanelFormat::text("S?P"), bytes(0x6D, 0x00, 0x73, 0x00));
    QCOMPARE(PanelFormat::text("StOPPED"), PanelFormat::stopHeld());
}

QTEST_MAIN(TestPanelFormat)
#include "tst_panelformat.moc"
