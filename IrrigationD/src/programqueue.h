#ifndef PROGRAMQUEUE_H
#define PROGRAMQUEUE_H

#include <QDateTime>
#include <QList>
#include <QObject>

#include <Kanoop/utility/loggingbaseclass.h>

#include "iclock.h"
#include "model/firedinstant.h"
#include "runrequest.h"

class IrrigationDataSource;
class ProgramRunner;

/**
 * @brief First-in, first-out queue of programs waiting for the one runner.
 *
 * A scheduled entry's fired_instants row is inserted queued by the Scheduler and
 * given its final outcome here. A manual entry has no row. Wires its own connections
 * to @p runner's programFinished and programAborted at construction and starts the head
 * entry each time the runner falls idle.
 *
 * @warning An aborted program hands the runner to the next queued entry, which opens
 *          valves. Call dropAll() before aborting the runner on a stop, and delete this
 *          object before aborting the runner during teardown.
 */
class ProgramQueue : public QObject,
                     public LoggingBaseClass
{
    Q_OBJECT
public:
    /** @brief One program waiting to run. */
    class Entry
    {
    public:
        int programId = 0;
        int startTimeId = 0;
        QDateTime scheduledAtUtc;
        QDateTime queuedAtUtc;

        /** @brief Returns true for an entry a start time produced, false for a manual run. */
        bool isScheduled() const { return startTimeId > 0; }
    };

    /** @brief Constructs a queue feeding @p runner, recording outcomes through @p source and timing entries by @p clock. */
    ProgramQueue(ProgramRunner* runner, IrrigationDataSource* source, IClock* clock, QObject* parent = nullptr);

    /**
     * @brief Runs or queues the firing a start time produced.
     *
     * The firing's row must already exist with outcome queued. Starts it now when the
     * runner is idle and nothing waits, recording ran or failed. A program that already
     * holds a queue entry is recorded skipped_duplicate. A running program may hold one entry.
     */
    void enqueueScheduled(int programId, int startTimeId, const QDateTime& scheduledAtUtc);

    /**
     * @brief Runs or queues a manual run of @p programId.
     * @return AlreadyQueued when the program is running or queued, Failed when it could not
     *         start, otherwise None.
     */
    RunRequest::Refusal enqueueManual(int programId);

    /** @brief Empties the queue, recording each scheduled entry with @p outcome. */
    void dropAll(FiredInstant::Outcome outcome);

    /** @brief Returns the waiting entries, head first. */
    QList<Entry> entries() const { return _entries; }

    /**
     * @brief Records every firing still marked queued as dropped_restart and logs how many.
     *
     * @warning Call once at startup, before the Scheduler's first tick: that tick inserts
     *          queued rows this would sweep.
     * @return False when the update failed.
     */
    bool recordRestartDrops();

private slots:
    void onProgramEnded(int programId);

private:
    bool start(const Entry& entry);
    void startNext();
    bool contains(int programId) const;
    bool isRainDelayed();
    void setOutcome(const Entry& entry, FiredInstant::Outcome outcome);

    ProgramRunner* _runner = nullptr;
    IrrigationDataSource* _source = nullptr;
    IClock* _clock = nullptr;
    QList<Entry> _entries;
};

#endif // PROGRAMQUEUE_H
