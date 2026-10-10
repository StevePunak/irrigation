#ifndef SHT30_H
#define SHT30_H

#include <QByteArray>
#include <QString>

#include "iclimatesensor.h"

/**
 * @brief Sensirion SHT30 on a Linux I2C bus, single-shot, high repeatability, no clock stretching.
 *
 * The bus device opens on the first startMeasurement() and again after any failure, so
 * a sensor or an i2c-dev module that appears after startup is picked up.
 *
 * @warning The result is ready 15 ms after startMeasurement(). A read before then is
 *          refused by the sensor and fails.
 */
class Sht30 : public IClimateSensor
{
public:
    /** @brief The SHT30's address with its ADDR pin low. */
    static constexpr quint8 DefaultAddress = 0x44;

    /** @brief The wait between startMeasurement() and readMeasurement() that covers the worst-case conversion. */
    static constexpr int MeasurementMilliseconds = 20;

    /** @brief Constructs a sensor at @p address on /dev/i2c-@p bus. */
    Sht30(int bus, quint8 address);

    /** @brief Destructor. Closes the bus device. */
    virtual ~Sht30();

    virtual bool startMeasurement() override;
    virtual bool readMeasurement(double& temperatureCelsius, double& humidityPercent) override;
    virtual QString errorText() const override { return _errorText; }

    /** @brief Returns the SHT3x CRC-8 (polynomial 0x31, initial 0xFF) of @p data. */
    static quint8 crc8(const QByteArray& data);

    /**
     * @brief Decodes a six-byte result frame: temperature word, its CRC, humidity word, its CRC.
     * @return False for a frame of any other length or with either CRC wrong.
     */
    static bool decode(const QByteArray& frame, double& temperatureCelsius, double& humidityPercent);

private:
    bool openDevice();
    void closeDevice();
    void fail(const QString& text);

    static constexpr quint8 MeasureHighRepeatabilityMsb = 0x24;
    static constexpr quint8 MeasureHighRepeatabilityLsb = 0x00;
    static constexpr int FrameLength = 6;

    QString _devicePath;
    quint8 _address = DefaultAddress;
    int _fd = -1;
    QString _errorText;
};

#endif // SHT30_H
