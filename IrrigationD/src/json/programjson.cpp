#include "json/programjson.h"

#include <QJsonArray>
#include <QJsonValue>

const QStringList ProgramJson::ValidDayModeNames = { "DaysOfWeek", "Odd", "Even", "EveryNDays" };

QJsonObject ProgramJson::toJson(const Program& program,
                                const ProgramStartTimeList& startTimes,
                                const ProgramZoneList& zones,
                                const QDateTime& nextRunUtc)
{
    QJsonArray startTimesArray;
    for(const ProgramStartTime& startTime : startTimes) {
        startTimesArray.append(toJson(startTime));
    }

    QJsonArray zonesArray;
    for(const ProgramZone& zone : zones) {
        zonesArray.append(toJson(zone));
    }

    QJsonObject object;
    object["id"] = program.id;
    object["name"] = program.name;
    object["enabled"] = program.enabled;
    object["dayMode"] = Program::dayModeToString(program.dayMode);
    object["dowMask"] = program.dowMask;
    object["intervalDays"] = program.intervalDays;
    object["anchorDate"] = dateToJson(program.anchorDate);
    object["startTimes"] = startTimesArray;
    object["zones"] = zonesArray;
    object["nextRunUtc"] = instantToJson(nextRunUtc);
    return object;
}

QJsonObject ProgramJson::toJson(const ProgramStartTime& startTime)
{
    QJsonObject object;
    object["id"] = startTime.id;
    object["minutesAfterMidnight"] = startTime.minutesAfterMidnight;
    object["timezone"] = startTime.timezone;
    return object;
}

QJsonObject ProgramJson::toJson(const ProgramZone& zone)
{
    QJsonObject object;
    object["id"] = zone.id;
    object["zoneId"] = zone.zoneId;
    object["sequence"] = zone.sequence;
    object["durationSeconds"] = zone.durationSeconds;
    return object;
}

bool ProgramJson::fromJson(const QJsonObject& object,
                           Program& program,
                           ProgramStartTimeList& startTimes,
                           ProgramZoneList& zones,
                           QString& errorMessage)
{
    if(object.value("name").isString() == false) {
        errorMessage = "name must be a string";
        return false;
    }

    if(object.value("dayMode").isString() == false) {
        errorMessage = "dayMode must be a string";
        return false;
    }

    const QString dayModeName = object.value("dayMode").toString();
    if(isValidDayModeName(dayModeName) == false) {
        errorMessage = QString("unknown day mode '%1'").arg(dayModeName);
        return false;
    }

    if(object.value("startTimes").isArray() == false) {
        errorMessage = "startTimes must be an array";
        return false;
    }

    if(object.value("zones").isArray() == false) {
        errorMessage = "zones must be an array";
        return false;
    }

    Program parsedProgram;
    parsedProgram.name = object.value("name").toString();
    parsedProgram.enabled = object.value("enabled").toBool(true);
    parsedProgram.dayMode = Program::dayModeFromString(dayModeName);
    parsedProgram.dowMask = object.value("dowMask").toInt(0);
    parsedProgram.intervalDays = object.value("intervalDays").toInt(0);

    const QJsonValue anchor = object.value("anchorDate");
    if(anchor.isString() && anchor.toString().isEmpty() == false) {
        parsedProgram.anchorDate = QDate::fromString(anchor.toString(), Qt::ISODate);
        if(parsedProgram.anchorDate.isValid() == false) {
            errorMessage = "anchorDate must be an ISO 8601 date";
            return false;
        }
    }

    ProgramStartTimeList parsedStartTimes;
    const QJsonArray startTimesArray = object.value("startTimes").toArray();
    for(const QJsonValue& value : startTimesArray) {
        if(value.isObject() == false) {
            errorMessage = "each start time must be an object";
            return false;
        }

        ProgramStartTime startTime;
        if(startTimeFromJson(value.toObject(), startTime, errorMessage) == false) {
            return false;
        }
        parsedStartTimes.append(startTime);
    }

    ProgramZoneList parsedZones;
    const QJsonArray zonesArray = object.value("zones").toArray();
    for(const QJsonValue& value : zonesArray) {
        if(value.isObject() == false) {
            errorMessage = "each zone must be an object";
            return false;
        }

        ProgramZone zone;
        if(zoneFromJson(value.toObject(), zone, errorMessage) == false) {
            return false;
        }
        parsedZones.append(zone);
    }

    program = parsedProgram;
    startTimes = parsedStartTimes;
    zones = parsedZones;
    return true;
}

bool ProgramJson::startTimeFromJson(const QJsonObject& object, ProgramStartTime& startTime, QString& errorMessage)
{
    if(object.value("minutesAfterMidnight").isDouble() == false) {
        errorMessage = "start times require a numeric minutesAfterMidnight";
        return false;
    }

    if(object.value("timezone").isString() == false) {
        errorMessage = "start times require a timezone";
        return false;
    }

    const int minutesAfterMidnight = object.value("minutesAfterMidnight").toInt();
    if(isValidMinutesAfterMidnight(minutesAfterMidnight) == false) {
        errorMessage = QString("minutesAfterMidnight %1 is out of range").arg(minutesAfterMidnight);
        return false;
    }

    startTime.minutesAfterMidnight = minutesAfterMidnight;
    startTime.timezone = object.value("timezone").toString();
    return true;
}

bool ProgramJson::zoneFromJson(const QJsonObject& object, ProgramZone& zone, QString& errorMessage)
{
    if(object.value("zoneId").isDouble() == false) {
        errorMessage = "zone entries require a numeric zoneId";
        return false;
    }

    if(object.value("sequence").isDouble() == false) {
        errorMessage = "zone entries require a numeric sequence";
        return false;
    }

    if(object.value("durationSeconds").isDouble() == false) {
        errorMessage = "zone entries require a numeric durationSeconds";
        return false;
    }

    zone.zoneId = object.value("zoneId").toInt();
    zone.sequence = object.value("sequence").toInt();
    zone.durationSeconds = object.value("durationSeconds").toInt();
    return true;
}

bool ProgramJson::isValidDayModeName(const QString& name)
{
    return ValidDayModeNames.contains(name);
}

bool ProgramJson::isValidMinutesAfterMidnight(int minutesAfterMidnight)
{
    return minutesAfterMidnight >= 0 && minutesAfterMidnight < 24 * 60;
}

QString ProgramJson::instantToJson(const QDateTime& value)
{
    return value.isValid() ? value.toUTC().toString(Qt::ISODate) : QString();
}

QString ProgramJson::dateToJson(const QDate& value)
{
    return value.isValid() ? value.toString(Qt::ISODate) : QString();
}
