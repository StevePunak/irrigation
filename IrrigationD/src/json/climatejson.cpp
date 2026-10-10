#include "json/climatejson.h"

#include <QJsonArray>

static QJsonValue optionalJson(const std::optional<double>& value)
{
    return value.has_value() ? QJsonValue(value.value()) : QJsonValue(QJsonValue::Null);
}

QJsonObject ClimateJson::toJson(const ClimateBucketList& buckets, int bucketSeconds,
                                const WeatherBucketList& weather, int weatherBucketSeconds,
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

    QJsonArray weatherArray;
    for(const WeatherBucket& bucket : weather) {
        QJsonObject entry;
        entry["startUtc"] = bucket.startUtc.toUTC().toString(Qt::ISODate);
        entry["precipitationMm"] = optionalJson(bucket.precipitationMm);
        entry["et0Mm"] = optionalJson(bucket.et0Mm);
        entry["temperatureC"] = optionalJson(bucket.temperatureCelsius);
        entry["humidityPercent"] = optionalJson(bucket.humidityPercent);
        weatherArray.append(entry);
    }

    QJsonObject object;
    object["fromUtc"] = fromUtc.toUTC().toString(Qt::ISODate);
    object["toUtc"] = toUtc.toUTC().toString(Qt::ISODate);
    object["bucketSeconds"] = bucketSeconds;
    object["buckets"] = array;
    object["weather"] = QJsonObject{ {"bucketSeconds", weatherBucketSeconds}, {"buckets", weatherArray} };
    return object;
}
