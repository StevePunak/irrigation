#ifndef IRRIGATIONSETTINGS_H
#define IRRIGATIONSETTINGS_H

#include <Kanoop/utility/appsettings.h>

#include <QMap>
#include <QString>

/**
 * @brief INI-backed daemon configuration.
 *
 * Malformed entries in the zone map are dropped; the surviving zones keep
 * their own numbers.
 */
class IrrigationSettings : public AppSettings
{
    Q_OBJECT

public:
    /** @brief Constructs settings backed by the INI file at @p path. */
    explicit IrrigationSettings(const QString& path);

    /** @brief Returns the INI key holding the zone map. */
    static QString zonesKey() { return KEY_ZONES; }

    /** @brief Returns the zone number to GPIO line offset map. Malformed entries are dropped. */
    QMap<int, quint32> zoneGpioMap() const;

    /** @brief Returns the kernel label of the GPIO chip to open. */
    QString chipLabel() const { return _settings.value(KEY_CHIP_LABEL, "pinctrl-bcm2711").toString(); }

    /** @brief Returns true when driving a zone line active energises a low-trigger relay. */
    bool zoneActiveLow() const { return _settings.value(KEY_ZONE_ACTIVE_LOW, true).toBool(); }

    /** @brief Returns the GPIO line offset of the stop button. */
    quint32 stopButtonOffset() const { return _settings.value(KEY_STOP_BUTTON_OFFSET, 25).toUInt(); }

    /** @brief Returns the GPIO line offset of the RUN button, or -1 when runButtonOffset is missing or malformed. */
    int runButtonOffset() const { return lineOffset(KEY_RUN_BUTTON_OFFSET); }

    /** @brief Returns the GPIO line offset of the display's CLK, or -1 when displayClockOffset is missing or malformed. */
    int displayClockOffset() const { return lineOffset(KEY_DISPLAY_CLOCK_OFFSET); }

    /** @brief Returns the GPIO line offset of the display's DIO, or -1 when displayDataOffset is missing or malformed. */
    int displayDataOffset() const { return lineOffset(KEY_DISPLAY_DATA_OFFSET); }

    /** @brief Returns the ceiling applied to any requested zone duration, in seconds. */
    int maxZoneSeconds() const { return _settings.value(KEY_MAX_ZONE_SECONDS, 3600).toInt(); }

    /** @brief Returns the address the control server binds to. */
    QString bindAddress() const { return _settings.value(KEY_BIND_ADDRESS, "127.0.0.1").toString(); }

    /** @brief Returns the port the control server listens on. */
    int listenPort() const { return _settings.value(KEY_LISTEN_PORT, 8080).toInt(); }

    /** @brief Returns the path of the SQLite database file. */
    QString databasePath() const { return _settings.value(KEY_DATABASE_PATH, "/var/lib/irrigationd/irrigation.db").toString(); }

    /**
     * @brief Returns the mount point the database directory must sit on, or an empty string for no check.
     *
     * The daemon refuses to start while the database directory belongs to any other mount.
     */
    QString databaseMountPoint() const { return _settings.value(KEY_DATABASE_MOUNT_POINT).toString(); }

private:
    int lineOffset(const QString& key) const;

    static const QString KEY_ZONES;
    static const QString KEY_CHIP_LABEL;
    static const QString KEY_ZONE_ACTIVE_LOW;
    static const QString KEY_STOP_BUTTON_OFFSET;
    static const QString KEY_RUN_BUTTON_OFFSET;
    static const QString KEY_DISPLAY_CLOCK_OFFSET;
    static const QString KEY_DISPLAY_DATA_OFFSET;
    static const QString KEY_MAX_ZONE_SECONDS;
    static const QString KEY_BIND_ADDRESS;
    static const QString KEY_LISTEN_PORT;
    static const QString KEY_DATABASE_PATH;
    static const QString KEY_DATABASE_MOUNT_POINT;
};

#endif // IRRIGATIONSETTINGS_H
