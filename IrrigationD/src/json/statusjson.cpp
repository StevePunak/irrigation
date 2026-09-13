#include "json/statusjson.h"

#include "irrigationcontrolserver.h"

QJsonObject StatusJson::toJson(const ServerStatus& status)
{
    QJsonObject object;
    object["runningZone"] = status.runningZone;
    object["secondsRemaining"] = status.secondsRemaining;
    object["nextRunUtc"] = instantToJson(status.nextRunUtc);
    object["timezone"] = status.timezone;
    object["masterEnabled"] = status.masterEnabled;
    object["rainDelayUntilUtc"] = instantToJson(status.rainDelayUntilUtc);
    return object;
}

QString StatusJson::instantToJson(const QDateTime& value)
{
    return value.isValid() ? value.toUTC().toString(Qt::ISODate) : QString();
}
