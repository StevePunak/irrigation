#ifndef PROGRAMJSON_H
#define PROGRAMJSON_H

#include "model/program.h"
#include "model/programstarttime.h"
#include "model/programstep.h"

#include <QDate>
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

/**
 * @brief Converts between Program (with its start times and steps) and the wire
 *        shape decoded by the web client's Program and ProgramDraft types.
 */
class ProgramJson
{
public:
    /** @brief Serializes @p program, its start times, steps and computed next run into a full program object. Each step carries its stored id. */
    static QJsonObject toJson(const Program& program,
                              const ProgramStartTimeList& startTimes,
                              const ProgramStepList& steps,
                              const QDateTime& nextRunUtc);

    /**
     * @brief Parses a program draft payload into its program, start-time and step parts.
     *
     * Rejects a dayMode that is not one of the four registered names, a
     * minutesAfterMidnight outside 0..1439, an EveryNDays rule without an
     * intervalDays of at least 1 and a valid anchorDate, a DaysOfWeek dowMask that
     * is zero or sets a bit outside the seven days, an enabled that is present
     * but not a boolean, and a step with no zones, a zone listed twice, a non-numeric
     * zone id, or a durationSeconds below 1. An absent enabled means true. Each step's
     * sequence is its array position plus one.
     * @param errorMessage Set to a human-readable reason when parsing fails.
     * @return True when @p object matched the contract and every value validated.
     */
    static bool fromJson(const QJsonObject& object,
                         Program& program,
                         ProgramStartTimeList& startTimes,
                         ProgramStepList& steps,
                         QString& errorMessage);

private:
    static QJsonObject toJson(const ProgramStartTime& startTime);
    static QJsonObject toJson(const ProgramStep& step);

    static bool startTimeFromJson(const QJsonObject& object, ProgramStartTime& startTime, QString& errorMessage);
    static bool stepFromJson(const QJsonObject& object, ProgramStep& step, QString& errorMessage);

    static bool isValidDayModeName(const QString& name);

    static QString instantToJson(const QDateTime& value);
    static QString dateToJson(const QDate& value);

    static const QStringList ValidDayModeNames;
    static constexpr int AllDaysOfWeekMask = 0x7F;
};

#endif // PROGRAMJSON_H
