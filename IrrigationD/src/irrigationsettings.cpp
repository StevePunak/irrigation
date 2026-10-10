#include "irrigationsettings.h"

#include <Kanoop/log.h>

#include <QDir>
#include <QFileInfo>

const QString IrrigationSettings::KEY_ZONES              = "gpio/zones";
const QString IrrigationSettings::KEY_CHIP_LABEL         = "gpio/chipLabel";
const QString IrrigationSettings::KEY_ZONE_ACTIVE_LOW    = "gpio/zoneActiveLow";
const QString IrrigationSettings::KEY_STOP_BUTTON_OFFSET = "gpio/stopButtonOffset";
const QString IrrigationSettings::KEY_RUN_BUTTON_OFFSET    = "gpio/runButtonOffset";
const QString IrrigationSettings::KEY_DISPLAY_CLOCK_OFFSET = "gpio/displayClockOffset";
const QString IrrigationSettings::KEY_DISPLAY_DATA_OFFSET  = "gpio/displayDataOffset";
const QString IrrigationSettings::KEY_MAX_ZONE_SECONDS   = "limits/maxZoneSeconds";
const QString IrrigationSettings::KEY_BIND_ADDRESS       = "server/bindAddress";
const QString IrrigationSettings::KEY_LISTEN_PORT        = "server/listenPort";
const QString IrrigationSettings::KEY_DATABASE_PATH      = "database/path";
const QString IrrigationSettings::KEY_DATABASE_MOUNT_POINT = "database/mountPoint";
const QString IrrigationSettings::KEY_CLIMATE_BUS          = "climate/bus";
const QString IrrigationSettings::KEY_CLIMATE_ADDRESS      = "climate/address";
const QString IrrigationSettings::KEY_CLIMATE_SAMPLE_SECONDS = "climate/intervalSeconds";
const QString IrrigationSettings::KEY_CLIMATE_DATABASE_PATH  = "climate/databasePath";

IrrigationSettings::IrrigationSettings(const QString& path) :
    AppSettings(path)
{
}

QMap<int, quint32> IrrigationSettings::zoneGpioMap() const
{
    QMap<int, quint32> result;

    QVariant value = _settings.value(KEY_ZONES);
    // QSettings returns a QStringList for an unquoted comma-separated value.
    QString raw = value.typeId() == QMetaType::QStringList ? value.toStringList().join(',') : value.toString();
    const QStringList pairs = raw.split(',', Qt::SkipEmptyParts);
    for(const QString& pair : pairs) {
        QStringList parts = pair.split('=');
        if(parts.count() != 2) {
            Log::logText(LVL_WARNING, QString("Skipping malformed zone entry \"%1\": expected zone=offset").arg(pair));
            continue;
        }

        bool zoneOk = false;
        bool offsetOk = false;
        int zone = parts.at(0).trimmed().toInt(&zoneOk);
        quint32 offset = parts.at(1).trimmed().toUInt(&offsetOk);
        if(zoneOk == false || offsetOk == false) {
            Log::logText(LVL_WARNING, QString("Skipping malformed zone entry \"%1\": zone or offset is not numeric").arg(pair));
            continue;
        }

        result.insert(zone, offset);
    }

    if(result.count() != pairs.count()) {
        Log::logText(LVL_WARNING, QString("Parsed %1 of %2 zone entries").arg(result.count()).arg(pairs.count()));
    }

    return result;
}

int IrrigationSettings::lineOffset(const QString& key) const
{
    if(_settings.contains(key) == false) {
        return -1;
    }

    const QString raw = _settings.value(key).toString();
    bool ok = false;
    const int offset = raw.trimmed().toInt(&ok);
    if(ok == false || offset < 0) {
        Log::logText(LVL_WARNING, QString("Ignoring %1=\"%2\": expected a line offset").arg(key, raw));
        return -1;
    }

    return offset;
}

int IrrigationSettings::climateBus() const
{
    if(_settings.contains(KEY_CLIMATE_BUS) == false) {
        return -1;
    }

    const QString raw = _settings.value(KEY_CLIMATE_BUS).toString();
    bool ok = false;
    const int bus = raw.trimmed().toInt(&ok);
    if(ok == false || bus < 0) {
        Log::logText(LVL_WARNING, QString("Ignoring %1=\"%2\": expected an I2C bus number").arg(KEY_CLIMATE_BUS, raw));
        return -1;
    }

    return bus;
}

quint8 IrrigationSettings::climateAddress() const
{
    const quint8 defaultAddress = 0x44;
    if(_settings.contains(KEY_CLIMATE_ADDRESS) == false) {
        return defaultAddress;
    }

    const QString raw = _settings.value(KEY_CLIMATE_ADDRESS).toString();
    bool ok = false;
    const uint address = raw.trimmed().toUInt(&ok, 0);
    if(ok == false || address < 0x03 || address > 0x77) {
        Log::logText(LVL_WARNING, QString("Ignoring %1=\"%2\": expected a 7-bit I2C address").arg(KEY_CLIMATE_ADDRESS, raw));
        return defaultAddress;
    }

    return static_cast<quint8>(address);
}

int IrrigationSettings::climateSampleSeconds() const
{
    const int defaultSeconds = 5;
    bool ok = false;
    const int seconds = _settings.value(KEY_CLIMATE_SAMPLE_SECONDS, defaultSeconds).toInt(&ok);
    if(ok == false || seconds < 1) {
        Log::logText(LVL_WARNING, QString("Ignoring %1: expected a whole number of seconds, at least 1").arg(KEY_CLIMATE_SAMPLE_SECONDS));
        return defaultSeconds;
    }

    return seconds;
}

QString IrrigationSettings::climateDatabasePath() const
{
    if(_settings.contains(KEY_CLIMATE_DATABASE_PATH)) {
        return _settings.value(KEY_CLIMATE_DATABASE_PATH).toString();
    }
    return QFileInfo(databasePath()).dir().filePath("climate.db");
}

#include "moc_irrigationsettings.cpp"
