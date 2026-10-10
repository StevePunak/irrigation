#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QUrlQuery>

#include "database/climatedatasource.h"
#include "iclock.h"
#include "json/openmeteojson.h"
#include "weatherpoller.h"

namespace
{
const QDateTime NOW = QDateTime(QDate(2026, 10, 10), QTime(15, 30, 0), QTimeZone::UTC);

qint64 hoursBeforeNow(int hours, int extraMinutes = 0)
{
    return NOW.toSecsSinceEpoch() - (30 * 60) - (hours - 1) * 3600 + extraMinutes * 60;
}

/** @brief A forecast body with hours ending 13:00, 14:00, 15:00 and 16:00 UTC; 16:00 has not finished at NOW. */
QByteArray forecastBody()
{
    const QJsonArray times{ hoursBeforeNow(3), hoursBeforeNow(2), hoursBeforeNow(1), hoursBeforeNow(0) };
    const QJsonObject hourly{
        { "time", times },
        { "precipitation", QJsonArray{ 0.0, 1.5, QJsonValue(QJsonValue::Null), 9.9 } },
        { "et0_fao_evapotranspiration", QJsonArray{ 0.35, 0.2, 0.1, 9.9 } },
        { "temperature_2m", QJsonArray{ 23.8, 21.0, 19.5, 99.0 } },
        { "relative_humidity_2m", QJsonArray{ 72, 80, 85, 99 } },
    };
    const QJsonObject current{
        { "time", NOW.toSecsSinceEpoch() - 15 * 60 },
        { "interval", 900 },
        { "precipitation", 0.4 },
        { "temperature_2m", 19.1 },
        { "relative_humidity_2m", 86 },
    };
    return QJsonDocument(QJsonObject{ { "hourly", hourly }, { "current", current } }).toJson(QJsonDocument::Compact);
}

int countRows(ClimateDataSource& source, const QString& table)
{
    QSqlQuery query(QSqlDatabase::database(source.connectionName()));
    if(query.exec(QString("SELECT COUNT(*) FROM %1").arg(table)) == false || query.next() == false) {
        return -1;
    }
    return query.value(0).toInt();
}

class FakeOpenMeteo
{
public:
    FakeOpenMeteo()
    {
        _server.route("/v1/forecast", QHttpServerRequest::Method::Get, [this](const QHttpServerRequest& request)
        {
            _requests.append(request.url());
            return QHttpServerResponse("application/json", _body, _status);
        });
    }

    bool start()
    {
        QTcpServer* tcp = new QTcpServer;
        if(tcp->listen(QHostAddress::LocalHost, 0) == false || _server.bind(tcp) == false) {
            delete tcp;
            return false;
        }
        _port = tcp->serverPort();
        return true;
    }

    QUrl baseUrl() const { return QUrl(QString("http://127.0.0.1:%1/v1/forecast").arg(_port)); }
    void respond(const QByteArray& body, QHttpServerResponder::StatusCode status) { _body = body; _status = status; }
    QList<QUrl> requests() const { return _requests; }

private:
    QHttpServer _server;
    QByteArray _body;
    QHttpServerResponder::StatusCode _status = QHttpServerResponder::StatusCode::Ok;
    QList<QUrl> _requests;
    quint16 _port = 0;
};
}

class TestWeather : public QObject
{
    Q_OBJECT

private slots:
    void requestUrlCarriesTheLocationAndVariables();
    void parseKeepsOnlyFinishedHoursAndNulls();
    void parseRejectsUnusableBodies_data();
    void parseRejectsUnusableBodies();
    void upsertReplacesARevisedHourAndStep();
    void bucketWeatherPutsTotalsWithTheirHourAndInstantsWithTheirTime();
    void migratesAVersion100DatabaseAndKeepsItsReadings();
    void pollerStoresWhatTheServiceReturns();
    void pollerStoresNothingFromAnErrorResponse();
    void pollerIsIdleWithoutALocation();
};

