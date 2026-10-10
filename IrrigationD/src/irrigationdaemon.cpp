#include "irrigationdaemon.h"

#include "climatelogger.h"
#include "database/climatedatasource.h"
#include "database/irrigationdatasource.h"
#include "irrigationcontrolserver.h"
#include "irrigationsettings.h"
#include "panelcontroller.h"
#include "panelhost.h"
#include "programqueue.h"
#include "programrunner.h"
#include "runbutton.h"
#include "scheduler.h"
#include "sht30.h"
#include "stopbutton.h"
#include "tm1637display.h"
#include "zonecontroller.h"

#include <Kanoop/commonexception.h>
#include <Kanoop/log.h>
#include <Kanoop/loggingtypes.h>
#include <Kanoop/timespan.h>
#include <Kanoop/pi/libgpiodbackend.h>

#include <QFileInfo>
#include <QStorageInfo>
#include <QTimeZone>
#include <QTimer>

const TimeSpan IrrigationDaemon::StatusInterval = TimeSpan::fromSeconds(1);

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

        const QMap<int, quint32> zoneGpioMap = _settings->zoneGpioMap();
        if(zoneGpioMap.isEmpty()) {
            throw CommonException(QString("%1 is empty or missing in %2")
                                  .arg(IrrigationSettings::zonesKey(), _settingsPath));
        }

        _backend = new LibGpiodBackend();
        if(_backend->openChipByLabel(_settings->chipLabel()) == false) {
            throw CommonException(QString("Failed to open GPIO chip '%1': %2")
                                  .arg(_settings->chipLabel(), _backend->errorText()));
        }

        _zoneController = new ZoneController(_backend,
                                             zoneGpioMap,
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

        const QString mountPoint = _settings->databaseMountPoint();
        if(mountPoint.isEmpty() == false) {
            const QString directory = QFileInfo(_settings->databasePath()).absolutePath();
            const QString actual = QStorageInfo(directory).rootPath();
            if(actual != mountPoint) {
                throw CommonException(QString("The database directory '%1' sits on mount '%2'; it must be on '%3'")
                                      .arg(directory, actual, mountPoint));
            }
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

        setUpClimate();

        _programRunner = new ProgramRunner(_zoneController, _dataSource);
        _programQueue = new ProgramQueue(_programRunner, _dataSource, &_clock);
        _programQueue->recordRestartDrops();
        _scheduler = new Scheduler(_dataSource, &_clock);

        setUpPanel();

        _controlServer = new IrrigationControlServer(_settings->databasePath());
        _controlServer->setBindAddress(_settings->bindAddress());
        _controlServer->setListenPort(_settings->listenPort());

        _statusTimer = new QTimer();
        _statusTimer->setInterval(static_cast<int>(StatusInterval.totalMilliseconds()));

        connectComponents();

        _panel->tick();
        _panelTimer->start();
        _panelRefreshTimer->start();
        if(_display != nullptr && _displayFailing == false) {
            logText(LVL_INFO, "The panel is showing its first frame");
        }
        else {
            logText(LVL_INFO, "The panel is running without its display");
        }

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

    delete _panelRefreshTimer;
    _panelRefreshTimer = nullptr;

    delete _panelTimer;
    _panelTimer = nullptr;

    // ~ClimateLogger commits its pending readings, so it goes before the store.
    delete _climateLogger;
    _climateLogger = nullptr;

    delete _climateStore;
    _climateStore = nullptr;

    delete _climateSensor;
    _climateSensor = nullptr;

    // The queue goes before the runner aborts: an aborted program starts the next queued
    // one, and nothing may open a valve during teardown.
    delete _programQueue;
    _programQueue = nullptr;

    delete _panel;
    _panel = nullptr;

    delete _panelHost;
    _panelHost = nullptr;

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

    delete _runButton;
    _runButton = nullptr;

    // ~Tm1637Display releases its lines through the backend, which must still hold the chip.
    delete _display;
    _display = nullptr;

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

void IrrigationDaemon::setUpClimate()
{
    const int bus = _settings->climateBus();
    if(bus < 0) {
        logText(LVL_INFO, "No climate sensor is configured");
        return;
    }

    const QString path = _settings->climateDatabasePath();
    _climateStore = new ClimateDataSource(path);
    if(_climateStore->open() == false) {
        logText(LVL_ERROR, QString("Failed to open the climate database '%1', so climate logging is off: %2")
                               .arg(path, _climateStore->errorText()));
        delete _climateStore;
        _climateStore = nullptr;
        return;
    }

    _climateSensor = new Sht30(bus, _settings->climateAddress());
    _climateLogger = new ClimateLogger(_climateSensor, _climateStore, &_clock,
                                       _settings->climateSampleSeconds(), ClimateFlushSeconds);
    _climateLogger->start();
    logText(LVL_INFO, QString("Logging climate from 0x%1 on I2C bus %2 every %3 s to %4")
                          .arg(static_cast<uint>(_settings->climateAddress()), 2, 16, QChar('0'))
                          .arg(bus).arg(_settings->climateSampleSeconds()).arg(path));
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
    connect(_controlServer, &IrrigationControlServer::zoneStopRequested,
            this, &IrrigationDaemon::onZoneStopRequested);
    connect(_controlServer, &IrrigationControlServer::settingsChanged,
            this, &IrrigationDaemon::onSettingsChanged);
    connect(_statusTimer, &QTimer::timeout, this, &IrrigationDaemon::publishStatus);

    if(_runButton != nullptr) {
        connect(_runButton, &RunButton::lineChanged, _panel, &PanelController::onRunLineChanged);
    }
    connect(_panel, &PanelController::frameChanged, this, &IrrigationDaemon::onPanelFrame);
    connect(_panel, &PanelController::stateChanged, this, &IrrigationDaemon::publishStatus);
    connect(_panelTimer, &QTimer::timeout, _panel, &PanelController::tick);
    connect(_panelRefreshTimer, &QTimer::timeout, this, &IrrigationDaemon::onPanelRefresh);
}

void IrrigationDaemon::onStopPressed()
{
    if(isTornDown()) {
        return;
    }

    logText(LVL_WARNING, "Stop requested");

    if(_panel != nullptr) {
        _panel->cancel();
    }

    QString errorText;
    if(_panelHost->clearController(&errorText) == false) {
        logText(LVL_ERROR, QString("Failed to close the zones on a stop request: %1").arg(errorText));
    }

    publishStatus();
}

void IrrigationDaemon::onProgramDue(int programId, int startTimeId, const QDateTime& scheduledAtUtc)
{
    if(isTornDown()) {
        return;
    }

    if(_stopButton->isHeld()) {
        logText(LVL_WARNING, QString("Program %1 came due while the stop button is held").arg(programId));
        if(_dataSource->setFiringOutcome(programId, startTimeId, scheduledAtUtc, FiredInstant::Outcome::SkippedStop) == false) {
            logText(LVL_ERROR, QString("Failed to record program %1 as skipped").arg(programId));
        }
    }
    else {
        _programQueue->enqueueScheduled(programId, startTimeId, scheduledAtUtc);
    }

    publishStatus();
}

void IrrigationDaemon::onManualZoneRunRequested(int zoneNumber, int seconds, const RunRequestPtr& decision)
{
    if(isTornDown()) {
        decision->complete(RunRequest::Refusal::Failed, "the controller is shutting down");
        return;
    }

    RunRequest::Refusal refusal = RunRequest::Refusal::None;
    QString message;

    if(_stopButton->isHeld()) {
        logText(LVL_WARNING, QString("Refused a manual run of zone %1: the stop button is held").arg(zoneNumber));
        refusal = RunRequest::Refusal::StopHeld;
        message = "the stop button is held";
    }
    else if(_dataSource->isMasterEnabled() == false) {
        logText(LVL_WARNING, QString("Refused a manual run of zone %1: the master enable is off").arg(zoneNumber));
        refusal = RunRequest::Refusal::MasterDisabled;
        message = "watering is turned off";
    }
    else if(isZoneEnabled(zoneNumber) == false) {
        logText(LVL_WARNING, QString("Refused a manual run of zone %1: the zone is disabled or has no database row").arg(zoneNumber));
        refusal = RunRequest::Refusal::ZoneDisabled;
        message = QString("zone %1 is disabled").arg(zoneNumber);
    }
    else if(_zoneController->hasSlotFor(zoneNumber) == false) {
        const int open = static_cast<int>(_zoneController->openZoneNumbers().count());
        logText(LVL_WARNING, QString("Refused a manual run of zone %1: %2 zones already running").arg(zoneNumber).arg(open));
        refusal = RunRequest::Refusal::CapReached;
        message = QString("%1 zones already running").arg(open);
    }
    else if(_zoneController->openZone(zoneNumber, seconds) == false) {
        logText(LVL_ERROR, QString("Failed to open zone %1 for %2 seconds: %3")
                               .arg(zoneNumber).arg(seconds).arg(_zoneController->errorText()));
        refusal = RunRequest::Refusal::Failed;
        message = _zoneController->errorText();
    }

    // publishStatus() precedes complete(): the snapshot then reaches the server thread
    // ahead of the reply, so the poll a client fires on the reply already sees the change.
    publishStatus();
    decision->complete(refusal, message);
}

void IrrigationDaemon::onProgramRunRequested(int programId, const RunRequestPtr& decision)
{
    if(isTornDown()) {
        decision->complete(RunRequest::Refusal::Failed, "the controller is shutting down");
        return;
    }

    RunRequest::Refusal refusal = RunRequest::Refusal::None;
    QString message;

    if(_stopButton->isHeld()) {
        logText(LVL_WARNING, QString("Refused a manual run of program %1: the stop button is held").arg(programId));
        refusal = RunRequest::Refusal::StopHeld;
        message = "the stop button is held";
    }
    else if(_dataSource->isMasterEnabled() == false) {
        logText(LVL_WARNING, QString("Refused a manual run of program %1: the master enable is off").arg(programId));
        refusal = RunRequest::Refusal::MasterDisabled;
        message = "watering is turned off";
    }
    else {
        refusal = _programQueue->enqueueManual(programId);
        if(refusal == RunRequest::Refusal::AlreadyQueued) {
            logText(LVL_WARNING, QString("Refused a manual run of program %1: it is already running or queued").arg(programId));
            message = "that program is already running or queued";
        }
        else if(refusal == RunRequest::Refusal::Failed) {
            if(_zoneController->isFaulted()) {
                message = QString("program %1 failed to start: %2").arg(programId).arg(_zoneController->errorText());
            }
            else {
                message = QString("program %1 could not start").arg(programId);
            }
        }
    }

    // publishStatus() precedes complete(): the snapshot then reaches the server thread
    // ahead of the reply, so the poll a client fires on the reply already sees the change.
    publishStatus();
    decision->complete(refusal, message);
}

void IrrigationDaemon::onZoneStopRequested(int zoneNumber)
{
    if(isTornDown()) {
        return;
    }

    logText(LVL_INFO, QString("Stop requested for zone %1").arg(zoneNumber));
    if(_zoneController->closeZone(zoneNumber) == false) {
        logText(LVL_ERROR, QString("Failed to close zone %1: %2").arg(zoneNumber).arg(_zoneController->errorText()));
    }

    publishStatus();
}

void IrrigationDaemon::onSettingsChanged()
{
    if(isTornDown()) {
        return;
    }

    applyRuntimeSettings();
    _panelHost->setRunMinutes(_panelRunMinutes);
    _programRunner->fillSlots();
    publishStatus();
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

    const QString storedCap = _dataSource->settingValue("max_concurrent_zones");
    bool parsedCap = false;
    const int cap = storedCap.toInt(&parsedCap);
    _zoneController->setMaxConcurrentZones(parsedCap == true ? cap : ZoneController::DefaultMaxConcurrentZones);
    logText(LVL_INFO, QString("At most %1 zones open at once (database '%2')")
                          .arg(_zoneController->maxConcurrentZones()).arg(storedCap));

    const QString storedPanelMinutes = _dataSource->settingValue("panel_run_minutes");
    bool parsedPanelMinutes = false;
    const int panelMinutes = storedPanelMinutes.toInt(&parsedPanelMinutes);
    _panelRunMinutes = parsedPanelMinutes == true
                           && panelMinutes >= PanelController::MinimumRunMinutes
                           && panelMinutes <= PanelController::MaximumRunMinutes
                       ? panelMinutes
                       : PanelController::DefaultRunMinutes;
    logText(LVL_INFO, QString("Panel runs last %1 %2 (database '%3')")
                          .arg(_panelRunMinutes)
                          .arg(_panelRunMinutes == 1 ? "minute" : "minutes")
                          .arg(storedPanelMinutes));

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

void IrrigationDaemon::setUpPanel()
{
    const int clockOffset = _settings->displayClockOffset();
    const int dataOffset = _settings->displayDataOffset();
    if(clockOffset < 0 || dataOffset < 0) {
        logText(LVL_WARNING, "displayClockOffset or displayDataOffset is not configured; the panel runs without its display");
    }
    else {
        logText(LVL_INFO, QString("Display clock line is %1, data line is %2").arg(clockOffset).arg(dataOffset));
        _display = new Tm1637Display(_backend, static_cast<quint32>(clockOffset), static_cast<quint32>(dataOffset));
        if(_display->begin() == false) {
            logText(LVL_ERROR, QString("Failed to request the display lines %1 and %2: %3; the panel runs without its display")
                                   .arg(clockOffset).arg(dataOffset).arg(_display->errorText()));
            delete _display;
            _display = nullptr;
        }
    }

    const int runOffset = _settings->runButtonOffset();
    if(runOffset < 0) {
        logText(LVL_WARNING, "runButtonOffset is not configured; the RUN button is disabled");
    }
    else {
        logText(LVL_INFO, QString("RUN button line is %1").arg(runOffset));
        _runButton = new RunButton(_backend, static_cast<quint32>(runOffset));
        if(_runButton->begin() == false) {
            logText(LVL_ERROR, QString("Failed to request the RUN button line %1: %2; the RUN button is disabled")
                                   .arg(runOffset).arg(_runButton->errorText()));
            delete _runButton;
            _runButton = nullptr;
        }
    }

    _panelHost = new PanelHost(_zoneController, _programRunner, _programQueue, _dataSource, _stopButton);
    _panelHost->setRunMinutes(_panelRunMinutes);

    _panel = new PanelController(_panelHost, &_clock);
    _panel->setTimeZone(QTimeZone::systemTimeZone());
    if(_runButton != nullptr) {
        _panel->setInitialRunLine(_runButton->isLow());
        if(_runButton->isLow() == true) {
            logText(LVL_WARNING, "The RUN button reads pressed at startup");
        }
    }

    _panelTimer = new QTimer();
    _panelTimer->setInterval(PanelTickMilliseconds);

    _panelRefreshTimer = new QTimer();
    _panelRefreshTimer->setInterval(PanelRefreshMilliseconds);
}

void IrrigationDaemon::onPanelFrame(const QByteArray& segments)
{
    if(_display == nullptr) {
        return;
    }

    if(_display->show(segments) == false) {
        if(_displayFailing == false) {
            logText(LVL_ERROR, QString("Failed to write the display: %1").arg(_display->errorText()));
        }
        _displayFailing = true;
    }
    else if(_displayFailing == true) {
        logText(LVL_INFO, "The display is writing again");
        _displayFailing = false;
    }
}

void IrrigationDaemon::onPanelRefresh()
{
    if(_display == nullptr || _panel == nullptr) {
        return;
    }

    const QByteArray frame = _panel->frame();
    if(frame.isEmpty() == true) {
        return;
    }

    onPanelFrame(frame);
}

void IrrigationDaemon::publishStatus()
{
    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();

    ServerStatus status;

    const QList<int> openZones = _zoneController->openZoneNumbers();
    for(int zoneNumber : openZones) {
        RunningZoneStatus running;
        running.zone = zoneNumber;
        running.secondsRemaining = _zoneController->secondsRemaining(zoneNumber);
        if(_programRunner->ownsZone(zoneNumber) == true) {
            running.source = RunningZoneStatus::Source::Program;
        }
        else if(_panel != nullptr && _panel->panelZone() == zoneNumber) {
            running.source = RunningZoneStatus::Source::Panel;
        }
        status.running.append(running);
    }

    if(_programRunner->isRunning()) {
        status.programId = _programRunner->runningProgramId();
        status.programName = _programRunner->runningProgramName();
        status.programStep = _programRunner->stepNumber();
        status.programStepCount = _programRunner->stepCount();
        status.waitingZones = _programRunner->waitingZones();
    }

    const QList<ProgramQueue::Entry> waiting = _programQueue->entries();
    if(waiting.isEmpty() == false) {
        const ProgramList programs = _dataSource->allPrograms();
        for(const ProgramQueue::Entry& entry : waiting) {
            QueuedProgramStatus queued;
            queued.programId = entry.programId;
            queued.queuedAtUtc = entry.queuedAtUtc;
            for(const Program& program : programs) {
                if(program.id == entry.programId) {
                    queued.name = program.name;
                    break;
                }
            }
            status.queue.append(queued);
        }
    }

    status.maxConcurrentZones = _zoneController->maxConcurrentZones();
    status.timezone = QString::fromUtf8(QTimeZone::systemTimeZoneId());
    status.masterEnabled = _dataSource->isMasterEnabled();
    status.stopHeld = _stopButton->isHeld();
    status.rainDelayUntilUtc =
        QDateTime::fromString(_dataSource->settingValue("rain_delay_until"), Qt::ISODate).toUTC();

    if(status.masterEnabled == true) {
        const bool rainDelayed = status.rainDelayUntilUtc.isValid() && nowUtc < status.rainDelayUntilUtc;
        status.nextRunUtc = nextScheduledRunUtc(rainDelayed == true ? status.rainDelayUntilUtc : nowUtc);
    }

    _controlServer->updateStatus(status);
}

QDateTime IrrigationDaemon::nextScheduledRunUtc(const QDateTime& fromUtc)
{
    QDateTime earliest;

    const ProgramList programs = _dataSource->enabledPrograms();
    for(const Program& program : programs) {
        const QDateTime candidate =
            Scheduler::nextRunUtc(program, _dataSource->startTimesFor(program.id), fromUtc);
        if(candidate.isValid() && (earliest.isValid() == false || candidate < earliest)) {
            earliest = candidate;
        }
    }

    return earliest;
}

#include "moc_irrigationdaemon.cpp"
