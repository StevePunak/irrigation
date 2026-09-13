#ifndef IRRIGATIONDAEMON_H
#define IRRIGATIONDAEMON_H

#include "iclock.h"

#include <Kanoop/utility/abstractthreadclass.h>

#include <QDateTime>
#include <QString>

class IGpioBackend;
class IrrigationControlServer;
class IrrigationDataSource;
class IrrigationSettings;
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
    void publishStatus();

private:
    void connectComponents();

    /** @brief Returns the earliest UTC instant at which any enabled program is next due. */
    QDateTime nextScheduledRunUtc(const QDateTime& nowUtc);

    static constexpr int StatusIntervalMilliseconds = 1000;
    static constexpr int ControlServerReadySeconds = 10;
    static constexpr int FiredInstantRetentionDays = 90;

    QString _settingsPath;
    QString _errorText;

    SystemClock _clock;
    IrrigationSettings* _settings = nullptr;
    IGpioBackend* _backend = nullptr;
    ZoneController* _zoneController = nullptr;
    StopButton* _stopButton = nullptr;
    IrrigationDataSource* _dataSource = nullptr;
    ProgramRunner* _programRunner = nullptr;
    Scheduler* _scheduler = nullptr;
    IrrigationControlServer* _controlServer = nullptr;
    QTimer* _statusTimer = nullptr;
};

#endif // IRRIGATIONDAEMON_H
