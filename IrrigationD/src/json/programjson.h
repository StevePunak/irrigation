#ifndef PROGRAMJSON_H
#define PROGRAMJSON_H

#include "model/program.h"
#include "model/programstarttime.h"
#include "model/programzone.h"

#include <QDate>
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

/**
 * @brief Converts between Program (with its start times and zones) and the wire
 *        shape decoded by the web client's Program and ProgramDraft types.
 */
class ProgramJson
{
public:
    /** @brief Serializes @p program, its start times, zones and computed next run into a full program object. */
    static QJsonObject toJson(const Program& program,
                              const ProgramStartTimeList& startTimes,
                              const ProgramZoneList& zones,
                              const QDateTime& nextRunUtc);

    /**
     * @brief Parses a program draft payload into its program, start-time and zone parts.
     *
     * Rejects a dayMode that is not one of the four registered names, a
     * minutesAfterMidnight outside 0..1439, an EveryNDays rule without an
     * intervalDays of at least 1 and a valid anchorDate, a DaysOfWeek dowMask that
     * is zero or sets a bit outside the seven days, and an enabled that is present
     * but not a boolean. An absent enabled means true.
     * @param errorMessage Set to a human-readable reason when parsing fails.
     * @return True when @p object matched the contract and every value validated.
     */
    static bool fromJson(const QJsonObject& object,
                         Program& program,
                         ProgramStartTimeList& startTimes,
                         ProgramZoneList& zones,
                         QString& errorMessage);

private:
    static QJsonObject toJson(const ProgramStartTime& startTime);
    static QJsonObject toJson(const ProgramZone& zone);

    static bool startTimeFromJson(const QJsonObject& object, ProgramStartTime& startTime, QString& errorMessage);
    static bool zoneFromJson(const QJsonObject& object, ProgramZone& zone, QString& errorMessage);

    static bool isValidDayModeName(const QString& name);

    static QString instantToJson(const QDateTime& value);
    static QString dateToJson(const QDate& value);

    static const QStringList ValidDayModeNames;
    static constexpr int AllDaysOfWeekMask = 0x7F;
};

#endif // PROGRAMJSON_H
