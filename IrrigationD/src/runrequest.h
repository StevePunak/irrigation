#ifndef RUNREQUEST_H
#define RUNREQUEST_H

#include <QMetaType>
#include <QMutex>
#include <QSharedPointer>
#include <QString>
#include <QWaitCondition>

#include <Kanoop/kanoopcommon.h>
#include <Kanoop/timespan.h>

/**
 * @brief A run request handed from the HTTP thread to the valve thread, and the decision the HTTP thread waits for.
 *
 * The valve thread calls complete() once. The HTTP thread blocks in wait() until then or
 * until its timeout. Shared through RunRequestPtr so either side may outlive the other.
 */
class RunRequest
{
public:
    /** @brief Why a run was refused, or None when it was accepted. */
    enum class Refusal
    {
        None,
        CapReached,
        StopHeld,
        MasterDisabled,
        ZoneDisabled,
        AlreadyQueued,
        Failed
    };

    /** @brief Records the decision and wakes the waiting thread. @p message is a sentence for a person. */
    void complete(Refusal refusal, const QString& message = QString());

    /** @brief Blocks until complete() has been called or @p timeout elapses. @return True when a decision arrived. */
    bool wait(const TimeSpan& timeout);

    /** @brief Returns the recorded refusal; None until complete() is called. */
    Refusal refusal() const;

    /** @brief Returns the recorded message. */
    QString message() const;

    /**
     * @brief Returns the wire name for @p value.
     *
     * The names are the 409 reason contract with the web client.
     */
    static QString refusalToString(Refusal value);

private:
    class RefusalToStringMap : public KANOOP::EnumToStringMap<Refusal>
    {
    public:
        RefusalToStringMap()
        {
            insert(Refusal::None,           "none");
            insert(Refusal::CapReached,     "cap_reached");
            insert(Refusal::StopHeld,       "stop_held");
            insert(Refusal::MasterDisabled, "master_disabled");
            insert(Refusal::ZoneDisabled,   "zone_disabled");
            insert(Refusal::AlreadyQueued,  "already_queued");
            insert(Refusal::Failed,         "failed");
        }
    };

    static const RefusalToStringMap _RefusalToStringMap;

    mutable QMutex _mutex;
    QWaitCondition _completed;
    bool _done = false;
    Refusal _refusal = Refusal::None;
    QString _message;
};

typedef QSharedPointer<RunRequest> RunRequestPtr;

Q_DECLARE_METATYPE(RunRequestPtr)

#endif // RUNREQUEST_H
