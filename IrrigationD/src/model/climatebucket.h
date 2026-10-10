#ifndef CLIMATEBUCKET_H
#define CLIMATEBUCKET_H

#include <QDateTime>
#include <QList>

#include <optional>

/** @brief The readings that fell in one fixed-width time bucket, reduced to their minimum, mean and maximum. */
class ClimateBucket
{
public:
    /** @brief The start of the bucket, a whole multiple of the bucket width since the epoch. */
    QDateTime startUtc;
    int count = 0;
    double temperatureMin = 0;
    double temperatureMean = 0;
    double temperatureMax = 0;
    double humidityMin = 0;
    double humidityMean = 0;
    double humidityMax = 0;
};

typedef QList<ClimateBucket> ClimateBucketList;

/**
 * @brief Modelled weather over one fixed-width time bucket.
 *
 * precipitationMm and et0Mm total the hours that lie inside the bucket; the
 * temperature and humidity average the hourly values taken inside it. A measure
 * with no value in the bucket stays empty.
 */
class WeatherBucket
{
public:
    /** @brief The start of the bucket, a whole multiple of the bucket width since the epoch. */
    QDateTime startUtc;
    std::optional<double> precipitationMm;
    std::optional<double> et0Mm;
    std::optional<double> temperatureCelsius;
    std::optional<double> humidityPercent;
};

typedef QList<WeatherBucket> WeatherBucketList;

#endif // CLIMATEBUCKET_H
