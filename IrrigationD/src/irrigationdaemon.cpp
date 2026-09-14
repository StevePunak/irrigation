#include "irrigationdaemon.h"

#include "database/irrigationdatasource.h"
#include "irrigationcontrolserver.h"
#include "irrigationsettings.h"
#include "programrunner.h"
#include "scheduler.h"
#include "stopbutton.h"
#include "zonecontroller.h"

#include <Kanoop/commonexception.h>
#include <Kanoop/log.h>
#include <Kanoop/loggingtypes.h>
#include <Kanoop/timespan.h>
#include <Kanoop/pi/libgpiodbackend.h>

#include <QTimeZone>
#include <QTimer>

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

        _stopButton = new StopButton(_backend, _settings->stopButtonOffset());
        if(_stopButton->begin() == false) {
            throw CommonException(QString("Failed to request the stop button line %1: %2")
                                  .arg(_settings->stopButtonOffset()).arg(_stopButton->errorText()));
        }
        if(_stopButton->isHeld()) {
            logText(LVL_WARNING, "The stop button is held at startup");
        }

        _dataSource = new IrrigationDataSource(_settings->databasePath());
        if(_dataSource->open() == false) {
            throw CommonException(QString("Failed to open database '%1': %2")
                                  .arg(_settings->databasePath(), _dataSource->errorText()));
        }
        if(_dataSource->pruneFiredInstantsOlderThan(QDateTime::currentDateTimeUtc().addDays(-FiredInstantRetentionDays)) == false) {
            logText(LVL_WARNING, "Failed to prune old fired instants");
        }

        applyRuntimeSettings();

        _programRunner = new ProgramRunner(_zoneController, _dataSource);
        _scheduler = new Scheduler(_dataSource, &_clock);

        _controlServer = new IrrigationControlServer(_settings->databasePath());
        _controlServer->setBindAddress(_settings->bindAddress());
        _controlServer->setListenPort(_settings->listenPort());

        _statusTimer = new QTimer();
        _statusTimer->setInterval(StatusIntervalMilliseconds);

        connectComponents();

        if(_controlServer->start() == false) {
            throw CommonException("The control server thread failed to start");
        }
        if(_controlServer->waitUntilReady(TimeSpan::fromSeconds(ControlServerReadySeconds)) == false) {
            throw CommonException(QString("The control server never bound %1:%2")
                                  .arg(_settings->bindAddress()).arg(_settings->listenPort()));
        }

        _scheduler->start();

        publishStatus();
        _statusTimer->start();

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

    delete _statusTimer;
    _statusTimer = nullptr;

    if(_programRunner != nullptr) {
        _programRunner->abort();
    }
    if(_zoneController != nullptr && _zoneController->allOff() == false) {
        logText(LVL_ERROR, QString("Failed to close the zones during teardown: %1")
                               .arg(_zoneController->errorText()));
    }

    if(_controlServer != nullptr) {
        // Never deleteLater() an AbstractThreadClass: once stop() returns, the thread
        // and the event loop that would run the deletion are gone.
        _controlServer->stop();
        delete _controlServer;
        _controlServer = nullptr;
    }

    delete _scheduler;
    _scheduler = nullptr;

    delete _programRunner;
    _programRunner = nullptr;

    delete _stopButton;
    _stopButton = nullptr;

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

void IrrigationDaemon::connectComponents()
{
    // ZoneController::zoneClosed and ZoneController::watchdogTripped are connected
    // by the ProgramRunner constructor. Connecting them again here delivers each
    // one to the runner twice.
    connect(_stopButton, &StopButton::pressed, this, &IrrigationDaemon::onStopPressed);
    connect(_scheduler, &Scheduler::programDue, this, &IrrigationDaemon::onProgramDue);
    connect(_controlServer, &IrrigationControlServer::stopRequested,
            this, &IrrigationDaemon::onStopPressed);
    connect(_controlServer, &IrrigationControlServer::manualZoneRunRequested,
            this, &IrrigationDaemon::onManualZoneRunRequested);
    connect(_controlServer, &IrrigationControlServer::programRunRequested,
            this, &IrrigationDaemon::onProgramRunRequested);
    connect(_controlServer, &IrrigationControlServer::settingsChanged,
            this, &IrrigationDaemon::onSettingsChanged);
    connect(_statusTimer, &QTimer::timeout, this, &IrrigationDaemon::publishStatus);
}

void IrrigationDaemon::onStopPressed()
{
    logText(LVL_WARNING, "Stop requested");

    // abort() precedes allOff(): allOff() emits zoneClosed, which advances a
    // running program onto its next zone.
    _programRunner->abort();

    if(_zoneController->allOff() == false) {
        logText(LVL_ERROR, QString("Failed to close the zones on a stop request: %1")
                               .arg(_zoneController->errorText()));
    }

    publishStatus();
}

void IrrigationDaemon::onProgramDue(int programId, int startTimeId, const QDateTime& scheduledAtUtc)
{
    if(_stopButton->isHeld()) {
        logText(LVL_WARNING, QString("Program %1 came due while the stop button is held").arg(programId));
        if(_dataSource->setFiringOutcome(programId, startTimeId, scheduledAtUtc, FiredInstant::Outcome::SkippedStop) == false) {
            logText(LVL_ERROR, QString("Failed to record program %1 as skipped").arg(programId));
        }
    }
    else if(_programRunner->isRunning() || _zoneController->openZoneNumber() != 0) {
        logText(LVL_WARNING, QString("Program %1 came due while zone %2 is busy")
                                 .arg(programId).arg(_zoneController->openZoneNumber()));
        if(_dataSource->setFiringOutcome(programId, startTimeId, scheduledAtUtc, FiredInstant::Outcome::SkippedBusy) == false) {
            logText(LVL_ERROR, QString("Failed to record program %1 as skipped").arg(programId));
        }
    }
    else if(_programRunner->startProgram(programId) == false) {
        logText(LVL_ERROR, QString("Failed to start program %1: %2")
                               .arg(programId).arg(_zoneController->errorText()));
        if(_dataSource->setFiringOutcome(programId, startTimeId, scheduledAtUtc, FiredInstant::Outcome::Failed) == false) {
            logText(LVL_ERROR, QString("Failed to record program %1 as failed").arg(programId));
        }
    }

    publishStatus();
}

