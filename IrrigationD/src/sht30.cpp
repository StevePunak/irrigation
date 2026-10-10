#include "sht30.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>

Sht30::Sht30(int bus, quint8 address) :
    _devicePath(QString("/dev/i2c-%1").arg(bus)),
    _address(address)
{
}

Sht30::~Sht30()
{
    closeDevice();
}

bool Sht30::startMeasurement()
{
    if(_fd < 0 && openDevice() == false) {
        return false;
    }

    const quint8 command[2] = { MeasureHighRepeatabilityMsb, MeasureHighRepeatabilityLsb };
    if(::write(_fd, command, sizeof(command)) != static_cast<ssize_t>(sizeof(command))) {
        const int error = errno;
        fail(QString("The measure command to 0x%1 on %2 failed: %3")
                 .arg(static_cast<uint>(_address), 2, 16, QChar('0')).arg(_devicePath, QString::fromLocal8Bit(strerror(error))));
        return false;
    }

    return true;
}

bool Sht30::readMeasurement(double& temperatureCelsius, double& humidityPercent)
{
    if(_fd < 0) {
        _errorText = QString("%1 is not open").arg(_devicePath);
        return false;
    }

    QByteArray frame(FrameLength, '\0');
    if(::read(_fd, frame.data(), FrameLength) != FrameLength) {
        const int error = errno;
        fail(QString("Reading the result from 0x%1 on %2 failed: %3")
                 .arg(static_cast<uint>(_address), 2, 16, QChar('0')).arg(_devicePath, QString::fromLocal8Bit(strerror(error))));
        return false;
    }

    if(decode(frame, temperatureCelsius, humidityPercent) == false) {
        _errorText = QString("The result from 0x%1 failed its checksum: %2")
                         .arg(static_cast<uint>(_address), 2, 16, QChar('0')).arg(QString::fromLatin1(frame.toHex(' ')));
        return false;
    }

    return true;
}

quint8 Sht30::crc8(const QByteArray& data)
{
    quint8 crc = 0xFF;
    for(char byte : data) {
        crc ^= static_cast<quint8>(byte);
        for(int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? static_cast<quint8>((crc << 1) ^ 0x31) : static_cast<quint8>(crc << 1);
        }
    }
    return crc;
}

bool Sht30::decode(const QByteArray& frame, double& temperatureCelsius, double& humidityPercent)
{
    if(frame.size() != FrameLength) {
        return false;
    }
    if(crc8(frame.mid(0, 2)) != static_cast<quint8>(frame.at(2)) || crc8(frame.mid(3, 2)) != static_cast<quint8>(frame.at(5))) {
        return false;
    }

    const quint16 rawTemperature = static_cast<quint16>((static_cast<quint8>(frame.at(0)) << 8) | static_cast<quint8>(frame.at(1)));
    const quint16 rawHumidity = static_cast<quint16>((static_cast<quint8>(frame.at(3)) << 8) | static_cast<quint8>(frame.at(4)));
    temperatureCelsius = -45.0 + 175.0 * rawTemperature / 65535.0;
    humidityPercent = 100.0 * rawHumidity / 65535.0;
    return true;
}

bool Sht30::openDevice()
{
    _fd = ::open(_devicePath.toLocal8Bit().constData(), O_RDWR | O_CLOEXEC);
    if(_fd < 0) {
        const int error = errno;
        _errorText = QString("Failed to open %1: %2").arg(_devicePath, QString::fromLocal8Bit(strerror(error)));
        return false;
    }

    if(::ioctl(_fd, I2C_SLAVE, static_cast<unsigned long>(_address)) < 0) {
        const int error = errno;
        fail(QString("Failed to address 0x%1 on %2: %3")
                 .arg(static_cast<uint>(_address), 2, 16, QChar('0')).arg(_devicePath, QString::fromLocal8Bit(strerror(error))));
        return false;
    }

    return true;
}

void Sht30::closeDevice()
{
    if(_fd >= 0) {
        ::close(_fd);
        _fd = -1;
    }
}

void Sht30::fail(const QString& text)
{
    _errorText = text;
    closeDevice();
}
