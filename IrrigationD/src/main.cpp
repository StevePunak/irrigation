#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QSocketNotifier>

#include <Kanoop/log.h>
#include <Kanoop/pi/libgpiodbackend.h>

#include "irrigationdaemon.h"
#include "irrigationsettings.h"
#include "panelformat.h"
#include "tm1637display.h"

namespace
{
    int signalFds[2] = { -1, -1 };

    void posixSignalHandler(int)
    {
        char byte = 1;
        ssize_t written = ::write(signalFds[0], &byte, sizeof(byte));
        Q_UNUSED(written)
    }

    bool showDashes(const QString& configPath)
    {
        IrrigationSettings settings(configPath);
        const int clockOffset = settings.displayClockOffset();
        const int dataOffset = settings.displayDataOffset();
        if(clockOffset < 0 || dataOffset < 0) {
            Log::logText(LVL_INFO, "No display lines are configured; there is nothing to clear");
            return true;
        }

        LibGpiodBackend backend;
        if(backend.openChipByLabel(settings.chipLabel()) == false) {
            Log::logText(LVL_ERROR, QString("Failed to open GPIO chip '%1': %2").arg(settings.chipLabel(), backend.errorText()));
            return false;
        }

        Tm1637Display display(&backend, static_cast<quint32>(clockOffset), static_cast<quint32>(dataOffset));
        if(display.begin() == false || display.show(PanelFormat::dashes()) == false) {
            Log::logText(LVL_ERROR, QString("Failed to write ---- to the display: %1").arg(display.errorText()));
            return false;
        }

        return true;
    }
}

const QString keyConfig =  "config";
const QString keyDashes =  "dashes";
const QString keyHelp =    "help";
const QString keyVerbose = "verbose";

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("irrigationd");
    QCoreApplication::setApplicationVersion("1.0.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("Irrigation controller daemon");
    parser.addOptions({
        // Short / Long name    Description                             Value name      Default
        {{ "c", keyConfig },    "Path to the INI configuration file",   "path",         "/etc/irrigationd.ini"  },
        {{ keyDashes },         "Write ---- to the panel display and exit",                                     },
        {{ "v", keyVerbose },   "Log at debug level",                   /** short option */                     },
        {{ "?", keyHelp },      "Print usage and exit",                 /** short option */                     },
    });
    parser.process(app);

    if(parser.isSet(keyHelp)) {
        parser.showHelp(0);
    }

    Log::setFlags(Log::Standard);
    Log::setLevel(parser.isSet(keyVerbose) ? Log::LogLevel::Debug : Log::LogLevel::Info);
    Log::systemLog()->openLog();

    if(parser.isSet(keyDashes)) {
        return showDashes(parser.value(keyConfig)) == true ? 0 : 1;
    }

    if(::socketpair(AF_UNIX, SOCK_STREAM, 0, signalFds) != 0) {
        Log::logText(LVL_ERROR, "Failed to create signal socketpair");
        return 1;
    }

    QSocketNotifier notifier(signalFds[1], QSocketNotifier::Read);
    QObject::connect(&notifier, &QSocketNotifier::activated, &app, [&notifier, &app]()
    {
        notifier.setEnabled(false);
        char byte = 0;
        ssize_t got = ::read(signalFds[1], &byte, sizeof(byte));
        Q_UNUSED(got)
        Log::logText(LVL_INFO, "Signal received, shutting down");
        app.quit();
    });

    ::signal(SIGINT, posixSignalHandler);
    ::signal(SIGTERM, posixSignalHandler);

    // ⚠ start() reports that the worker thread started. Readiness of the
    //   components is a separate query on the daemon.
    IrrigationDaemon daemon(parser.value(keyConfig));
    daemon.setVerboseLogging(parser.isSet(keyVerbose));
    daemon.start();
    if(daemon.isReady() == false) {
        Log::logText(LVL_ERROR, QString("Failed to start: %1").arg(daemon.errorText()));
        return 1;
    }

    int result = app.exec();
    daemon.stop();
    return result;
}
