#include "json/programjson.h"

#include "scheduler.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QTimeZone>

const QStringList ProgramJson::ValidDayModeNames = { "DaysOfWeek", "Odd", "Even", "EveryNDays" };

QJsonObject ProgramJson::toJson(const Program& program,
                                const ProgramStartTimeList& startTimes,
                                const ProgramStepList& steps,
                                const QDateTime& nextRunUtc)
{
    QJsonArray startTimesArray;
    for(const ProgramStartTime& startTime : startTimes) {
        startTimesArray.append(toJson(startTime));
    }

    QJsonArray stepsArray;
    for(const ProgramStep& step : steps) {
        stepsArray.append(toJson(step));
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
    object["steps"] = stepsArray;
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

QJsonObject ProgramJson::toJson(const ProgramStep& step)
{
    QJsonArray zones;
    for(int zoneId : step.zoneIds) {
        zones.append(zoneId);
    }

    QJsonObject object;
    object["id"] = step.id;
    object["zones"] = zones;
    object["durationSeconds"] = step.durationSeconds;
    return object;
}

bool ProgramJson::fromJson(const QJsonObject& object,
                           Program& program,
                           ProgramStartTimeList& startTimes,
                           ProgramStepList& steps,
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

    if(object.value("steps").isArray() == false) {
        errorMessage = "steps must be an array";
        return false;
    }

    const QJsonValue enabled = object.value("enabled");
    if(enabled.isUndefined() == false && enabled.isBool() == false) {
        errorMessage = "enabled must be a boolean";
        return false;
    }

    Program parsedProgram;
    parsedProgram.name = object.value("name").toString();
    parsedProgram.enabled = enabled.toBool(true);
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

    if(parsedProgram.dayMode == Program::DayMode::EveryNDays) {
        if(parsedProgram.intervalDays < 1) {
            errorMessage = "EveryNDays requires an intervalDays of at least 1";
            return false;
        }
        if(parsedProgram.anchorDate.isValid() == false) {
            errorMessage = "EveryNDays requires an anchorDate";
            return false;
        }
    }

    if(parsedProgram.dayMode == Program::DayMode::DaysOfWeek) {
        if(parsedProgram.dowMask == 0 || (parsedProgram.dowMask & ~AllDaysOfWeekMask) != 0) {
            errorMessage = "DaysOfWeek requires a dowMask naming at least one day and no bit outside the seven days";
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

    ProgramStepList parsedSteps;
    const QJsonArray stepsArray = object.value("steps").toArray();
    for(const QJsonValue& value : stepsArray) {
        if(value.isObject() == false) {
            errorMessage = "each step must be an object";
            return false;
        }

        ProgramStep step;
        if(stepFromJson(value.toObject(), step, errorMessage) == false) {
            return false;
        }
        step.sequence = static_cast<int>(parsedSteps.count()) + 1;
        parsedSteps.append(step);
    }

    program = parsedProgram;
    startTimes = parsedStartTimes;
    steps = parsedSteps;
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
    if(Scheduler::isValidMinutesAfterMidnight(minutesAfterMidnight) == false) {
        errorMessage = QString("minutesAfterMidnight %1 is out of range").arg(minutesAfterMidnight);
        return false;
    }

    const QString timezone = object.value("timezone").toString();
    if(QTimeZone(timezone.toUtf8()).isValid() == false) {
        errorMessage = QString("unknown timezone '%1'").arg(timezone);
        return false;
    }

    startTime.minutesAfterMidnight = minutesAfterMidnight;
    startTime.timezone = timezone;
    return true;
}

bool ProgramJson::stepFromJson(const QJsonObject& object, ProgramStep& step, QString& errorMessage)
{
    if(object.value("zones").isArray() == false) {
        errorMessage = "steps require a zones array";
        return false;
    }

    const QJsonArray zones = object.value("zones").toArray();
    if(zones.isEmpty()) {
        errorMessage = "each step needs at least one zone";
        return false;
    }

    QList<int> zoneIds;
    for(const QJsonValue& zone : zones) {
        if(zone.isDouble() == false) {
            errorMessage = "step zones must be numeric zone ids";
            return false;
        }
        const int zoneId = zone.toInt();
        if(zoneIds.contains(zoneId)) {
            errorMessage = QString("a step lists zone id %1 twice").arg(zoneId);
            return false;
        }
        zoneIds.append(zoneId);
    }

    if(object.value("durationSeconds").isDouble() == false) {
        errorMessage = "steps require a numeric durationSeconds";
        return false;
    }

    const int durationSeconds = object.value("durationSeconds").toInt();
    if(durationSeconds < 1) {
        errorMessage = "durationSeconds must be positive";
        return false;
    }

    step.zoneIds = zoneIds;
    step.durationSeconds = durationSeconds;
    return true;
}

bool ProgramJson::isValidDayModeName(const QString& name)
{
    return ValidDayModeNames.contains(name);
}

QString ProgramJson::instantToJson(const QDateTime& value)
{
    return value.isValid() ? value.toUTC().toString(Qt::ISODate) : QString();
}

QString ProgramJson::dateToJson(const QDate& value)
{
    return value.isValid() ? value.toString(Qt::ISODate) : QString();
}
