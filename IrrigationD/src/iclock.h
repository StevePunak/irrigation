#ifndef ICLOCK_H
#define ICLOCK_H

#include <QDateTime>

/** @brief Source of the current instant. */
class IClock
{
public:
    virtual ~IClock() {}

    /** @brief Returns the current instant in UTC. */
    virtual QDateTime nowUtc() const = 0;
};

/** @brief Clock backed by the system time. */
class SystemClock : public IClock
{
public:
    virtual QDateTime nowUtc() const override { return QDateTime::currentDateTimeUtc(); }
};

/** @brief Clock the test drives by hand. */
class TestClock : public IClock
{
public:
    /** @brief Constructs a clock reading @p start. */
    explicit TestClock(const QDateTime& start) : _now(start.toUTC()) {}

    virtual QDateTime nowUtc() const override { return _now; }

    /** @brief Sets the instant this clock reports. */
    void setNowUtc(const QDateTime& value) { _now = value.toUTC(); }

    /** @brief Moves the clock forward by @p seconds. */
    void advance(qint64 seconds) { _now = _now.addSecs(seconds); }

private:
    QDateTime _now;
};

#endif // ICLOCK_H
