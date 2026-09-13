#include "irrigationsettings.h"

const QString IrrigationSettings::KEY_ZONES              = "gpio/zones";
const QString IrrigationSettings::KEY_CHIP_LABEL         = "gpio/chipLabel";
const QString IrrigationSettings::KEY_ZONE_ACTIVE_LOW    = "gpio/zoneActiveLow";
const QString IrrigationSettings::KEY_STOP_BUTTON_OFFSET = "gpio/stopButtonOffset";
const QString IrrigationSettings::KEY_MAX_ZONE_SECONDS   = "limits/maxZoneSeconds";
const QString IrrigationSettings::KEY_BIND_ADDRESS       = "server/bindAddress";
const QString IrrigationSettings::KEY_LISTEN_PORT        = "server/listenPort";
const QString IrrigationSettings::KEY_DATABASE_PATH      = "database/path";

IrrigationSettings::IrrigationSettings(const QString& path) :
    AppSettings(path)
{
}

QMap<int, quint32> IrrigationSettings::zoneGpioMap() const
{
    QMap<int, quint32> result;

    QString raw = _settings.value(KEY_ZONES).toString();
    const QStringList pairs = raw.split(',', Qt::SkipEmptyParts);
    for(const QString& pair : pairs) {
        QStringList parts = pair.split('=');
        if(parts.count() != 2) {
            continue;
        }

        bool zoneOk = false;
        bool offsetOk = false;
        int zone = parts.at(0).trimmed().toInt(&zoneOk);
        quint32 offset = parts.at(1).trimmed().toUInt(&offsetOk);
        if(zoneOk == false || offsetOk == false) {
            continue;
        }

        result.insert(zone, offset);
    }

    return result;
}

#include "moc_irrigationsettings.cpp"
