#ifndef FIREDINSTANT_H
#define FIREDINSTANT_H

#include <Kanoop/kanoopcommon.h>

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

    int id = 0;
    int programId = 0;
    int startTimeId = 0;
    QDateTime scheduledAtUtc;
    Outcome outcome = Outcome::Ran;

    /** @brief Returns true when this record came from the database. */
    bool isValid() const { return id > 0; }

private:
    class OutcomeToStringMap : public KANOOP::EnumToStringMap<Outcome>
    {
    public:
        OutcomeToStringMap()
        {
            insert(Outcome::Ran,         "ran");
            insert(Outcome::SkippedBusy, "skipped_busy");
            insert(Outcome::SkippedRain, "skipped_rain");
            insert(Outcome::Missed,      "missed");
        }
    };

    static const OutcomeToStringMap _OutcomeToStringMap;
};

#endif // FIREDINSTANT_H
