#ifndef IRRIGATIONSETTINGS_H
#define IRRIGATIONSETTINGS_H

#include <QMap>
#include <QSettings>
#include <QString>

/**
 * @brief INI-backed daemon configuration.
 *
 * The zone-to-GPIO map lives here rather than in the database because it
 * describes the wiring of one particular box; changing it must not require a
 * schema migration.
 */
class IrrigationSettings
{
public:
    /** @brief Constructs settings backed by the INI file at @p path. */
    explicit IrrigationSettings(const QString& path);

    /** @brief Returns the zone number to GPIO line offset map. Malformed entries are dropped. */
    QMap<int, quint32> zoneGpioMap() const;

    /** @brief Returns the kernel label of the GPIO chip to open. */
    QString chipLabel() const;

    /** @brief Returns true when driving a zone line active energises a low-trigger relay. */
    bool zoneActiveLow() const;

    /** @brief Returns the GPIO line offset of the stop button. */
    quint32 stopButtonOffset() const;

    /** @brief Returns the ceiling applied to any requested zone duration, in seconds. */
    int maxZoneSeconds() const;

    /** @brief Returns the address the control server binds to. */
    QString bindAddress() const;

    /** @brief Returns the port the control server listens on. */
    int listenPort() const;

    /** @brief Returns the path of the SQLite database file. */
    QString databasePath() const;

private:
    mutable QSettings _settings;
};

#endif // IRRIGATIONSETTINGS_H
