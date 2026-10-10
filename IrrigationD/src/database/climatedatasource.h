#ifndef CLIMATEDATASOURCE_H
#define CLIMATEDATASOURCE_H

#include <Kanoop/database/datasource.h>

#include "model/climatebucket.h"
#include "model/climatereading.h"
#include "model/weatherreport.h"

/**
 * @brief SQLite store for climate readings, kept in its own file beside the irrigation database.
 *
 * Creates the schema from :/database/climate-schema.sql on first open, and on later
 * opens applies the scripts under :/database/migrate/climate newer than the stored
 * version. Nothing is ever pruned.
 *
 * @warning A failed migration fails the open and leaves the file alone. Never
 *          recreate this database to recover: it holds the only copy of the history.
 *
 * @warning synchronous=NORMAL in WAL mode: a power cut can lose the most recent
 *          commits. Fine for climate history; never copy these pragmas to the
 *          irrigation database.
 */
class ClimateDataSource : public DataSource
{
    Q_OBJECT
public:
    /** @brief Constructs a data source for the SQLite file at @p path. */
    explicit ClimateDataSource(const QString& path);

    /** @brief Opens the database, creating it as required. @return True on success. */
    bool open() { return openConnection(); }

    /** @brief The schema version stamped into a newly created database. */
    static QString schemaVersion() { return "1.1.0"; }

    /** @brief Inserts every reading in @p readings in one transaction. @return True when all were committed. */
    bool insertReadings(const ClimateReadingList& readings);

    /**
     * @brief Writes every hour and the current conditions in @p report in one transaction.
     *
     * A row already stored for the same hour or 15-minute step is replaced: the service
     * revises recent values as observations arrive. @return True when all were committed.
     */
    bool upsertWeather(const WeatherReport& report);

    /** @brief Returns the stored hours ending at or after @p fromUtc and before @p toUtc, oldest first. */
    WeatherHourList weatherHoursBetween(const QDateTime& fromUtc, const QDateTime& toUtc);

    /** @brief Returns the readings taken at or after @p fromUtc and before @p toUtc, oldest first. */
    ClimateReadingList readingsBetween(const QDateTime& fromUtc, const QDateTime& toUtc);

    /**
     * @brief Returns the readings taken at or after @p fromUtc and before @p toUtc, grouped into
     *        @p bucketSeconds-wide buckets aligned to the epoch, oldest first. Empty buckets are omitted.
     */
    ClimateBucketList bucketsBetween(const QDateTime& fromUtc, const QDateTime& toUtc, int bucketSeconds);

    /** @brief Returns the narrowest standard bucket width that splits @p spanSeconds into at most @p maximumBuckets buckets. */
    static int bucketSecondsFor(qint64 spanSeconds, int maximumBuckets);

protected:
    /** @brief Returns the full create-from-scratch schema. */
    virtual QString createSql() const override;

    /** @brief Stamps schemaVersion() into the info table. @return True on success. */
    virtual bool executePostCreateScripts() override;

    /** @brief Applies the per-connection pragmas and any pending migration. @return True on success. */
    virtual bool migrate() override;

private:
    bool applyPragmas();
    bool applyMigration(const QString& version);
};

#endif // CLIMATEDATASOURCE_H
