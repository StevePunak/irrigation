#ifndef PROGRAMRUNNER_H
#define PROGRAMRUNNER_H

#include <QList>
#include <QObject>
#include <QString>

#include <Kanoop/utility/loggingbaseclass.h>

#include "model/programstep.h"
#include "zonecontroller.h"

class IrrigationDataSource;

/**
 * @brief Walks one program's ordered steps, opening each step's zones together under the zone cap.
 *
 * Advances only on ZoneController's zoneClosed and watchdogTripped signals; this class
 * holds no timer of its own. Wires its own connections to @p controller's zoneClosed
 * and watchdogTripped signals at construction.
 *
 * @warning programFinished and programAborted are emitted after every member is reset.
 *          A slot may call startProgram() from inside either signal.
 */
class ProgramRunner : public QObject,
                      public LoggingBaseClass
{
    Q_OBJECT
public:
    /** @brief Constructs a runner driving @p controller from steps read through @p source. */
    ProgramRunner(ZoneController* controller, IrrigationDataSource* source, QObject* parent = nullptr);

    /** @brief Starts @p programId at its first step. @return False when a program is already running or an open in the first step failed. */
    bool startProgram(int programId);

    /** @brief Stops the running program and closes the zones it opened. Zones opened by anything else stay open. */
    void abort();

    /** @brief Returns whether a program is currently running. */
    bool isRunning() const { return _running; }

    /** @brief Returns the id of the running program, or zero when none is running. */
    int runningProgramId() const { return _programId; }

    /** @brief Returns the name of the running program, or an empty string when none is running. */
    QString runningProgramName() const { return _programName; }

    /** @brief Returns the 1-based number of the current step, or zero when none is running. */
    int stepNumber() const { return _running == true ? _stepIndex + 1 : 0; }

    /** @brief Returns how many steps the running program has, or zero when none is running. */
    int stepCount() const { return _running == true ? static_cast<int>(_steps.count()) : 0; }

    /** @brief Returns the zone numbers of the current step still waiting for a slot or for a pending close to land, ascending. */
    QList<int> waitingZones() const { return _waiting; }

    /** @brief Returns whether @p zoneNumber is an open zone of the current step. */
    bool ownsZone(int zoneNumber) const { return _openZones.contains(zoneNumber); }

    /** @brief Opens every waiting zone of the current step that now fits under the cap. */
    void fillSlots();

public slots:
    /** @brief Fills freed slots after a deadline or a per-zone stop; aborts the program on an all-off or watchdog close of one of its open or waiting zones. */
    void onZoneClosed(int zoneNumber, ZoneController::CloseReason reason);

    /** @brief Stops the running program when the controller's watchdog trips. */
    void onWatchdogTripped();

signals:
    /** @brief Emitted when @p programId starts. */
    void programStarted(int programId);

    /** @brief Emitted after @p programId's last step has completed. */
    void programFinished(int programId);

    /** @brief Emitted when @p programId is stopped before finishing, by abort(), a failed zone open, an all-off, or a watchdog trip. */
    void programAborted(int programId);

private:
    bool advance();
    void loadStep();
    bool openWaiting();
    void finish();
    void stopRunning();

    ZoneController* _controller = nullptr;
    IrrigationDataSource* _source = nullptr;
    ProgramStepList _steps;
    QString _programName;
    int _programId = 0;
    int _stepIndex = -1;
    QList<int> _waiting;
    QList<int> _openZones;
    bool _running = false;
};

#endif // PROGRAMRUNNER_H
