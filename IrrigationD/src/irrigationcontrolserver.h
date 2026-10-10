#ifndef IRRIGATIONCONTROLSERVER_H
#define IRRIGATIONCONTROLSERVER_H

#include "model/program.h"
#include "model/programstarttime.h"
#include "model/programstep.h"
#include "model/zone.h"
#include "runrequest.h"

#include <Kanoop/mutexevent.h>
#include <Kanoop/timespan.h>
#include <Kanoop/utility/abstractthreadclass.h>

#include <QAtomicInt>
#include <QDateTime>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

class ClimateDataSource;
class IrrigationDataSource;
class QHttpServerResponse;
class QTcpServer;

/** @brief One open zone in a status snapshot. zone is the zone number. */
struct RunningZoneStatus
{
    /** @brief What opened the zone. */
    enum class Source
    {
        Manual,     ///< A manual run from the app or the API.
        Program,    ///< The running program owns the zone.
        Panel       ///< The gardener panel's current panel run.
    };

    int zone = 0;
    int secondsRemaining = 0;
    Source source = Source::Manual; ///< What opened the zone.
};

/** @brief One waiting program in a status snapshot. */
struct QueuedProgramStatus
{
    int programId = 0;
    QString name;
    QDateTime queuedAtUtc;
};

/**
 * @brief Snapshot of the daemon's runtime state, published through updateStatus() and served by GET /admin/status.
 *
 * programId is zero when no program runs. running is ordered by zone number. waitingZones
 * holds zone numbers. The temperature and humidity are meaningful only while climateFresh.
 */
struct ServerStatus
{
    QList<RunningZoneStatus> running;
    int programId = 0;
    QString programName;
    int programStep = 0;
    int programStepCount = 0;
    QList<int> waitingZones;
    QList<QueuedProgramStatus> queue;
    int maxConcurrentZones = 0;
    QDateTime nextRunUtc;
    QString timezone;
    bool masterEnabled = false;
    bool stopHeld = false;
    QDateTime rainDelayUntilUtc;
    bool climateConfigured = false;
    bool climateFresh = false;
    double temperatureCelsius = 0;
    double humidityPercent = 0;
};

Q_DECLARE_METATYPE(ServerStatus)

/**
 * @brief REST control surface for the irrigation daemon, served by QHttpServer on its own thread.
 *
 * Every route lives under the admin prefix; nginx proxies the public API prefix to
 * it. No route touches GPIO directly -- state-changing routes emit a request signal
 * and let the daemon's owner of ZoneController and ProgramRunner act on it. Run
 * routes wait for that owner's decision.
 *
 * @warning Owns its own IrrigationDataSource, opened in threadStarted() on the worker
 *          thread. A QSqlDatabase connection cannot be shared across threads, so this
 *          object never accepts one from elsewhere.
 */
class IrrigationControlServer : public AbstractThreadClass
{
    Q_OBJECT
public:
    /** @brief Constructs a control server backed by the database at @p databasePath. */
    explicit IrrigationControlServer(const QString& databasePath);

    /** @brief Destructor. Joins the worker thread. */
    virtual ~IrrigationControlServer() override;

    /**
     * @brief Sets the climate database GET /admin/climate reads. Call before start().
     *
     * Left empty, the route answers 404: no sensor is configured.
     */
    void setClimateDatabasePath(const QString& value) { _climateDatabasePath = value; }

    /** @brief Sets the address the listener binds to. Call before start(). */
    void setBindAddress(const QString& value) { _bindAddress = value; }

    /** @brief Sets the port the listener binds to. Zero picks an ephemeral port. Call before start(). */
    void setListenPort(int value) { _listenPort = value; }

    /** @brief Sets how long a run route waits for the valve thread's decision before answering 503. Call before start(). */
    void setDecisionTimeout(const TimeSpan& value) { _decisionTimeout = value; }

    /** @brief Publishes a new status snapshot for GET /admin/status to serve. Safe to call from any thread. */
    void updateStatus(const ServerStatus& status) { emit statusUpdateRequested(status); }

    /** @brief Blocks until the listener has bound or @p timeout elapses. @return True once the listener is bound. */
    bool waitUntilReady(const TimeSpan& timeout);

    /** @brief Returns the port the listener bound to. Valid only after waitUntilReady() returns true. */
    int boundPort() const { return _boundPort; }

    /** @brief Requests cooperative shutdown. Equivalent to stop(). */
    virtual void abort() override;

signals:
    /**
     * @brief Emitted when POST /admin/zones/{number}/run passes validation.
     *
     * The route blocks until a slot calls @p decision's complete(), or until the decision timeout.
     */
    void manualZoneRunRequested(int zoneNumber, int seconds, const RunRequestPtr& decision);

