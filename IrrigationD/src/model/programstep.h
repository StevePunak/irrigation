#ifndef PROGRAMSTEP_H
#define PROGRAMSTEP_H

#include <QList>

/**
 * @brief One step of a program: a set of zones that water together for one duration.
 *
 * sequence orders steps lowest first. zoneIds holds zones.id values.
 */
class ProgramStep
{
public:
    int id = 0;
    int programId = 0;
    int sequence = 0;
    int durationSeconds = 0;
    QList<int> zoneIds;

    /** @brief Returns true when this step came from the database. */
    bool isValid() const { return id > 0; }
};

typedef QList<ProgramStep> ProgramStepList;

#endif // PROGRAMSTEP_H