void TestWeather::requestUrlCarriesTheLocationAndVariables()
{
    const QUrl url = OpenMeteoJson::requestUrl(QUrl("https://api.open-meteo.com/v1/forecast"), 33.46349, -117.65823);
    QCOMPARE(url.host(), QString("api.open-meteo.com"));

    const QUrlQuery query(url);
    QCOMPARE(query.queryItemValue("latitude"), QString("33.46349"));
    QCOMPARE(query.queryItemValue("longitude"), QString("-117.65823"));
    QCOMPARE(query.queryItemValue("hourly"),
             QString("precipitation,et0_fao_evapotranspiration,temperature_2m,relative_humidity_2m"));
    QCOMPARE(query.queryItemValue("current"), QString("precipitation,temperature_2m,relative_humidity_2m"));
    QCOMPARE(query.queryItemValue("past_days"), QString("2"));
    QCOMPARE(query.queryItemValue("timeformat"), QString("unixtime"));
    QCOMPARE(query.queryItemValue("timezone"), QString("GMT"));
}

void TestWeather::parseKeepsOnlyFinishedHoursAndNulls()
{
    WeatherReport report;
    QString errorText;
    QVERIFY2(OpenMeteoJson::parse(forecastBody(), NOW, report, errorText), qPrintable(errorText));

    QCOMPARE(report.hours.count(), 3);
    QCOMPARE(report.hours.at(0).hourEndUtc, QDateTime(QDate(2026, 10, 10), QTime(13, 0), QTimeZone::UTC));
    QCOMPARE(report.hours.at(2).hourEndUtc, QDateTime(QDate(2026, 10, 10), QTime(15, 0), QTimeZone::UTC));
    QCOMPARE(report.hours.at(1).precipitationMm.value(), 1.5);
    QCOMPARE(report.hours.at(1).et0Mm.value(), 0.2);
    QCOMPARE(report.hours.at(1).temperatureCelsius.value(), 21.0);
    QCOMPARE(report.hours.at(1).humidityPercent.value(), 80.0);
    QVERIFY(report.hours.at(2).precipitationMm.has_value() == false);
    QCOMPARE(report.hours.at(2).et0Mm.value(), 0.1);

    QVERIFY(report.current.has_value());
    QCOMPARE(report.current->atUtc, QDateTime(QDate(2026, 10, 10), QTime(15, 15), QTimeZone::UTC));
    QCOMPARE(report.current->precipitationMm.value(), 0.4);
    QCOMPARE(report.current->humidityPercent.value(), 86.0);
}

void TestWeather::parseRejectsUnusableBodies_data()
{
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<QString>("expectedError");

    QTest::newRow("not JSON") << QByteArray("<html>Bad Gateway</html>") << QString("not a JSON object");
    QTest::newRow("an error body") << QByteArray(R"({"error":true,"reason":"Latitude must be in range of -90 to 90°."})")
                                   << QString("Latitude must be in range");
    QTest::newRow("no hourly block") << QByteArray(R"({"current":{"time":1}})") << QString("unequal length");
    QTest::newRow("a short array")
        << QByteArray(R"({"hourly":{"time":[1,2],"precipitation":[0],"et0_fao_evapotranspiration":[0,0],)"
                      R"("temperature_2m":[0,0],"relative_humidity_2m":[0,0]}})")
        << QString("unequal length");
}

void TestWeather::parseRejectsUnusableBodies()
{
    QFETCH(QByteArray, body);
    QFETCH(QString, expectedError);

    WeatherReport report;
    QString errorText;
    QVERIFY(OpenMeteoJson::parse(body, NOW, report, errorText) == false);
    QVERIFY2(errorText.contains(expectedError), qPrintable(errorText));
    QVERIFY(report.hours.isEmpty());
}

