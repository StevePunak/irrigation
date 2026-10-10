#ifndef OPENMETEOJSON_H
#define OPENMETEOJSON_H

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUrl>

#include "model/weatherreport.h"

/** @brief Builds Open-Meteo forecast requests and reads their responses. */
class OpenMeteoJson
{
public:
    /** @brief The hours of history each request asks for, so an outage up to about a day fills in. */
    static constexpr int PastDays = 2;

    /**
     * @brief Returns the forecast URL under @p baseUrl for the point @p latitude, @p longitude.
     *
     * @warning parse() depends on timeformat=unixtime and on the hourly and current variable
     *          lists here. Change them together.
     */
    static QUrl requestUrl(const QUrl& baseUrl, double latitude, double longitude);

    /**
     * @brief Reads a forecast response into @p report's hours and current conditions.
     *
     * Only hours ending at or before @p nowUtc are kept. A malformed body, an error
     * body, or hourly arrays of unequal length fail the parse.
     * @return False with @p errorText set when the body cannot be used.
     */
    static bool parse(const QByteArray& body, const QDateTime& nowUtc, WeatherReport& report, QString& errorText);
};

#endif // OPENMETEOJSON_H
