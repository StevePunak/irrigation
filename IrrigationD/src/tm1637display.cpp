#include "tm1637display.h"

#include <chrono>

Tm1637Display::Tm1637Display(IGpioBackend* backend, quint32 clockOffset, quint32 dataOffset, QObject* parent) :
    QObject(parent),
    _backend(backend),
    _clockOffset(clockOffset),
    _dataOffset(dataOffset)
{
}

bool Tm1637Display::begin()
{
    if(_bank != nullptr) {
        _errorText = QString("The display lines are already requested");
        return false;
    }

    _bank = new OutputBank(_backend, "irrigationd-display", { _clockOffset, _dataOffset }, false, this);
    if(_bank->request() == false) {
        _errorText = _bank->errorText();
        delete _bank;
        _bank = nullptr;
        return false;
    }

    return true;
}

bool Tm1637Display::show(const QByteArray& segments)
{
    if(_bank == nullptr) {
        _errorText = QString("The display lines are not requested");
        return false;
    }
    if(segments.size() != 4) {
        _errorText = QString("Expected 4 segment bytes, got %1").arg(segments.size());
        return false;
    }

    bool ok = startCondition() && writeByte(DataCommand) && stopCondition();
    ok = ok && startCondition() && writeByte(AddressCommand);
    for(int i = 0; ok == true && i < segments.size(); i++) {
        ok = writeByte(static_cast<quint8>(segments.at(i)));
    }
    ok = ok && stopCondition();
    ok = ok && startCondition() && writeByte(DisplayControlCommand | Brightness) && stopCondition();
    return ok;
}

bool Tm1637Display::startCondition()
{
    return setLine(_clockOffset, true) && setLine(_dataOffset, true)
        && setLine(_dataOffset, false) && setLine(_clockOffset, false);
}

bool Tm1637Display::stopCondition()
{
    return setLine(_clockOffset, false) && setLine(_dataOffset, false)
        && setLine(_clockOffset, true) && setLine(_dataOffset, true);
}

bool Tm1637Display::writeByte(quint8 value)
{
    for(int bit = 0; bit < 8; bit++) {
        if(setLine(_clockOffset, false) == false
           || setLine(_dataOffset, ((value >> bit) & 1) != 0) == false
           || setLine(_clockOffset, true) == false) {
            return false;
        }
    }

    // Acknowledge clock: DIO stays driven low.
    return setLine(_clockOffset, false) && setLine(_dataOffset, false)
        && setLine(_clockOffset, true) && setLine(_clockOffset, false);
}

bool Tm1637Display::setLine(quint32 offset, bool high)
{
    if(_bank->setValue(offset, high) == false) {
        _errorText = _bank->errorText();
        return false;
    }
    pause();
    return true;
}

void Tm1637Display::pause() const
{
    if(_edgeDelayMicroseconds <= 0) {
        return;
    }

    const std::chrono::steady_clock::time_point until =
        std::chrono::steady_clock::now() + std::chrono::microseconds(_edgeDelayMicroseconds);
    while(std::chrono::steady_clock::now() < until) {
    }
}

#include "moc_tm1637display.cpp"
