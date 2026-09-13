#ifndef IRRIGATIONCONTROLSERVER_H
#define IRRIGATIONCONTROLSERVER_H

#include "model/program.h"
#include "model/programstarttime.h"
#include "model/programzone.h"
#include "model/zone.h"

#include <Kanoop/mutexevent.h>
#include <Kanoop/timespan.h>
#include <Kanoop/utility/abstractthreadclass.h>

#include <QAtomicInt>
#include <QDateTime>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QJsonObject>
#include <QString>
#include <QStringList>

class IrrigationDataSource;
class QHttpServerResponse;
class QTcpServer;

/** @brief Snapshot of the daemon's runtime state, published through updateStatus() and served by GET /admin/status. */
struct ServerStatus
{
    int runningZone = 0;
    int secondsRemaining = 0;
    QDateTime nextRunUtc;
    QString timezone;
    bool masterEnabled = false;
    QDateTime rainDelayUntilUtc;
};

Q_DECLARE_METATYPE(ServerStatus)

/**
 * @brief REST control surface for the irrigation daemon, served by QHttpServer on its own thread.
 *
 * Every route lives under the admin prefix; nginx proxies the public API prefix to
 * it. No route touches GPIO directly -- state-changing routes emit a request signal
 * and let the daemon's owner of ZoneController and ProgramRunner act on it.
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

    /** @brief Sets the address the listener binds to. Call before start(). */
    void setBindAddress(const QString& value) { _bindAddress = value; }

    /** @brief Sets the port the listener binds to. Zero picks an ephemeral port. Call before start(). */
    void setListenPort(int value) { _listenPort = value; }

    /**
     * @brief Publishes a new status snapshot for GET /admin/status to serve.
     *
     * Safe to call from any thread: delivery to the worker thread happens through a
     * queued signal, so the served snapshot is only ever written on the thread that
     * also serves GET /admin/status.
     */
    void updateStatus(const ServerStatus& status) { emit statusUpdateRequested(status); }

    /**
     * @brief Blocks until the listener has bound, or the timeout elapses.
     *
     * start() reports that the worker thread is running. The listener's bound port
     * is known only once threadStarted() has run on that thread; call this
     * afterward to learn when the listener itself is ready.
     */
    bool waitUntilReady(const TimeSpan& timeout);

    /** @brief Returns the port the listener bound to. Valid only after waitUntilReady() returns true. */
    int boundPort() const { return _boundPort; }

    /** @brief Requests cooperative shutdown. Equivalent to stop(). */
    virtual void abort() override;

signals:
    /** @brief Emitted when POST /admin/zones/{number}/run is accepted. */
    void manualZoneRunRequested(int zoneNumber, int seconds);

    /** @brief Emitted when POST /admin/programs/{id}/run is accepted. */
    void programRunRequested(int programId);

    /** @brief Emitted when POST /admin/stop is accepted. */
    void stopRequested();

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
    QHttpServerResponse handleProgramsGet(const QHttpServerRequest& request);
    QHttpServerResponse handleProgramPost(const QHttpServerRequest& request);
    QHttpServerResponse handleProgramPut(int programId, const QHttpServerRequest& request);
    QHttpServerResponse handleProgramDelete(int programId, const QHttpServerRequest& request);
    QHttpServerResponse handleProgramRun(int programId, const QHttpServerRequest& request);
    QHttpServerResponse handleStop(const QHttpServerRequest& request);
    QHttpServerResponse handleSettingsGet(const QHttpServerRequest& request);
    QHttpServerResponse handleSettingsPut(const QHttpServerRequest& request);

    static QJsonObject zoneToJson(const Zone& zone);

    /** @brief Returns whether every zone.zoneId in @p zones names a row present in @p knownZones. */
    static bool zoneIdsAreKnown(const ProgramZoneList& zones, const ZoneList& knownZones);

    /** @brief Returns whether @p value is a legal value for the settings key @p key. */
    static bool isValidSettingValue(const QString& key, const QString& value);

    bool beginTransaction();
    bool commitTransaction();
    void rollbackTransaction();

    static const QStringList SettingsKeys;

    QString _databasePath;
    QString _bindAddress;
    int _listenPort;
    IrrigationDataSource* _source = nullptr;
    QHttpServer* _httpServer = nullptr;
    QTcpServer* _tcpServer = nullptr;
    int _boundPort = 0;
    QAtomicInt _ready;
    MutexEvent _readyEvent;
    ServerStatus _status;
};

#endif // IRRIGATIONCONTROLSERVER_H
