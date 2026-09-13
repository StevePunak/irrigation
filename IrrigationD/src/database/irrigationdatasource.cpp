#include <algorithm>

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSqlQuery>
#include <QVariant>

#include <Kanoop/database/sqlparser.h>

#include "irrigationdatasource.h"

namespace
{
    Program programFromQuery(const QSqlQuery& query)
    {
        Program program;
        program.id = query.value("id").toInt();
        program.name = query.value("name").toString();
        program.enabled = query.value("enabled").toBool();
        program.dayMode = Program::dayModeFromString(query.value("day_mode").toString());
        program.dowMask = query.value("dow_mask").toInt();
        program.intervalDays = query.value("interval_days").toInt();
        const QString anchorDate = query.value("anchor_date").toString();
        if(anchorDate.isEmpty() == false) {
            program.anchorDate = QDate::fromString(anchorDate, Qt::ISODate);
        }
        return program;
    }

    void bindProgram(QSqlQuery& query, const Program& program)
    {
        query.bindValue(":name",    program.name);
        query.bindValue(":enabled", program.enabled);
        query.bindValue(":dayMode", Program::dayModeToString(program.dayMode));
        query.bindValue(":dowMask", program.dowMask);
        query.bindValue(":intervalDays", program.intervalDays);
        if(program.anchorDate.isValid()) {
            query.bindValue(":anchorDate", program.anchorDate.toString(Qt::ISODate));
        }
        else {
            query.bindValue(":anchorDate", QVariant());
        }
    }
}

IrrigationDataSource::IrrigationDataSource(const QString& path) :
    DataSource(DatabaseCredentials(path)),
    _path(path)
{
}

QString IrrigationDataSource::readResource(const QString& path)
{
    QFile file(path);
    if(file.open(QIODevice::ReadOnly) == false) {
        return QString();
    }
    QString result = QString::fromUtf8(file.readAll());
    file.close();
    return result;
}

QString IrrigationDataSource::createSql() const
{
    return readResource(":/database/schema.sql");
}

bool IrrigationDataSource::executePostCreateScripts()
{
    return applyDurabilityPragmas() && writeStoredVersion(QVersionNumber::fromString(compiledDatabaseVersion()));
}

bool IrrigationDataSource::applyDurabilityPragmas()
{
    const QStringList pragmas = {
        "PRAGMA journal_mode=WAL",
        "PRAGMA synchronous=FULL"
    };

    return executeMultiple(pragmas);
}

bool IrrigationDataSource::readStoredVersion(QVersionNumber& version)
{
    bool success = false;
    QSqlQuery query = executeQuery("SELECT sw_version FROM info WHERE id = 1", &success);
    if(success == false || query.next() == false) {
        return false;
    }

    version = QVersionNumber::fromString(query.value(0).toString());
    return version.isNull() == false;
}

bool IrrigationDataSource::writeStoredVersion(const QVersionNumber& version)
{
    bool success = false;
    QSqlQuery query = prepareQuery(
        "INSERT INTO info (id, sw_version) VALUES (1, :version) "
        "ON CONFLICT(id) DO UPDATE SET sw_version = excluded.sw_version",
        &success);
    if(success == false) {
        return false;
    }

    query.bindValue(":version", version.toString());
    return executeQuery(query);
}

bool IrrigationDataSource::applyScriptResource(const QString& resourcePath)
{
    const QString sql = readResource(resourcePath);
    if(sql.isEmpty()) {
        return false;
    }

    SqlParser parser(sql);
    if(parser.isValid() == false) {
        return false;
    }

    return executeMultiple(parser.statements());
}

QList<QVersionNumber> IrrigationDataSource::sortedMigrationVersions(const QString& directory)
{
    QList<QVersionNumber> result;

    const QStringList entries = QDir(directory).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for(const QString& entry : entries) {
        QVersionNumber version = QVersionNumber::fromString(entry);
        if(version.isNull() == false) {
            result.append(version);
        }
    }

    std::sort(result.begin(), result.end());
    return result;
}