void TestWeather::upsertReplacesARevisedHourAndStep()
{
    QTemporaryDir dir;
    ClimateDataSource source(dir.filePath("climate.db"));
    QVERIFY(source.open());

    WeatherReport first;
    first.latitude = 33.46349;
    first.longitude = -117.65823;
    first.fetchedAtUtc = NOW;
    QString errorText;
    QVERIFY(OpenMeteoJson::parse(forecastBody(), NOW, first, errorText));
    QVERIFY(source.upsertWeather(first));

    WeatherReport revised = first;
    revised.fetchedAtUtc = NOW.addSecs(900);
    revised.hours = { first.hours.at(1) };
    revised.hours[0].precipitationMm = 2.25;
    revised.current->precipitationMm = 0.8;
    QVERIFY(source.upsertWeather(revised));

    const WeatherHourList stored = source.weatherHoursBetween(NOW.addDays(-1), NOW);
    QCOMPARE(stored.count(), 3);
    QCOMPARE(stored.at(0).precipitationMm.value(), 0.0);
    QCOMPARE(stored.at(1).precipitationMm.value(), 2.25);
    QVERIFY(stored.at(2).precipitationMm.has_value() == false);
    QCOMPARE(stored.at(2).temperatureCelsius.value(), 19.5);

    QCOMPARE(countRows(source, "weather_current"), 1);
    QSqlQuery query(QSqlDatabase::database(source.connectionName()));
    QVERIFY(query.exec("SELECT precipitation_mm, latitude, fetched_at_utc FROM weather_current"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toDouble(), 0.8);
    QCOMPARE(query.value(1).toDouble(), 33.46349);
    QCOMPARE(query.value(2).toLongLong(), NOW.addSecs(900).toSecsSinceEpoch());
}

void TestWeather::bucketWeatherPutsTotalsWithTheirHourAndInstantsWithTheirTime()
{
    const QDateTime noon(QDate(2026, 10, 10), QTime(12, 0), QTimeZone::UTC);
    WeatherHourList hours;
    for(int i = 1; i <= 4; i++) {
        WeatherHour hour;
        hour.hourEndUtc = noon.addSecs(i * 3600);
        hour.precipitationMm = i * 1.0;
        hour.et0Mm = 0.5;
        hour.temperatureCelsius = 20.0 + i;
        hour.humidityPercent = i == 2 ? std::optional<double>() : std::optional<double>(60.0);
        hours.append(hour);
    }

    const WeatherBucketList hourly = ClimateDataSource::bucketWeather(hours, noon, noon.addSecs(3 * 3600), 3600);
    QCOMPARE(hourly.count(), 3);
    QCOMPARE(hourly.at(0).startUtc, noon);
    QCOMPARE(hourly.at(0).precipitationMm.value(), 1.0);
    QVERIFY(hourly.at(0).temperatureCelsius.has_value() == false);
    QCOMPARE(hourly.at(1).precipitationMm.value(), 2.0);
    QCOMPARE(hourly.at(1).temperatureCelsius.value(), 21.0);
    QCOMPARE(hourly.at(2).temperatureCelsius.value(), 22.0);
    QVERIFY(hourly.at(2).humidityPercent.has_value() == false);

    const WeatherBucketList twoHourly = ClimateDataSource::bucketWeather(hours, noon, noon.addSecs(4 * 3600), 7200);
    QCOMPARE(twoHourly.count(), 2);
    QCOMPARE(twoHourly.at(0).precipitationMm.value(), 3.0);
    QCOMPARE(twoHourly.at(0).et0Mm.value(), 1.0);
    QCOMPARE(twoHourly.at(0).temperatureCelsius.value(), 21.0);
    QCOMPARE(twoHourly.at(0).humidityPercent.value(), 60.0);
    QCOMPARE(twoHourly.at(1).precipitationMm.value(), 7.0);
    QCOMPARE(twoHourly.at(1).temperatureCelsius.value(), 22.5);
    QCOMPARE(twoHourly.at(1).humidityPercent.value(), 60.0);
}

void TestWeather::migratesAVersion100DatabaseAndKeepsItsReadings()
{
    QTemporaryDir dir;
    const QString path = dir.filePath("climate.db");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "seed");
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec("CREATE TABLE info (id INTEGER PRIMARY KEY, sw_version TEXT NOT NULL)"));
        QVERIFY(query.exec("CREATE TABLE readings (at_utc INTEGER NOT NULL, temperature_c REAL NOT NULL, "
                           "humidity_pct REAL NOT NULL)"));
        QVERIFY(query.exec("INSERT INTO info (id, sw_version) VALUES (1, '1.0.0')"));
        QVERIFY(query.exec("INSERT INTO readings VALUES (1791670000, 24.5, 64.0)"));
        db.close();
    }
    QSqlDatabase::removeDatabase("seed");

    ClimateDataSource source(path);
    QVERIFY2(source.open(), qPrintable(source.errorText()));
    QCOMPARE(countRows(source, "readings"), 1);
    QCOMPARE(countRows(source, "weather_hours"), 0);
    QCOMPARE(countRows(source, "weather_current"), 0);

    QSqlQuery query(QSqlDatabase::database(source.connectionName()));
    QVERIFY(query.exec("SELECT sw_version FROM info WHERE id = 1"));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toString(), ClimateDataSource::schemaVersion());
    QCOMPARE(ClimateDataSource::schemaVersion(), QString("1.1.0"));
}

