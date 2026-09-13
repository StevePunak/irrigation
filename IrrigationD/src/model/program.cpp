#include "model/program.h"

const Program::DayModeToStringMap Program::_DayModeToStringMap;

Program::DayMode Program::dayModeFromString(const QString& value)
{
    return _DayModeToStringMap.getType(value, DayMode::DaysOfWeek);
}

QString Program::dayModeToString(DayMode value)
{
    return _DayModeToStringMap.getString(value, "DaysOfWeek");
}