bool IrrigationDataSource::migrate()
{
    applyDurabilityPragmas();

    QVersionNumber stored;
    if(readStoredVersion(stored) == false) {
        executeMultiple({ "CREATE TABLE IF NOT EXISTS info (id INTEGER PRIMARY KEY, sw_version TEXT NOT NULL)" });
        stored = QVersionNumber(0, 0, 0);
        writeStoredVersion(stored);
    }

    const QVersionNumber compiled = QVersionNumber::fromString(compiledDatabaseVersion());
    const QList<QVersionNumber> versions = sortedMigrationVersions(migrationScriptDirectory());

    int applied = 0;
    for(const QVersionNumber& version : versions) {
        if(version <= stored || version > compiled) {
            continue;
        }

        const QString directory = QString("%1/%2").arg(migrationScriptDirectory(), version.toString());
        const QStringList scripts = QDir(directory).entryList(QDir::Files, QDir::Name);
        for(const QString& script : scripts) {
            const QString resource = QString("%1/%2").arg(directory, script);
            if(applyScriptResource(resource) == false) {
                return recreateAndReopen(QString("migration %1 failed").arg(resource));
            }
            applied++;
        }
    }

    if(applied > 0) {
        return writeStoredVersion(compiled);
    }

    return true;
}

ZoneList IrrigationDataSource::allZones()
{
    ZoneList result;

    bool success = false;
    QSqlQuery query = executeQuery("SELECT id, number, name, enabled FROM zones ORDER BY number", &success);
    if(success == false) {
        return result;
    }

    while(query.next()) {
        Zone zone;
        zone.id = query.value("id").toInt();
        zone.number = query.value("number").toInt();
        zone.name = query.value("name").toString();
        zone.enabled = query.value("enabled").toBool();
        result.append(zone);
    }
    return result;
}

bool IrrigationDataSource::updateZone(const Zone& zone)
{
    bool success = false;
    QSqlQuery query = prepareQuery(
        "UPDATE zones SET number = :number, name = :name, enabled = :enabled WHERE id = :id",
        &success);
    if(success == false) {
        return false;
    }

    query.bindValue(":number",  zone.number);
    query.bindValue(":name",    zone.name);
    query.bindValue(":enabled", zone.enabled);
    query.bindValue(":id",      zone.id);

    return executeQuery(query);
}

ProgramList IrrigationDataSource::enabledPrograms()
{
    ProgramList result;

    bool success = false;
    QSqlQuery query = executeQuery(
        "SELECT id, name, enabled, day_mode, dow_mask, interval_days, anchor_date "
        "FROM programs WHERE enabled = 1 ORDER BY id",
        &success);
    if(success == false) {
        return result;
    }

    while(query.next()) {
        result.append(programFromQuery(query));
    }
    return result;
}

ProgramList IrrigationDataSource::allPrograms()
{
    ProgramList result;

    bool success = false;
    QSqlQuery query = executeQuery(
        "SELECT id, name, enabled, day_mode, dow_mask, interval_days, anchor_date "
        "FROM programs ORDER BY id",
        &success);
    if(success == false) {
        return result;
    }

    while(query.next()) {
        result.append(programFromQuery(query));
    }
    return result;
}

bool IrrigationDataSource::insertProgram(Program& program)
{
    bool success = false;
    QSqlQuery query = prepareQuery(
        "INSERT INTO programs (name, enabled, day_mode, dow_mask, interval_days, anchor_date) "
        "VALUES (:name, :enabled, :dayMode, :dowMask, :intervalDays, :anchorDate)",
        &success);
    if(success == false) {
        return false;
    }

    bindProgram(query, program);

    success = executeQuery(query);
    if(success) {
        program.id = query.lastInsertId().toInt();
    }
    return success;
}

bool IrrigationDataSource::updateProgram(const Program& program)
{
    bool success = false;
    QSqlQuery query = prepareQuery(
        "UPDATE programs SET name = :name, enabled = :enabled, day_mode = :dayMode, "
        "dow_mask = :dowMask, interval_days = :intervalDays, anchor_date = :anchorDate "
        "WHERE id = :id",
        &success);
    if(success == false) {
        return false;
    }

    bindProgram(query, program);
    query.bindValue(":id", program.id);

    return executeQuery(query);
}

