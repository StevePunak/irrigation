#include "stopbutton.h"

StopButton::StopButton(IGpioBackend* backend, quint32 offset, QObject* parent) :
    QObject(parent),
    _backend(backend),
    _offset(offset)
{
}

bool StopButton::begin()
{
    _pin = new InputPin(_backend, "irrigationd-stop", _offset, this);
    // Button is 1NO to ground: PullUp and activeLow together are what make a press assert.
    _pin->setActiveLow(true);
    _pin->setBias(Gpio::Bias::PullUp);
    _pin->setEdge(Gpio::Edge::Both);
    _pin->setDebounce(TimeSpan::fromMilliseconds(20));

    if(_pin->request() == false) {
        _errorText = _pin->errorText();
        return false;
    }

    connect(_pin, &InputPin::asserted, this, &StopButton::pressed);

    bool ok = false;
    _held = _pin->isAsserted(&ok);
    if(ok == false) {
        _errorText = _pin->errorText();
        return false;
    }

    return true;
}

#include "moc_stopbutton.cpp"
