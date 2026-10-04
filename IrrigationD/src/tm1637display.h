#ifndef TM1637DISPLAY_H
#define TM1637DISPLAY_H

#include <QByteArray>
#include <QObject>
#include <QString>

#include <Kanoop/pi/outputbank.h>

/**
 * @brief Writes four segment bytes to a TM1637 over two GPIO outputs.
 *
 * Each update sends the data command, the address command with the four bytes, then
 * the display-control command with the fixed brightness, every byte least significant
 * bit first.
 *
 * @warning Both lines must be driven push-pull, and DIO must be driven low through
 *          every acknowledge clock. The module has no pull-up on CLK, and a DIO left
 *          to the module's own pull-up stayed blank on the bench.
 */
class Tm1637Display : public QObject
{
    Q_OBJECT
public:
    /** @brief The display-control brightness, 0 through 7. */
    static constexpr int Brightness = 4;

    /** @brief The pause after each line change unless setEdgeDelayMicroseconds() says otherwise. */
    static constexpr int DefaultEdgeDelayMicroseconds = 10;

    /**
     * @brief Constructs a display on @p clockOffset and @p dataOffset.
     * @param backend The GPIO backend. Must already have an open chip.
     * @param clockOffset The CLK line offset.
     * @param dataOffset The DIO line offset.
     * @param parent The Qt parent object.
     */
    Tm1637Display(IGpioBackend* backend, quint32 clockOffset, quint32 dataOffset, QObject* parent = nullptr);

    /** @brief Requests both lines as outputs, driven low. @return True on success. */
    bool begin();

    /**
     * @brief Shows @p segments, four bytes with digit 0 leftmost.
     * @return True when every line write succeeded. False for a byte count other than
     *         four, lines that are not requested, or a failed write; a failed write
     *         leaves the frame unfinished and the next show() starts a whole one.
     */
    bool show(const QByteArray& segments);

    /** @brief Sets the pause after each line change in microseconds. Zero removes it. */
    void setEdgeDelayMicroseconds(int value) { _edgeDelayMicroseconds = value; }

    /** @brief Returns the text of the most recent failure. */
    QString errorText() const { return _errorText; }

private:
    bool startCondition();
    bool stopCondition();
    bool writeByte(quint8 value);
    bool setLine(quint32 offset, bool high);
    void pause() const;

    static constexpr quint8 DataCommand = 0x40;
    static constexpr quint8 AddressCommand = 0xC0;
    static constexpr quint8 DisplayControlCommand = 0x88;

    IGpioBackend* _backend = nullptr;
    quint32 _clockOffset = 0;
    quint32 _dataOffset = 0;
    OutputBank* _bank = nullptr;
    int _edgeDelayMicroseconds = DefaultEdgeDelayMicroseconds;
    QString _errorText;
};

#endif // TM1637DISPLAY_H
