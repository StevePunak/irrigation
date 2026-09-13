#include "model/firedinstant.h"

FiredInstant::Outcome FiredInstant::outcomeFromString(const QString& value)
{
    if(value == "ran") {
        return Outcome::Ran;
    }
    else if(value == "skipped_busy") {
        return Outcome::SkippedBusy;
    }
    else if(value == "skipped_rain") {
        return Outcome::SkippedRain;
    }
    return Outcome::Missed;
}

QString FiredInstant::outcomeToString(Outcome value)
{
    switch(value)
    {
    case Outcome::Ran:          return "ran";
    case Outcome::SkippedBusy:  return "skipped_busy";
    case Outcome::SkippedRain:  return "skipped_rain";
    case Outcome::Missed:       break;
    }
    return "missed";
}
