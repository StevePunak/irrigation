#ifndef PROGRAMSTARTTIME_H
#define PROGRAMSTARTTIME_H

#include <QList>
#include <QString>

/**
 * @brief One time-of-day trigger for a program, in the program's own local zone.
 *
 * timezone is an IANA zone id.
 */
class ProgramStartTime
{
public:
    int id = 0;
    int programId = 0;
    int minutesAfterMidnight = 0;
    QString timezone;

    /** @brief Returns true when this start time came from the database. */
    bool isValid() const { return id > 0; }
};

typedef QList<ProgramStartTime> ProgramStartTimeList;

#endif // PROGRAMSTARTTIME_H
