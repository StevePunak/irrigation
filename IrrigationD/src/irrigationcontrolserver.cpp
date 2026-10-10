#include "irrigationcontrolserver.h"

#include "database/climatedatasource.h"
#include "database/irrigationdatasource.h"
#include "json/climatejson.h"
#include "json/programjson.h"
#include "json/statusjson.h"
#include "panelcontroller.h"
#include "scheduler.h"
#include "zonecontroller.h"

#include <Kanoop/loggingtypes.h>

#include <algorithm>

#include <QCoreApplication>
#include <QHostAddress>
#include <QHttpServerResponder>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTimeZone>
#include <QUrlQuery>
#include <QUuid>

const QStringList IrrigationControlServer::SettingsKeys = {
    "rain_delay_until", "master_enabled", "max_zone_seconds", "log_level", "max_concurrent_zones",
    "panel_run_minutes", "latitude", "longitude"
};

// Bounded: the daemon stops this server from its own thread during teardown, and a
// route still waiting on that thread for a decision would block the stop() forever.
const TimeSpan IrrigationControlServer::DefaultDecisionTimeout = TimeSpan::fromSeconds(5);

IrrigationControlServer::IrrigationControlServer(const QString& databasePath) :
    AbstractThreadClass("control-server"),
    _databasePath(databasePath),
    _bindAddress("127.0.0.1"),
    _listenPort(8080),
    _decisionTimeout(DefaultDecisionTimeout)
{
    IrrigationControlServer::setObjectName(IrrigationControlServer::metaObject()->className());
    qRegisterMetaType<ServerStatus>();
    qRegisterMetaType<RunRequestPtr>();
    connect(this, &IrrigationControlServer::statusUpdateRequested,
            this, &IrrigationControlServer::onStatusUpdateRequested);
}

IrrigationControlServer::~IrrigationControlServer()
{
    stop();
}

void IrrigationControlServer::abort()
{
    stop();
}

bool IrrigationControlServer::waitUntilReady(const TimeSpan& timeout)
{
    if(_ready.loadAcquire() != 0) {
        return true;
    }
    return _readyEvent.wait(timeout);
}

void IrrigationControlServer::onStatusUpdateRequested(const ServerStatus& status)
{
    _status = status;
}

