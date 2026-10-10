#ifndef PANELHOST_H
#define PANELHOST_H

#include <QList>
#include <QString>

#include <Kanoop/utility/loggingbaseclass.h>

#include "ipanelhost.h"

class IrrigationDataSource;
class ProgramQueue;
class ProgramRunner;
class StopButton;
class ZoneController;

/**
 * @brief The daemon's side of the gardener panel: reads the controller, and opens, swaps, closes and clears zones for it.
 *
 * A panel run is refused for the same reasons as a manual run from the app: the fault
 * latch, STOP held, master enable off, zone disabled, cap reached.
 *
 * @warning Every method must be called on the thread that owns the zone controller.
 */
class PanelHost : public IPanelHost,
                  public LoggingBaseClass
{
public:
    /** @brief Constructs a host over the daemon's components. None is owned. */
    PanelHost(ZoneController* controller,
              ProgramRunner* runner,
              ProgramQueue* queue,
              IrrigationDataSource* source,
              StopButton* stopButton);

    /** @brief Sets the panel run time in minutes. */
    void setRunMinutes(int value);

    /** @brief Returns the panel run time in minutes. */
    int runMinutes() const { return _runMinutes; }

    /** @brief Returns the seconds a panel run opens for: the run time clamped by the zone controller's ceiling. */
    int runSeconds() const;

    /**
     * @brief Clears the controller as STOP does: the queue empties recording dropped_stop, the program aborts, every zone closes.
     * @param errorText When given and the close fails, set to the failure text.
     * @return True on success.
     */
    bool clearController(QString* errorText = nullptr);

    /**
     * @brief Closes @p zoneNumber as an app Stop does: the queue empties recording dropped_stop, the program aborts, and the zone closes. Other manual and panel zones stay open.
     * @param errorText When given and the close fails, set to the failure text.
     * @return True on success.
     */
    bool stopZone(int zoneNumber, QString* errorText = nullptr);

    /** @brief Reads the open zones, the enabled zones, the run time, STOP, the fault latch and the master enable. */
    virtual PanelSnapshot panelSnapshot() override;

    /** @brief Decides the refusals, then opens @p zoneNumber for runSeconds(), swapping out @p replacingZone when it is open. */
    virtual RunRequest::Refusal openPanelZone(int zoneNumber, int replacingZone) override;

    /** @brief Closes @p zoneNumber. */
    virtual void closePanelZone(int zoneNumber) override;

    /** @brief Logs the takeover and calls clearController(). */
    virtual void takeOverForPanel() override;

private:
    void cancelPrograms();
    QList<int> enabledZoneNumbers();

    ZoneController* _controller = nullptr;
    ProgramRunner* _runner = nullptr;
    ProgramQueue* _queue = nullptr;
    IrrigationDataSource* _source = nullptr;
    StopButton* _stopButton = nullptr;
    int _runMinutes = 0;
};

#endif // PANELHOST_H
