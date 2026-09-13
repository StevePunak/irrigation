#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <QDate>
#include <QDateTime>
#include <QObject>
#include <QTimer>

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
class Scheduler : public QObject
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
     * @param valid Set to false when the local time does not exist (spring-forward gap)
     *              or the zone id is unrecognised; the return value is invalid in that case.
     */
    static QDateTime resolveToUtc(const ProgramStartTime& startTime, const QDate& localDate, bool* valid);

signals:
    /** @brief Emitted once a scheduled start time has come due and been recorded. */
    void programDue(int programId, int startTimeId, const QDateTime& scheduledAtUtc);

private:
    void recordOnce(int programId, int startTimeId, const QDateTime& scheduledAtUtc, FiredInstant::Outcome outcome);

    static constexpr qint64 GraceWindowSeconds = 120;

    IrrigationDataSource* _source = nullptr;
    IClock* _clock = nullptr;
    QTimer _tickTimer;
};

#endif // SCHEDULER_H
