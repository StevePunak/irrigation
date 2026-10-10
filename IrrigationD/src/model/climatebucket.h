#ifndef CLIMATEBUCKET_H
#define CLIMATEBUCKET_H

#include <QDateTime>
#include <QList>

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

#endif // CLIMATEBUCKET_H
