#ifndef IRRIGATIONDATASOURCE_H
#define IRRIGATIONDATASOURCE_H

#include <QDateTime>
#include <QSqlQuery>
#include <QVersionNumber>

#include <Kanoop/database/datasource.h>

#include "model/firedinstant.h"
#include "model/program.h"
#include "model/programstarttime.h"
#include "model/programzone.h"
#include "model/zone.h"

/**
 * @brief SQLite data source for the irrigation controller.
 *
 * Creates the schema from :/database/schema.sql on first open and runs any
 * pending scripts under :/database/migrate/irrigation on every subsequent open.
 *
 * @warning A migration that fails renames the database to
 *          <file>.<utc>.backup and recreates it empty. Every migration must be
 *          tested against a populated database before it ships; there is no
 *          second chance and the user is not told.
 */
class IrrigationDataSource : public DataSource
{
    Q_OBJECT
public:
    /** @brief Constructs a data source for the SQLite file at @p path. */
    explicit IrrigationDataSource(const QString& path);

    /** @brief Opens the database, creating or migrating it as required. @return True on success. */
    bool open() { return openConnection(); }

    /** @brief Returns the schema version compiled into this binary. */
    QString compiledDatabaseVersion() const { return QT_STRINGIFY(IRRIGATION_DB_VERSION); }

    /** @brief Returns the Qt resource path holding the migration script directories. */
    QString migrationScriptDirectory() const { return ":/database/migrate/irrigation"; }

    /** @brief Returns every zone, ordered by manifold number. */
    ZoneList allZones();

    /** @brief Updates a zone's name and enabled flag by id. @return True on success. */
    bool updateZone(const Zone& zone);

    /** @brief Returns every enabled program. */
    ProgramList enabledPrograms();

    /** @brief Returns every program. */
    ProgramList allPrograms();

    /** @brief Inserts @p program and writes the generated id back into it. @return True on success. */
    bool insertProgram(Program& program);

    /** @brief Updates a program's fields by id. @return True on success. */
    bool updateProgram(const Program& program);

    /** @brief Deletes a program by id, cascading to its start times and zones. @return True on success. */
    bool deleteProgram(int programId);

    /** @brief Returns the start times belonging to @p programId. */
    ProgramStartTimeList startTimesFor(int programId);

    /** @brief Inserts @p startTime and writes the generated id back into it. @return True on success. */
    bool insertStartTime(ProgramStartTime& startTime);

    /** @brief Returns the zones belonging to @p programId, ordered by sequence. */
    ProgramZoneList zonesFor(int programId);

    /** @brief Inserts @p programZone and writes the generated id back into it. @return True on success. */
    bool insertProgramZone(ProgramZone& programZone);

    /** @brief Records that a start time fired. A repeat of the same instant is a no-op. @return True on success. */
    bool recordFiring(const FiredInstant& instant);

    /** @brief Returns true when a firing already exists for this program, start time and instant. */
    bool hasFired(int programId, int startTimeId, const QDateTime& scheduledAtUtc);

    /** @brief Updates the outcome of an existing firing, addressed by its program, start time and instant. @return True when exactly one row changed. */
    bool setFiringOutcome(int programId, int startTimeId, const QDateTime& scheduledAtUtc, FiredInstant::Outcome outcome);

    /** @brief Deletes fired instants scheduled before @p cutoffUtc. @return True on success. */
    bool pruneFiredInstantsOlderThan(const QDateTime& cutoffUtc);

    /** @brief Returns the value stored for @p key, or an empty string when absent. */
    QString settingValue(const QString& key);

    /** @brief Sets the value stored for @p key, inserting or replacing it. @return True on success. */
    bool setSettingValue(const QString& key, const QString& value);

    /** @brief Returns whether the master enable permits water, disabled only on the exact stored value "0". */
    bool isMasterEnabled();

    /** @brief Public passthrough to executeQuery(). */
    QSqlQuery rawQuery(const QString& sql, bool* ok);

protected:
    /** @brief Returns the full create-from-scratch schema. */
    virtual QString createSql() const override;

    /** @brief Stamps the compiled version into the info table. @return True on success. */
    virtual bool executePostCreateScripts() override;

    /** @brief Applies every migration between the stored and compiled versions. @return True on success. */
    virtual bool migrate() override;

private:
    bool applyDurabilityPragmas();
    bool readStoredVersion(QVersionNumber& version);
    bool writeStoredVersion(const QVersionNumber& version);
    bool applyScriptResource(const QString& resourcePath);
    bool recreateAndReopen(const QString& reason);

    static QString readResource(const QString& path);
    static QList<QVersionNumber> sortedMigrationVersions(const QString& directory);

    QString _path;
};

#endif // IRRIGATIONDATASOURCE_H
