#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <QDate>
#include <QDateTime>
#include <QObject>
#include <QTimer>

#include <Kanoop/utility/loggingbaseclass.h>

#include "iclock.h"
#include "model/firedinstant.h"
#include "model/program.h"
#include "model/programstarttime.h"

class IrrigationDataSource;

/**
 * @brief Resolves enabled programs into due UTC instants and signals when one is due.
 *
 * Schedule rules are stored as local wall-clock time plus an IANA zone id.
 * Each tick resolves the rule to UTC against the current date, so the fire
 * instant shifts across DST transitions.
 */
class Scheduler : public QObject,
                  public LoggingBaseClass
{
    Q_OBJECT
public:
    /** @brief Constructs a scheduler reading programs from @p source and time from @p clock. */
    Scheduler(IrrigationDataSource* source, IClock* clock, QObject* parent = nullptr);

    /** @brief Starts the one-second tick timer. */
    void start();

    /** @brief Stops the tick timer. */
    void stop();

    /** @brief Evaluates every enabled program's start times against the current instant. */
    void tick();

    /** @brief Returns whether @p localDate is a watering day for @p program's day rule. */
    static bool isWateringDay(const Program& program, const QDate& localDate);

    /**
     * @brief Resolves @p startTime on @p localDate to a UTC instant.
     * @param valid Set to false when the local time does not exist (spring-forward gap),
     *              the zone id is unrecognised, or minutesAfterMidnight is outside 0..1439;
     *              the return value is invalid in that case.
     */
    static QDateTime resolveToUtc(const ProgramStartTime& startTime, const QDate& localDate, bool* valid);

signals:
    /** @brief Emitted once a scheduled start time has come due and been recorded. */
    void programDue(int programId, int startTimeId, const QDateTime& scheduledAtUtc);

private:
    /** @brief Records a firing. @return Whatever IrrigationDataSource::recordFiring() returned. */
    bool recordOnce(int programId, int startTimeId, const QDateTime& scheduledAtUtc, FiredInstant::Outcome outcome);

    /** @brief Returns a best-effort UTC instant for a start time that failed to resolve, for the Missed audit row. */
    static QDateTime missedInstantFor(const ProgramStartTime& startTime, const QDate& localDate);

    static constexpr qint64 GraceWindowSeconds = 120;

    IrrigationDataSource* _source = nullptr;
    IClock* _clock = nullptr;
    QTimer _tickTimer;
};

#endif // SCHEDULER_H
