#include "openmeteojson.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QTimeZone>
#include <QUrlQuery>

static std::optional<double> optionalNumber(const QJsonValue& value)
{
    if(value.isDouble() == false) {
        return std::nullopt;
    }
    return value.toDouble();
}

QUrl OpenMeteoJson::requestUrl(const QUrl& baseUrl, double latitude, double longitude)
{
    QUrlQuery query;
    query.addQueryItem("latitude", QString::number(latitude, 'f', 5));
    query.addQueryItem("longitude", QString::number(longitude, 'f', 5));
    query.addQueryItem("hourly", "precipitation,et0_fao_evapotranspiration,temperature_2m,relative_humidity_2m");
    query.addQueryItem("current", "precipitation,temperature_2m,relative_humidity_2m");
    query.addQueryItem("past_days", QString::number(PastDays));
    query.addQueryItem("forecast_days", "1");
    query.addQueryItem("timeformat", "unixtime");
    query.addQueryItem("timezone", "GMT");

    QUrl url(baseUrl);
    url.setQuery(query);
    return url;
}

bool OpenMeteoJson::parse(const QByteArray& body, const QDateTime& nowUtc, WeatherReport& report, QString& errorText)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if(parseError.error != QJsonParseError::NoError || document.isObject() == false) {
        errorText = "the response is not a JSON object";
        return false;
    }

    const QJsonObject root = document.object();
    if(root.value("error").toBool() == true) {
        errorText = root.value("reason").toString("the service reported an error");
        return false;
    }

    const QJsonObject hourly = root.value("hourly").toObject();
    const QJsonArray times = hourly.value("time").toArray();
    const QJsonArray precipitation = hourly.value("precipitation").toArray();
    const QJsonArray et0 = hourly.value("et0_fao_evapotranspiration").toArray();
    const QJsonArray temperature = hourly.value("temperature_2m").toArray();
    const QJsonArray humidity = hourly.value("relative_humidity_2m").toArray();
    if(times.isEmpty() || precipitation.count() != times.count() || et0.count() != times.count()
       || temperature.count() != times.count() || humidity.count() != times.count()) {
        errorText = "the hourly arrays are missing or of unequal length";
        return false;
    }

    WeatherHourList hours;
    const qint64 now = nowUtc.toSecsSinceEpoch();
    for(int i = 0; i < times.count(); i++) {
        if(times.at(i).isDouble() == false) {
            errorText = "an hourly time is not a number";
            return false;
        }
        const qint64 end = static_cast<qint64>(times.at(i).toDouble());
        if(end > now) {
            continue;
        }
        WeatherHour hour;
        hour.hourEndUtc = QDateTime::fromSecsSinceEpoch(end, QTimeZone::UTC);
        hour.precipitationMm = optionalNumber(precipitation.at(i));
        hour.et0Mm = optionalNumber(et0.at(i));
        hour.temperatureCelsius = optionalNumber(temperature.at(i));
        hour.humidityPercent = optionalNumber(humidity.at(i));
        hours.append(hour);
    }

    std::optional<WeatherCurrent> current;
    const QJsonObject currentObject = root.value("current").toObject();
    if(currentObject.value("time").isDouble()) {
        WeatherCurrent value;
        value.atUtc = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(currentObject.value("time").toDouble()),
                                                    QTimeZone::UTC);
        value.precipitationMm = optionalNumber(currentObject.value("precipitation"));
        value.temperatureCelsius = optionalNumber(currentObject.value("temperature_2m"));
        value.humidityPercent = optionalNumber(currentObject.value("relative_humidity_2m"));
        current = value;
    }

    report.hours = hours;
    report.current = current;
    return true;
}