void IrrigationDaemon::onManualZoneRunRequested(int zoneNumber, int seconds)
{
    if(_stopButton->isHeld()) {
        logText(LVL_WARNING, QString("Refused a manual run of zone %1: the stop button is held").arg(zoneNumber));
    }
    else if(_dataSource->isMasterEnabled() == false) {
        logText(LVL_WARNING, QString("Refused a manual run of zone %1: the master enable is off").arg(zoneNumber));
    }
    else if(isZoneEnabled(zoneNumber) == false) {
        logText(LVL_WARNING, QString("Refused a manual run of zone %1: the zone is disabled or has no database row").arg(zoneNumber));
    }
    else {
        // abort() precedes openZone(): openZone() closes the open zone, and the
        // zoneClosed it emits advances a running program onto its next zone.
        _programRunner->abort();

        if(_zoneController->openZone(zoneNumber, seconds) == false) {
            logText(LVL_ERROR, QString("Failed to open zone %1 for %2 seconds: %3")
                                   .arg(zoneNumber).arg(seconds).arg(_zoneController->errorText()));
        }

        publishStatus();
    }
}

void IrrigationDaemon::onProgramRunRequested(int programId)
{
    if(_stopButton->isHeld()) {
        logText(LVL_WARNING, QString("Refused a manual run of program %1: the stop button is held").arg(programId));
    }
    else if(_dataSource->isMasterEnabled() == false) {
        logText(LVL_WARNING, QString("Refused a manual run of program %1: the master enable is off").arg(programId));
    }
    else {
        // abort() precedes startProgram(): startProgram() refuses while a program
        // is already running, so calling it before abort() drops the request.
        _programRunner->abort();

        if(_programRunner->startProgram(programId) == false) {
            logText(LVL_ERROR, QString("Failed to start program %1: %2")
                                   .arg(programId).arg(_zoneController->errorText()));
        }

        publishStatus();
    }
}

void IrrigationDaemon::onSettingsChanged()
{
    applyRuntimeSettings();
}

void IrrigationDaemon::applyRuntimeSettings()
{
    const int iniCeiling = _settings->maxZoneSeconds();
    const QString storedCeiling = _dataSource->settingValue("max_zone_seconds");
    bool parsed = false;
    const int databaseCeiling = storedCeiling.toInt(&parsed);
    _zoneController->setMaxZoneSeconds(parsed == true && databaseCeiling > 0 ? databaseCeiling : iniCeiling);
    logText(LVL_INFO, QString("Zone ceiling is %1 seconds (database '%2', INI %3)")
                          .arg(_zoneController->maxZoneSeconds()).arg(storedCeiling).arg(iniCeiling));

    const QString levelName = _dataSource->settingValue("log_level");
    if(_verboseLogging == true) {
        logText(LVL_INFO, QString("Ignoring log_level '%1': --verbose was given").arg(levelName));
    }
    else if(levelName.isEmpty() == false) {
        bool known = false;
        for(Log::LogLevel level : Log::getLogLevels()) {
            if(known == false && QString::compare(Log::getLogLevelString(level), levelName, Qt::CaseInsensitive) == 0) {
                known = true;
                logText(LVL_INFO, QString("Log level is %1").arg(Log::getLogLevelString(level)));
                Log::setLevel(level);
            }
        }
        if(known == false) {
            logText(LVL_WARNING, QString("Ignoring unrecognised log_level '%1'").arg(levelName));
        }
    }
}

bool IrrigationDaemon::isZoneEnabled(int zoneNumber)
{
    bool enabled = false;
    const ZoneList zones = _dataSource->allZones();
    for(const Zone& zone : zones) {
        if(zone.number == zoneNumber) {
            enabled = zone.enabled;
            break;
        }
    }
    return enabled;
}

void IrrigationDaemon::publishStatus()
{
    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();

    ServerStatus status;
    status.runningZone = _zoneController->openZoneNumber();
    status.secondsRemaining = _zoneController->secondsRemaining();
    status.nextRunUtc = nextScheduledRunUtc(nowUtc);
    status.timezone = QString::fromUtf8(QTimeZone::systemTimeZoneId());
    status.masterEnabled = _dataSource->isMasterEnabled();
    status.rainDelayUntilUtc =
        QDateTime::fromString(_dataSource->settingValue("rain_delay_until"), Qt::ISODate).toUTC();

    _controlServer->updateStatus(status);
}

QDateTime IrrigationDaemon::nextScheduledRunUtc(const QDateTime& nowUtc)
{
    QDateTime earliest;

    const ProgramList programs = _dataSource->enabledPrograms();
    for(const Program& program : programs) {
        const QDateTime candidate =
            Scheduler::nextRunUtc(program, _dataSource->startTimesFor(program.id), nowUtc);
        if(candidate.isValid() && (earliest.isValid() == false || candidate < earliest)) {
            earliest = candidate;
        }
    }

    return earliest;
}

#include "moc_irrigationdaemon.cpp"