void IrrigationControlServer::threadStarted()
{
    logText(LVL_INFO, "Control server thread started");

    _source = new IrrigationDataSource(_databasePath);
    _source->setConnectionName(QString("irrigation-http-%1")
                                   .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if(_source->open() == false) {
        logText(LVL_ERROR, QString("Control server could not open the database: %1")
                               .arg(_source->errorText()));
        return;
    }

    if(_climateDatabasePath.isEmpty() == false) {
        _climateSource = new ClimateDataSource(_climateDatabasePath);
        _climateSource->setConnectionName(QString("climate-http-%1")
                                              .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if(_climateSource->open() == false) {
            logText(LVL_ERROR, QString("Control server could not open the climate database: %1")
                                   .arg(_climateSource->errorText()));
            delete _climateSource;
            _climateSource = nullptr;
        }
    }

    _httpServer = new QHttpServer;

    _httpServer->route("/admin/health", QHttpServerRequest::Method::Get,
                       [this](const QHttpServerRequest& request)
    {
        return this->handleHealth(request);
    });

    _httpServer->route("/admin/version", QHttpServerRequest::Method::Get,
                       [this](const QHttpServerRequest& request)
    {
        return this->handleVersion(request);
    });

    _httpServer->route("/admin/status", QHttpServerRequest::Method::Get,
                       [this](const QHttpServerRequest& request)
    {
        return this->handleStatus(request);
    });

    _httpServer->route("/admin/zones", QHttpServerRequest::Method::Get,
                       [this](const QHttpServerRequest& request)
    {
        return this->handleZonesGet(request);
    });

    _httpServer->route("/admin/zones/<arg>", QHttpServerRequest::Method::Put,
                       [this](int zoneNumber, const QHttpServerRequest& request)
    {
        return this->handleZonePut(zoneNumber, request);
    });

    _httpServer->route("/admin/zones/<arg>/run", QHttpServerRequest::Method::Post,
                       [this](int zoneNumber, const QHttpServerRequest& request)
    {
        return this->handleZoneRun(zoneNumber, request);
    });

    _httpServer->route("/admin/zones/<arg>/stop", QHttpServerRequest::Method::Post,
                       [this](int zoneNumber, const QHttpServerRequest& request)
    {
        return this->handleZoneStop(zoneNumber, request);
    });

    _httpServer->route("/admin/programs", QHttpServerRequest::Method::Get,
                       [this](const QHttpServerRequest& request)
    {
        return this->handleProgramsGet(request);
    });

    _httpServer->route("/admin/programs", QHttpServerRequest::Method::Post,
                       [this](const QHttpServerRequest& request)
    {
        return this->handleProgramPost(request);
    });

    _httpServer->route("/admin/programs/<arg>", QHttpServerRequest::Method::Put,
                       [this](int programId, const QHttpServerRequest& request)
    {
        return this->handleProgramPut(programId, request);
    });

    _httpServer->route("/admin/programs/<arg>", QHttpServerRequest::Method::Delete,
                       [this](int programId, const QHttpServerRequest& request)
    {
        return this->handleProgramDelete(programId, request);
    });

    _httpServer->route("/admin/programs/<arg>/run", QHttpServerRequest::Method::Post,
                       [this](int programId, const QHttpServerRequest& request)
    {
        return this->handleProgramRun(programId, request);
    });

    _httpServer->route("/admin/stop", QHttpServerRequest::Method::Post,
                       [this](const QHttpServerRequest& request)
    {
        return this->handleStop(request);
    });

    _httpServer->route("/admin/settings", QHttpServerRequest::Method::Get,
                       [this](const QHttpServerRequest& request)
    {
        return this->handleSettingsGet(request);
    });

    _httpServer->route("/admin/settings", QHttpServerRequest::Method::Put,
                       [this](const QHttpServerRequest& request)
    {
        return this->handleSettingsPut(request);
    });

    _httpServer->route("/admin/climate", QHttpServerRequest::Method::Get,
                       [this](const QHttpServerRequest& request)
    {
        return this->handleClimateGet(request);
    });

    _tcpServer = new QTcpServer;
    if(_tcpServer->listen(QHostAddress(_bindAddress), _listenPort) == false) {
        logText(LVL_ERROR, QString("Failed to listen on %1:%2 - %3")
                               .arg(_bindAddress).arg(_listenPort).arg(_tcpServer->errorString()));
        delete _tcpServer;
        _tcpServer = nullptr;
        return;
    }

    if(_httpServer->bind(_tcpServer) == false) {
        logText(LVL_ERROR, "Failed to bind the HTTP server to the listening socket");
        delete _tcpServer;
        _tcpServer = nullptr;
        return;
    }

    _boundPort = _tcpServer->serverPort();
    _ready.storeRelease(1);
    _readyEvent.set();
}

void IrrigationControlServer::threadAboutToFinish()
{
    logText(LVL_INFO, "Control server thread finishing");

    _ready.storeRelease(0);
    _readyEvent.clear();

    delete _httpServer;
    _httpServer = nullptr;

    // _tcpServer is owned by _httpServer after bind() and was just destroyed with it.
    _tcpServer = nullptr;

    delete _source;
    _source = nullptr;

    delete _climateSource;
    _climateSource = nullptr;
}

QHttpServerResponse IrrigationControlServer::handleHealth(const QHttpServerRequest& request)
{
    Q_UNUSED(request)
    return QHttpServerResponse(QJsonObject{{"status", "ok"}}, QHttpServerResponder::StatusCode::Ok);
}

QHttpServerResponse IrrigationControlServer::handleVersion(const QHttpServerRequest& request)
{
    Q_UNUSED(request)
    return QHttpServerResponse(QJsonObject{{"version", QCoreApplication::applicationVersion()}},
                               QHttpServerResponder::StatusCode::Ok);
}

QHttpServerResponse IrrigationControlServer::handleStatus(const QHttpServerRequest& request)
{
    Q_UNUSED(request)
    return QHttpServerResponse(StatusJson::toJson(_status), QHttpServerResponder::StatusCode::Ok);
}

QJsonObject IrrigationControlServer::zoneToJson(const Zone& zone)
{
    QJsonObject object;
    object["id"] = zone.id;
    object["number"] = zone.number;
    object["name"] = zone.name;
    object["enabled"] = zone.enabled;
    return object;
}

QHttpServerResponse IrrigationControlServer::handleZonesGet(const QHttpServerRequest& request)
{
    Q_UNUSED(request)
    QJsonArray array;
    const ZoneList zones = _source->allZones();
    for(const Zone& zone : zones) {
        array.append(zoneToJson(zone));
    }
    return QHttpServerResponse(array, QHttpServerResponder::StatusCode::Ok);
}

QHttpServerResponse IrrigationControlServer::handleZonePut(int zoneNumber, const QHttpServerRequest& request)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(request.body(), &error);
    if(error.error != QJsonParseError::NoError || document.isObject() == false) {
        return QHttpServerResponse(QJsonObject{{"error", "malformed JSON"}},
                                   QHttpServerResponder::StatusCode::BadRequest);
    }

    const QJsonObject body = document.object();
    if(body.value("name").isString() == false || body.value("enabled").isBool() == false) {
        return QHttpServerResponse(QJsonObject{{"error", "name and enabled are required"}},
                                   QHttpServerResponder::StatusCode::BadRequest);
    }

    const ZoneList zones = _source->allZones();
    Zone zone;
    bool known = false;
    for(const Zone& candidate : zones) {
        if(candidate.number == zoneNumber) {
            zone = candidate;
            known = true;
            break;
        }
    }

    if(known == false) {
        return QHttpServerResponse(QJsonObject{{"error", "unknown zone"}},
                                   QHttpServerResponder::StatusCode::NotFound);
    }

    zone.name = body.value("name").toString();
    zone.enabled = body.value("enabled").toBool();

    if(_source->updateZone(zone) == false) {
        return QHttpServerResponse(QJsonObject{{"error", "failed to update zone"}},
                                   QHttpServerResponder::StatusCode::InternalServerError);
    }

    return QHttpServerResponse(zoneToJson(zone), QHttpServerResponder::StatusCode::Ok);
}

QHttpServerResponse IrrigationControlServer::handleZoneRun(int zoneNumber,
                                                           const QHttpServerRequest& request)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(request.body(), &error);
    if(error.error != QJsonParseError::NoError) {
        return QHttpServerResponse(QJsonObject{{"error", "malformed JSON"}},
                                   QHttpServerResponder::StatusCode::BadRequest);
    }

    const ZoneList zones = _source->allZones();
    bool known = false;
    bool enabled = false;
    for(const Zone& zone : zones) {
        if(zone.number == zoneNumber) {
            known = true;
            enabled = zone.enabled;
            break;
        }
    }

    if(known == false) {
        return QHttpServerResponse(QJsonObject{{"error", "unknown zone"}},
                                   QHttpServerResponder::StatusCode::NotFound);
    }

    const int seconds = document.object().value("seconds").toInt(0);
    if(seconds < 1) {
        return QHttpServerResponse(QJsonObject{{"error", "seconds must be positive"}},
                                   QHttpServerResponder::StatusCode::BadRequest);
    }

    if(enabled == false) {
        return QHttpServerResponse(QJsonObject{{"error", QString("zone %1 is disabled").arg(zoneNumber)},
                                               {"reason", RunRequest::refusalToString(RunRequest::Refusal::ZoneDisabled)}},
                                   QHttpServerResponder::StatusCode::Conflict);
    }

    const RunRequestPtr decision(new RunRequest);
    emit manualZoneRunRequested(zoneNumber, seconds, decision);
    return decisionResponse(decision);
}

