#ifndef IPANELHOST_H
#define IPANELHOST_H

#include <QList>

#include "runrequest.h"

/** @brief The controller state the gardener panel reads before each decision and each frame. */
class PanelSnapshot
{
public:
    /** @brief One open zone. */
    class OpenZone
    {
    public:
        int zone = 0;               ///< The zone number.
        int secondsRemaining = 0;   ///< Seconds until the zone's deadline.
    };

    QList<OpenZone> openZones;      ///< Every open zone, ascending by zone number.
    QList<int> enabledZones;        ///< Every enabled zone number, ascending.
    int runMinutes = 0;             ///< The panel run time as it will open, in whole minutes rounded up.
    bool stopHeld = false;          ///< The STOP button is held.
    bool faulted = false;           ///< The zone controller's watchdog latch is set.
    bool masterEnabled = true;      ///< The master enable is on.

    /** @brief Returns the seconds remaining on @p zoneNumber, or -1 when it is not open. */
    int secondsRemaining(int zoneNumber) const
    {
        for(const OpenZone& open : openZones) {
            if(open.zone == zoneNumber) {
                return open.secondsRemaining;
            }
        }
        return -1;
    }
};

/**
 * @brief What the gardener panel asks of the daemon.
 *
 * Every call runs on the thread that owns the zone controller and returns once the
 * work is done.
 */
class IPanelHost
{
public:
    /** @brief Destroys the host. */
    virtual ~IPanelHost() {}

    /** @brief Returns the controller state now. */
    virtual PanelSnapshot panelSnapshot() = 0;

    /**
     * @brief Opens @p zoneNumber as a panel run, closing @p replacingZone when it is not zero.
     *
     * Every refusal is decided before anything closes, counting @p replacingZone's slot
     * as free, and a refused call leaves @p replacingZone open. The one exception is a
     * CapReached after the close: a waiting program zone took the freed slot.
     * @return None when @p zoneNumber opened, otherwise why it did not.
     */
    virtual RunRequest::Refusal openPanelZone(int zoneNumber, int replacingZone) = 0;

    /** @brief Closes @p zoneNumber. */
    virtual void closePanelZone(int zoneNumber) = 0;

    /** @brief Clears the controller as an API STOP does: the queue empties recording dropped_stop, the program aborts, every zone closes. */
    virtual void takeOverForPanel() = 0;
};

#endif // IPANELHOST_H
