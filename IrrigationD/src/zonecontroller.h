#ifndef ZONECONTROLLER_H
#define ZONECONTROLLER_H

#include <QDateTime>
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
 *  4. Watchdog — a periodic tick reads the lines back and closes the bank if a
 *     zone is open past its deadline.
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
     * @param maxZoneSeconds The ceiling applied to every requested duration.
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

    /** @brief Opens @p zoneNumber for @p seconds, closing any open zone. @return True on success. */
    bool openZone(int zoneNumber, int seconds);

    /** @brief Closes every zone. Callable from any component; always takes precedence. */
    void allOff();

    /** @brief Returns the open zone number, or zero when none is open. */
    int openZoneNumber() const { return _openZone; }

    /** @brief Returns the seconds remaining on the open zone, or zero. */
    int secondsRemaining() const;

    /** @brief Sets how often the watchdog verifies line state against the deadline. */
    void setWatchdogInterval(const TimeSpan& value);

    /** @brief Stops the close timer without closing the zone. Test seam for the watchdog. */
    void disableCloseTimerForTest() { _closeTimer.stop(); }

    /** @brief Runs the close path immediately. Test seam so sequences do not wait on wall time. */
    void expireCloseTimerForTest() { _closeTimer.stop(); onCloseTimer(); }

    /** @brief Returns the text of the most recent failure. */
    QString errorText() const { return _errorText; }

signals:
    /** @brief Emitted after @p zoneNumber has been driven active for @p seconds. */
    void zoneOpened(int zoneNumber, int seconds);

    /** @brief Emitted after @p zoneNumber has been driven inactive. */
    void zoneClosed(int zoneNumber);

    /** @brief Emitted when the watchdog found @p zoneNumber open past its deadline. */
    void watchdogTripped(int zoneNumber);

private slots:
    void onCloseTimer();
    void onWatchdogTimer();

private:
    bool writeExclusive(int zoneNumber);

    IGpioBackend* _backend = nullptr;
    QMap<int, quint32> _zoneGpioMap;
    int _maxZoneSeconds = 3600;
    OutputBank* _bank = nullptr;
    QTimer _closeTimer;
    QTimer _watchdogTimer;
    int _openZone = 0;
    QDateTime _deadlineUtc;
    QString _errorText;
};

#endif // ZONECONTROLLER_H
