#include "runbutton.h"

RunButton::RunButton(IGpioBackend* backend, quint32 offset, QObject* parent) :
    QObject(parent),
    _backend(backend),
    _offset(offset)
{
}

bool RunButton::begin()
{
    if(_pin != nullptr) {
        _errorText = QString("The RUN button line is already requested");
        return false;
    }

    _pin = new InputPin(_backend, "irrigationd-run", _offset, this);
    // 1NO to ground: PullUp and activeLow together make a press assert.
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

    connect(_pin, &InputPin::asserted, this, &RunButton::onAsserted);
    connect(_pin, &InputPin::deasserted, this, &RunButton::onDeasserted);

    bool ok = false;
    _low = _pin->isAsserted(&ok);
    if(ok == false) {
        _errorText = _pin->errorText();
        delete _pin;
        _pin = nullptr;
        return false;
    }

    return true;
}

void RunButton::onAsserted()
{
    _low = true;
    emit lineChanged(true);
}

void RunButton::onDeasserted()
{
    _low = false;
    emit lineChanged(false);
}

#include "moc_runbutton.cpp"
