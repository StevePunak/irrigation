#ifndef CLIMATEDATASOURCE_H
#define CLIMATEDATASOURCE_H

#include <Kanoop/database/datasource.h>

#include "model/climatereading.h"

/**
 * @brief SQLite store for climate readings, kept in its own file beside the irrigation database.
 *
 * Creates the schema from :/database/climate-schema.sql on first open. Readings are
 * never pruned.
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
    static QString schemaVersion() { return "1.0.0"; }

    /** @brief Inserts every reading in @p readings in one transaction. @return True when all were committed. */
    bool insertReadings(const ClimateReadingList& readings);

    /** @brief Returns the readings taken at or after @p fromUtc and before @p toUtc, oldest first. */
    ClimateReadingList readingsBetween(const QDateTime& fromUtc, const QDateTime& toUtc);

protected:
    /** @brief Returns the full create-from-scratch schema. */
    virtual QString createSql() const override;

    /** @brief Stamps schemaVersion() into the info table. @return True on success. */
    virtual bool executePostCreateScripts() override;

    /** @brief Applies the per-connection pragmas. There are no migrations yet. @return True on success. */
    virtual bool migrate() override;

private:
    bool applyPragmas();
};

#endif // CLIMATEDATASOURCE_H
