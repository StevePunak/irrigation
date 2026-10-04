#ifndef PANELCONTROLLER_H
#define PANELCONTROLLER_H

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QTimeZone>

#include <Kanoop/utility/loggingbaseclass.h>

#include "iclock.h"
#include "ipanelhost.h"

/**
 * @brief The gardener panel's RUN-button state machine and the frame the display shows.
 *
 * Holds no timer and touches no GPIO. The owner feeds it RUN line changes and calls
 * tick() on a short interval. It reads the controller through IPanelHost on every press
 * and every tick, times every interval on IClock::monotonicMsecs(), and emits each frame
 * once, when its bytes change.
 */
class PanelController : public QObject,
                        public LoggingBaseClass
{
    Q_OBJECT
public:
    /** @brief What the panel is doing. */
    enum class Mode
    {
        Idle,       ///< No selection and no panel run.
        Selecting,  ///< A zone is offered and commits three seconds after the last press.
        PanelRun    ///< The panel zone is open.
    };
    Q_ENUM(Mode)

    /** @brief The panel run time until panel_run_minutes says otherwise. */
    static constexpr int DefaultRunMinutes = 10;

    /** @brief The lowest panel_run_minutes accepted. */
    static constexpr int MinimumRunMinutes = 1;

    /** @brief The highest panel_run_minutes accepted. */
    static constexpr int MaximumRunMinutes = 60;

    /** @brief How long after the last press a selection commits. */
    static constexpr qint64 CommitDelayMsecs = 3000;

    /** @brief How long OFF, FuLL and nonE stay on the display. */
    static constexpr qint64 MessageMsecs = 2000;

    /** @brief How long each of several other running zones is shown. */
    static constexpr qint64 AlternateMsecs = 2000;

    /** @brief Each half of a blink: the clock colon and the selected zone digit. */
    static constexpr qint64 BlinkHalfPeriodMsecs = 500;

    /** @brief How long the RUN line may read low before it is logged as stuck. */
    static constexpr qint64 StuckLineMsecs = 10000;

    /**
     * @brief Constructs a panel reading the controller through @p host and time from @p clock.
     *
     * The clock face reads UTC until setTimeZone() is called.
     */
    PanelController(IPanelHost* host, IClock* clock, QObject* parent = nullptr);

    /** @brief Sets the zone the clock face shows. */
    void setTimeZone(const QTimeZone& value) { _timeZone = value; }

    /** @brief Records the RUN line's level at startup. A line already low produces no press until it has been seen high. */
    void setInitialRunLine(bool low);

    /** @brief Ends any selection, panel run and message without touching a zone. Called when STOP clears the controller. */
    void cancel();

    /** @brief Returns what the panel is doing. */
    Mode mode() const { return _mode; }

    /** @brief Returns the zone number on offer, or zero unless a selection is pending. */
    int selectedZone() const { return _selectedZone; }

    /** @brief Returns the panel zone's number, or zero unless a panel run is active. */
    int panelZone() const { return _panelZone; }

    /** @brief Returns whether the RUN line has read low for longer than StuckLineMsecs without being seen high. */
    bool isRunLineStuck() const { return _stuckLogged; }

    /** @brief Returns the frame most recently emitted, or an empty array before the first. */
    QByteArray frame() const { return _lastFrame; }

public slots:
    /**
     * @brief Takes the RUN line's new level.
     *
     * A change to low is a press. A low while the line already reads low is ignored: the
     * line must be seen high first.
     */
    void onRunLineChanged(bool low);

    /** @brief Commits a due selection, ends a panel run whose zone closed, checks the stuck line, and refreshes the frame. */
    void tick();

signals:
    /** @brief Emitted when the frame's bytes change. @p segments holds four segment bytes, digit 0 leftmost. */
    void frameChanged(const QByteArray& segments);

    /** @brief Emitted after a press is handled, and after a tick that committed, cancelled or ended something. */
    void stateChanged();

private:
    void press();
    void pressWhileIdle(const PanelSnapshot& snapshot);
    void pressWhileSelecting(const PanelSnapshot& snapshot);
    void pressDuringRun(const PanelSnapshot& snapshot);
    void commit(const PanelSnapshot& snapshot);
    bool endRunIfClosed(const PanelSnapshot& snapshot);
    void showRefusal(RunRequest::Refusal refusal);
    void showMessage(const QByteArray& segments);
    void publishFrame(const PanelSnapshot& snapshot);
    QByteArray render(const PanelSnapshot& snapshot);
    QByteArray otherZonesFrame(const PanelSnapshot& snapshot, qint64 now);
    static int nextEnabledZone(const QList<int>& enabledZones, int afterZone, bool wrap);

    IPanelHost* _host = nullptr;
    IClock* _clock = nullptr;
    QTimeZone _timeZone;
    Mode _mode = Mode::Idle;
    int _selectedZone = 0;
    qint64 _lastPressMsecs = 0;
    int _panelZone = 0;
    QByteArray _message;
    qint64 _messageUntilMsecs = 0;
    int _shownZone = 0;
    qint64 _shownSinceMsecs = 0;
    bool _lineLow = false;
    qint64 _lowSinceMsecs = 0;
    bool _stuckLogged = false;
    QByteArray _lastFrame;
};

#endif // PANELCONTROLLER_H
