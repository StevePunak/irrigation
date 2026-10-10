#ifndef STATUSJSON_H
#define STATUSJSON_H

#include <QDateTime>
#include <QJsonObject>

#include "irrigationcontrolserver.h"

/**
 * @brief Converts ServerStatus into the wire shape decoded by the web client's Status type.
 *
 * climate is null when no sensor is configured, and its two values are null while the
 * sensor has no fresh reading.
 */
class StatusJson
{
public:
    /** @brief Serializes @p status into the GET /admin/status response body. */
    static QJsonObject toJson(const ServerStatus& status);

    /**
     * @brief Returns the wire name for @p value.
     *
     * The names are the running[].source contract with the web client's RunSource type.
     */
    static QString sourceToJson(RunningZoneStatus::Source value);

private:
    static QString instantToJson(const QDateTime& value);
};

#endif // STATUSJSON_H