QHttpServerResponse IrrigationControlServer::handleZoneStop(int zoneNumber, const QHttpServerRequest& request)
{
    Q_UNUSED(request)
    const ZoneList zones = _source->allZones();
    bool known = false;
    for(const Zone& zone : zones) {
        if(zone.number == zoneNumber) {
            known = true;
            break;
        }
    }

    if(known == false) {
        return QHttpServerResponse(QJsonObject{{"error", "unknown zone"}},
                                   QHttpServerResponder::StatusCode::NotFound);
    }

    emit zoneStopRequested(zoneNumber);
    return QHttpServerResponse(QJsonObject{{"accepted", true}},
                               QHttpServerResponder::StatusCode::Accepted);
}

QHttpServerResponse IrrigationControlServer::handleProgramsGet(const QHttpServerRequest& request)
{
    Q_UNUSED(request)
    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();

    QJsonArray array;
    const ProgramList programs = _source->allPrograms();
    for(const Program& program : programs) {
        bool ok = true;
        const ProgramStartTimeList startTimes = _source->startTimesFor(program.id, &ok);
        ProgramStepList steps;
        if(ok) {
            steps = _source->stepsFor(program.id, &ok);
        }
        if(ok == false) {
            return QHttpServerResponse(QJsonObject{{"error", "failed to read programs"}},
                                       QHttpServerResponder::StatusCode::InternalServerError);
        }
        const QDateTime nextRunUtc = Scheduler::nextRunUtc(program, startTimes, nowUtc);
        array.append(ProgramJson::toJson(program, startTimes, steps, nextRunUtc));
    }
    return QHttpServerResponse(array, QHttpServerResponder::StatusCode::Ok);
}

