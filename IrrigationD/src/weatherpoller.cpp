#include "weatherpoller.h"

#include "database/climatedatasource.h"
#include "json/openmeteojson.h"

#include <QNetworkReply>
#include <QNetworkRequest>

WeatherPoller::WeatherPoller(ClimateDataSource* store, IClock* clock, QObject* parent) :
    QObject(parent),
    LoggingBaseClass("weather"),
    _store(store),
    _clock(clock),
    _baseUrl("https://api.open-meteo.com/v1/forecast")
{
    _timer.setInterval(PollSeconds * 1000);
    connect(&_timer, &QTimer::timeout, this, &WeatherPoller::poll);
}

void WeatherPoller::setLocation(double latitude, double longitude)
{
    if(_hasLocation == true && latitude == _latitude && longitude == _longitude) {
        return;
    }

    abandonReply();
    _latitude = latitude;
    _longitude = longitude;
    _hasLocation = true;
    logText(LVL_INFO, QString("Polling Open-Meteo for %1, %2 every %3 minutes")
                          .arg(latitude, 0, 'f', 5).arg(longitude, 0, 'f', 5).arg(PollSeconds / 60));
    _timer.start();
    poll();
}

void WeatherPoller::clearLocation()
{
    if(_hasLocation == false) {
        return;
    }

    abandonReply();
    _timer.stop();
    _hasLocation = false;
    logText(LVL_INFO, "No location is set, so weather polling is off");
}

void WeatherPoller::poll()
{
    if(_hasLocation == false || _reply.isNull() == false) {
        return;
    }

    QNetworkRequest request(OpenMeteoJson::requestUrl(_baseUrl, _latitude, _longitude));
    request.setHeader(QNetworkRequest::UserAgentHeader, "irrigationd");
    request.setTransferTimeout(TimeoutSeconds * 1000);

    const double latitude = _latitude;
    const double longitude = _longitude;
    QNetworkReply* reply = _network.get(request);
    _reply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, latitude, longitude]()
    {
        onReplyFinished(reply, latitude, longitude);
    });
}

void WeatherPoller::onReplyFinished(QNetworkReply* reply, double latitude, double longitude)
{
    reply->deleteLater();
    if(_reply == reply) {
        _reply.clear();
    }

    const QByteArray body = reply->readAll();
    WeatherReport report;
    report.latitude = latitude;
    report.longitude = longitude;
    report.fetchedAtUtc = _clock->nowUtc();

    QString errorText;
    if(OpenMeteoJson::parse(body, report.fetchedAtUtc, report, errorText) == false) {
        if(reply->error() != QNetworkReply::NoError) {
            errorText = QString("%1 (%2)").arg(reply->errorString(), errorText);
        }
        noteFailure(errorText);
        emit pollFinished(false);
        return;
    }

    if(_store->upsertWeather(report) == false) {
        noteFailure(QString("failed to store the weather: %1").arg(_store->errorText()));
        emit pollFinished(false);
        return;
    }

    if(_failing) {
        _failing = false;
        logText(LVL_INFO, QString("Open-Meteo is answering again; stored %1 hours").arg(report.hours.count()));
    }
    else {
        logText(LVL_DEBUG, QString("Stored %1 hours of weather").arg(report.hours.count()));
    }
    emit pollFinished(true);
}

void WeatherPoller::abandonReply()
{
    if(_reply.isNull()) {
        return;
    }

    QNetworkReply* reply = _reply.data();
    _reply.clear();
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

void WeatherPoller::noteFailure(const QString& text)
{
    if(_failing == false) {
        _failing = true;
        logText(LVL_WARNING, QString("Weather poll failed: %1").arg(text));
    }
}
