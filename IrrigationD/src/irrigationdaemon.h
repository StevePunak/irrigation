#ifndef IRRIGATIONDAEMON_H
#define IRRIGATIONDAEMON_H

#include "iclock.h"
#include "runrequest.h"

#include <Kanoop/timespan.h>
#include <Kanoop/utility/abstractthreadclass.h>

#include <QByteArray>
#include <QDateTime>
#include <QString>

class IGpioBackend;
class IrrigationControlServer;
class IrrigationDataSource;
class IrrigationSettings;
class PanelController;
class PanelHost;
class ProgramQueue;
class ProgramRunner;
class QTimer;
class RunButton;
class Scheduler;
class StopButton;
class Tm1637Display;
class ZoneController;

/**
 * @brief Owns the lifetime of every daemon component.
 *
 * @warning Construction order is a hardware contract. ZoneController is
 *          constructed and drives all eight lines de-energised before the
 *          scheduler or the HTTP server exist, so nothing can ask for water
 *          before the valves are known shut.
 *
 * @warning Every component is created in threadStarted() and so belongs to the
 *          worker thread. Other threads reach them by signal. Constructing one
 *          anywhere else gives it the caller's affinity and puts a second
 *          thread on the valve lines.
 */
class IrrigationDaemon : public AbstractThreadClass
{
    Q_OBJECT
public:
    /** @brief Constructs the daemon against the settings at @p settingsPath. */
    explicit IrrigationDaemon(const QString& settingsPath);

    /** @brief Destructor. Joins the worker thread. */
    virtual ~IrrigationDaemon();

    /**
     * @brief Shuts the daemon down cooperatively.
     *
     * @warning The inherited implementation terminates the worker thread
     *          wherever it happens to be, including mid-write to a valve line.
     */
    virtual void abort() override;

    /** @brief Sets whether --verbose was given. While true the database log_level is never applied. Call before start(). */
    void setVerboseLogging(bool value) { _verboseLogging = value; }

    /** @brief Returns whether every component came up. Readable once a blocking start() has returned. */
    bool isReady() const { return _errorText.isEmpty(); }

    /** @brief Returns the text of the most recent failure. */
    QString errorText() const { return _errorText; }

protected:
    virtual void threadStarted() override;
    virtual void threadAboutToFinish() override;

private slots:
    void onStopPressed();
    void onProgramDue(int programId, int startTimeId, const QDateTime& scheduledAtUtc);
    void onManualZoneRunRequested(int zoneNumber, int seconds, const RunRequestPtr& decision);
    void onProgramRunRequested(int programId, const RunRequestPtr& decision);
    void onZoneStopRequested(int zoneNumber);
    void onSettingsChanged();
    void publishStatus();
    void onPanelFrame(const QByteArray& segments);
    void onPanelRefresh();

private:
    void connectComponents();

    /**
     * @brief Requests the display and RUN lines that are configured and builds the panel.
     *
     * A missing key or a failed request is logged and leaves that part out; the daemon
     * runs on without it.
     */
    void setUpPanel();

    /**
     * @brief Returns whether threadAboutToFinish() has deleted the components.
     *
     * @warning A request the control server queued before it stopped can still be
     *          delivered after teardown. Every slot reached from another thread checks
     *          this first.
     */
    bool isTornDown() const { return _programQueue == nullptr; }

    /**
     * @brief Applies the database max_zone_seconds, max_concurrent_zones, panel_run_minutes and log_level.
     *
     * The zone ceiling becomes the database value bounded by the INI ceiling, or the INI
     * ceiling when the database value is absent or not a positive integer. The concurrent
     * zone cap becomes the database value, or ZoneController::DefaultMaxConcurrentZones
     * when the database value is absent or not an integer. The panel run time becomes the
     * database value, or PanelController::DefaultRunMinutes when the database value is
     * absent, unparsable, or outside PanelController::MinimumRunMinutes through
     * MaximumRunMinutes (1-60). log_level is skipped while --verbose was given.
     */
    void applyRuntimeSettings();

    /** @brief Returns whether @p zoneNumber has a zones row with enabled set. False when no row matches. */
    bool isZoneEnabled(int zoneNumber);

    /** @brief Returns the earliest UTC instant at or after @p fromUtc at which any enabled program is due. */
    QDateTime nextScheduledRunUtc(const QDateTime& fromUtc);

    static const TimeSpan StatusInterval;
    static constexpr int ControlServerReadySeconds = 10;
    static constexpr int FiredInstantRetentionDays = 90;
    static constexpr int PanelTickMilliseconds = 100;
    static constexpr int PanelRefreshMilliseconds = 1000;

    QString _settingsPath;
    QString _errorText;
    bool _verboseLogging = false;

    SystemClock _clock;
    IrrigationSettings* _settings = nullptr;
    IGpioBackend* _backend = nullptr;
    ZoneController* _zoneController = nullptr;
    StopButton* _stopButton = nullptr;
    IrrigationDataSource* _dataSource = nullptr;
    ProgramRunner* _programRunner = nullptr;
    ProgramQueue* _programQueue = nullptr;
    Scheduler* _scheduler = nullptr;
    IrrigationControlServer* _controlServer = nullptr;
    QTimer* _statusTimer = nullptr;
    Tm1637Display* _display = nullptr;
    RunButton* _runButton = nullptr;
    PanelHost* _panelHost = nullptr;
    PanelController* _panel = nullptr;
    QTimer* _panelTimer = nullptr;
    QTimer* _panelRefreshTimer = nullptr;
    int _panelRunMinutes = 0;
    bool _displayFailing = false;
};

#endif // IRRIGATIONDAEMON_H
