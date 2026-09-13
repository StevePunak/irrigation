#ifndef PROGRAMSTARTTIME_H
#define PROGRAMSTARTTIME_H

#include <QList>
#include <QString>

/** @brief One time-of-day trigger for a program, in the program's own local zone. */
class ProgramStartTime
{
public:
    /** @brief Primary key. Zero when not yet persisted. */
    int id = 0;

    /** @brief Owning program's id. */
    int programId = 0;

    /** @brief Local wall-clock start time, expressed as minutes after midnight. */
    int minutesAfterMidnight = 0;

    /** @brief IANA zone id the minutesAfterMidnight value is local to. */
    QString timezone;

    /** @brief Returns true when this start time came from the database. */
    bool isValid() const { return id > 0; }
};

typedef QList<ProgramStartTime> ProgramStartTimeList;

#endif // PROGRAMSTARTTIME_H
