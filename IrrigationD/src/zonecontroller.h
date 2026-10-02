#ifndef ZONECONTROLLER_H
#define ZONECONTROLLER_H

#include <QDeadlineTimer>
#include <QList>
#include <QMap>
#include <QObject>
#include <QTimer>

#include <Kanoop/timespan.h>
#include <Kanoop/utility/loggingbaseclass.h>
#include <Kanoop/pi/outputbank.h>

/**
 * @brief Sole owner of the valve outputs.
 *
 * Enforces five invariants regardless of caller:
 *  1. Concurrency cap — an open that would put more than maxConcurrentZones() zones
 *     open together fails. Every line change for one request goes out in one bank
 *     write. Re-opening an open zone takes no slot and resets its deadline.
 *  2. No open without a deadline — every open zone has its own single-shot close.
 *  3. Duration clamp — each request is clamped to the configured ceiling.
 *  4. Watchdog — a periodic tick reads the lines back and closes the bank when a
 *     zone is open past its deadline with no close pending, the lines differ from
 *     the expected open set, or the read-back fails. A zone found past its deadline
 *     with its close timer still armed is closed on the spot. A trip latches a fault
 *     that refuses every openZone() until a later tick finds no zone open and reads
 *     back exactly the expected set. A zone whose close failed inside the trip holds
 *     the latch until a retried close lands.
 *  5. Count check — the watchdog also trips when more lines read back asserted than
 *     the highest cap in force when the open zones were opened.
 *
 * A zone whose close write failed stays listed as open and is retried until the
 * close lands. Every later write drives its line inactive, and openZone() refuses
 * it. While a failed allOff() or watchdog close is pending, openZone() refuses
 * every zone.
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
    /** @brief Why a zone closed. */
    enum class CloseReason
    {
        Deadline,   ///< The zone's own close timer fired, or the watchdog found it due.
        Stopped,    ///< closeZone() closed the zone.
        AllOff,     ///< allOff() closed the zone with the rest of the bank.
        Watchdog    ///< The watchdog closed the bank.
    };
    Q_ENUM(CloseReason)

    /** @brief The cap in force until setMaxConcurrentZones() is called. */
    static constexpr int DefaultMaxConcurrentZones = 2;

    /** @brief The highest cap setMaxConcurrentZones() accepts. */
    static constexpr int MaxConcurrentZonesCeiling = 8;

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
     * @brief Opens @p zoneNumber for @p seconds alongside the zones already open.
     *
     * Re-opening an open zone writes nothing, takes no slot, and sets its deadline to
     * now plus the clamped duration.
     * @return True on success. False for an unknown zone, a non-positive duration, no
     *         free slot under the cap, a zone whose close is pending, a failed write,
     *         while a failed allOff() or watchdog close is pending, or while the
     *         watchdog fault latch is set.
     */
    bool openZone(int zoneNumber, int seconds);

    /**
     * @brief Closes @p zoneNumber, reporting CloseReason::Stopped.
     * @return True when the zone closed or was not open. False when the write failed;
     *         the close is then retried on the zone's own timer.
     */
    bool closeZone(int zoneNumber);

    /**
     * @brief Closes every zone, reporting CloseReason::AllOff. Callable from any component; always takes precedence.
     * @return True when every line was driven inactive, or when the lines are not
     *         requested, in which case nothing is written and the open zones are forgotten.
     *         False when the write failed; the whole bank is then retried in one write.
     */
    bool allOff();

    /** @brief Returns the open zone numbers in ascending order. */
    QList<int> openZoneNumbers() const { return _open.keys(); }

    /** @brief Returns whether @p zoneNumber is open. */
    bool isOpen(int zoneNumber) const { return _open.contains(zoneNumber); }

    /** @brief Returns whether openZone(@p zoneNumber) would fit under the cap: the zone is open already or fewer than the cap are open. */
    bool hasSlotFor(int zoneNumber) const { return _open.contains(zoneNumber) || _open.count() < _maxConcurrentZones; }

    /** @brief Returns the seconds remaining on @p zoneNumber, or zero when it is not open. */
    int secondsRemaining(int zoneNumber) const;

    /** @brief Returns the ceiling the next openZone() clamps to, in seconds. */
    int maxZoneSeconds() const { return _maxZoneSeconds; }

    /**
     * @brief Sets the ceiling the next openZone() clamps to, bounded to 1 through the constructor's hard ceiling.
     *
     * A zone already open keeps the deadline it opened with.
     */
    void setMaxZoneSeconds(int value);

    /** @brief Returns how many zones may be open together. */
    int maxConcurrentZones() const { return _maxConcurrentZones; }

    /**
     * @brief Sets how many zones may be open together, bounded to 1 through MaxConcurrentZonesCeiling.
     *
     * Lowering the cap closes nothing. It refuses new opens until the open count falls below it.
     */
    void setMaxConcurrentZones(int value);

    /** @brief Returns whether the watchdog fault latch is refusing openZone(). */
    bool isFaulted() const { return _faulted; }

    /** @brief Sets how often the watchdog verifies line state against the deadlines. */
    void setWatchdogInterval(const TimeSpan& value);

    /** @brief Stops @p zoneNumber's close timer without closing the zone. Test seam for the watchdog. */
    void disableCloseTimerForTest(int zoneNumber);

    /** @brief Runs @p zoneNumber's close path immediately, or the pending whole-bank retry that covers it. Test seam. */
    void expireCloseTimerForTest(int zoneNumber);

    /** @brief Returns whether @p zoneNumber's close timer, or a whole-bank retry that covers it, is armed. Test seam. */
    bool closeTimerActiveForTest(int zoneNumber) const;

    /** @brief Runs one watchdog check immediately. Test seam. */
    void triggerWatchdogForTest() { onWatchdogTimer(); }

    /** @brief Returns the text of the most recent failure. */
    QString errorText() const { return _errorText; }