bool IrrigationDataSource::deleteProgram(int programId)
{
    bool success = false;
    QSqlQuery query = prepareQuery("DELETE FROM programs WHERE id = :id", &success);
    if(success == false) {
        return false;
    }

    query.bindValue(":id", programId);
    return executeQuery(query);
}

ProgramStartTimeList IrrigationDataSource::startTimesFor(int programId)
{
    ProgramStartTimeList result;

    bool success = false;
    QSqlQuery query = prepareQuery(
        "SELECT id, program_id, minutes_after_midnight, timezone "
        "FROM program_start_times WHERE program_id = :programId ORDER BY id",
        &success);
    if(success == false) {
        return result;
    }

    query.bindValue(":programId", programId);
    if(executeQuery(query) == false) {
        return result;
    }

    while(query.next()) {
        ProgramStartTime startTime;
        startTime.id = query.value("id").toInt();
        startTime.programId = query.value("program_id").toInt();
        startTime.minutesAfterMidnight = query.value("minutes_after_midnight").toInt();
        startTime.timezone = query.value("timezone").toString();
        result.append(startTime);
    }
    return result;
}

bool IrrigationDataSource::insertStartTime(ProgramStartTime& startTime)
{
    bool success = false;
    QSqlQuery query = prepareQuery(
        "INSERT INTO program_start_times (program_id, minutes_after_midnight, timezone) "
        "VALUES (:programId, :minutes, :timezone)",
        &success);
    if(success == false) {
        return false;
    }

    query.bindValue(":programId", startTime.programId);
    query.bindValue(":minutes",   startTime.minutesAfterMidnight);
    query.bindValue(":timezone",  startTime.timezone);

    success = executeQuery(query);
    if(success) {
        startTime.id = query.lastInsertId().toInt();
    }
    return success;
}

ProgramZoneList IrrigationDataSource::zonesFor(int programId)
{
    ProgramZoneList result;

    bool success = false;
    QSqlQuery query = prepareQuery(
        "SELECT id, program_id, zone_id, sequence, duration_seconds "
        "FROM program_zones WHERE program_id = :programId ORDER BY sequence",
        &success);
    if(success == false) {
        return result;
    }

    query.bindValue(":programId", programId);
    if(executeQuery(query) == false) {
        return result;
    }

    while(query.next()) {
        ProgramZone programZone;
        programZone.id = query.value("id").toInt();
        programZone.programId = query.value("program_id").toInt();
        programZone.zoneId = query.value("zone_id").toInt();
        programZone.sequence = query.value("sequence").toInt();
        programZone.durationSeconds = query.value("duration_seconds").toInt();
        result.append(programZone);
    }
    return result;
}

bool IrrigationDataSource::insertProgramZone(ProgramZone& programZone)
{
    bool success = false;
    QSqlQuery query = prepareQuery(
        "INSERT INTO program_zones (program_id, zone_id, sequence, duration_seconds) "
        "VALUES (:programId, :zoneId, :sequence, :durationSeconds)",
        &success);
    if(success == false) {
        return false;
    }

    query.bindValue(":programId",       programZone.programId);
    query.bindValue(":zoneId",          programZone.zoneId);
    query.bindValue(":sequence",        programZone.sequence);
    query.bindValue(":durationSeconds", programZone.durationSeconds);

    success = executeQuery(query);
    if(success) {
        programZone.id = query.lastInsertId().toInt();
    }
    return success;
}

bool IrrigationDataSource::recordFiring(const FiredInstant& instant)
{
    bool success = false;
    QSqlQuery query = prepareQuery(
        "INSERT OR IGNORE INTO fired_instants "
        "(program_id, start_time_id, scheduled_at_utc, outcome) "
        "VALUES (:programId, :startTimeId, :scheduledAt, :outcome)",
        &success);
    if(success == false) {
        return false;
    }

    query.bindValue(":programId",   instant.programId);
    query.bindValue(":startTimeId", instant.startTimeId);
    query.bindValue(":scheduledAt", instant.scheduledAtUtc.toUTC().toString(Qt::ISODate));
    query.bindValue(":outcome",     FiredInstant::outcomeToString(instant.outcome));

    return executeQuery(query);
}

