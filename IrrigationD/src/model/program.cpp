#include "model/program.h"

Program::DayMode Program::dayModeFromString(const QString& value)
{
    if(value == "Odd") {
        return DayMode::Odd;
    }
    else if(value == "Even") {
        return DayMode::Even;
    }
    else if(value == "EveryNDays") {
        return DayMode::EveryNDays;
    }
    return DayMode::DaysOfWeek;
}

QString Program::dayModeToString(DayMode value)
{
    switch(value)
    {
    case DayMode::Odd:         return "Odd";
    case DayMode::Even:        return "Even";
    case DayMode::EveryNDays:  return "EveryNDays";
    case DayMode::DaysOfWeek:  break;
    }
    return "DaysOfWeek";
}
