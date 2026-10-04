#ifndef PANELFORMAT_H
#define PANELFORMAT_H

#include <QByteArray>
#include <QString>
#include <QTime>

/**
 * @brief Builds the four segment bytes the TM1637 shows, digit 0 leftmost.
 *
 * Bit 0 is segment a through bit 6 segment g. Bit 7 of digit 1 lights the centre colon.
 */
class PanelFormat
{
public:
    static constexpr quint8 SegA = 0x01;    ///< Top.
    static constexpr quint8 SegB = 0x02;    ///< Upper right.
    static constexpr quint8 SegC = 0x04;    ///< Lower right.
    static constexpr quint8 SegD = 0x08;    ///< Bottom.
    static constexpr quint8 SegE = 0x10;    ///< Lower left.
    static constexpr quint8 SegF = 0x20;    ///< Upper left.
    static constexpr quint8 SegG = 0x40;    ///< Middle.
    static constexpr quint8 Colon = 0x80;   ///< The centre colon, carried by digit 1.

    /** @brief The number of digits on the module. */
    static constexpr int DigitCount = 4;

    /**
     * @brief Returns @p time as a 12-hour h:mm.
     *
     * The leading digit is blank before 10 o'clock; midnight and noon read 12. The colon
     * is lit when @p colonOn is true.
     */
    static QByteArray clock(const QTime& time, bool colonOn);

    /**
     * @brief Returns @p zoneNumber in the left digit and @p minutes right-aligned in the right two.
     *
     * The second digit and the colon stay dark. The zone digit is blank when @p zoneVisible
     * is false. @p minutes is bounded to 0 through 99.
     */
    static QByteArray zoneMinutes(int zoneNumber, int minutes, bool zoneVisible);

    /** @brief Returns the whole minutes in @p seconds rounded up, and 1 for anything below one second. */
    static int minutesLeft(int seconds);

    /** @brief Returns @p glyphs left-aligned and padded with blanks to four digits. A character without a glyph shows blank. */
    static QByteArray text(const QString& glyphs);

    /** @brief Returns the segment byte for the decimal digit @p value, bounded to 0 through 9. */
    static quint8 digit(int value);

    /** @brief Returns StOP. */
    static QByteArray stopHeld() { return text("StOP"); }

    /** @brief Returns Err. */
    static QByteArray fault() { return text("Err"); }

    /** @brief Returns OFF. */
    static QByteArray masterOff() { return text("OFF"); }

    /** @brief Returns FuLL. */
    static QByteArray full() { return text("FuLL"); }

    /** @brief Returns nonE. */
    static QByteArray noZones() { return text("nonE"); }

    /** @brief Returns ----. */
    static QByteArray dashes() { return text("----"); }

private:
    static quint8 glyph(QChar character);
};

#endif // PANELFORMAT_H
