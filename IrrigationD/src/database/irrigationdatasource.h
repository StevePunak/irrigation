#ifndef IRRIGATIONDATASOURCE_H
#define IRRIGATIONDATASOURCE_H

#include <QVersionNumber>

#include <Kanoop/database/datasource.h>

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
