#ifndef PROGRAM_H
#define PROGRAM_H

#include <Kanoop/kanoopcommon.h>

#include <QDate>
#include <QList>
#include <QString>

/**
 * @brief A watering program: a day rule, a set of start times and an ordered zone list.
 *
 * dowMask bit 0 is Monday, matching QDate::dayOfWeek() minus one; bit 6 is Sunday.
 */
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

    int id = 0;
    QString name;
    bool enabled = true;
    DayMode dayMode = DayMode::DaysOfWeek;
    int dowMask = 0;
    int intervalDays = 0;
    QDate anchorDate;

    /** @brief Returns true when this program came from the database. */
    bool isValid() const { return id > 0; }

private:
    class DayModeToStringMap : public KANOOP::EnumToStringMap<DayMode>
    {
    public:
        DayModeToStringMap()
        {
            insert(DayMode::DaysOfWeek, "DaysOfWeek");
            insert(DayMode::Odd,        "Odd");
            insert(DayMode::Even,       "Even");
            insert(DayMode::EveryNDays, "EveryNDays");
        }
    };

    static const DayModeToStringMap _DayModeToStringMap;
};

typedef QList<Program> ProgramList;

#endif // PROGRAM_H
