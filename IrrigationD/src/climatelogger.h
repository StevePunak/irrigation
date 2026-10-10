#ifndef CLIMATELOGGER_H
#define CLIMATELOGGER_H

#include <QObject>
#include <QTimer>

#include <Kanoop/utility/loggingbaseclass.h>

#include "iclock.h"
#include "model/climatereading.h"

class ClimateDataSource;
class IClimateSensor;

/**
 * @brief Samples a climate sensor on a fixed interval and commits the readings in batches.
 *
 * Each sample starts a measurement and collects it Sht30::MeasurementMilliseconds later,
 * so the event loop never waits on the bus. A failed start or collect writes no row.
 * The first failure after a good reading is logged once, and so is the recovery.
 *
 * A failed commit keeps its readings for the next flush, up to MaximumPendingReadings;
 * beyond that the oldest are dropped.
 */
class ClimateLogger : public QObject,
                      public LoggingBaseClass
{
    Q_OBJECT
public:
    /** @brief The pending readings kept across failed commits, one hour at five seconds. */
    static constexpr int MaximumPendingReadings = 720;

    /**
     * @brief Constructs a logger.
     * @param sensor The sensor to sample. Not owned.
     * @param store Where readings are committed. Not owned.
     * @param clock Stamps each reading.
     * @param sampleSeconds The interval between samples.
     * @param flushSeconds The interval between commits.
     * @param parent The Qt parent object.
     */
    ClimateLogger(IClimateSensor* sensor, ClimateDataSource* store, IClock* clock,
                  int sampleSeconds, int flushSeconds, QObject* parent = nullptr);

    /** @brief Destructor. Commits whatever is pending. */
    virtual ~ClimateLogger();

    /** @brief Takes the first sample now and starts the sample and flush timers. */
    void start();

    /** @brief Returns the readings collected and not yet committed. */
    int pendingCount() const { return static_cast<int>(_pending.count()); }

public slots:
    /** @brief Starts one measurement and arms the collect timer. */
    void sample();

    /** @brief Reads the measurement sample() started and queues it for the next flush. */
    void collect();

    /** @brief Commits every pending reading. */
    void flush();

private:
    void noteFailure(const QString& text);

    IClimateSensor* _sensor = nullptr;
    ClimateDataSource* _store = nullptr;
    IClock* _clock = nullptr;
    QTimer _sampleTimer;
    QTimer _collectTimer;
    QTimer _flushTimer;
    ClimateReadingList _pending;
    bool _failing = false;
};

#endif // CLIMATELOGGER_H