bool IrrigationDataSource::hasFired(int programId, int startTimeId, const QDateTime& scheduledAtUtc)
{
    bool success = false;
    QSqlQuery query = prepareQuery(
        "SELECT 1 FROM fired_instants "
        "WHERE program_id = :programId AND start_time_id = :startTimeId AND scheduled_at_utc = :scheduledAt "
        "LIMIT 1",
        &success);
    if(success == false) {
        return false;
    }

    query.bindValue(":programId",   programId);
    query.bindValue(":startTimeId", startTimeId);
    query.bindValue(":scheduledAt", scheduledAtUtc.toUTC().toString(Qt::ISODate));

    if(executeQuery(query) == false) {
        return false;
    }
    return query.next();
}

bool IrrigationDataSource::setFiringOutcome(int programId, int startTimeId, const QDateTime& scheduledAtUtc, FiredInstant::Outcome outcome)
{
    bool success = false;
    QSqlQuery query = prepareQuery(
        "UPDATE fired_instants SET outcome = :outcome "
        "WHERE program_id = :programId AND start_time_id = :startTimeId AND scheduled_at_utc = :scheduledAt",
        &success);
    if(success == false) {
        return false;
    }

    query.bindValue(":outcome",     FiredInstant::outcomeToString(outcome));
    query.bindValue(":programId",   programId);
    query.bindValue(":startTimeId", startTimeId);
    query.bindValue(":scheduledAt", scheduledAtUtc.toUTC().toString(Qt::ISODate));

    if(executeQuery(query) == false) {
        return false;
    }
    return query.numRowsAffected() == 1;
}

bool IrrigationDataSource::pruneFiredInstantsOlderThan(const QDateTime& cutoffUtc)
{
    bool success = false;
    QSqlQuery query = prepareQuery(
        "DELETE FROM fired_instants WHERE scheduled_at_utc < :cutoff",
        &success);
    if(success == false) {
        return false;
    }

    query.bindValue(":cutoff", cutoffUtc.toUTC().toString(Qt::ISODate));
    return executeQuery(query);
}

QString IrrigationDataSource::settingValue(const QString& key)
{
    bool success = false;
    QSqlQuery query = prepareQuery("SELECT value FROM settings WHERE key = :key", &success);
    if(success == false) {
        return QString();
    }

    query.bindValue(":key", key);
    if(executeQuery(query) == false || query.next() == false) {
        return QString();
    }
    return query.value(0).toString();
}

bool IrrigationDataSource::setSettingValue(const QString& key, const QString& value)
{
    bool success = false;
    QSqlQuery query = prepareQuery(
        "INSERT INTO settings (key, value) VALUES (:key, :value) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
        &success);
    if(success == false) {
        return false;
    }

    query.bindValue(":key",   key);
    query.bindValue(":value", value);

    return executeQuery(query);
}

bool IrrigationDataSource::isMasterEnabled()
{
    return settingValue("master_enabled") != "0";
}

QSqlQuery IrrigationDataSource::rawQuery(const QString& sql, bool* ok)
{
    return executeQuery(sql, ok);
}

bool IrrigationDataSource::recreateAndReopen(const QString& reason)
{
    logText(LVL_ERROR, QString("%1 - recreating the database").arg(reason));

    closeConnection();

    const QString timestamp = QDateTime::currentDateTimeUtc().toString("yyyyMMddHHmmsszzz");
    QString backup = QString("%1.%2.backup").arg(_path, timestamp);
    for(int suffix = 1; QFile::exists(backup); suffix++) {
        backup = QString("%1.%2-%3.backup").arg(_path, timestamp).arg(suffix);
    }

    if(QFile::rename(_path, backup) == false) {
        logText(LVL_ERROR, QString("Failed to move the database aside to %1").arg(backup));
        return false;
    }

    logText(LVL_WARNING, QString("Previous database preserved at %1").arg(backup));

    // A version-comparison change that lets a freshly recreated database select a
    // migration turns this call into infinite recursion that destroys the file on
    // every pass — termination depends on executePostCreateScripts() stamping the
    // recreated database at the compiled version so no migration is selected.
    return openConnection();
}

#include "database/moc_irrigationdatasource.cpp"
