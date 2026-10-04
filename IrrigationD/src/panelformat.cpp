#include "panelformat.h"

QByteArray PanelFormat::clock(const QTime& time, bool colonOn)
{
    int hour = time.hour() % 12;
    if(hour == 0) {
        hour = 12;
    }

    QByteArray result(DigitCount, '\0');
    result[0] = static_cast<char>(hour >= 10 ? digit(hour / 10) : 0);
    result[1] = static_cast<char>(digit(hour % 10) | (colonOn == true ? Colon : 0));
    result[2] = static_cast<char>(digit(time.minute() / 10));
    result[3] = static_cast<char>(digit(time.minute() % 10));
    return result;
}

QByteArray PanelFormat::zoneMinutes(int zoneNumber, int minutes, bool zoneVisible)
{
    const int bounded = qBound(0, minutes, 99);

    QByteArray result(DigitCount, '\0');
    result[0] = static_cast<char>(zoneVisible == true ? digit(zoneNumber % 10) : 0);
    result[2] = static_cast<char>(bounded >= 10 ? digit(bounded / 10) : 0);
    result[3] = static_cast<char>(digit(bounded % 10));
    return result;
}

int PanelFormat::minutesLeft(int seconds)
{
    return seconds <= 0 ? 1 : (seconds + 59) / 60;
}

QByteArray PanelFormat::text(const QString& glyphs)
{
    QByteArray result(DigitCount, '\0');
    for(int i = 0; i < DigitCount && i < glyphs.length(); i++) {
        result[i] = static_cast<char>(glyph(glyphs.at(i)));
    }
    return result;
}

quint8 PanelFormat::digit(int value)
{
    static const quint8 digits[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    return digits[qBound(0, value, 9)];
}

quint8 PanelFormat::glyph(QChar character)
{
    if(character.isDigit() == true) {
        return digit(character.digitValue());
    }

    switch(character.unicode()) {
    case '-':
        return SegG;
    case 'E':
        return SegA | SegD | SegE | SegF | SegG;
    case 'F':
        return SegA | SegE | SegF | SegG;
    case 'L':
        return SegD | SegE | SegF;
    case 'O':
        return SegA | SegB | SegC | SegD | SegE | SegF;
    case 'P':
        return SegA | SegB | SegE | SegF | SegG;
    case 'S':
        return SegA | SegC | SegD | SegF | SegG;
    case 'n':
        return SegC | SegE | SegG;
    case 'o':
        return SegC | SegD | SegE | SegG;
    case 'r':
        return SegE | SegG;
    case 't':
        return SegD | SegE | SegF | SegG;
    case 'u':
        return SegC | SegD | SegE;
    default:
        return 0;
    }
}
