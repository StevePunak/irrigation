#ifndef PROGRAM_H
#define PROGRAM_H

#include <QDate>
#include <QList>
#include <QString>

/** @brief A watering program: a day rule, a set of start times and an ordered zone list. */
class Program
{
public:
    /** @brief How the program decides whether today is a watering day. */
    enum class DayMode
    {
        DaysOfWeek,
        Odd,
        Even,
        EveryNDays
    };

    /** @brief Parses @p value into a DayMode. Returns DaysOfWeek when unrecognised. */
    static DayMode dayModeFromString(const QString& value);

    /** @brief Returns the storage string for @p value. */
    static QString dayModeToString(DayMode value);

    /** @brief Primary key. Zero when not yet persisted. */
    int id = 0;

    /** @brief Display name. */
    QString name;

    /** @brief Whether the scheduler considers this program at all. */
    bool enabled = true;

    /** @brief Which rule decides watering days. */
    DayMode dayMode = DayMode::DaysOfWeek;

    /**
     * @brief Bitmask of watering days, used when dayMode is DaysOfWeek.
     *
     * Bit 0 is Monday, matching QDate::dayOfWeek() minus one. Bit 6 is Sunday.
     */
    int dowMask = 0;

    /** @brief Days between waterings, used when dayMode is EveryNDays. */
    int intervalDays = 0;

    /** @brief Reference date the EveryNDays interval counts from. */
    QDate anchorDate;

    /** @brief Returns true when this program came from the database. */
    bool isValid() const { return id > 0; }
};

typedef QList<Program> ProgramList;

#endif // PROGRAM_H
