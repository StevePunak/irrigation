#ifndef WEATHERPOLLER_H
#define WEATHERPOLLER_H

#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <Kanoop/utility/loggingbaseclass.h>

#include "iclock.h"

class ClimateDataSource;
class QNetworkReply;

/**
 * @brief Fetches modelled weather for the yard from Open-Meteo on a fixed interval and stores it.
 *
 * Idle until setLocation() gives it a point. Each poll writes the finished hours of the
 * last OpenMeteoJson::PastDays days and the current 15-minute conditions to the climate
 * database, replacing any rows already stored for the same times. A failed poll writes
 * nothing; the next one fetches the same days again. The first failure after a good poll
 * is logged once, and so is the recovery.
 */
class WeatherPoller : public QObject,
                      public LoggingBaseClass
{
    Q_OBJECT
public:
    /** @brief Seconds between polls; Open-Meteo's finest data step. */
    static constexpr int PollSeconds = 900;

    /** @brief Seconds a request may take before it is abandoned. */
    static constexpr int TimeoutSeconds = 30;

    /**
     * @brief Constructs an idle poller.
     * @param store Where weather is written. Not owned.
     * @param clock Decides which hours have finished and stamps each fetch.
     * @param parent The Qt parent object.
     */
    WeatherPoller(ClimateDataSource* store, IClock* clock, QObject* parent = nullptr);

    /** @brief Sets the forecast endpoint. The default is Open-Meteo's public forecast API. */
    void setBaseUrl(const QUrl& value) { _baseUrl = value; }

    /** @brief Points the poller at @p latitude, @p longitude. A changed point is polled at once. */
    void setLocation(double latitude, double longitude);

    /** @brief Stops polling and abandons any request in flight. */
    void clearLocation();

    /** @brief Returns whether a location is set. */
    bool hasLocation() const { return _hasLocation; }

public slots:
    /** @brief Requests the weather for the current location. Does nothing without one or while a request is in flight. */
    void poll();

signals:
    /** @brief Emitted when a poll ends; @p stored is true when its weather was written. */
    void pollFinished(bool stored);

private:
    void onReplyFinished(QNetworkReply* reply, double latitude, double longitude);
    void abandonReply();
    void noteFailure(const QString& text);

    ClimateDataSource* _store = nullptr;
    IClock* _clock = nullptr;
    QNetworkAccessManager _network;
    QTimer _timer;
    QUrl _baseUrl;
    QPointer<QNetworkReply> _reply;
    double _latitude = 0;
    double _longitude = 0;
    bool _hasLocation = false;
    bool _failing = false;
};

#endif // WEATHERPOLLER_H
