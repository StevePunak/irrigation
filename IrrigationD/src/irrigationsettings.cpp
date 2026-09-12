#include "irrigationsettings.h"

IrrigationSettings::IrrigationSettings(const QString& path) :
    _settings(path, QSettings::IniFormat)
{
}

QMap<int, quint32> IrrigationSettings::zoneGpioMap() const
{
    QMap<int, quint32> result;

    QString raw = _settings.value("gpio/zones").toString();
    const QStringList pairs = raw.split(',', Qt::SkipEmptyParts);
    for(const QString& pair : pairs) {
        QStringList parts = pair.split('=');
        if(parts.count() != 2) {
            continue;
        }

        bool zoneOk = false;
        bool offsetOk = false;
        int zone = parts.at(0).trimmed().toInt(&zoneOk);
        uint offset = parts.at(1).trimmed().toUInt(&offsetOk);
        if(zoneOk == false || offsetOk == false) {
            continue;
        }

        result.insert(zone, offset);
    }

    return result;
}

QString IrrigationSettings::chipLabel() const
{
    return _settings.value("gpio/chipLabel", "pinctrl-bcm2711").toString();
}

bool IrrigationSettings::zoneActiveLow() const
{
    return _settings.value("gpio/zoneActiveLow", true).toBool();
}

quint32 IrrigationSettings::stopButtonOffset() const
{
    return _settings.value("gpio/stopButtonOffset", 25).toUInt();
}

int IrrigationSettings::maxZoneSeconds() const
{
    return _settings.value("limits/maxZoneSeconds", 3600).toInt();
}

QString IrrigationSettings::bindAddress() const
{
    return _settings.value("server/bindAddress", "127.0.0.1").toString();
}

int IrrigationSettings::listenPort() const
{
    return _settings.value("server/listenPort", 8080).toInt();
}

QString IrrigationSettings::databasePath() const
{
    return _settings.value("database/path", "/var/lib/irrigationd/irrigation.db").toString();
}