bool IrrigationControlServer::zoneIdsAreKnown(const ProgramStepList& steps, const ZoneList& knownZones)
{
    for(const ProgramStep& step : steps) {
        for(int zoneId : step.zoneIds) {
            bool found = false;
            for(const Zone& candidate : knownZones) {
                if(candidate.id == zoneId) {
                    found = true;
                    break;
                }
            }
            if(found == false) {
                return false;
            }
        }
    }
    return true;
}

bool IrrigationControlServer::beginTransaction()
{
    bool ok = false;
    _source->rawQuery("BEGIN", &ok);
    if(ok == false) {
        logText(LVL_ERROR, "Failed to begin a database transaction");
    }
    return ok;
}

bool IrrigationControlServer::commitTransaction()
{
    bool ok = false;
    _source->rawQuery("COMMIT", &ok);
    if(ok == false) {
        logText(LVL_ERROR, "Failed to commit a database transaction");
    }
    return ok;
}

void IrrigationControlServer::rollbackTransaction()
{
    bool ok = false;
    _source->rawQuery("ROLLBACK", &ok);
    if(ok == false) {
        logText(LVL_ERROR, "Failed to roll back a database transaction");
    }
}

bool IrrigationControlServer::reconcileStartTimes(int programId, ProgramStartTimeList& startTimes)
{
    bool ok = false;
    const ProgramStartTimeList stored = _source->startTimesFor(programId, &ok);

    QList<bool> matched(stored.count(), false);
    for(ProgramStartTime& startTime : startTimes) {
        startTime.programId = programId;
        startTime.id = 0;
        for(int i = 0; i < stored.count(); i++) {
            const ProgramStartTime& candidate = stored.at(i);
            if(matched.at(i) == false
               && candidate.minutesAfterMidnight == startTime.minutesAfterMidnight
               && candidate.timezone == startTime.timezone) {
                matched[i] = true;
                startTime.id = candidate.id;
                break;
            }
        }
    }

    for(int i = 0; ok == true && i < stored.count(); i++) {
        if(matched.at(i) == false) {
            ok = _source->deleteStartTime(stored.at(i).id);
        }
    }

    for(ProgramStartTime& startTime : startTimes) {
        if(ok == false) {
            break;
        }
        if(startTime.id == 0) {
            ok = _source->insertStartTime(startTime);
        }
    }

    return ok;
}

