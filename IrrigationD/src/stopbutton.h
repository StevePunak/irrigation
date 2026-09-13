#ifndef STOPBUTTON_H
#define STOPBUTTON_H

#include <QObject>
#include <QString>

#include <Kanoop/pi/inputpin.h>

/**
 * @brief Reports every press of the physical emergency-stop button.
 *
 * @warning This is the daemon's physical kill switch. begin() reads the
 *          line's starting state so a daemon restarted while the button is
 *          already held knows it immediately, without waiting for an edge
 *          that already happened.
 */
class StopButton : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief Constructs a stop button over @p offset.
     * @param backend The GPIO backend.
     * @param offset The line offset.
     * @param parent The Qt parent object.
     */
    StopButton(IGpioBackend* backend, quint32 offset, QObject* parent = nullptr);

    /** @brief Requests the line and reads its starting state. @return True on success. */
    bool begin();

    /** @brief Returns whether the button was held as of the last known state. */
    bool isHeld() const { return _held; }

    /** @brief Returns the text of the most recent failure. */
    QString errorText() const { return _errorText; }

signals:
    /** @brief Emitted for every press of the button. */
    void pressed();

private:
    IGpioBackend* _backend;
    quint32 _offset;
    InputPin* _pin = nullptr;
    bool _held = false;
    QString _errorText;
};

#endif // STOPBUTTON_H
