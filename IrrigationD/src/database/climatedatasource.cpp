#include "climatedatasource.h"

#include <Kanoop/database/sqlparser.h>

#include <QDir>
#include <QFile>
#include <QSqlQuery>
#include <QTimeZone>
#include <QVariant>
#include <QVersionNumber>

static QVariant optionalValue(const std::optional<double>& value)
{
    return value.has_value() ? QVariant(value.value()) : QVariant();
}

static std::optional<double> optionalDouble(const QVariant& value)
{
    if(value.isNull()) {
        return std::nullopt;
    }
    return value.toDouble();
}

ClimateDataSource::ClimateDataSource(const QString& path) :
    DataSource(DatabaseCredentials(path))
{
}

QString ClimateDataSource::createSql() const
{
    QFile file(":/database/climate-schema.sql");
    if(file.open(QIODevice::ReadOnly) == false) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

bool ClimateDataSource::executePostCreateScripts()
{
    if(applyPragmas() == false) {
        return false;
    }

    bool success = false;
    QSqlQuery query = prepareQuery("INSERT INTO info (id, sw_version) VALUES (1, :version)", &success);
    if(success == false) {
        return false;
    }
    query.bindValue(":version", schemaVersion());
    return executeQuery(query);
}

bool ClimateDataSource::migrate()
{
    if(applyPragmas() == false) {
        return false;
    }

    bool success = false;
    QSqlQuery query = executeQuery("SELECT sw_version FROM info WHERE id = 1", &success);
    if(success == false || query.next() == false) {
        return false;
    }
    const QVersionNumber stored = QVersionNumber::fromString(query.value(0).toString());
    query.finish();

    const QString directory = ":/database/migrate/climate";
    QList<QVersionNumber> versions;
    for(const QString& entry : QDir(directory).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QVersionNumber version = QVersionNumber::fromString(entry);
        if(version.isNull() == false && version > stored
           && version <= QVersionNumber::fromString(schemaVersion())) {
            versions.append(version);
        }
    }
    std::sort(versions.begin(), versions.end());

    for(const QVersionNumber& version : versions) {
        if(applyMigration(version.toString()) == false) {
            return false;
        }
    }
    return true;
}

bool ClimateDataSource::applyMigration(const QString& version)
{
    const QString directory = QString(":/database/migrate/climate/%1").arg(version);
    QStringList statements;
    for(const QString& script : QDir(directory).entryList(QDir::Files, QDir::Name)) {
        QFile file(QString("%1/%2").arg(directory, script));
        if(file.open(QIODevice::ReadOnly) == false) {
            return false;
        }
        SqlParser parser(QString::fromUtf8(file.readAll()));
        if(parser.isValid() == false) {
            return false;
        }
        statements.append(parser.statements());
    }
    statements.append(QString("UPDATE info SET sw_version = '%1' WHERE id = 1").arg(version));

    bool success = false;
    executeQuery("BEGIN", &success);
    if(success == false) {
        return false;
    }
    if(executeMultiple(statements) == false) {
        executeQuery("ROLLBACK");
        return false;
    }
    executeQuery("COMMIT", &success);
    return success;
}

bool ClimateDataSource::applyPragmas()
{
    return executeMultiple({
        "PRAGMA journal_mode=WAL",
        "PRAGMA synchronous=NORMAL"
    });
}

bool ClimateDataSource::insertReadings(const ClimateReadingList& readings)
{
    if(readings.isEmpty()) {
        return true;
    }

    bool success = false;
    executeQuery("BEGIN", &success);
    if(success == false) {
        return false;
    }

    QSqlQuery query = prepareQuery(
        "INSERT INTO readings (at_utc, temperature_c, humidity_pct) VALUES (:at, :temperature, :humidity)",
        &success);
    for(int i = 0; success == true && i < readings.count(); i++) {
        const ClimateReading& reading = readings.at(i);
        query.bindValue(":at", reading.atUtc.toSecsSinceEpoch());
        query.bindValue(":temperature", reading.temperatureCelsius);
        query.bindValue(":humidity", reading.humidityPercent);
        success = executeQuery(query);
    }

    if(success == true) {
        executeQuery("COMMIT", &success);
    }
    if(success == false) {
        executeQuery("ROLLBACK");
    }
    return success;
}

bool ClimateDataSource::upsertWeather(const WeatherReport& report)
{
    bool success = false;
    executeQuery("BEGIN", &success);
    if(success == false) {
        return false;
    }

    const qint64 fetchedAt = report.fetchedAtUtc.toSecsSinceEpoch();
    QSqlQuery hourQuery = prepareQuery(
        "INSERT OR REPLACE INTO weather_hours "
        "(hour_end_utc, latitude, longitude, precipitation_mm, et0_mm, temperature_c, humidity_pct, fetched_at_utc) "
        "VALUES (:end, :latitude, :longitude, :precipitation, :et0, :temperature, :humidity, :fetched)",
        &success);
    for(int i = 0; success == true && i < report.hours.count(); i++) {
        const WeatherHour& hour = report.hours.at(i);
        hourQuery.bindValue(":end", hour.hourEndUtc.toSecsSinceEpoch());
        hourQuery.bindValue(":latitude", report.latitude);
        hourQuery.bindValue(":longitude", report.longitude);
        hourQuery.bindValue(":precipitation", optionalValue(hour.precipitationMm));
        hourQuery.bindValue(":et0", optionalValue(hour.et0Mm));
        hourQuery.bindValue(":temperature", optionalValue(hour.temperatureCelsius));
        hourQuery.bindValue(":humidity", optionalValue(hour.humidityPercent));
        hourQuery.bindValue(":fetched", fetchedAt);
        success = executeQuery(hourQuery);
    }

    if(success == true && report.current.has_value()) {
        const WeatherCurrent& current = report.current.value();
        QSqlQuery currentQuery = prepareQuery(
            "INSERT OR REPLACE INTO weather_current "
            "(at_utc, latitude, longitude, precipitation_mm, temperature_c, humidity_pct, fetched_at_utc) "
            "VALUES (:at, :latitude, :longitude, :precipitation, :temperature, :humidity, :fetched)",
            &success);
        if(success == true) {
            currentQuery.bindValue(":at", current.atUtc.toSecsSinceEpoch());
            currentQuery.bindValue(":latitude", report.latitude);
            currentQuery.bindValue(":longitude", report.longitude);
            currentQuery.bindValue(":precipitation", optionalValue(current.precipitationMm));
            currentQuery.bindValue(":temperature", optionalValue(current.temperatureCelsius));
            currentQuery.bindValue(":humidity", optionalValue(current.humidityPercent));
            currentQuery.bindValue(":fetched", fetchedAt);
            success = executeQuery(currentQuery);
        }
    }

    if(success == true) {
        executeQuery("COMMIT", &success);
    }
    if(success == false) {
        executeQuery("ROLLBACK");
    }
    return success;
}

WeatherHourList ClimateDataSource::weatherHoursBetween(const QDateTime& fromUtc, const QDateTime& toUtc)
{
    WeatherHourList result;

    bool success = false;
    QSqlQuery query = prepareQuery(
        "SELECT hour_end_utc, precipitation_mm, et0_mm, temperature_c, humidity_pct FROM weather_hours "
        "WHERE hour_end_utc >= :from AND hour_end_utc < :to ORDER BY hour_end_utc",
        &success);
    if(success == false) {
        return result;
    }

    query.bindValue(":from", fromUtc.toSecsSinceEpoch());
    query.bindValue(":to", toUtc.toSecsSinceEpoch());
    if(executeQuery(query) == false) {
        return result;
    }

    while(query.next()) {
        WeatherHour hour;
        hour.hourEndUtc = QDateTime::fromSecsSinceEpoch(query.value(0).toLongLong(), QTimeZone::UTC);
        hour.precipitationMm = optionalDouble(query.value(1));
        hour.et0Mm = optionalDouble(query.value(2));
        hour.temperatureCelsius = optionalDouble(query.value(3));
        hour.humidityPercent = optionalDouble(query.value(4));
        result.append(hour);
    }
    return result;
}

ClimateReadingList ClimateDataSource::readingsBetween(const QDateTime& fromUtc, const QDateTime& toUtc)
{
    ClimateReadingList result;

    bool success = false;
    QSqlQuery query = prepareQuery(
        "SELECT at_utc, temperature_c, humidity_pct FROM readings "
        "WHERE at_utc >= :from AND at_utc < :to ORDER BY at_utc, rowid",
        &success);
    if(success == false) {
        return result;
    }

    query.bindValue(":from", fromUtc.toSecsSinceEpoch());
    query.bindValue(":to", toUtc.toSecsSinceEpoch());
    if(executeQuery(query) == false) {
        return result;
    }

    while(query.next()) {
        ClimateReading reading;
        reading.atUtc = QDateTime::fromSecsSinceEpoch(query.value(0).toLongLong(), QTimeZone::UTC);
        reading.temperatureCelsius = query.value(1).toDouble();
        reading.humidityPercent = query.value(2).toDouble();
        result.append(reading);
    }
    return result;
}

ClimateBucketList ClimateDataSource::bucketsBetween(const QDateTime& fromUtc, const QDateTime& toUtc, int bucketSeconds)
{
    ClimateBucketList result;
    if(bucketSeconds <= 0) {
        return result;
    }

    bool success = false;
    QSqlQuery query = prepareQuery(
        "SELECT (at_utc / :width) * :width AS start, count(*), "
        "min(temperature_c), avg(temperature_c), max(temperature_c), "
        "min(humidity_pct), avg(humidity_pct), max(humidity_pct) "
        "FROM readings WHERE at_utc >= :from AND at_utc < :to "
        "GROUP BY start ORDER BY start",
        &success);
    if(success == false) {
        return result;
    }

    query.bindValue(":width", bucketSeconds);
    query.bindValue(":from", fromUtc.toSecsSinceEpoch());
    query.bindValue(":to", toUtc.toSecsSinceEpoch());
    if(executeQuery(query) == false) {
        return result;
    }

    while(query.next()) {
        ClimateBucket bucket;
        bucket.startUtc = QDateTime::fromSecsSinceEpoch(query.value(0).toLongLong(), QTimeZone::UTC);
        bucket.count = query.value(1).toInt();
        bucket.temperatureMin = query.value(2).toDouble();
        bucket.temperatureMean = query.value(3).toDouble();
        bucket.temperatureMax = query.value(4).toDouble();
        bucket.humidityMin = query.value(5).toDouble();
        bucket.humidityMean = query.value(6).toDouble();
        bucket.humidityMax = query.value(7).toDouble();
        result.append(bucket);
    }
    return result;
}

int ClimateDataSource::bucketSecondsFor(qint64 spanSeconds, int maximumBuckets)
{
    static const QList<int> Widths = {
        60, 300, 600, 900, 1800, 3600, 7200, 10800, 21600, 43200, 86400, 172800, 604800
    };

    for(int width : Widths) {
        if((spanSeconds + width - 1) / width <= maximumBuckets) {
            return width;
        }
    }
    return Widths.last();
}