QHttpServerResponse IrrigationControlServer::handleProgramPost(const QHttpServerRequest& request)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(request.body(), &error);
    if(error.error != QJsonParseError::NoError || document.isObject() == false) {
        return QHttpServerResponse(QJsonObject{{"error", "malformed JSON"}},
                                   QHttpServerResponder::StatusCode::BadRequest);
    }

    Program program;
    ProgramStartTimeList startTimes;
    ProgramStepList steps;
    QString errorMessage;
    if(ProgramJson::fromJson(document.object(), program, startTimes, steps, errorMessage) == false) {
        return QHttpServerResponse(QJsonObject{{"error", errorMessage}},
                                   QHttpServerResponder::StatusCode::BadRequest);
    }

    if(zoneIdsAreKnown(steps, _source->allZones()) == false) {
        return QHttpServerResponse(QJsonObject{{"error", "unknown zoneId in steps"}},
                                   QHttpServerResponder::StatusCode::BadRequest);
    }

    if(beginTransaction() == false) {
        return QHttpServerResponse(QJsonObject{{"error", "failed to create program"}},
                                   QHttpServerResponder::StatusCode::InternalServerError);
    }

    bool ok = _source->insertProgram(program);

    for(ProgramStartTime& startTime : startTimes) {
        if(ok == false) {
            break;
        }
        startTime.programId = program.id;
        ok = _source->insertStartTime(startTime);
    }

    for(ProgramStep& step : steps) {
        if(ok == false) {
            break;
        }
        step.programId = program.id;
        ok = _source->insertProgramStep(step);
    }

    if(ok == false || commitTransaction() == false) {
        rollbackTransaction();
        return QHttpServerResponse(QJsonObject{{"error", "failed to create program"}},
                                   QHttpServerResponder::StatusCode::InternalServerError);
    }

    const QDateTime nextRunUtc = Scheduler::nextRunUtc(program, startTimes, QDateTime::currentDateTimeUtc());
    return QHttpServerResponse(ProgramJson::toJson(program, startTimes, steps, nextRunUtc),
                               QHttpServerResponder::StatusCode::Created);
}

