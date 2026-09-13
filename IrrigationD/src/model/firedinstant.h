#ifndef FIREDINSTANT_H
#define FIREDINSTANT_H

#include <QDateTime>
#include <QString>

/** @brief A historical record that a program start time fired, and what happened. */
class FiredInstant
{
public:
    /** @brief What happened when a scheduled start time came due. */
    enum class Outcome
    {
        Ran,
        SkippedBusy,
        SkippedRain,
        Missed
    };

    /** @brief Parses @p value into an Outcome. Returns Missed when unrecognised. */
    static Outcome outcomeFromString(const QString& value);

    /** @brief Returns the storage string for @p value. */
    static QString outcomeToString(Outcome value);

    /** @brief Primary key. Zero when not yet persisted. */
    int id = 0;

    /** @brief Owning program's id. */
    int programId = 0;

    /** @brief Start time that fired. */
    int startTimeId = 0;

    /** @brief The scheduled moment, in UTC. */
    QDateTime scheduledAtUtc;

    /** @brief What happened at that moment. */
    Outcome outcome = Outcome::Ran;

    /** @brief Returns true when this record came from the database. */
    bool isValid() const { return id > 0; }
};

#endif // FIREDINSTANT_H