void TestWeather::pollerStoresWhatTheServiceReturns()
{
    FakeOpenMeteo service;
    service.respond(forecastBody(), QHttpServerResponder::StatusCode::Ok);
    QVERIFY(service.start());

    QTemporaryDir dir;
    ClimateDataSource source(dir.filePath("climate.db"));
    QVERIFY(source.open());
    TestClock clock(NOW);

    WeatherPoller poller(&source, &clock);
    poller.setBaseUrl(service.baseUrl());
    QSignalSpy finished(&poller, &WeatherPoller::pollFinished);
    poller.setLocation(33.46349, -117.65823);

    QVERIFY(finished.wait(5000));
    QCOMPARE(finished.at(0).at(0).toBool(), true);
    QCOMPARE(service.requests().count(), 1);
    QCOMPARE(QUrlQuery(service.requests().at(0)).queryItemValue("latitude"), QString("33.46349"));
    QCOMPARE(countRows(source, "weather_hours"), 3);
    QCOMPARE(countRows(source, "weather_current"), 1);

    poller.setLocation(33.46349, -117.65823);
    QVERIFY(finished.wait(300) == false);
    QCOMPARE(service.requests().count(), 1);
}

void TestWeather::pollerStoresNothingFromAnErrorResponse()
{
    FakeOpenMeteo service;
    service.respond(R"({"error":true,"reason":"Latitude must be in range of -90 to 90°."})",
                    QHttpServerResponder::StatusCode::BadRequest);
    QVERIFY(service.start());

    QTemporaryDir dir;
    ClimateDataSource source(dir.filePath("climate.db"));
    QVERIFY(source.open());
    TestClock clock(NOW);

    WeatherPoller poller(&source, &clock);
    poller.setBaseUrl(service.baseUrl());
    QSignalSpy finished(&poller, &WeatherPoller::pollFinished);
    poller.setLocation(33.46349, -117.65823);

    QVERIFY(finished.wait(5000));
    QCOMPARE(finished.at(0).at(0).toBool(), false);
    QCOMPARE(countRows(source, "weather_hours"), 0);
    QCOMPARE(countRows(source, "weather_current"), 0);
}

void TestWeather::pollerIsIdleWithoutALocation()
{
    FakeOpenMeteo service;
    service.respond(forecastBody(), QHttpServerResponder::StatusCode::Ok);
    QVERIFY(service.start());

    QTemporaryDir dir;
    ClimateDataSource source(dir.filePath("climate.db"));
    QVERIFY(source.open());
    TestClock clock(NOW);

    WeatherPoller poller(&source, &clock);
    poller.setBaseUrl(service.baseUrl());
    QSignalSpy finished(&poller, &WeatherPoller::pollFinished);

    poller.poll();
    QVERIFY(finished.wait(300) == false);

    poller.setLocation(33.46349, -117.65823);
    poller.clearLocation();
    QVERIFY(poller.hasLocation() == false);
    poller.poll();
    QVERIFY(finished.wait(300) == false);
    QCOMPARE(countRows(source, "weather_hours"), 0);
}

QTEST_MAIN(TestWeather)

#include "tst_weather.moc"
