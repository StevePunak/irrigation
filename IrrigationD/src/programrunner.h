#ifndef PROGRAMRUNNER_H
#define PROGRAMRUNNER_H

#include <QObject>

#include <Kanoop/utility/loggingbaseclass.h>

#include "model/programzone.h"

class IrrigationDataSource;
class ZoneController;

/**
 * @brief Walks one program's ordered zone list, one zone open at a time.
 *
 * Advances only when the driving ZoneController reports the open zone closed.
 * This class holds no timer of its own; ZoneController is the sole timing
 * authority. Wires its own connections to @p controller's zoneClosed and
 * watchdogTripped signals at construction.
 */
class ProgramRunner : public QObject,
                      public LoggingBaseClass
{
    Q_OBJECT
public:
    /** @brief Constructs a runner driving @p controller from zone lists read through @p source. */
    ProgramRunner(ZoneController* controller, IrrigationDataSource* source, QObject* parent = nullptr);

    /** @brief Starts @p programId at its first zone. @return False when a program is already running or its first zone failed to open. */
    bool startProgram(int programId);

    /** @brief Stops the running program without advancing to its next zone. */
    void abort();

    /** @brief Returns whether a program is currently running. */
    bool isRunning() const { return _running; }

    /** @brief Returns the id of the running program, or zero when none is running. */
    int runningProgramId() const { return _programId; }

public slots:
    /** @brief Advances to the next zone when @p zoneNumber is the one this runner opened. */
    void onZoneClosed(int zoneNumber);

    /** @brief Stops the running program when the controller's watchdog trips. */
    void onWatchdogTripped(int zoneNumber);

signals:
    /** @brief Emitted when @p programId starts. */
    void programStarted(int programId);

    /** @brief Emitted after @p programId's last zone has closed. */
    void programFinished(int programId);

    /** @brief Emitted when @p programId is stopped before finishing, by abort(), a failed zone open, or a watchdog trip. */
    void programAborted(int programId);

private:
    bool advance();
    void stopRunning();

    ZoneController* _controller = nullptr;
    IrrigationDataSource* _source = nullptr;
    ProgramZoneList _zones;
    int _programId = 0;
    int _index = -1;
    int _expectedZone = 0;
    bool _running = false;
};

#endif // PROGRAMRUNNER_H
