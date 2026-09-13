#ifndef STATUSJSON_H
#define STATUSJSON_H

#include <QDateTime>
#include <QJsonObject>

struct ServerStatus;

/** @brief Converts ServerStatus into the wire shape decoded by the web client's Status type. */
class StatusJson
{
public:
    /** @brief Serializes @p status into the GET /admin/status response body. */
    static QJsonObject toJson(const ServerStatus& status);

private:
    static QString instantToJson(const QDateTime& value);
};

#endif // STATUSJSON_H
