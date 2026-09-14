#include "irrigationcontrolserver.h"

#include "database/irrigationdatasource.h"
#include "json/programjson.h"
#include "json/statusjson.h"
#include "scheduler.h"

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
#include <QTcpServer>
#include <QTimeZone>
#include <QUuid>

const QStringList IrrigationControlServer::SettingsKeys = {
    "rain_delay_until", "master_enabled", "max_zone_seconds", "log_level"
};

IrrigationControlServer::IrrigationControlServer(const QString& databasePath) :
    AbstractThreadClass("control-server"),
    _databasePath(databasePath),
    _bindAddress("127.0.0.1"),
    _listenPort(8080)
{
    IrrigationControlServer::setObjectName(IrrigationControlServer::metaObject()->className());
    qRegisterMetaType<ServerStatus>();
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
        return QHttpServerResponse(QJsonObject{{"error", "zone is disabled"}},
                                   QHttpServerResponder::StatusCode::Conflict);
    }

    emit manualZoneRunRequested(zoneNumber, seconds);

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
        const ProgramStartTimeList startTimes = _source->startTimesFor(program.id);
        const ProgramZoneList zones = _source->zonesFor(program.id);
        const QDateTime nextRunUtc = Scheduler::nextRunUtc(program, startTimes, nowUtc);
        array.append(ProgramJson::toJson(program, startTimes, zones, nextRunUtc));
    }
    return QHttpServerResponse(array, QHttpServerResponder::StatusCode::Ok);
}

bool IrrigationControlServer::zoneIdsAreKnown(const ProgramZoneList& zones, const ZoneList& knownZones)
{
    for(const ProgramZone& zone : zones) {
        bool found = false;
        for(const Zone& candidate : knownZones) {
            if(candidate.id == zone.zoneId) {
                found = true;
                break;
            }
        }
        if(found == false) {
            return false;
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
    ProgramZoneList zones;
    QString errorMessage;
    if(ProgramJson::fromJson(document.object(), program, startTimes, zones, errorMessage) == false) {
        return QHttpServerResponse(QJsonObject{{"error", errorMessage}},
                                   QHttpServerResponder::StatusCode::BadRequest);
    }

    if(zoneIdsAreKnown(zones, _source->allZones()) == false) {
        return QHttpServerResponse(QJsonObject{{"error", "unknown zoneId in zones"}},
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

    for(ProgramZone& zone : zones) {
        if(ok == false) {
            break;
        }
        zone.programId = program.id;
        ok = _source->insertProgramZone(zone);
    }

    if(ok == false || commitTransaction() == false) {
        rollbackTransaction();
        return QHttpServerResponse(QJsonObject{{"error", "failed to create program"}},
                                   QHttpServerResponder::StatusCode::InternalServerError);
    }

    const QDateTime nextRunUtc = Scheduler::nextRunUtc(program, startTimes, QDateTime::currentDateTimeUtc());
    return QHttpServerResponse(ProgramJson::toJson(program, startTimes, zones, nextRunUtc),
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
    ProgramZoneList zones;
    QString errorMessage;
    if(ProgramJson::fromJson(document.object(), program, startTimes, zones, errorMessage) == false) {
        return QHttpServerResponse(QJsonObject{{"error", errorMessage}},
                                   QHttpServerResponder::StatusCode::BadRequest);
    }

    if(zoneIdsAreKnown(zones, _source->allZones()) == false) {
        return QHttpServerResponse(QJsonObject{{"error", "unknown zoneId in zones"}},
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
        ok = _source->deleteProgramZones(programId);
    }

    for(ProgramZone& zone : zones) {
        if(ok == false) {
            break;
        }
        zone.programId = programId;
        ok = _source->insertProgramZone(zone);
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
    return QHttpServerResponse(ProgramJson::toJson(program, startTimes, zones, nextRunUtc),
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

    emit programRunRequested(programId);

    return QHttpServerResponse(QJsonObject{{"accepted", true}},
                               QHttpServerResponder::StatusCode::Accepted);
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

#include "moc_irrigationcontrolserver.cpp"
