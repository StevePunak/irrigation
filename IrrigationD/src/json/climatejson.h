#ifndef CLIMATEJSON_H
#define CLIMATEJSON_H

#include <QDateTime>
#include <QJsonObject>

#include "model/climatebucket.h"

/** @brief Converts climate history into the wire shape decoded by the web client's ClimateHistory type. */
class ClimateJson
{
public:
    /** @brief Serializes @p buckets, @p bucketSeconds wide and covering [@p fromUtc, @p toUtc), into the GET /admin/climate response body. */
    static QJsonObject toJson(const ClimateBucketList& buckets, int bucketSeconds,
                              const QDateTime& fromUtc, const QDateTime& toUtc);
};

#endif // CLIMATEJSON_H
