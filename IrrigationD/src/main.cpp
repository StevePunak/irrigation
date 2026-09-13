#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QSocketNotifier>

#include <Kanoop/log.h>

#include "irrigationdaemon.h"

namespace
{
    int signalFds[2] = { -1, -1 };

    void posixSignalHandler(int)
    {
        char byte = 1;
        ssize_t written = ::write(signalFds[0], &byte, sizeof(byte));
        Q_UNUSED(written)
    }
}

const QString keyConfig =  "config";
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
    daemon.start();
    if(daemon.isReady() == false) {
        Log::logText(LVL_ERROR, QString("Failed to start: %1").arg(daemon.errorText()));
        return 1;
    }

    int result = app.exec();
    daemon.stop();
    return result;
}
