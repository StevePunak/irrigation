#include "json/climatejson.h"

#include <QJsonArray>

QJsonObject ClimateJson::toJson(const ClimateBucketList& buckets, int bucketSeconds,
                                const QDateTime& fromUtc, const QDateTime& toUtc)
{
    QJsonArray array;
    for(const ClimateBucket& bucket : buckets) {
        QJsonObject entry;
        entry["startUtc"] = bucket.startUtc.toUTC().toString(Qt::ISODate);
        entry["count"] = bucket.count;
        entry["temperatureC"] = QJsonObject{
            {"min", bucket.temperatureMin}, {"mean", bucket.temperatureMean}, {"max", bucket.temperatureMax}
        };
        entry["humidityPercent"] = QJsonObject{
            {"min", bucket.humidityMin}, {"mean", bucket.humidityMean}, {"max", bucket.humidityMax}
        };
        array.append(entry);
    }

    QJsonObject object;
    object["fromUtc"] = fromUtc.toUTC().toString(Qt::ISODate);
    object["toUtc"] = toUtc.toUTC().toString(Qt::ISODate);
    object["bucketSeconds"] = bucketSeconds;
    object["buckets"] = array;
    return object;
}
