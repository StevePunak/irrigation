#include "model/firedinstant.h"

const FiredInstant::OutcomeToStringMap FiredInstant::_OutcomeToStringMap;

FiredInstant::Outcome FiredInstant::outcomeFromString(const QString& value)
{
    return _OutcomeToStringMap.getType(value, Outcome::Missed);
}

QString FiredInstant::outcomeToString(Outcome value)
{
    return _OutcomeToStringMap.getString(value, "missed");
}
