#include "irrigationdaemon.h"

#include "database/irrigationdatasource.h"
#include "irrigationsettings.h"
#include "scheduler.h"
#include "zonecontroller.h"

#include <Kanoop/commonexception.h>
#include <Kanoop/pi/libgpiodbackend.h>

IrrigationDaemon::IrrigationDaemon(const QString& settingsPath) :
    AbstractThreadClass("daemon"),
    _settingsPath(settingsPath),
    _errorText("Startup has not run")
{
    setBlockingStart(true);
}

IrrigationDaemon::~IrrigationDaemon()
{
    stop();
}

void IrrigationDaemon::abort()
{
    stop();
}

void IrrigationDaemon::threadStarted()
{
    try
    {
        logText(LVL_INFO, "Starting irrigationd");

        _settings = new IrrigationSettings(_settingsPath);

        _backend = new LibGpiodBackend();
        if(_backend->openChipByLabel(_settings->chipLabel()) == false) {
            throw CommonException(QString("Failed to open GPIO chip '%1': %2")
                                  .arg(_settings->chipLabel(), _backend->errorText()));
        }

        _zoneController = new ZoneController(_backend,
                                             _settings->zoneGpioMap(),
                                             _settings->zoneActiveLow(),
                                             _settings->maxZoneSeconds());
        if(_zoneController->begin() == false) {
            throw CommonException(QString("Failed to request the zone lines: %1").arg(_zoneController->errorText()));
        }

        _dataSource = new IrrigationDataSource(_settings->databasePath());
        if(_dataSource->open() == false) {
            throw CommonException(QString("Failed to open database '%1': %2")
                                  .arg(_settings->databasePath(), _dataSource->errorText()));
        }

        _scheduler = new Scheduler(_dataSource, &_clock);
        _scheduler->start();

        _errorText = QString();
        logText(LVL_INFO, "irrigationd is running");
    }
    catch(const CommonException& e)
    {
        _errorText = e.message();
        logText(LVL_ERROR, _errorText);
    }
}

void IrrigationDaemon::threadAboutToFinish()
{
    logText(LVL_INFO, "Stopping irrigationd");

    delete _scheduler;
    _scheduler = nullptr;

    // ~ZoneController drives every zone inactive, which needs the backend to
    // still hold the chip.
    delete _zoneController;
    _zoneController = nullptr;

    delete _dataSource;
    _dataSource = nullptr;

    delete _backend;
    _backend = nullptr;

    delete _settings;
    _settings = nullptr;
}

#include "moc_irrigationdaemon.cpp"
