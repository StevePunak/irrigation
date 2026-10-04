#ifndef RUNBUTTON_H
#define RUNBUTTON_H

#include <QObject>
#include <QString>

#include <Kanoop/pi/inputpin.h>

/**
 * @brief Reports every level change of the gardener panel's RUN button line.
 *
 * The button is 1NO to ground, so a press drives the line low. Every edge is reported;
 * deciding which of them is a press is the caller's job.
 */
class RunButton : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief Constructs a RUN button over @p offset.
     * @param backend The GPIO backend.
     * @param offset The line offset.
     * @param parent The Qt parent object.
     */
    RunButton(IGpioBackend* backend, quint32 offset, QObject* parent = nullptr);

    /** @brief Requests the line and reads its starting level. @return True on success. */
    bool begin();

    /** @brief Returns whether the line reads low, tracked from its edges. */
    bool isLow() const { return _low; }

    /** @brief Returns the text of the most recent failure. */
    QString errorText() const { return _errorText; }

signals:
    /** @brief Emitted on every edge with the line's new level; @p low is true while the button is pressed. */
    void lineChanged(bool low);

private slots:
    void onAsserted();
    void onDeasserted();

private:
    IGpioBackend* _backend = nullptr;
    quint32 _offset = 0;
    InputPin* _pin = nullptr;
    bool _low = false;
    QString _errorText;
};

#endif // RUNBUTTON_H
