#include "irrigationdaemon.h"

IrrigationDaemon::IrrigationDaemon(const QString& settingsPath, QObject* parent) :
    QObject(parent),
    LoggingBaseClass("daemon"),
    _settings(settingsPath)
{
}

IrrigationDaemon::~IrrigationDaemon()
{
    stop();
}

bool IrrigationDaemon::start()
{
    logText(LVL_INFO, "Starting irrigationd");
    _running = true;
    return true;
}

void IrrigationDaemon::stop()
{
    if(_running == false) {
        return;
    }

    logText(LVL_INFO, "Stopping irrigationd");
    _running = false;
}

#include "moc_irrigationdaemon.cpp"
