#ifndef PROGRAMZONE_H
#define PROGRAMZONE_H

#include <QList>

/**
 * @brief One zone's position and run duration within a program's watering sequence.
 *
 * sequence orders zones lowest first.
 */
class ProgramZone
{
public:
    int id = 0;
    int programId = 0;
    int zoneId = 0;
    int sequence = 0;
    int durationSeconds = 0;

    /** @brief Returns true when this program zone came from the database. */
    bool isValid() const { return id > 0; }
};

typedef QList<ProgramZone> ProgramZoneList;

#endif // PROGRAMZONE_H