QHttpServerResponse IrrigationControlServer::handleProgramPut(int programId, const QHttpServerRequest& request)
{
    const ProgramList programs = _source->allPrograms();
    bool known = false;
    for(const Program& candidate : programs) {
        if(candidate.id == programId) {
            known = true;
            break;
        }
    }

    if(known == false) {
        return QHttpServerResponse(QJsonObject{{"error", "unknown program"}},
                                   QHttpServerResponder::StatusCode::NotFound);
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(request.body(), &error);
    if(error.error != QJsonParseError::NoError || document.isObject() == false) {
        return QHttpServerResponse(QJsonObject{{"error", "malformed JSON"}},
                                   QHttpServerResponder::StatusCode::BadRequest);
    }

    Program program;
    ProgramStartTimeList startTimes;
    ProgramStepList steps;
    QString errorMessage;
    if(ProgramJson::fromJson(document.object(), program, startTimes, steps, errorMessage) == false) {
        return QHttpServerResponse(QJsonObject{{"error", errorMessage}},
                                   QHttpServerResponder::StatusCode::BadRequest);
    }

    if(zoneIdsAreKnown(steps, _source->allZones()) == false) {
        return QHttpServerResponse(QJsonObject{{"error", "unknown zoneId in steps"}},
                                   QHttpServerResponder::StatusCode::BadRequest);
    }

    program.id = programId;

    if(beginTransaction() == false) {
        return QHttpServerResponse(QJsonObject{{"error", "failed to update program"}},
                                   QHttpServerResponder::StatusCode::InternalServerError);
    }

    bool ok = _source->updateProgram(program);

    if(ok) {
        ok = reconcileStartTimes(programId, startTimes);
    }

    if(ok) {
        ok = _source->deleteProgramSteps(programId);
    }

    for(ProgramStep& step : steps) {
        if(ok == false) {
            break;
        }
        step.programId = programId;
        ok = _source->insertProgramStep(step);
    }

    if(ok == false || commitTransaction() == false) {
        rollbackTransaction();
        return QHttpServerResponse(QJsonObject{{"error", "failed to update program"}},
                                   QHttpServerResponder::StatusCode::InternalServerError);
    }

    std::sort(startTimes.begin(), startTimes.end(), [](const ProgramStartTime& a, const ProgramStartTime& b)
    {
        return a.id < b.id;
    });

    const QDateTime nextRunUtc = Scheduler::nextRunUtc(program, startTimes, QDateTime::currentDateTimeUtc());
    return QHttpServerResponse(ProgramJson::toJson(program, startTimes, steps, nextRunUtc),
                               QHttpServerResponder::StatusCode::Ok);
}

QHttpServerResponse IrrigationControlServer::handleProgramDelete(int programId, const QHttpServerRequest& request)
{
    Q_UNUSED(request)
    const ProgramList programs = _source->allPrograms();
    bool known = false;
    for(const Program& candidate : programs) {
        if(candidate.id == programId) {
            known = true;
            break;
        }
    }

    if(known == false) {
        return QHttpServerResponse(QJsonObject{{"error", "unknown program"}},
                                   QHttpServerResponder::StatusCode::NotFound);
    }

    if(_source->deleteProgram(programId) == false) {
        return QHttpServerResponse(QJsonObject{{"error", "failed to delete program"}},
                                   QHttpServerResponder::StatusCode::InternalServerError);
    }

    return QHttpServerResponse(QHttpServerResponder::StatusCode::NoContent);
}

QHttpServerResponse IrrigationControlServer::handleProgramRun(int programId, const QHttpServerRequest& request)
{
    Q_UNUSED(request)
    const ProgramList programs = _source->allPrograms();
    bool known = false;
    for(const Program& candidate : programs) {
        if(candidate.id == programId) {
            known = true;
            break;
        }
    }

    if(known == false) {
        return QHttpServerResponse(QJsonObject{{"error", "unknown program"}},
                                   QHttpServerResponder::StatusCode::NotFound);
    }

    const RunRequestPtr decision(new RunRequest);
    emit programRunRequested(programId, decision);
    return decisionResponse(decision);
}

QHttpServerResponse IrrigationControlServer::handleStop(const QHttpServerRequest& request)
{
    Q_UNUSED(request)
    emit stopRequested();
    return QHttpServerResponse(QJsonObject{{"accepted", true}},
                               QHttpServerResponder::StatusCode::Accepted);
}

QHttpServerResponse IrrigationControlServer::handleSettingsGet(const QHttpServerRequest& request)
{
    Q_UNUSED(request)
    QJsonObject object;
    for(const QString& key : SettingsKeys) {
        object[key] = _source->settingValue(key);
    }
    return QHttpServerResponse(object, QHttpServerResponder::StatusCode::Ok);
}

bool IrrigationControlServer::isValidSettingValue(const QString& key, const QString& value)
{
    if(key == "master_enabled") {
        return value == "0" || value == "1";
    }

    if(key == "rain_delay_until") {
        return value.isEmpty() || QDateTime::fromString(value, Qt::ISODate).isValid();
    }

    if(key == "max_zone_seconds") {
        bool ok = false;
        const int seconds = value.toInt(&ok);
        return ok && seconds > 0;
    }

    if(key == "log_level") {
        for(Log::LogLevel level : Log::getLogLevels()) {
            if(QString::compare(Log::getLogLevelString(level), value, Qt::CaseInsensitive) == 0) {
                return true;
            }
        }
        return false;
    }

    if(key == "max_concurrent_zones") {
        bool ok = false;
        const int zones = value.toInt(&ok);
        return ok && zones >= 1 && zones <= ZoneController::MaxConcurrentZonesCeiling;
    }

    if(key == "panel_run_minutes") {
        bool ok = false;
        const int minutes = value.toInt(&ok);
        return ok && minutes >= PanelController::MinimumRunMinutes && minutes <= PanelController::MaximumRunMinutes;
    }

    if(key == "latitude" || key == "longitude") {
        if(value.isEmpty()) {
            return true;
        }
        static const QRegularExpression decimal("^-?[0-9]{1,3}(\\.[0-9]+)?$");
        if(decimal.match(value).hasMatch() == false) {
            return false;
        }
        const double limit = key == "latitude" ? 90 : 180;
        return qAbs(value.toDouble()) <= limit;
    }

    return false;
}

QHttpServerResponse IrrigationControlServer::handleSettingsPut(const QHttpServerRequest& request)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(request.body(), &error);
    if(error.error != QJsonParseError::NoError || document.isObject() == false) {
        return QHttpServerResponse(QJsonObject{{"error", "malformed JSON"}},
                                   QHttpServerResponder::StatusCode::BadRequest);
    }

    const QJsonObject body = document.object();
    for(auto it = body.constBegin(); it != body.constEnd(); ++it) {
        if(SettingsKeys.contains(it.key()) == false) {
            return QHttpServerResponse(QJsonObject{{"error", QString("unknown setting '%1'").arg(it.key())}},
                                       QHttpServerResponder::StatusCode::BadRequest);
        }
        if(it.value().isString() == false || isValidSettingValue(it.key(), it.value().toString()) == false) {
            return QHttpServerResponse(QJsonObject{{"error", QString("invalid value for '%1'").arg(it.key())}},
                                       QHttpServerResponder::StatusCode::BadRequest);
        }
    }

    for(auto it = body.constBegin(); it != body.constEnd(); ++it) {
        if(_source->setSettingValue(it.key(), it.value().toString()) == false) {
            return QHttpServerResponse(QJsonObject{{"error", "failed to write settings"}},
                                       QHttpServerResponder::StatusCode::InternalServerError);
        }
    }

    emit settingsChanged();

    QJsonObject responseObject;
    for(const QString& key : SettingsKeys) {
        responseObject[key] = _source->settingValue(key);
    }
    return QHttpServerResponse(responseObject, QHttpServerResponder::StatusCode::Ok);
}

