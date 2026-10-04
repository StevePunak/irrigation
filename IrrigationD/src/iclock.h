#ifndef ICLOCK_H
#define ICLOCK_H

#include <QDateTime>
#include <QElapsedTimer>

/** @brief Source of the current instant and of a millisecond count that never steps. */
class IClock
{
public:
    virtual ~IClock() {}

    /** @brief Returns the current instant in UTC. */
    virtual QDateTime nowUtc() const = 0;

    /**
     * @brief Returns milliseconds on a clock that never steps.
     *
     * @warning Measure intervals with this. nowUtc() steps whenever the RTC or NTP
     *          corrects the system clock.
     */
    virtual qint64 monotonicMsecs() const = 0;
};

/** @brief Clock backed by the system time. */
class SystemClock : public IClock
{
public:
    /** @brief Constructs a clock whose monotonic count starts at zero now. */
    SystemClock() { _elapsed.start(); }

    /** @brief Returns the system time in UTC. */
    virtual QDateTime nowUtc() const override { return QDateTime::currentDateTimeUtc(); }

    /** @brief Returns the milliseconds since construction on the system's monotonic clock. */
    virtual qint64 monotonicMsecs() const override { return _elapsed.elapsed(); }

private:
    QElapsedTimer _elapsed;
};

/** @brief Clock the test drives by hand. */
class TestClock : public IClock
{
public:
    /** @brief Constructs a clock reading @p start, with a monotonic count of zero. */
    explicit TestClock(const QDateTime& start) : _now(start.toUTC()) {}

    /** @brief Returns the instant this clock was set or advanced to. */
    virtual QDateTime nowUtc() const override { return _now; }

    /** @brief Returns the milliseconds this clock has been advanced by. */
    virtual qint64 monotonicMsecs() const override { return _monotonicMsecs; }

    /** @brief Sets the instant this clock reports and leaves the monotonic count alone, as a system clock step does. */
    void setNowUtc(const QDateTime& value) { _now = value.toUTC(); }

    /** @brief Moves the wall time and the monotonic count forward by @p seconds. */
    void advance(qint64 seconds) { advanceMsecs(seconds * 1000); }

    /** @brief Moves the wall time and the monotonic count forward by @p msecs. */
    void advanceMsecs(qint64 msecs)
    {
        _now = _now.addMSecs(msecs);
        _monotonicMsecs += msecs;
    }

private:
    QDateTime _now;
    qint64 _monotonicMsecs = 0;
};

#endif // ICLOCK_H
