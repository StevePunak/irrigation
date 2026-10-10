#ifndef CLIMATEREADING_H
#define CLIMATEREADING_H

#include <QDateTime>
#include <QList>

/** @brief One temperature and relative humidity sample. */
class ClimateReading
{
public:
    QDateTime atUtc;
    double temperatureCelsius = 0;
    double humidityPercent = 0;
};

typedef QList<ClimateReading> ClimateReadingList;

#endif // CLIMATEREADING_H