QHttpServerResponse IrrigationControlServer::decisionResponse(const RunRequestPtr& decision)
{
    if(decision->wait(_decisionTimeout) == false) {
        logText(LVL_ERROR, "A run request went unanswered by the valve thread");
        return QHttpServerResponse(QJsonObject{{"error", "the controller did not answer"}, {"reason", "timeout"}},
                                   QHttpServerResponder::StatusCode::ServiceUnavailable);
    }

    const RunRequest::Refusal refusal = decision->refusal();
    if(refusal == RunRequest::Refusal::None) {
        return QHttpServerResponse(QJsonObject{{"accepted", true}},
                                   QHttpServerResponder::StatusCode::Accepted);
    }

    const QJsonObject body{{"error", decision->message()}, {"reason", RunRequest::refusalToString(refusal)}};
    if(refusal == RunRequest::Refusal::Failed) {
        return QHttpServerResponse(body, QHttpServerResponder::StatusCode::InternalServerError);
    }
    return QHttpServerResponse(body, QHttpServerResponder::StatusCode::Conflict);
}

#include "moc_irrigationcontrolserver.cpp"

QHttpServerResponse IrrigationControlServer::handleClimateGet(const QHttpServerRequest& request)
{
    if(_climateDatabasePath.isEmpty()) {
        return QHttpServerResponse(QJsonObject{{"error", "no climate sensor is configured"}},
                                   QHttpServerResponder::StatusCode::NotFound);
    }
    if(_climateSource == nullptr) {
        return QHttpServerResponse(QJsonObject{{"error", "the climate database is not open"}},
                                   QHttpServerResponder::StatusCode::ServiceUnavailable);
    }

    int hours = 24;
    const QUrlQuery query = request.query();
    if(query.hasQueryItem("hours")) {
        bool ok = false;
        hours = query.queryItemValue("hours").toInt(&ok);
        if(ok == false || hours < 1 || hours > MaximumClimateHours) {
            return QHttpServerResponse(QJsonObject{{"error", QString("hours must be a whole number from 1 to %1").arg(MaximumClimateHours)}},
                                       QHttpServerResponder::StatusCode::BadRequest);
        }
    }

    const qint64 spanSeconds = static_cast<qint64>(hours) * 3600;
    const QDateTime toUtc = QDateTime::currentDateTimeUtc().addSecs(1);
    const QDateTime fromUtc = toUtc.addSecs(-spanSeconds);
    const int bucketSeconds = ClimateDataSource::bucketSecondsFor(spanSeconds, MaximumClimateBuckets);
    const ClimateBucketList buckets = _climateSource->bucketsBetween(fromUtc, toUtc, bucketSeconds);
    const int weatherBucketSeconds = qMax(bucketSeconds, 3600);
    const WeatherBucketList weather = _climateSource->weatherBucketsBetween(fromUtc, toUtc, weatherBucketSeconds);
    return QHttpServerResponse(ClimateJson::toJson(buckets, bucketSeconds, weather, weatherBucketSeconds, fromUtc, toUtc),
                               QHttpServerResponder::StatusCode::Ok);
}
