#include "irrigationdaemon.h"

#include "database/irrigationdatasource.h"
#include "irrigationcontrolserver.h"
#include "irrigationsettings.h"
#include "programrunner.h"
#include "scheduler.h"
#include "stopbutton.h"
#include "zonecontroller.h"

#include <Kanoop/commonexception.h>
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
        _dataSource->pruneFiredInstantsOlderThan(QDateTime::currentDateTimeUtc().addDays(-FiredInstantRetentionDays));

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
    if(_programRunner->isRunning()) {
        logText(LVL_WARNING, QString("Program %1 came due while program %2 is running")
                                 .arg(programId).arg(_programRunner->runningProgramId()));

        FiredInstant instant;
        instant.programId = programId;
        instant.startTimeId = startTimeId;
        instant.scheduledAtUtc = scheduledAtUtc;
        instant.outcome = FiredInstant::Outcome::SkippedBusy;
        if(_dataSource->recordFiring(instant) == false) {
            logText(LVL_ERROR, QString("Failed to record program %1 as skipped").arg(programId));
        }
    }
    else if(_programRunner->startProgram(programId) == false) {
        logText(LVL_ERROR, QString("Failed to start program %1").arg(programId));
    }

    publishStatus();
}

void IrrigationDaemon::onManualZoneRunRequested(int zoneNumber, int seconds)
{
    if(isMasterEnabled() == false) {
        logText(LVL_WARNING, QString("Refused a manual run of zone %1: the master enable is off").arg(zoneNumber));
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
    if(isMasterEnabled() == false) {
        logText(LVL_WARNING, QString("Refused a manual run of program %1: the master enable is off").arg(programId));
    }
    else {
        if(_programRunner->startProgram(programId) == false) {
            logText(LVL_ERROR, QString("Failed to start program %1, program %2 is running")
                                   .arg(programId).arg(_programRunner->runningProgramId()));
        }

        publishStatus();
    }
}

void IrrigationDaemon::publishStatus()
{
    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();

    ServerStatus status;
    status.runningZone = _zoneController->openZoneNumber();
    status.secondsRemaining = _zoneController->secondsRemaining();
    status.nextRunUtc = nextScheduledRunUtc(nowUtc);
    status.timezone = QString::fromUtf8(QTimeZone::systemTimeZoneId());
    status.masterEnabled = isMasterEnabled();
    status.rainDelayUntilUtc =
        QDateTime::fromString(_dataSource->settingValue("rain_delay_until"), Qt::ISODate).toUTC();

    _controlServer->updateStatus(status);
}

bool IrrigationDaemon::isMasterEnabled()
{
    // Scheduler::tick() gates scheduled runs on this exact string.
    return _dataSource->settingValue("master_enabled") != "0";
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
