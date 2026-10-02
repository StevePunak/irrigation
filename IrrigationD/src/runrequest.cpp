#include "runrequest.h"

#include <QDeadlineTimer>
#include <QMutexLocker>

const RunRequest::RefusalToStringMap RunRequest::_RefusalToStringMap;

void RunRequest::complete(Refusal refusal, const QString& message)
{
    QMutexLocker locker(&_mutex);
    _refusal = refusal;
    _message = message;
    _done = true;
    _completed.wakeAll();
}

bool RunRequest::wait(const TimeSpan& timeout)
{
    QMutexLocker locker(&_mutex);
    const QDeadlineTimer deadline(static_cast<qint64>(timeout.totalMilliseconds()));

    bool waiting = true;
    while(_done == false && waiting == true) {
        waiting = _completed.wait(&_mutex, deadline);
    }

    return _done;
}

RunRequest::Refusal RunRequest::refusal() const
{
    QMutexLocker locker(&_mutex);
    return _refusal;
}

QString RunRequest::message() const
{
    QMutexLocker locker(&_mutex);
    return _message;
}

QString RunRequest::refusalToString(Refusal value)
{
    return _RefusalToStringMap.getString(value, "failed");
}
