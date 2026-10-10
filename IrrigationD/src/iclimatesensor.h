#ifndef ICLIMATESENSOR_H
#define ICLIMATESENSOR_H

#include <QString>

/** @brief A temperature and humidity sensor read in two steps: start a measurement, then collect it. */
class IClimateSensor
{
public:
    virtual ~IClimateSensor() {}

    /** @brief Starts one measurement. @return True when the sensor accepted the command. */
    virtual bool startMeasurement() = 0;

    /**
     * @brief Collects the measurement the last startMeasurement() began.
     * @param temperatureCelsius Set on success.
     * @param humidityPercent Set on success.
     * @return True when a whole, checksum-valid result was read.
     */
    virtual bool readMeasurement(double& temperatureCelsius, double& humidityPercent) = 0;

    /** @brief Returns the text of the most recent failure. */
    virtual QString errorText() const = 0;
};

#endif // ICLIMATESENSOR_H
