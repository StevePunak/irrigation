#include "stopbutton.h"

StopButton::StopButton(IGpioBackend* backend, quint32 offset, QObject* parent) :
    QObject(parent),
    _backend(backend),
    _offset(offset)
{
}

bool StopButton::begin()
{
    // Non-null _pin means a previous call already succeeded; a failed call leaves it null again.
    if(_pin != nullptr) {
        _errorText = QString("The stop button line is already requested");
        return false;
    }

    _pin = new InputPin(_backend, "irrigationd-stop", _offset, this);
    // Button is 1NO to ground: PullUp and activeLow together are what make a press assert.
    _pin->setActiveLow(true);
    _pin->setBias(Gpio::Bias::PullUp);
    _pin->setEdge(Gpio::Edge::Both);
    _pin->setDebounce(TimeSpan::fromMilliseconds(20));

    if(_pin->request() == false) {
        _errorText = _pin->errorText();
        delete _pin;
        _pin = nullptr;
        return false;
    }

    connect(_pin, &InputPin::asserted, this, &StopButton::onAsserted);
    connect(_pin, &InputPin::deasserted, this, &StopButton::onDeasserted);

    bool ok = false;
    _held = _pin->isAsserted(&ok);
    if(ok == false) {
        _errorText = _pin->errorText();
        delete _pin;
        _pin = nullptr;
        return false;
    }

    return true;
}

void StopButton::onAsserted()
{
    _held = true;
    emit pressed();
}

void StopButton::onDeasserted()
{
    _held = false;
}

#include "moc_stopbutton.cpp"
