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

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("irrigationd");
    QCoreApplication::setApplicationVersion("1.0.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("Irrigation controller daemon");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption configOption(
        QStringList() << "c" << "config",
        "Path to the INI configuration file.",
        "path",
        "/etc/irrigationd.ini");
    parser.addOption(configOption);

    QCommandLineOption verboseOption(
        "verbose",
        "Log at debug level.");
    parser.addOption(verboseOption);

    parser.process(app);

    Log::setFlags(Log::Standard);
    Log::setLevel(parser.isSet(verboseOption) ? Log::LogLevel::Debug : Log::LogLevel::Info);
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

    IrrigationDaemon daemon(parser.value(configOption));
    if(daemon.start() == false) {
        Log::logText(LVL_ERROR, QString("Failed to start: %1").arg(daemon.errorText()));
        return 1;
    }

    int result = app.exec();
    daemon.stop();
    return result;
}