signals:
    /** @brief Emitted after @p zoneNumber has been driven active, or re-opened, for @p seconds. */
    void zoneOpened(int zoneNumber, int seconds);

    /** @brief Emitted after @p zoneNumber has been driven inactive, with the reason it closed. */
    void zoneClosed(int zoneNumber, ZoneController::CloseReason reason);

    /**
     * @brief Emitted after the watchdog closed the bank.
     *
     * The watchdog trips when a zone is open past its deadline, when the lines read
     * back different from the expected open set, when more lines are asserted than the
     * cap allows, or when the read-back fails. @p zoneNumbers holds the zones that were
     * open, possibly none. Never emitted when the close inside the trip failed.
     */
    void watchdogTripped(const QList<int>& zoneNumbers);

private slots:
    void onWatchdogTimer();

private:
    class OpenZone
    {
    public:
        QDeadlineTimer deadline;
        int capAtOpen = 0;
        CloseReason pendingReason = CloseReason::Deadline;
        bool closing = false;
    };

    QList<int> activeZoneNumbers() const;
    bool writeOpenSet(const QList<int>& zoneNumbers);
    bool closeOne(int zoneNumber, CloseReason reason);
    bool closeAll(CloseReason reason);
    void startCloseTimer(int zoneNumber, const TimeSpan& delay);
    void onCloseTimer(int zoneNumber);
    void onBankRetryTimer();
    void tripWatchdog();

    static const TimeSpan RetryInterval;
    static const TimeSpan DefaultWatchdogInterval;
    static const TimeSpan MinimumWatchdogInterval;

    QMap<int, quint32> _zoneGpioMap;
    int _hardMaxZoneSeconds = 3600;
    int _maxZoneSeconds = 3600;
    int _maxConcurrentZones = DefaultMaxConcurrentZones;
    OutputBank* _bank = nullptr;
    QMap<int, QTimer*> _closeTimers;
    QTimer _watchdogTimer;
    QTimer _bankRetryTimer;
    QMap<int, OpenZone> _open;
    bool _bankClosePending = false;
    CloseReason _bankCloseReason = CloseReason::AllOff;
    bool _faulted = false;
    QString _errorText;
};

#endif // ZONECONTROLLER_H
