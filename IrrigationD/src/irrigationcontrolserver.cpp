#include "irrigationcontrolserver.h"

#include "database/irrigationdatasource.h"
#include "json/programjson.h"
#include "json/statusjson.h"
#include "scheduler.h"

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
    _bindAddress("0.0.0.0"),
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
        return;
    }

    if(_httpServer->bind(_tcpServer) == false) {
        logText(LVL_ERROR, "Failed to bind the HTTP server to the listening socket");
        return;
    }

    _boundPort = _tcpServer->serverPort();
    _ready = true;
    _readyEvent.set();
}

void IrrigationControlServer::threadAboutToFinish()
{
    logText(LVL_INFO, "Control server thread finishing");

    delete _httpServer;
    _httpServer = nullptr;

    // _tcpServer is owned by _httpServer after bind() and was just destroyed with it.
    _tcpServer = nullptr;

    delete _source;
    _source = nullptr;

    _ready = false;
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

    const int seconds = document.object().value("seconds").toInt(0);
    if(seconds < 1) {
        return QHttpServerResponse(QJsonObject{{"error", "seconds must be positive"}},
                                   QHttpServerResponder::StatusCode::BadRequest);
    }

    emit manualZoneRunRequested(zoneNumber, seconds);

    return QHttpServerResponse(QJsonObject{{"accepted", true}},
                               QHttpServerResponder::StatusCode::Accepted);
}

QDateTime IrrigationControlServer::nextRunUtcFor(const Program& program,
                                                 const ProgramStartTimeList& startTimes,
                                                 const QDateTime& nowUtc)
{
    QDateTime earliest;
    if(program.enabled == false) {
        return earliest;
    }

    for(const ProgramStartTime& startTime : startTimes) {
        const QTimeZone zone(startTime.timezone.toUtf8());
        const QDate today = zone.isValid() ? nowUtc.toTimeZone(zone).date() : nowUtc.date();

        for(int offset = 0; offset <= HorizonDays; offset++) {
            const QDate candidate = today.addDays(offset);
            if(Scheduler::isWateringDay(program, candidate) == false) {
                continue;
            }

            bool valid = false;
            const QDateTime resolved = Scheduler::resolveToUtc(startTime, candidate, &valid);
            if(valid == false || resolved < nowUtc) {
                continue;
            }

            if(earliest.isValid() == false || resolved < earliest) {
                earliest = resolved;
            }
            break;
        }
    }

    return earliest;
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
        const QDateTime nextRunUtc = nextRunUtcFor(program, startTimes, nowUtc);
        array.append(ProgramJson::toJson(program, startTimes, zones, nextRunUtc));
    }
    return QHttpServerResponse(array, QHttpServerResponder::StatusCode::Ok);
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

    if(_source->insertProgram(program) == false) {
        return QHttpServerResponse(QJsonObject{{"error", "failed to create program"}},
                                   QHttpServerResponder::StatusCode::InternalServerError);
    }

    for(ProgramStartTime& startTime : startTimes) {
        startTime.programId = program.id;
        _source->insertStartTime(startTime);
    }

    for(ProgramZone& zone : zones) {
        zone.programId = program.id;
        _source->insertProgramZone(zone);
    }

    const QDateTime nextRunUtc = nextRunUtcFor(program, startTimes, QDateTime::currentDateTimeUtc());
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

    program.id = programId;
    if(_source->updateProgram(program) == false) {
        return QHttpServerResponse(QJsonObject{{"error", "failed to update program"}},
                                   QHttpServerResponder::StatusCode::InternalServerError);
    }

    bool ok = false;
    _source->rawQuery(QString("DELETE FROM program_start_times WHERE program_id = %1").arg(programId), &ok);
    if(ok == false) {
        logText(LVL_WARNING, QString("Failed to clear start times for program %1").arg(programId));
    }

    _source->rawQuery(QString("DELETE FROM program_zones WHERE program_id = %1").arg(programId), &ok);
    if(ok == false) {
        logText(LVL_WARNING, QString("Failed to clear zones for program %1").arg(programId));
    }

    for(ProgramStartTime& startTime : startTimes) {
        startTime.programId = programId;
        _source->insertStartTime(startTime);
    }

    for(ProgramZone& zone : zones) {
        zone.programId = programId;
        _source->insertProgramZone(zone);
    }

    const QDateTime nextRunUtc = nextRunUtcFor(program, startTimes, QDateTime::currentDateTimeUtc());
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
        if(it.value().isString() == false) {
            return QHttpServerResponse(QJsonObject{{"error", "setting values must be strings"}},
                                       QHttpServerResponder::StatusCode::BadRequest);
        }
        _source->setSettingValue(it.key(), it.value().toString());
    }

    QJsonObject responseObject;
    for(const QString& key : SettingsKeys) {
        responseObject[key] = _source->settingValue(key);
    }
    return QHttpServerResponse(responseObject, QHttpServerResponder::StatusCode::Ok);
}

#include "moc_irrigationcontrolserver.cpp"
