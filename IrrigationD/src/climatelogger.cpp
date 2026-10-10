#include "climatelogger.h"

#include "database/climatedatasource.h"
#include "iclimatesensor.h"
#include "sht30.h"

ClimateLogger::ClimateLogger(IClimateSensor* sensor, ClimateDataSource* store, IClock* clock,
                             int sampleSeconds, int flushSeconds, QObject* parent) :
    QObject(parent),
    LoggingBaseClass("climate"),
    _sensor(sensor),
    _store(store),
    _clock(clock)
{
    _sampleTimer.setInterval(sampleSeconds * 1000);
    _collectTimer.setSingleShot(true);
    _collectTimer.setInterval(Sht30::MeasurementMilliseconds);
    _flushTimer.setInterval(flushSeconds * 1000);

    connect(&_sampleTimer, &QTimer::timeout, this, &ClimateLogger::sample);
    connect(&_collectTimer, &QTimer::timeout, this, &ClimateLogger::collect);
    connect(&_flushTimer, &QTimer::timeout, this, &ClimateLogger::flush);
}

ClimateLogger::~ClimateLogger()
{
    flush();
}

void ClimateLogger::start()
{
    sample();
    _sampleTimer.start();
    _flushTimer.start();
}

void ClimateLogger::sample()
{
    if(_sensor->startMeasurement() == false) {
        noteFailure(_sensor->errorText());
        return;
    }
    _collectTimer.start();
}

void ClimateLogger::collect()
{
    ClimateReading reading;
    if(_sensor->readMeasurement(reading.temperatureCelsius, reading.humidityPercent) == false) {
        noteFailure(_sensor->errorText());
        return;
    }

    reading.atUtc = _clock->nowUtc();
    _pending.append(reading);
    _latest = reading;
    _latestMonotonicMsecs = _clock->monotonicMsecs();

    if(_failing) {
        _failing = false;
        logText(LVL_INFO, QString("The climate sensor is reading again: %1 C, %2 %RH")
                              .arg(reading.temperatureCelsius, 0, 'f', 1).arg(reading.humidityPercent, 0, 'f', 1));
    }
}

bool ClimateLogger::latestReading(ClimateReading& reading, int maxAgeSeconds) const
{
    if(_latestMonotonicMsecs < 0 || _clock->monotonicMsecs() - _latestMonotonicMsecs > maxAgeSeconds * 1000LL) {
        return false;
    }
    reading = _latest;
    return true;
}

void ClimateLogger::flush()
{
    if(_pending.isEmpty()) {
        return;
    }

    if(_store->insertReadings(_pending)) {
        _pending.clear();
        return;
    }

    logText(LVL_WARNING, QString("Failed to commit %1 climate readings: %2").arg(_pending.count()).arg(_store->errorText()));
    if(_pending.count() > MaximumPendingReadings) {
        const qsizetype dropped = _pending.count() - MaximumPendingReadings;
        _pending.remove(0, dropped);
        logText(LVL_WARNING, QString("Dropped the %1 oldest uncommitted climate readings").arg(dropped));
    }
}

void ClimateLogger::noteFailure(const QString& text)
{
    if(_failing == false) {
        _failing = true;
        logText(LVL_WARNING, QString("The climate sensor failed: %1").arg(text));
    }
}