    /**
     * @brief Emitted when POST /admin/programs/{id}/run names a known program.
     *
     * The route blocks until a slot calls @p decision's complete(), or until the decision timeout.
     */
    void programRunRequested(int programId, const RunRequestPtr& decision);

    /** @brief Emitted when POST /admin/zones/{number}/stop names a known zone. */
    void zoneStopRequested(int zoneNumber);

    /** @brief Emitted when POST /admin/stop is accepted. */
    void stopRequested();

    /** @brief Emitted after PUT /admin/settings has stored every value in its body. */
    void settingsChanged();

protected:
    virtual void threadStarted() override;
    virtual void threadAboutToFinish() override;

private:
    // A direct _status = status write here would race a route handler's read of it.
    Q_SIGNAL void statusUpdateRequested(const ServerStatus& status);
    Q_SLOT void onStatusUpdateRequested(const ServerStatus& status);

    QHttpServerResponse handleHealth(const QHttpServerRequest& request);
    QHttpServerResponse handleVersion(const QHttpServerRequest& request);
    QHttpServerResponse handleStatus(const QHttpServerRequest& request);
    QHttpServerResponse handleZonesGet(const QHttpServerRequest& request);
    QHttpServerResponse handleZonePut(int zoneNumber, const QHttpServerRequest& request);
    QHttpServerResponse handleZoneRun(int zoneNumber, const QHttpServerRequest& request);
    QHttpServerResponse handleZoneStop(int zoneNumber, const QHttpServerRequest& request);
    QHttpServerResponse handleProgramsGet(const QHttpServerRequest& request);
    QHttpServerResponse handleProgramPost(const QHttpServerRequest& request);
    QHttpServerResponse handleProgramPut(int programId, const QHttpServerRequest& request);
    QHttpServerResponse handleProgramDelete(int programId, const QHttpServerRequest& request);
    QHttpServerResponse handleProgramRun(int programId, const QHttpServerRequest& request);
    QHttpServerResponse handleStop(const QHttpServerRequest& request);
    QHttpServerResponse handleSettingsGet(const QHttpServerRequest& request);
    QHttpServerResponse handleSettingsPut(const QHttpServerRequest& request);
    QHttpServerResponse handleClimateGet(const QHttpServerRequest& request);
    QHttpServerResponse decisionResponse(const RunRequestPtr& decision);

    static QJsonObject zoneToJson(const Zone& zone);

    /** @brief Returns whether every zone id in every step of @p steps names a row present in @p knownZones. */
    static bool zoneIdsAreKnown(const ProgramStepList& steps, const ZoneList& knownZones);

    /** @brief Returns whether @p value is a legal value for the settings key @p key. */
    static bool isValidSettingValue(const QString& key, const QString& value);

    bool beginTransaction();
    bool commitTransaction();
    void rollbackTransaction();

    /**
     * @brief Replaces the start times stored for @p programId with @p startTimes, reusing stored rows by value.
     *
     * Each incoming entry takes the id of one not-yet-matched stored row with the same
     * minutesAfterMidnight and timezone. Unmatched stored rows are deleted and unmatched
     * entries inserted; every id is written back into @p startTimes. Call inside a transaction.
     *
     * @warning fired_instants is keyed on start_time_id. A start time deleted and
     *          re-inserted gets a new id and fires again inside the grace window.
     * @return False when a read or write failed.
     */
    bool reconcileStartTimes(int programId, ProgramStartTimeList& startTimes);

    static const QStringList SettingsKeys;
    static const TimeSpan DefaultDecisionTimeout;

    /** @brief The most hours GET /admin/climate accepts, one leap year. */
    static constexpr int MaximumClimateHours = 8784;

    /** @brief The most buckets one GET /admin/climate response carries. */
    static constexpr int MaximumClimateBuckets = 400;

    QString _databasePath;
    QString _bindAddress;
    int _listenPort;
    TimeSpan _decisionTimeout;
    QString _climateDatabasePath;
    IrrigationDataSource* _source = nullptr;
    ClimateDataSource* _climateSource = nullptr;
    QHttpServer* _httpServer = nullptr;
    QTcpServer* _tcpServer = nullptr;
    int _boundPort = 0;
    QAtomicInt _ready;
    MutexEvent _readyEvent;
    ServerStatus _status;
};

#endif // IRRIGATIONCONTROLSERVER_H
