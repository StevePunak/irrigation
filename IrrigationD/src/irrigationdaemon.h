#ifndef IRRIGATIONDAEMON_H
#define IRRIGATIONDAEMON_H

#include "iclock.h"

#include <Kanoop/timespan.h>
#include <Kanoop/utility/abstractthreadclass.h>

#include <QDateTime>
#include <QString>

class IGpioBackend;
class IrrigationControlServer;
class IrrigationDataSource;
class IrrigationSettings;
class ProgramQueue;
class ProgramRunner;
class QTimer;
class Scheduler;
class StopButton;
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
    void onManualZoneRunRequested(int zoneNumber, int seconds);
    void onProgramRunRequested(int programId);
    void onSettingsChanged();
    void publishStatus();

private:
    void connectComponents();

    /**
     * @brief Applies the database max_zone_seconds and log_level.
     *
     * The zone ceiling becomes the database value bounded by the INI ceiling, or the INI
     * ceiling when the database value is absent or not a positive integer. log_level is
     * skipped while --verbose was given.
     */
    void applyRuntimeSettings();

    /** @brief Returns whether @p zoneNumber has a zones row with enabled set. False when no row matches. */
    bool isZoneEnabled(int zoneNumber);

    /** @brief Returns the earliest UTC instant at or after @p fromUtc at which any enabled program is due. */
    QDateTime nextScheduledRunUtc(const QDateTime& fromUtc);

    static const TimeSpan StatusInterval;
    static constexpr int ControlServerReadySeconds = 10;
    static constexpr int FiredInstantRetentionDays = 90;

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
};

#endif // IRRIGATIONDAEMON_H
