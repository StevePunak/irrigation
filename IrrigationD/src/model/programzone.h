#ifndef PROGRAMZONE_H
#define PROGRAMZONE_H

#include <QList>

/** @brief One zone's position and run duration within a program's watering sequence. */
class ProgramZone
{
public:
    /** @brief Primary key. Zero when not yet persisted. */
    int id = 0;

    /** @brief Owning program's id. */
    int programId = 0;

    /** @brief Zone to run. */
    int zoneId = 0;

    /** @brief Position in the program's run order, lowest first. */
    int sequence = 0;

    /** @brief How long to run this zone, in seconds. */
    int durationSeconds = 0;

    /** @brief Returns true when this program zone came from the database. */
    bool isValid() const { return id > 0; }
};

typedef QList<ProgramZone> ProgramZoneList;

#endif // PROGRAMZONE_H
