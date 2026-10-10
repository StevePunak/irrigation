#ifndef WEATHERREPORT_H
#define WEATHERREPORT_H

#include <QDateTime>
#include <QList>

#include <optional>

/**
 * @brief One finished hour of modelled weather.
 *
 * precipitationMm and et0Mm are totals for the hour ending at hourEndUtc; the
 * temperature and humidity are the values at hourEndUtc. A value the service
 * left null stays empty.
 */
class WeatherHour
{
public:
    QDateTime hourEndUtc;
    std::optional<double> precipitationMm;
    std::optional<double> et0Mm;
    std::optional<double> temperatureCelsius;
    std::optional<double> humidityPercent;
};

typedef QList<WeatherHour> WeatherHourList;

/**
 * @brief The service's current conditions: one 15-minute step.
 *
 * precipitationMm is the total for the 15 minutes ending at atUtc.
 */
class WeatherCurrent
{
public:
    QDateTime atUtc;
    std::optional<double> precipitationMm;
    std::optional<double> temperatureCelsius;
    std::optional<double> humidityPercent;
};

/** @brief Everything one weather fetch returned, for the location it was asked about. */
class WeatherReport
{
public:
    double latitude = 0;
    double longitude = 0;
    QDateTime fetchedAtUtc;
    WeatherHourList hours;
    std::optional<WeatherCurrent> current;
};

#endif // WEATHERREPORT_H
