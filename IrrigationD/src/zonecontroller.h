#ifndef ZONECONTROLLER_H
#define ZONECONTROLLER_H

#include <QDeadlineTimer>
#include <QMap>
#include <QObject>
#include <QTimer>

#include <Kanoop/timespan.h>
#include <Kanoop/utility/loggingbaseclass.h>
#include <Kanoop/pi/outputbank.h>

/**
 * @brief Sole owner of the valve outputs.
 *
 * Enforces four invariants regardless of caller:
 *  1. Mutual exclusion — opening a zone closes any open zone in the same write.
 *  2. No open without a deadline — there is no overload that opens indefinitely.
 *  3. Duration clamp — requests are clamped to the configured ceiling.
 *  4. Watchdog — a periodic tick reads the lines back and closes the bank when a
 *     zone is open past its deadline, a line differs from the expected state, or
 *     the read-back fails. A trip latches a fault that refuses every openZone()
 *     until a later tick finds no zone open and reads back exactly the expected
 *     state. A zone whose close failed inside the trip holds the latch until a
 *     retried close lands.
 *
 * @warning Every method must be called on the thread that owns this object.
 *          Other threads emit a request signal instead. Two threads writing the
 *          bank is the one race this class exists to prevent.
 */
class ZoneController : public QObject,
                       public LoggingBaseClass
{
    Q_OBJECT
public:
    /**
     * @brief Constructs a controller over @p zoneGpioMap.
     * @param backend The GPIO backend. Must already have an open chip.
     * @param zoneGpioMap Zone number to line offset.
     * @param activeLow Whether the valve lines are active-low.
     * @param maxZoneSeconds The hard ceiling applied to every requested duration.
     *        setMaxZoneSeconds() can lower the ceiling and never raise it past this value.
     */
    ZoneController(IGpioBackend* backend,
                   const QMap<int, quint32>& zoneGpioMap,
                   bool activeLow,
                   int maxZoneSeconds,
                   QObject* parent = nullptr);

    /** @brief Destructor. Closes every zone. */
    virtual ~ZoneController();

    /** @brief Requests the lines and drives all of them inactive. @return True on success. */
    bool begin();

    /**
     * @brief Opens @p zoneNumber for @p seconds, closing any open zone.
     * @return True on success. False for an unknown zone, a non-positive duration,
     *         a failed write, or while the watchdog fault latch is set.
     */
    bool openZone(int zoneNumber, int seconds);

    /**
     * @brief Closes every zone. Callable from any component; always takes precedence.
     * @return True when every line was driven inactive, or when the lines are not
     *         requested, in which case nothing is written and the open zone is forgotten.
     */
    bool allOff();

    /** @brief Returns the open zone number, or zero when none is open. */
    int openZoneNumber() const { return _openZone; }

    /** @brief Returns the seconds remaining on the open zone, or zero. */
    int secondsRemaining() const;

    /** @brief Returns the ceiling the next openZone() clamps to, in seconds. */
    int maxZoneSeconds() const { return _maxZoneSeconds; }

    /**
     * @brief Sets the ceiling the next openZone() clamps to, bounded to 1 through the constructor's hard ceiling.
     *
     * A zone already open keeps the deadline it opened with.
     */
    void setMaxZoneSeconds(int value);

    /** @brief Returns whether the watchdog fault latch is refusing openZone(). */
    bool isFaulted() const { return _faulted; }

    /** @brief Sets how often the watchdog verifies line state against the deadline. */
    void setWatchdogInterval(const TimeSpan& value);

    /** @brief Stops the close timer without closing the zone. Test seam for the watchdog. */
    void disableCloseTimerForTest() { _closeTimer.stop(); }

    /** @brief Runs the close path immediately. Test seam. */
    void expireCloseTimerForTest() { _closeTimer.stop(); onCloseTimer(); }

    /** @brief Returns whether the close timer is currently armed. Test seam. */
    bool closeTimerActiveForTest() const { return _closeTimer.isActive(); }

    /** @brief Runs one watchdog check immediately. Test seam. */
    void triggerWatchdogForTest() { onWatchdogTimer(); }

    /** @brief Returns the text of the most recent failure. */
    QString errorText() const { return _errorText; }

signals:
    /** @brief Emitted after @p zoneNumber has been driven active for @p seconds. */
    void zoneOpened(int zoneNumber, int seconds);

    /** @brief Emitted after @p zoneNumber has been driven inactive. */
    void zoneClosed(int zoneNumber);

    /**
     * @brief Emitted after the watchdog closed the bank.
     *
     * The watchdog trips when @p zoneNumber is open past its deadline, when a line
     * reads back different from the expected state, or when the read-back fails.
     * @p zoneNumber is the zone that was open, or zero. Never emitted when the
     * close inside the trip failed.
     */
    void watchdogTripped(int zoneNumber);

private slots:
    void onCloseTimer();
    void onWatchdogTimer();

private:
    bool writeExclusive(int zoneNumber);
    void tripWatchdog();

    static const TimeSpan RetryInterval;
    static const TimeSpan DefaultWatchdogInterval;
    static const TimeSpan MinimumWatchdogInterval;

    QMap<int, quint32> _zoneGpioMap;
    int _hardMaxZoneSeconds = 3600;
    int _maxZoneSeconds = 3600;
    OutputBank* _bank = nullptr;
    QTimer _closeTimer;
    QTimer _watchdogTimer;
    int _openZone = 0;
    QDeadlineTimer _deadline;
    bool _faulted = false;
    QString _errorText;
};

#endif // ZONECONTROLLER_H
