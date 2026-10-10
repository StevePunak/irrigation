#include "json/statusjson.h"

#include "irrigationcontrolserver.h"

#include <QJsonArray>

QJsonObject StatusJson::toJson(const ServerStatus& status)
{
    QJsonArray running;
    for(const RunningZoneStatus& zone : status.running) {
        QJsonObject entry;
        entry["zone"] = zone.zone;
        entry["secondsRemaining"] = zone.secondsRemaining;
        entry["source"] = sourceToJson(zone.source);
        running.append(entry);
    }

    QJsonValue program = QJsonValue(QJsonValue::Null);
    if(status.programId != 0) {
        QJsonArray waitingZones;
        for(int zoneNumber : status.waitingZones) {
            waitingZones.append(zoneNumber);
        }

        QJsonObject entry;
        entry["id"] = status.programId;
        entry["name"] = status.programName;
        entry["step"] = status.programStep;
        entry["stepCount"] = status.programStepCount;
        entry["waitingZones"] = waitingZones;
        program = entry;
    }

    QJsonArray queue;
    for(const QueuedProgramStatus& waiting : status.queue) {
        QJsonObject entry;
        entry["programId"] = waiting.programId;
        entry["name"] = waiting.name;
        entry["queuedAtUtc"] = instantToJson(waiting.queuedAtUtc);
        queue.append(entry);
    }

    QJsonObject object;
    object["running"] = running;
    object["program"] = program;
    object["queue"] = queue;
    object["maxConcurrentZones"] = status.maxConcurrentZones;
    object["nextRunUtc"] = instantToJson(status.nextRunUtc);
    object["timezone"] = status.timezone;
    object["masterEnabled"] = status.masterEnabled;
    object["stopHeld"] = status.stopHeld;
    object["rainDelayUntilUtc"] = instantToJson(status.rainDelayUntilUtc);

    QJsonValue climate = QJsonValue(QJsonValue::Null);
    if(status.climateConfigured == true) {
        QJsonObject entry;
        entry["temperatureC"] = status.climateFresh ? QJsonValue(status.temperatureCelsius) : QJsonValue(QJsonValue::Null);
        entry["humidityPercent"] = status.climateFresh ? QJsonValue(status.humidityPercent) : QJsonValue(QJsonValue::Null);
        climate = entry;
    }
    object["climate"] = climate;
    return object;
}

QString StatusJson::sourceToJson(RunningZoneStatus::Source value)
{
    switch(value) {
    case RunningZoneStatus::Source::Program:
        return "program";
    case RunningZoneStatus::Source::Panel:
        return "panel";
    case RunningZoneStatus::Source::Manual:
        break;
    }
    return "manual";
}

QString StatusJson::instantToJson(const QDateTime& value)
{
    return value.isValid() ? value.toUTC().toString(Qt::ISODate) : QString();
}
