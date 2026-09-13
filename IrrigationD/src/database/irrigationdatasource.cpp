#include <algorithm>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSqlQuery>

#include <Kanoop/database/sqlparser.h>

#include "irrigationdatasource.h"

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
