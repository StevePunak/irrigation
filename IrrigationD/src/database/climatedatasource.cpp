#include "climatedatasource.h"

#include <QFile>
#include <QSqlQuery>
#include <QTimeZone>
#include <QVariant>

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
    return applyPragmas();
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
