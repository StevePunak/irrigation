#ifndef IRRIGATIONDAEMON_H
#define IRRIGATIONDAEMON_H

#include <QObject>

#include <Kanoop/utility/loggingbaseclass.h>

#include "irrigationsettings.h"

/**
 * @brief Owns the lifetime of every daemon component.
 *
 * @warning Construction order is a hardware contract, not a preference.
 *          ZoneController is constructed and drives all eight lines
 *          de-energised before the scheduler or the HTTP server exist, so
 *          nothing can ask for water before the valves are known shut.
 */
class IrrigationDaemon : public QObject,
                         public LoggingBaseClass
{
    Q_OBJECT
public:
    /** @brief Constructs the daemon against the settings at @p settingsPath. */
    explicit IrrigationDaemon(const QString& settingsPath, QObject* parent = nullptr);

    /** @brief Destructor. Stops anything still running. */
    virtual ~IrrigationDaemon();

    /** @brief Brings up every component in order. @return True when the daemon is serving. */
    bool start();

    /** @brief Tears every component down in reverse order. */
    void stop();

    /** @brief Returns the text of the most recent failure. */
    QString errorText() const { return _errorText; }

private:
    IrrigationSettings _settings;
    QString _errorText;
    bool _running = false;
};

#endif // IRRIGATIONDAEMON_H
