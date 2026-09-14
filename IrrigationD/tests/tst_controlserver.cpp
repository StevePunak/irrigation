#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSqlQuery>
#include <QStringList>
#include <QTcpServer>
#include <QTimeZone>
#include <QUrl>

#include <utility>

#include <Kanoop/timespan.h>

#include "database/irrigationdatasource.h"
#include "irrigationcontrolserver.h"
#include "model/program.h"
#include "model/programstarttime.h"
#include "model/programzone.h"
#include "model/zone.h"

namespace
{
    QNetworkReply* getJson(QNetworkAccessManager& manager, int port, const QString& path)
    {
        QNetworkRequest request(QUrl(QString("http://127.0.0.1:%1%2").arg(port).arg(path)));
        QNetworkReply* reply = manager.get(request);
        QSignalSpy spy(reply, &QNetworkReply::finished);
        spy.wait(5000);
        return reply;
    }

    QNetworkReply* postJson(QNetworkAccessManager& manager, int port, const QString& path, const QByteArray& body)
    {
        QNetworkRequest request(QUrl(QString("http://127.0.0.1:%1%2").arg(port).arg(path)));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        QNetworkReply* reply = manager.post(request, body);
        QSignalSpy spy(reply, &QNetworkReply::finished);
        spy.wait(5000);
        return reply;
    }

    QNetworkReply* putJson(QNetworkAccessManager& manager, int port, const QString& path, const QByteArray& body)
    {
        QNetworkRequest request(QUrl(QString("http://127.0.0.1:%1%2").arg(port).arg(path)));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        QNetworkReply* reply = manager.put(request, body);
        QSignalSpy spy(reply, &QNetworkReply::finished);
        spy.wait(5000);
        return reply;
    }

    int statusCode(QNetworkReply* reply)
    {
        return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    }

    bool listenerIsLoopbackOnly(quint16 port)
    {
        QFile procNetTcp("/proc/net/tcp");
        if(procNetTcp.open(QIODevice::ReadOnly | QIODevice::Text) == false) {
            return false;
        }

        const QString portHex = QString::number(port, 16).toUpper().rightJustified(4, QLatin1Char('0'));
        const QList<QByteArray> lines = procNetTcp.readAll().split('\n');
        bool found = false;
        bool loopback = false;
        for(const QByteArray& line : lines) {
            const QList<QByteArray> fields = line.simplified().split(' ');
            if(fields.count() < 2) {
                continue;
            }
            const QList<QByteArray> localAddress = fields.at(1).split(':');
            if(localAddress.count() != 2 || QString::fromLatin1(localAddress.at(1)) != portHex) {
                continue;
            }
            found = true;
            loopback = localAddress.at(0) == QByteArray("0100007F");
            break;
        }
        return found && loopback;
    }

    bool startServerOnLoopback(IrrigationControlServer& server)
    {
        server.setBindAddress("127.0.0.1");
        server.setListenPort(0);
        return server.start(TimeSpan::fromSeconds(5)) && server.waitUntilReady(TimeSpan::fromSeconds(5));
    }

    // Renumbers zone id 1 away from its seeded number so id and number diverge; no other
    // zone's id equals the new number. Must run before IrrigationControlServer::start()
    // opens its own connection on the same file.
    void seedRenumberedZone(const QString& dbPath, int newNumber, bool enabled)
    {
        IrrigationDataSource seed(dbPath);
        QVERIFY(seed.open());
        bool ok = false;
        seed.rawQuery(QString("UPDATE zones SET number = %1, enabled = %2 WHERE id = 1")
                          .arg(newNumber).arg(enabled ? 1 : 0), &ok);
        QVERIFY(ok);
    }

    QNetworkReply* deleteResource(QNetworkAccessManager& manager, int port, const QString& path)
    {
        QNetworkRequest request(QUrl(QString("http://127.0.0.1:%1%2").arg(port).arg(path)));
        QNetworkReply* reply = manager.deleteResource(request);
        QSignalSpy spy(reply, &QNetworkReply::finished);
        spy.wait(5000);
        return reply;
    }

    // Writes directly through IrrigationDataSource, skipping every REST-layer validation
    // route handlers apply. Writes the database-generated ids back into program, startTimes
    // and zones.
    bool seedProgramDirect(const QString& dbPath,
                           Program& program,
                           QList<ProgramStartTime>& startTimes,
                           QList<ProgramZone>& zones)
    {
        IrrigationDataSource seed(dbPath);
        if(seed.open() == false) {
            return false;
        }
        if(seed.insertProgram(program) == false) {
            return false;
        }
        for(ProgramStartTime& startTime : startTimes) {
            startTime.programId = program.id;
            if(seed.insertStartTime(startTime) == false) {
                return false;
            }
        }
        for(ProgramZone& zone : zones) {
            zone.programId = program.id;
            if(seed.insertProgramZone(zone) == false) {
                return false;
            }
        }
        return true;
    }

    bool containsStartTimeId(const ProgramStartTimeList& list, int id)
    {
        for(const ProgramStartTime& item : list) {
            if(item.id == id) {
                return true;
            }
        }
        return false;
    }

    QJsonObject baseValidProgramBody()
    {
        QJsonObject startTime{
            { "minutesAfterMidnight", 360 },
            { "timezone", "America/Los_Angeles" }
        };
        QJsonObject zone{
            { "zoneId", 2 },
            { "sequence", 0 },
            { "durationSeconds", 300 }
        };
        QJsonObject body{
            { "name", "Valid Program" },
            { "enabled", true },
            { "dayMode", "DaysOfWeek" },
            { "dowMask", 5 },
            { "intervalDays", 0 },
            { "anchorDate", "" },
            { "startTimes", QJsonArray{ startTime } },
            { "zones", QJsonArray{ zone } }
        };
        return body;
    }

    // scope selects which part of the body the override lands in: "root" for a top-level
    // program field, "startTime" for startTimes[0], "zone" for zones[0].
    QJsonObject withProgramField(QJsonObject body, const QString& scope, const QString& key, const QJsonValue& value)
    {
        if(scope == QString("startTime")) {
            QJsonArray array = body.value("startTimes").toArray();
            QJsonObject entry = array.at(0).toObject();
            entry[key] = value;
            array[0] = entry;
            body["startTimes"] = array;
        }
        else if(scope == QString("zone")) {
            QJsonArray array = body.value("zones").toArray();
            QJsonObject entry = array.at(0).toObject();
            entry[key] = value;
            array[0] = entry;
            body["zones"] = array;
        }
        else {
            body[key] = value;
        }
        return body;
    }

    QJsonObject everyNDaysBody(int intervalDays, const QJsonValue& anchorDate)
    {
        QJsonObject body = baseValidProgramBody();
        body["dayMode"] = "EveryNDays";
        body["intervalDays"] = intervalDays;
        body["anchorDate"] = anchorDate;
        return body;
    }

    QList<std::pair<QString, QByteArray>> programValidationRows()
    {
        QList<std::pair<QString, QByteArray>> rows;
        auto add = [&rows](const QString& name, const QJsonObject& body)
        {
            rows.append({ name, QJsonDocument(body).toJson(QJsonDocument::Compact) });
        };

        add("dayMode unknown name", withProgramField(baseValidProgramBody(), "root", "dayMode", "Whenever"));
        add("EveryNDays intervalDays zero", everyNDaysBody(0, "2027-01-01"));
        add("EveryNDays intervalDays negative", everyNDaysBody(-3, "2027-01-01"));
        add("EveryNDays anchorDate missing", everyNDaysBody(3, ""));
        add("EveryNDays anchorDate invalid", everyNDaysBody(3, "not-a-date"));
        add("DaysOfWeek mask zero", withProgramField(baseValidProgramBody(), "root", "dowMask", 0));
        add("DaysOfWeek mask outside seven days", withProgramField(baseValidProgramBody(), "root", "dowMask", 128));
        add("enabled non-boolean", withProgramField(baseValidProgramBody(), "root", "enabled", "true"));
        add("minutesAfterMidnight below range", withProgramField(baseValidProgramBody(), "startTime", "minutesAfterMidnight", -1));
        add("minutesAfterMidnight above range", withProgramField(baseValidProgramBody(), "startTime", "minutesAfterMidnight", 1440));
        add("minutesAfterMidnight as a JSON string", withProgramField(baseValidProgramBody(), "startTime", "minutesAfterMidnight", "360"));
        add("minutesAfterMidnight as a JSON boolean", withProgramField(baseValidProgramBody(), "startTime", "minutesAfterMidnight", true));
        add("unknown timezone", withProgramField(baseValidProgramBody(), "startTime", "timezone", "Mars/Olympus"));
        add("durationSeconds zero", withProgramField(baseValidProgramBody(), "zone", "durationSeconds", 0));
        add("durationSeconds as a JSON string", withProgramField(baseValidProgramBody(), "zone", "durationSeconds", "300"));
        add("zoneId names no zone", withProgramField(baseValidProgramBody(), "zone", "zoneId", 9999));
        add("zoneId as a JSON string", withProgramField(baseValidProgramBody(), "zone", "zoneId", "2"));
        add("sequence as a JSON string", withProgramField(baseValidProgramBody(), "zone", "sequence", "0"));

        return rows;
    }
}

class TestControlServer : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();

    void waitUntilReadyReturnsTrueImmediatelyOnASecondCall();
    void waitUntilReadyIsResetAcrossStopThenStart();
    void boundPortIsNonZeroAfterListeningOnPortZero();
    void defaultBindAddressIsLoopbackOnly();
    void listenFailureOnAnOccupiedPortLeavesReadyFalse();
    void stopCompletesWithinItsTimeout();

    void healthReturns200WithStatusOk();
    void versionReturns200WithTheApplicationVersion();

    void statusKeySetMatchesTheSerializer();
    void statusFieldTypesMatchTheWebDecoder();
    void updateStatusFromTheTestThreadAppearsInTheNextStatusGet();

    void zonesGetListsAllEightSeededZones();
    void zonePutUpdatesTheZoneWhoseNumberMatches();
    void zonePutUnknownNumberReturns404();
    void zonePutMalformedBodyReturns400();
    void zonePutMissingOrInvalidFieldReturns400_data();
    void zonePutMissingOrInvalidFieldReturns400();

    void zoneRunEmitsManualZoneRunRequestedWithTheExactZoneAndSeconds();
    void zoneRunUnknownZoneReturns404AndEmitsNothing();
    void zoneRunMalformedBodyReturns400AndEmitsNothing();
    void zoneRunSecondsBoundaryAcceptsOneRejectsZero();
    void zoneRunRejectsAMissingOrNonNumericSeconds_data();
    void zoneRunRejectsAMissingOrNonNumericSeconds();
    void zoneRunDisabledZoneReturns409AndEmitsNothing();

    void stopEmitsStopRequestedExactlyOnce();

    void settingsGetReturnsExactlyTheFourAllowlistedKeys();
    void settingsPutRejectsInvalidValue_data();
    void settingsPutRejectsInvalidValue();
    void settingsPutRejectsAMultiKeyBodyWhenAnyKeyIsInvalid();
    void settingsPutRejectsAMultiKeyBodyWhenALaterKeyIsNull();
    void settingsPutRejectsNonStringValue_data();
    void settingsPutRejectsNonStringValue();
    void settingsPutMalformedBodyReturns400_data();
    void settingsPutMalformedBodyReturns400();
    void settingsPutAcceptsValidValueAtBothEdges_data();
    void settingsPutAcceptsValidValueAtBothEdges();
    void settingsPutSuccessReadBackThroughGetAndSeparateDataSourceEmitsSignalOnce();

    void programsGetReturnsFieldsStartTimesZonesAndPerProgramNextRunUtc();

    void programPostCreatesStoredProgramReadBackThroughGetAndSeparateConnection();
    void programPostValidationRejectsAndWritesNothing_data();
    void programPostValidationRejectsAndWritesNothing();
    void programPostAcceptsBoundaryValuesForMinutesAfterMidnightAndDurationSeconds_data();
    void programPostAcceptsBoundaryValuesForMinutesAfterMidnightAndDurationSeconds();
    void programPostMalformedBodyReturns400AndWritesNothing();
    void programPostAcceptsOddDayMode();
    void programPostAcceptsEvenDayMode();
    void programPostAcceptsEveryNDaysWithIntervalOne();
    void programPostAcceptsDaysOfWeekWithAllSevenDaysBit();
    void programPostRejectsAnUnknownSecondZoneId();
    void programPostAcceptsAZoneIdThatDiffersFromItsRenumberedZoneNumber();
    void programPostAnswers500WhenCommitFailsWritingNothing();

    void programPutUnknownIdReturns404();
    void programPutValidationRejectsAndPreservesEverything_data();
    void programPutValidationRejectsAndPreservesEverything();
    void programPutMalformedBodyReturns400AndPreservesEverything();
    void programPutWithUnchangedStartTimesPreservesTheirIds();
    void programPutChangedStartTimesDeleteUnmatchedInsertUnmatched();
    void programPutDuplicateStartTimesReconcileAsAMultiset();
    void programPutReorderedIdenticalStartTimesKeepsIds();
    void programPutReplacesProgramZonesAsSent();
    void programPutRollsBackOnGenuineDatabaseFailureLeavingOriginalDataIntact();
    void programPutStoresEveryMutableProgramField();
    void programPutTimezoneOnlyChangeReconcilesToANewRow();
    void programPutStartTimesSharingMinutesDifferingTimezoneReconcileIndependently();
    void programPutStoredDuplicateStartTimesShrinkToOneKeepingTheFirstId();
    void programPutAnswers500WhenCommitFailsLeavingOriginalDataIntact();
    void programPutAnswers500WhenAStartTimeInsertFailsLeavingOriginalDataIntact();

    void programsGetReportsEmptyNextRunUtcForADisabledProgram();

    void programDeleteRemovesProgramCascadingStartTimesAndZonesLeavesOtherProgramsIntact();
    void programDeleteUnknownIdReturns404();

    void programRunEmitsProgramRunRequestedWithTheExactProgramId();
    void programRunUnknownIdReturns404AndEmitsNothing();
};

void TestControlServer::initTestCase()
{
    QCoreApplication::setApplicationVersion("t3a-test-1.2.3");
}

void TestControlServer::waitUntilReadyReturnsTrueImmediatelyOnASecondCall()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QElapsedTimer timer;
    timer.start();
    QVERIFY(server.waitUntilReady(TimeSpan::fromSeconds(5)));
    QVERIFY(timer.elapsed() < 500);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));
}

void TestControlServer::waitUntilReadyIsResetAcrossStopThenStart()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));
    QVERIFY(server.boundPort() != 0);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));
    QVERIFY(server.waitUntilReady(TimeSpan::fromMilliseconds(300)) == false);

    QVERIFY(server.start(TimeSpan::fromSeconds(5)));
    QVERIFY(server.waitUntilReady(TimeSpan::fromSeconds(5)));
    QVERIFY(server.boundPort() != 0);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));
}

void TestControlServer::boundPortIsNonZeroAfterListeningOnPortZero()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QVERIFY(server.boundPort() != 0);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));
}

void TestControlServer::defaultBindAddressIsLoopbackOnly()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    // setBindAddress() deliberately not called: this pins the constructor's default.
    server.setListenPort(0);
    QVERIFY(server.start(TimeSpan::fromSeconds(5)));
    QVERIFY(server.waitUntilReady(TimeSpan::fromSeconds(5)));

    QVERIFY(listenerIsLoopbackOnly(static_cast<quint16>(server.boundPort())));

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));
}

void TestControlServer::listenFailureOnAnOccupiedPortLeavesReadyFalse()
{
    QTcpServer occupier;
    QVERIFY(occupier.listen(QHostAddress::LocalHost, 0));
    const quint16 occupiedPort = occupier.serverPort();

    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    server.setBindAddress("127.0.0.1");
    server.setListenPort(occupiedPort);
    QVERIFY(server.start(TimeSpan::fromSeconds(5)));

    QVERIFY(server.waitUntilReady(TimeSpan::fromMilliseconds(500)) == false);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));
    occupier.close();
}

void TestControlServer::stopCompletesWithinItsTimeout()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QElapsedTimer timer;
    timer.start();
    QVERIFY(server.stop(TimeSpan::fromSeconds(10)));
    QVERIFY(timer.elapsed() < 2000);
}

void TestControlServer::healthReturns200WithStatusOk()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = getJson(manager, server.boundPort(), "/admin/health");
    QCOMPARE(statusCode(reply), 200);

    const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
    QCOMPARE(body.value("status").toString(), QString("ok"));

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::versionReturns200WithTheApplicationVersion()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = getJson(manager, server.boundPort(), "/admin/version");
    QCOMPARE(statusCode(reply), 200);

    const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
    QCOMPARE(body.value("version").toString(), QCoreApplication::applicationVersion());

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::statusKeySetMatchesTheSerializer()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = getJson(manager, server.boundPort(), "/admin/status");
    QCOMPARE(statusCode(reply), 200);

    const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
    QStringList keys = body.keys();
    std::sort(keys.begin(), keys.end());

    const QStringList expected = {
        "masterEnabled", "nextRunUtc", "rainDelayUntilUtc", "runningZone",
        "secondsRemaining", "stopHeld", "timezone"
    };
    QCOMPARE(keys, expected);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::statusFieldTypesMatchTheWebDecoder()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = getJson(manager, server.boundPort(), "/admin/status");
    const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();

    QVERIFY(body.value("runningZone").isDouble());
    QVERIFY(body.value("secondsRemaining").isDouble());
    QVERIFY(body.value("nextRunUtc").isString());
    QVERIFY(body.value("timezone").isString());
    QVERIFY(body.value("masterEnabled").isBool());
    QVERIFY(body.value("stopHeld").isBool());
    QVERIFY(body.value("rainDelayUntilUtc").isString());

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::updateStatusFromTheTestThreadAppearsInTheNextStatusGet()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;

    ServerStatus first;
    first.runningZone = 4;
    first.secondsRemaining = 137;
    first.nextRunUtc = QDateTime(QDate(2026, 9, 20), QTime(13, 15, 0), QTimeZone::UTC);
    first.rainDelayUntilUtc = QDateTime(QDate(2026, 9, 25), QTime(6, 30, 0), QTimeZone::UTC);
    first.timezone = "America/Los_Angeles";
    first.masterEnabled = true;
    first.stopHeld = false;
    server.updateStatus(first);

    QJsonObject firstBody;
    for(int attempt = 0; attempt < 20; attempt++) {
        QNetworkReply* reply = getJson(manager, server.boundPort(), "/admin/status");
        firstBody = QJsonDocument::fromJson(reply->readAll()).object();
        if(firstBody.value("runningZone").toInt() == 4) {
            break;
        }
    }

    QCOMPARE(firstBody.value("runningZone").toInt(), 4);
    QCOMPARE(firstBody.value("secondsRemaining").toInt(), 137);
    QCOMPARE(firstBody.value("nextRunUtc").toString(), first.nextRunUtc.toUTC().toString(Qt::ISODate));
    QCOMPARE(firstBody.value("rainDelayUntilUtc").toString(), first.rainDelayUntilUtc.toUTC().toString(Qt::ISODate));
    QCOMPARE(firstBody.value("timezone").toString(), QString("America/Los_Angeles"));
    QCOMPARE(firstBody.value("masterEnabled").toBool(), true);
    QCOMPARE(firstBody.value("stopHeld").toBool(), false);

    // masterEnabled and stopHeld must both stay true here; nextRunUtc must stay in a
    // non-UTC zone.
    ServerStatus second;
    second.runningZone = 7;
    second.secondsRemaining = 42;
    second.nextRunUtc = QDateTime(QDate(2026, 11, 3), QTime(3, 5, 0), QTimeZone("America/Denver"));
    second.rainDelayUntilUtc = QDateTime(QDate(2026, 11, 10), QTime(21, 50, 0), QTimeZone::UTC);
    second.timezone = "Europe/London";
    second.masterEnabled = true;
    second.stopHeld = true;
    server.updateStatus(second);

    QJsonObject secondBody;
    for(int attempt = 0; attempt < 20; attempt++) {
        QNetworkReply* reply = getJson(manager, server.boundPort(), "/admin/status");
        secondBody = QJsonDocument::fromJson(reply->readAll()).object();
        if(secondBody.value("runningZone").toInt() == 7) {
            break;
        }
    }

    QCOMPARE(secondBody.value("runningZone").toInt(), 7);
    QCOMPARE(secondBody.value("secondsRemaining").toInt(), 42);
    QCOMPARE(secondBody.value("nextRunUtc").toString(), second.nextRunUtc.toUTC().toString(Qt::ISODate));
    QCOMPARE(secondBody.value("rainDelayUntilUtc").toString(), second.rainDelayUntilUtc.toUTC().toString(Qt::ISODate));
    QCOMPARE(secondBody.value("timezone").toString(), QString("Europe/London"));
    QCOMPARE(secondBody.value("masterEnabled").toBool(), true);
    QCOMPARE(secondBody.value("stopHeld").toBool(), true);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::zonesGetListsAllEightSeededZones()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = getJson(manager, server.boundPort(), "/admin/zones");
    QCOMPARE(statusCode(reply), 200);

    const QJsonArray array = QJsonDocument::fromJson(reply->readAll()).array();
    QCOMPARE(array.count(), 8);
    for(int i = 0; i < array.count(); i++) {
        const QJsonObject zone = array.at(i).toObject();
        QCOMPARE(zone.value("id").toInt(), i + 1);
        QCOMPARE(zone.value("number").toInt(), i + 1);
        QCOMPARE(zone.value("name").toString(), QString("Zone %1").arg(i + 1));
        QCOMPARE(zone.value("enabled").toBool(), true);
    }

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::zonePutUpdatesTheZoneWhoseNumberMatches()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");
    seedRenumberedZone(dbPath, 99, true);

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = putJson(manager, server.boundPort(), "/admin/zones/99",
                                   R"({"name": "Renumbered", "enabled": false})");
    QCOMPARE(statusCode(reply), 200);

    const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
    QCOMPARE(body.value("id").toInt(), 1);
    QCOMPARE(body.value("number").toInt(), 99);
    QCOMPARE(body.value("name").toString(), QString("Renumbered"));
    QCOMPARE(body.value("enabled").toBool(), false);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));

    IrrigationDataSource verify(dbPath);
    QVERIFY(verify.open());
    const ZoneList zones = verify.allZones();
    bool foundUpdated = false;
    bool foundNeighbor = false;
    for(const Zone& zone : zones) {
        if(zone.id == 1) {
            foundUpdated = true;
            QCOMPARE(zone.number, 99);
            QCOMPARE(zone.name, QString("Renumbered"));
            QCOMPARE(zone.enabled, false);
        }
        if(zone.id == 2) {
            foundNeighbor = true;
            QCOMPARE(zone.number, 2);
            QCOMPARE(zone.name, QString("Zone 2"));
            QCOMPARE(zone.enabled, true);
        }
    }
    QVERIFY(foundUpdated);
    QVERIFY(foundNeighbor);
}

void TestControlServer::zonePutUnknownNumberReturns404()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = putJson(manager, server.boundPort(), "/admin/zones/999",
                                   R"({"name": "Nobody", "enabled": true})");
    QCOMPARE(statusCode(reply), 404);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::zonePutMalformedBodyReturns400()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = putJson(manager, server.boundPort(), "/admin/zones/1", "not json");
    QCOMPARE(statusCode(reply), 400);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::zonePutMissingOrInvalidFieldReturns400_data()
{
    QTest::addColumn<QByteArray>("body");

    QTest::newRow("enabled missing") << QByteArray(R"({"name": "Missing Enabled"})");
    QTest::newRow("name missing") << QByteArray(R"({"enabled": true})");
    QTest::newRow("enabled as a JSON string") << QByteArray(R"({"name": "Stringly Enabled", "enabled": "false"})");
}

void TestControlServer::zonePutMissingOrInvalidFieldReturns400()
{
    QFETCH(QByteArray, body);

    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = putJson(manager, server.boundPort(), "/admin/zones/1", body);
    QCOMPARE(statusCode(reply), 400);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::zoneRunEmitsManualZoneRunRequestedWithTheExactZoneAndSeconds()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");
    seedRenumberedZone(dbPath, 99, true);

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QSignalSpy spy(&server, &IrrigationControlServer::manualZoneRunRequested);

    QNetworkAccessManager manager;
    // seconds, the zone number and the zone's id must all stay numerically distinct.
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/zones/99/run", R"({"seconds": 77})");
    QCOMPARE(statusCode(reply), 202);

    if(spy.count() == 0) {
        QVERIFY(spy.wait(5000));
    }
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toInt(), 99);
    QCOMPARE(spy.first().at(1).toInt(), 77);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::zoneRunUnknownZoneReturns404AndEmitsNothing()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");
    seedRenumberedZone(dbPath, 99, true);

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QSignalSpy spy(&server, &IrrigationControlServer::manualZoneRunRequested);

    QNetworkAccessManager manager;
    // Number 1 belongs to no zone once id 1 is renumbered to 99, but id 1 still exists:
    // a lookup keyed on id would find it.
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/zones/1/run", R"({"seconds": 60})");
    QCOMPARE(statusCode(reply), 404);

    spy.wait(200);
    QCOMPARE(spy.count(), 0);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::zoneRunMalformedBodyReturns400AndEmitsNothing()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");
    seedRenumberedZone(dbPath, 99, true);

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QSignalSpy spy(&server, &IrrigationControlServer::manualZoneRunRequested);

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/zones/99/run", "not json");
    QCOMPARE(statusCode(reply), 400);

    spy.wait(200);
    QCOMPARE(spy.count(), 0);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::zoneRunSecondsBoundaryAcceptsOneRejectsZero()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");
    seedRenumberedZone(dbPath, 99, true);

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QSignalSpy spy(&server, &IrrigationControlServer::manualZoneRunRequested);
    QNetworkAccessManager manager;

    QNetworkReply* zero = postJson(manager, server.boundPort(), "/admin/zones/99/run", R"({"seconds": 0})");
    QCOMPARE(statusCode(zero), 400);
    spy.wait(200);
    QCOMPARE(spy.count(), 0);

    QNetworkReply* one = postJson(manager, server.boundPort(), "/admin/zones/99/run", R"({"seconds": 1})");
    QCOMPARE(statusCode(one), 202);
    if(spy.count() == 0) {
        QVERIFY(spy.wait(5000));
    }
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toInt(), 99);
    QCOMPARE(spy.first().at(1).toInt(), 1);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::zoneRunRejectsAMissingOrNonNumericSeconds_data()
{
    QTest::addColumn<QByteArray>("body");

    QTest::newRow("seconds absent") << QByteArray(R"({})");
    QTest::newRow("seconds as a JSON string") << QByteArray(R"({"seconds": "77"})");
}

void TestControlServer::zoneRunRejectsAMissingOrNonNumericSeconds()
{
    QFETCH(QByteArray, body);

    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");
    seedRenumberedZone(dbPath, 99, true);

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QSignalSpy spy(&server, &IrrigationControlServer::manualZoneRunRequested);
    QNetworkAccessManager manager;

    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/zones/99/run", body);
    QCOMPARE(statusCode(reply), 400);

    spy.wait(200);
    QCOMPARE(spy.count(), 0);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::zoneRunDisabledZoneReturns409AndEmitsNothing()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");
    seedRenumberedZone(dbPath, 99, false);

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QSignalSpy spy(&server, &IrrigationControlServer::manualZoneRunRequested);

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/zones/99/run", R"({"seconds": 30})");
    QCOMPARE(statusCode(reply), 409);

    spy.wait(200);
    QCOMPARE(spy.count(), 0);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::stopEmitsStopRequestedExactlyOnce()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QSignalSpy spy(&server, &IrrigationControlServer::stopRequested);

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/stop", QByteArray());
    QCOMPARE(statusCode(reply), 202);

    if(spy.count() == 0) {
        QVERIFY(spy.wait(5000));
    }
    QCOMPARE(spy.count(), 1);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::settingsGetReturnsExactlyTheFourAllowlistedKeys()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = getJson(manager, server.boundPort(), "/admin/settings");
    QCOMPARE(statusCode(reply), 200);

    const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
    QStringList keys = body.keys();
    std::sort(keys.begin(), keys.end());

    const QStringList expected = { "log_level", "master_enabled", "max_zone_seconds", "rain_delay_until" };
    QCOMPARE(keys, expected);

    QCOMPARE(body.value("master_enabled").toString(), QString("1"));
    QCOMPARE(body.value("max_zone_seconds").toString(), QString("3600"));
    QCOMPARE(body.value("log_level").toString(), QString("info"));
    QCOMPARE(body.value("rain_delay_until").toString(), QString(""));

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::settingsPutRejectsInvalidValue_data()
{
    QTest::addColumn<QString>("key");
    QTest::addColumn<QString>("value");

    QTest::newRow("unknown key is not allowlisted") << QString("nonsense_key") << QString("1");
    QTest::newRow("master_enabled above the accepted range") << QString("master_enabled") << QString("2");
    QTest::newRow("master_enabled below the accepted range") << QString("master_enabled") << QString("-1");
    QTest::newRow("master_enabled as a word") << QString("master_enabled") << QString("true");
    QTest::newRow("rain_delay_until unparsable") << QString("rain_delay_until") << QString("not-a-date");
    QTest::newRow("max_zone_seconds at the zero boundary") << QString("max_zone_seconds") << QString("0");
    QTest::newRow("max_zone_seconds negative") << QString("max_zone_seconds") << QString("-5");
    QTest::newRow("max_zone_seconds non-numeric") << QString("max_zone_seconds") << QString("abc");
    QTest::newRow("log_level unknown name") << QString("log_level") << QString("Verbose");
    QTest::newRow("master_enabled zero-padded") << QString("master_enabled") << QString("00");
    QTest::newRow("master_enabled leading space") << QString("master_enabled") << QString(" 0");
    QTest::newRow("master_enabled trailing space") << QString("master_enabled") << QString("0 ");
}

void TestControlServer::settingsPutRejectsInvalidValue()
{
    QFETCH(QString, key);
    QFETCH(QString, value);

    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");
    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QSignalSpy spy(&server, &IrrigationControlServer::settingsChanged);
    QNetworkAccessManager manager;

    const QJsonObject requestBody{ { key, value } };
    QNetworkReply* reply = putJson(manager, server.boundPort(), "/admin/settings",
                                   QJsonDocument(requestBody).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 400);

    spy.wait(200);
    QCOMPARE(spy.count(), 0);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));

    IrrigationDataSource verify(dbPath);
    QVERIFY(verify.open());
    if(key == QString("master_enabled")) {
        QCOMPARE(verify.settingValue("master_enabled"), QString("1"));
    }
    else if(key == QString("rain_delay_until")) {
        QCOMPARE(verify.settingValue("rain_delay_until"), QString(""));
    }
    else if(key == QString("max_zone_seconds")) {
        QCOMPARE(verify.settingValue("max_zone_seconds"), QString("3600"));
    }
    else if(key == QString("log_level")) {
        QCOMPARE(verify.settingValue("log_level"), QString("info"));
    }
    else {
        QVERIFY(verify.settingValue(key).isEmpty());
    }
}

void TestControlServer::settingsPutRejectsAMultiKeyBodyWhenAnyKeyIsInvalid()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");
    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QSignalSpy spy(&server, &IrrigationControlServer::settingsChanged);
    QNetworkAccessManager manager;

    // QJsonObject iterates its keys in sorted order: "log_level" (valid on its own)
    // sorts before "master_enabled" (invalid).
    QNetworkReply* firstReply = putJson(manager, server.boundPort(), "/admin/settings",
                                        R"({"log_level": "debug", "master_enabled": "true"})");
    QCOMPARE(statusCode(firstReply), 400);

    // "master_enabled" (valid on its own, and a value that differs from the default)
    // sorts before the unknown key.
    QNetworkReply* secondReply = putJson(manager, server.boundPort(), "/admin/settings",
                                         R"({"master_enabled": "0", "zzz_unknown_key": "1"})");
    QCOMPARE(statusCode(secondReply), 400);

    // "rain_delay_until" (valid on its own) is first in SettingsKeys' own declared order,
    // ahead of "log_level" (invalid), though it sorts after "log_level" alphabetically.
    QNetworkReply* thirdReply = putJson(manager, server.boundPort(), "/admin/settings",
                                        R"({"rain_delay_until": "2026-10-05T08:00:00Z", "log_level": "Verbose"})");
    QCOMPARE(statusCode(thirdReply), 400);

    spy.wait(200);
    QCOMPARE(spy.count(), 0);

    QNetworkReply* getReply = getJson(manager, server.boundPort(), "/admin/settings");
    const QJsonObject body = QJsonDocument::fromJson(getReply->readAll()).object();
    QCOMPARE(body.value("log_level").toString(), QString("info"));
    QCOMPARE(body.value("master_enabled").toString(), QString("1"));
    QCOMPARE(body.value("rain_delay_until").toString(), QString(""));

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));

    IrrigationDataSource verify(dbPath);
    QVERIFY(verify.open());
    QCOMPARE(verify.settingValue("log_level"), QString("info"));
    QCOMPARE(verify.settingValue("master_enabled"), QString("1"));
    QCOMPARE(verify.settingValue("rain_delay_until"), QString(""));
}

void TestControlServer::settingsPutRejectsAMultiKeyBodyWhenALaterKeyIsNull()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");
    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QSignalSpy spy(&server, &IrrigationControlServer::settingsChanged);
    QNetworkAccessManager manager;

    // "log_level" (valid) sorts before "rain_delay_until" (a JSON null); a handler that
    // writes a key as soon as it validates would store "log_level" before ever reaching
    // the null, whatever status code the null value ends up answering with.
    QNetworkReply* reply = putJson(manager, server.boundPort(), "/admin/settings",
                                   R"({"log_level": "debug", "rain_delay_until": null})");
    QCOMPARE(statusCode(reply), 400);

    spy.wait(200);
    QCOMPARE(spy.count(), 0);

    QNetworkReply* getReply = getJson(manager, server.boundPort(), "/admin/settings");
    const QJsonObject body = QJsonDocument::fromJson(getReply->readAll()).object();
    QCOMPARE(body.value("log_level").toString(), QString("info"));
    QCOMPARE(body.value("rain_delay_until").toString(), QString(""));

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));

    IrrigationDataSource verify(dbPath);
    QVERIFY(verify.open());
    QCOMPARE(verify.settingValue("log_level"), QString("info"));
    QCOMPARE(verify.settingValue("rain_delay_until"), QString(""));
}

void TestControlServer::settingsPutRejectsNonStringValue_data()
{
    QTest::addColumn<QByteArray>("body");
    QTest::addColumn<QString>("key");
    QTest::addColumn<QString>("expectedUnchangedValue");

    QTest::newRow("rain_delay_until as a JSON null")
        << QByteArray(R"({"rain_delay_until": null})") << QString("rain_delay_until") << QString("");
    QTest::newRow("rain_delay_until as a JSON number")
        << QByteArray(R"({"rain_delay_until": 12345})") << QString("rain_delay_until") << QString("");
    QTest::newRow("max_zone_seconds as a JSON number")
        << QByteArray(R"({"max_zone_seconds": 1800})") << QString("max_zone_seconds") << QString("3600");
    QTest::newRow("master_enabled as a JSON boolean")
        << QByteArray(R"({"master_enabled": true})") << QString("master_enabled") << QString("1");
}

void TestControlServer::settingsPutRejectsNonStringValue()
{
    QFETCH(QByteArray, body);
    QFETCH(QString, key);
    QFETCH(QString, expectedUnchangedValue);

    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");
    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QSignalSpy spy(&server, &IrrigationControlServer::settingsChanged);
    QNetworkAccessManager manager;

    QNetworkReply* reply = putJson(manager, server.boundPort(), "/admin/settings", body);
    QCOMPARE(statusCode(reply), 400);

    spy.wait(200);
    QCOMPARE(spy.count(), 0);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));

    IrrigationDataSource verify(dbPath);
    QVERIFY(verify.open());
    QCOMPARE(verify.settingValue(key), expectedUnchangedValue);
}

void TestControlServer::settingsPutMalformedBodyReturns400_data()
{
    QTest::addColumn<QByteArray>("body");

    QTest::newRow("not JSON") << QByteArray("not json");
    QTest::newRow("a JSON array body") << QByteArray("[]");
}

void TestControlServer::settingsPutMalformedBodyReturns400()
{
    QFETCH(QByteArray, body);

    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QSignalSpy spy(&server, &IrrigationControlServer::settingsChanged);
    QNetworkAccessManager manager;

    QNetworkReply* reply = putJson(manager, server.boundPort(), "/admin/settings", body);
    QCOMPARE(statusCode(reply), 400);

    spy.wait(200);
    QCOMPARE(spy.count(), 0);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::settingsPutAcceptsValidValueAtBothEdges_data()
{
    QTest::addColumn<QString>("key");
    QTest::addColumn<QString>("value");

    QTest::newRow("master_enabled zero") << QString("master_enabled") << QString("0");
    QTest::newRow("master_enabled one") << QString("master_enabled") << QString("1");
    QTest::newRow("rain_delay_until empty clears the delay") << QString("rain_delay_until") << QString("");
    QTest::newRow("rain_delay_until ISO-8601") << QString("rain_delay_until") << QString("2026-09-20T00:00:00Z");
    QTest::newRow("max_zone_seconds at the minimum positive value") << QString("max_zone_seconds") << QString("1");
    QTest::newRow("max_zone_seconds well above the minimum") << QString("max_zone_seconds") << QString("7200");
    QTest::newRow("log_level mixed case") << QString("log_level") << QString("Info");
    QTest::newRow("log_level lower case") << QString("log_level") << QString("debug");
}

void TestControlServer::settingsPutAcceptsValidValueAtBothEdges()
{
    QFETCH(QString, key);
    QFETCH(QString, value);

    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    const QJsonObject requestBody{ { key, value } };
    QNetworkReply* putReply = putJson(manager, server.boundPort(), "/admin/settings",
                                      QJsonDocument(requestBody).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(putReply), 200);

    QNetworkReply* getReply = getJson(manager, server.boundPort(), "/admin/settings");
    const QJsonObject body = QJsonDocument::fromJson(getReply->readAll()).object();
    QCOMPARE(body.value(key).toString(), value);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::settingsPutSuccessReadBackThroughGetAndSeparateDataSourceEmitsSignalOnce()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");
    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QSignalSpy spy(&server, &IrrigationControlServer::settingsChanged);
    QNetworkAccessManager manager;

    // Qt::DirectConnection invokes this lambda synchronously on the server's worker
    // thread, at the point settingsChanged is emitted; the connection it opens must
    // be its own, since a QSqlDatabase connection is bound to the thread that opened it.
    QString maxZoneSecondsAtEmit;
    connect(&server, &IrrigationControlServer::settingsChanged, &server, [&maxZoneSecondsAtEmit, dbPath]()
    {
        IrrigationDataSource reader(dbPath);
        if(reader.open() == true) {
            maxZoneSecondsAtEmit = reader.settingValue("max_zone_seconds");
        }
    }, Qt::DirectConnection);

    // Four keys, four distinct values, none matching the schema defaults or each other.
    const QJsonObject requestBody{
        { "master_enabled", "0" },
        { "rain_delay_until", "2026-10-05T08:00:00Z" },
        { "max_zone_seconds", "1800" },
        { "log_level", "Debug" }
    };
    QNetworkReply* putReply = putJson(manager, server.boundPort(), "/admin/settings",
                                      QJsonDocument(requestBody).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(putReply), 200);

    if(spy.count() == 0) {
        QVERIFY(spy.wait(5000));
    }
    QCOMPARE(spy.count(), 1);
    QCOMPARE(maxZoneSecondsAtEmit, QString("1800"));

    QNetworkReply* getReply = getJson(manager, server.boundPort(), "/admin/settings");
    const QJsonObject body = QJsonDocument::fromJson(getReply->readAll()).object();
    QCOMPARE(body.value("master_enabled").toString(), QString("0"));
    QCOMPARE(body.value("rain_delay_until").toString(), QString("2026-10-05T08:00:00Z"));
    QCOMPARE(body.value("max_zone_seconds").toString(), QString("1800"));
    QCOMPARE(body.value("log_level").toString(), QString("Debug"));

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));

    IrrigationDataSource verify(dbPath);
    QVERIFY(verify.open());
    QCOMPARE(verify.settingValue("master_enabled"), QString("0"));
    QCOMPARE(verify.settingValue("rain_delay_until"), QString("2026-10-05T08:00:00Z"));
    QCOMPARE(verify.settingValue("max_zone_seconds"), QString("1800"));
    QCOMPARE(verify.settingValue("log_level"), QString("Debug"));
}

void TestControlServer::programsGetReturnsFieldsStartTimesZonesAndPerProgramNextRunUtc()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");

    // The expected value must not come from Scheduler::nextRunUtc. An anchor 30 days out
    // stays inside the 366-day horizon and ahead of "today" at any point this case runs.
    const QDate anchor = QDateTime::currentDateTimeUtc().date().addDays(30);

    Program program;
    program.name = "Front Lawn";
    program.enabled = true;
    program.dayMode = Program::DayMode::EveryNDays;
    program.dowMask = 0;
    program.intervalDays = 1;
    program.anchorDate = anchor;

    // Inserted out of time order: the later time must keep the lower id.
    ProgramStartTime late;
    late.minutesAfterMidnight = 1000;
    late.timezone = "UTC";
    ProgramStartTime early;
    early.minutesAfterMidnight = 360;
    early.timezone = "UTC";
    QList<ProgramStartTime> startTimes{ late, early };

    ProgramZone firstZone;
    firstZone.zoneId = 2;
    firstZone.sequence = 0;
    firstZone.durationSeconds = 333;
    ProgramZone secondZone;
    secondZone.zoneId = 7;
    secondZone.sequence = 1;
    secondZone.durationSeconds = 555;
    QList<ProgramZone> zones{ firstZone, secondZone };

    QVERIFY(seedProgramDirect(dbPath, program, startTimes, zones));

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = getJson(manager, server.boundPort(), "/admin/programs");
    QCOMPARE(statusCode(reply), 200);

    const QJsonArray array = QJsonDocument::fromJson(reply->readAll()).array();
    QCOMPARE(array.count(), 1);
    const QJsonObject body = array.at(0).toObject();

    QCOMPARE(body.value("id").toInt(), program.id);
    QCOMPARE(body.value("name").toString(), QString("Front Lawn"));
    QCOMPARE(body.value("enabled").toBool(), true);
    QCOMPARE(body.value("dayMode").toString(), QString("EveryNDays"));
    QCOMPARE(body.value("dowMask").toInt(), 0);
    QCOMPARE(body.value("intervalDays").toInt(), 1);
    QCOMPARE(body.value("anchorDate").toString(), anchor.toString(Qt::ISODate));

    // startTimes[0] is "late" (the lower id), startTimes[1] is "early" (the higher id) --
    // the id order the server reports them in is the opposite of watering time order.
    const QJsonArray startTimesArray = body.value("startTimes").toArray();
    QCOMPARE(startTimesArray.count(), 2);
    QCOMPARE(startTimesArray.at(0).toObject().value("id").toInt(), startTimes.at(0).id);
    QCOMPARE(startTimesArray.at(0).toObject().value("minutesAfterMidnight").toInt(), 1000);
    QCOMPARE(startTimesArray.at(0).toObject().value("timezone").toString(), QString("UTC"));
    QCOMPARE(startTimesArray.at(1).toObject().value("id").toInt(), startTimes.at(1).id);
    QCOMPARE(startTimesArray.at(1).toObject().value("minutesAfterMidnight").toInt(), 360);

    const QJsonArray zonesArray = body.value("zones").toArray();
    QCOMPARE(zonesArray.count(), 2);
    QCOMPARE(zonesArray.at(0).toObject().value("id").toInt(), zones.at(0).id);
    QCOMPARE(zonesArray.at(0).toObject().value("zoneId").toInt(), 2);
    QCOMPARE(zonesArray.at(0).toObject().value("sequence").toInt(), 0);
    QCOMPARE(zonesArray.at(0).toObject().value("durationSeconds").toInt(), 333);
    QCOMPARE(zonesArray.at(1).toObject().value("id").toInt(), zones.at(1).id);
    QCOMPARE(zonesArray.at(1).toObject().value("zoneId").toInt(), 7);
    QCOMPARE(zonesArray.at(1).toObject().value("sequence").toInt(), 1);
    QCOMPARE(zonesArray.at(1).toObject().value("durationSeconds").toInt(), 555);

    // The earliest occurrence is "early" (06:00 UTC) on the anchor date, regardless of its
    // higher id and later position in startTimesArray.
    const QString expectedNextRunUtc = anchor.toString(Qt::ISODate) + QString("T06:00:00Z");
    QCOMPARE(body.value("nextRunUtc").toString(), expectedNextRunUtc);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::programPostCreatesStoredProgramReadBackThroughGetAndSeparateConnection()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");

    // A filler program with two start times and three zone entries advances the three
    // independent autoincrement counters by different amounts, so the real program's id,
    // start-time id and zone-entry id below cannot coincide.
    {
        Program filler;
        filler.name = "Filler";
        filler.dayMode = Program::DayMode::DaysOfWeek;
        filler.dowMask = 1;
        ProgramStartTime fillerA; fillerA.minutesAfterMidnight = 100; fillerA.timezone = "UTC";
        ProgramStartTime fillerB; fillerB.minutesAfterMidnight = 200; fillerB.timezone = "UTC";
        QList<ProgramStartTime> fillerStartTimes{ fillerA, fillerB };
        ProgramZone fillerZoneA; fillerZoneA.zoneId = 1; fillerZoneA.sequence = 0; fillerZoneA.durationSeconds = 10;
        ProgramZone fillerZoneB; fillerZoneB.zoneId = 2; fillerZoneB.sequence = 1; fillerZoneB.durationSeconds = 10;
        ProgramZone fillerZoneC; fillerZoneC.zoneId = 3; fillerZoneC.sequence = 2; fillerZoneC.durationSeconds = 10;
        QList<ProgramZone> fillerZones{ fillerZoneA, fillerZoneB, fillerZoneC };
        QVERIFY(seedProgramDirect(dbPath, filler, fillerStartTimes, fillerZones));
    }

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    const QJsonObject requestBody{
        { "name", "Backyard" },
        { "enabled", false },
        { "dayMode", "DaysOfWeek" },
        { "dowMask", 21 },
        { "intervalDays", 0 },
        { "anchorDate", "" },
        { "startTimes", QJsonArray{ QJsonObject{
            { "minutesAfterMidnight", 725 }, { "timezone", "America/Denver" }
        } } },
        { "zones", QJsonArray{ QJsonObject{
            { "zoneId", 5 }, { "sequence", 2 }, { "durationSeconds", 417 }
        } } }
    };

    QNetworkAccessManager manager;
    QNetworkReply* postReply = postJson(manager, server.boundPort(), "/admin/programs",
                                        QJsonDocument(requestBody).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(postReply), 201);

    const QJsonObject postBody = QJsonDocument::fromJson(postReply->readAll()).object();
    QVERIFY(postBody.value("id").toInt() > 0);
    QCOMPARE(postBody.value("name").toString(), QString("Backyard"));
    QCOMPARE(postBody.value("enabled").toBool(), false);
    QCOMPARE(postBody.value("dayMode").toString(), QString("DaysOfWeek"));
    QCOMPARE(postBody.value("dowMask").toInt(), 21);
    QCOMPARE(postBody.value("intervalDays").toInt(), 0);
    QCOMPARE(postBody.value("anchorDate").toString(), QString(""));

    const QJsonArray postStartTimes = postBody.value("startTimes").toArray();
    QCOMPARE(postStartTimes.count(), 1);
    const int startTimeId = postStartTimes.at(0).toObject().value("id").toInt();
    QVERIFY(startTimeId > 0);
    QCOMPARE(postStartTimes.at(0).toObject().value("minutesAfterMidnight").toInt(), 725);
    QCOMPARE(postStartTimes.at(0).toObject().value("timezone").toString(), QString("America/Denver"));

    const QJsonArray postZones = postBody.value("zones").toArray();
    QCOMPARE(postZones.count(), 1);
    const int zoneEntryId = postZones.at(0).toObject().value("id").toInt();
    QVERIFY(zoneEntryId > 0);
    QCOMPARE(postZones.at(0).toObject().value("zoneId").toInt(), 5);
    QCOMPARE(postZones.at(0).toObject().value("sequence").toInt(), 2);
    QCOMPARE(postZones.at(0).toObject().value("durationSeconds").toInt(), 417);

    const int programId = postBody.value("id").toInt();
    QVERIFY(programId != startTimeId);
    QVERIFY(programId != zoneEntryId);
    QVERIFY(startTimeId != zoneEntryId);

    QNetworkReply* getReply = getJson(manager, server.boundPort(), "/admin/programs");
    const QJsonArray getArray = QJsonDocument::fromJson(getReply->readAll()).array();
    QCOMPARE(getArray.count(), 2);
    QJsonObject getBody;
    for(const QJsonValue& value : getArray) {
        if(value.toObject().value("id").toInt() == programId) {
            getBody = value.toObject();
        }
    }
    QCOMPARE(getBody.value("id").toInt(), programId);
    QCOMPARE(getBody.value("name").toString(), QString("Backyard"));
    QCOMPARE(getBody.value("enabled").toBool(), false);
    QCOMPARE(getBody.value("dayMode").toString(), QString("DaysOfWeek"));
    QCOMPARE(getBody.value("dowMask").toInt(), 21);
    QCOMPARE(getBody.value("startTimes").toArray().count(), 1);
    QCOMPARE(getBody.value("startTimes").toArray().at(0).toObject().value("id").toInt(), startTimeId);
    QCOMPARE(getBody.value("zones").toArray().count(), 1);
    QCOMPARE(getBody.value("zones").toArray().at(0).toObject().value("id").toInt(), zoneEntryId);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));

    IrrigationDataSource verify(dbPath);
    QVERIFY(verify.open());
    const ProgramList programs = verify.allPrograms();
    QCOMPARE(programs.count(), 2);
    Program stored;
    for(const Program& candidate : programs) {
        if(candidate.id == programId) {
            stored = candidate;
        }
    }
    QCOMPARE(stored.id, programId);
    QCOMPARE(stored.name, QString("Backyard"));
    QCOMPARE(stored.enabled, false);
    QCOMPARE(stored.dayMode, Program::DayMode::DaysOfWeek);
    QCOMPARE(stored.dowMask, 21);

    const ProgramStartTimeList verifyStartTimes = verify.startTimesFor(programId);
    QCOMPARE(verifyStartTimes.count(), 1);
    QCOMPARE(verifyStartTimes.at(0).id, startTimeId);
    QCOMPARE(verifyStartTimes.at(0).minutesAfterMidnight, 725);
    QCOMPARE(verifyStartTimes.at(0).timezone, QString("America/Denver"));

    const ProgramZoneList verifyZones = verify.zonesFor(programId);
    QCOMPARE(verifyZones.count(), 1);
    QCOMPARE(verifyZones.at(0).id, zoneEntryId);
    QCOMPARE(verifyZones.at(0).zoneId, 5);
    QCOMPARE(verifyZones.at(0).sequence, 2);
    QCOMPARE(verifyZones.at(0).durationSeconds, 417);
}

void TestControlServer::programPostValidationRejectsAndWritesNothing_data()
{
    QTest::addColumn<QByteArray>("body");
    const auto rows = programValidationRows();
    for(const auto& row : rows) {
        QTest::newRow(row.first.toUtf8().constData()) << row.second;
    }
}

void TestControlServer::programPostValidationRejectsAndWritesNothing()
{
    QFETCH(QByteArray, body);

    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/programs", body);
    QCOMPARE(statusCode(reply), 400);

    QNetworkReply* getReply = getJson(manager, server.boundPort(), "/admin/programs");
    const QJsonArray array = QJsonDocument::fromJson(getReply->readAll()).array();
    QCOMPARE(array.count(), 0);

    // The second write must follow the rejection on this same server.
    QNetworkReply* secondReply = postJson(manager, server.boundPort(), "/admin/programs",
                                          QJsonDocument(baseValidProgramBody()).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(secondReply), 201);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::programPostAcceptsBoundaryValuesForMinutesAfterMidnightAndDurationSeconds_data()
{
    QTest::addColumn<QString>("scope");
    QTest::addColumn<QString>("field");
    QTest::addColumn<int>("value");

    QTest::newRow("minutesAfterMidnight zero") << QString("startTime") << QString("minutesAfterMidnight") << 0;
    QTest::newRow("minutesAfterMidnight 1439") << QString("startTime") << QString("minutesAfterMidnight") << 1439;
    QTest::newRow("durationSeconds one") << QString("zone") << QString("durationSeconds") << 1;
}

void TestControlServer::programPostAcceptsBoundaryValuesForMinutesAfterMidnightAndDurationSeconds()
{
    QFETCH(QString, scope);
    QFETCH(QString, field);
    QFETCH(int, value);

    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    const QJsonObject body = withProgramField(baseValidProgramBody(), scope, field, value);

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/programs",
                                    QJsonDocument(body).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 201);

    const QJsonObject responseBody = QJsonDocument::fromJson(reply->readAll()).object();
    if(scope == QString("startTime")) {
        QCOMPARE(responseBody.value("startTimes").toArray().at(0).toObject().value(field).toInt(), value);
    }
    else {
        QCOMPARE(responseBody.value("zones").toArray().at(0).toObject().value(field).toInt(), value);
    }

    QNetworkReply* getReply = getJson(manager, server.boundPort(), "/admin/programs");
    const QJsonArray array = QJsonDocument::fromJson(getReply->readAll()).array();
    QCOMPARE(array.count(), 1);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::programPostMalformedBodyReturns400AndWritesNothing()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/programs", "not json");
    QCOMPARE(statusCode(reply), 400);

    QNetworkReply* getReply = getJson(manager, server.boundPort(), "/admin/programs");
    const QJsonArray array = QJsonDocument::fromJson(getReply->readAll()).array();
    QCOMPARE(array.count(), 0);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::programPostAcceptsOddDayMode()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");
    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/programs",
                                    QJsonDocument(withProgramField(baseValidProgramBody(), "root", "dayMode", "Odd"))
                                        .toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 201);
    const QJsonObject postBody = QJsonDocument::fromJson(reply->readAll()).object();
    const int programId = postBody.value("id").toInt();
    QCOMPARE(postBody.value("dayMode").toString(), QString("Odd"));

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));

    IrrigationDataSource verify(dbPath);
    QVERIFY(verify.open());
    const ProgramList programs = verify.allPrograms();
    QCOMPARE(programs.count(), 1);
    QCOMPARE(programs.at(0).id, programId);
    QCOMPARE(programs.at(0).dayMode, Program::DayMode::Odd);
}

void TestControlServer::programPostAcceptsEvenDayMode()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");
    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/programs",
                                    QJsonDocument(withProgramField(baseValidProgramBody(), "root", "dayMode", "Even"))
                                        .toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 201);
    const QJsonObject postBody = QJsonDocument::fromJson(reply->readAll()).object();
    const int programId = postBody.value("id").toInt();
    QCOMPARE(postBody.value("dayMode").toString(), QString("Even"));

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));

    IrrigationDataSource verify(dbPath);
    QVERIFY(verify.open());
    const ProgramList programs = verify.allPrograms();
    QCOMPARE(programs.count(), 1);
    QCOMPARE(programs.at(0).id, programId);
    QCOMPARE(programs.at(0).dayMode, Program::DayMode::Even);
}

void TestControlServer::programPostAcceptsEveryNDaysWithIntervalOne()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");
    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/programs",
                                    QJsonDocument(everyNDaysBody(1, "2027-01-01")).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 201);

    const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
    const int programId = body.value("id").toInt();
    QCOMPARE(body.value("dayMode").toString(), QString("EveryNDays"));
    QCOMPARE(body.value("intervalDays").toInt(), 1);
    QCOMPARE(body.value("anchorDate").toString(), QString("2027-01-01"));

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));

    IrrigationDataSource verify(dbPath);
    QVERIFY(verify.open());
    const ProgramList programs = verify.allPrograms();
    QCOMPARE(programs.count(), 1);
    QCOMPARE(programs.at(0).id, programId);
    QCOMPARE(programs.at(0).dayMode, Program::DayMode::EveryNDays);
    QCOMPARE(programs.at(0).intervalDays, 1);
    QCOMPARE(programs.at(0).anchorDate, QDate(2027, 1, 1));
}

void TestControlServer::programPostAcceptsDaysOfWeekWithAllSevenDaysBit()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");
    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/programs",
                                    QJsonDocument(withProgramField(baseValidProgramBody(), "root", "dowMask", 127))
                                        .toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 201);
    const QJsonObject postBody = QJsonDocument::fromJson(reply->readAll()).object();
    const int programId = postBody.value("id").toInt();
    QCOMPARE(postBody.value("dowMask").toInt(), 127);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));

    IrrigationDataSource verify(dbPath);
    QVERIFY(verify.open());
    const ProgramList programs = verify.allPrograms();
    QCOMPARE(programs.count(), 1);
    QCOMPARE(programs.at(0).id, programId);
    QCOMPARE(programs.at(0).dowMask, 127);
}

void TestControlServer::programPostRejectsAnUnknownSecondZoneId()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    const QJsonObject body{
        { "name", "Two Zones" }, { "enabled", true }, { "dayMode", "DaysOfWeek" }, { "dowMask", 5 },
        { "intervalDays", 0 }, { "anchorDate", "" },
        { "startTimes", QJsonArray{ QJsonObject{ { "minutesAfterMidnight", 360 }, { "timezone", "UTC" } } } },
        { "zones", QJsonArray{
            QJsonObject{ { "zoneId", 2 }, { "sequence", 0 }, { "durationSeconds", 300 } },
            QJsonObject{ { "zoneId", 9999 }, { "sequence", 1 }, { "durationSeconds", 300 } }
        } }
    };

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/programs",
                                    QJsonDocument(body).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 400);

    QNetworkReply* getReply = getJson(manager, server.boundPort(), "/admin/programs");
    QCOMPARE(QJsonDocument::fromJson(getReply->readAll()).array().count(), 0);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::programPostAcceptsAZoneIdThatDiffersFromItsRenumberedZoneNumber()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");
    // Zone id 1 is renumbered to 99: no zone's NUMBER is 1, but zone id 1 still exists.
    seedRenumberedZone(dbPath, 99, true);

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/programs",
                                    QJsonDocument(withProgramField(baseValidProgramBody(), "zone", "zoneId", 1))
                                        .toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 201);
    QCOMPARE(QJsonDocument::fromJson(reply->readAll()).object()
                 .value("zones").toArray().at(0).toObject().value("zoneId").toInt(), 1);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::programPostAnswers500WhenCommitFailsWritingNothing()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");

    // A deferred foreign key, tripped from an AFTER INSERT trigger, makes COMMIT itself fail.
    {
        IrrigationDataSource trigger(dbPath);
        QVERIFY(trigger.open());
        bool ok = false;
        trigger.rawQuery(
            "CREATE TABLE commit_poison (program_ref INTEGER REFERENCES programs(id) "
            "DEFERRABLE INITIALLY DEFERRED)",
            &ok);
        QVERIFY(ok);
        trigger.rawQuery(
            "CREATE TRIGGER poison_commit AFTER INSERT ON program_zones "
            "WHEN NEW.duration_seconds = 777777 "
            "BEGIN INSERT INTO commit_poison VALUES (-1); END;",
            &ok);
        QVERIFY(ok);
    }

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    const QJsonObject body{
        { "name", "Commit Attempt" }, { "enabled", true }, { "dayMode", "DaysOfWeek" }, { "dowMask", 3 },
        { "intervalDays", 0 }, { "anchorDate", "" },
        { "startTimes", QJsonArray{ QJsonObject{ { "minutesAfterMidnight", 400 }, { "timezone", "UTC" } } } },
        { "zones", QJsonArray{ QJsonObject{ { "zoneId", 8 }, { "sequence", 0 }, { "durationSeconds", 777777 } } } }
    };
    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/programs",
                                    QJsonDocument(body).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 500);

    // Read back on the server's own connection while it is still running: a POST failure path
    // that skips rollbackTransaction() would serve the orphaned program row here, even though
    // a read after stop() would not.
    QNetworkReply* getReply = getJson(manager, server.boundPort(), "/admin/programs");
    QCOMPARE(QJsonDocument::fromJson(getReply->readAll()).array().count(), 0);

    {
        IrrigationDataSource verify(dbPath);
        QVERIFY(verify.open());
        QCOMPARE(verify.allPrograms().count(), 0);
    }

    // The connection must still accept a transaction: a left-open transaction answers 500 here.
    const QJsonObject validBody{
        { "name", "Good" }, { "enabled", true }, { "dayMode", "DaysOfWeek" }, { "dowMask", 3 },
        { "intervalDays", 0 }, { "anchorDate", "" },
        { "startTimes", QJsonArray{ QJsonObject{ { "minutesAfterMidnight", 400 }, { "timezone", "UTC" } } } },
        { "zones", QJsonArray{ QJsonObject{ { "zoneId", 8 }, { "sequence", 0 }, { "durationSeconds", 111 } } } }
    };
    QNetworkReply* secondReply = postJson(manager, server.boundPort(), "/admin/programs",
                                          QJsonDocument(validBody).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(secondReply), 201);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::programPutUnknownIdReturns404()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = putJson(manager, server.boundPort(), "/admin/programs/999",
                                   QJsonDocument(baseValidProgramBody()).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 404);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::programPutValidationRejectsAndPreservesEverything_data()
{
    QTest::addColumn<QByteArray>("body");
    const auto rows = programValidationRows();
    for(const auto& row : rows) {
        QTest::newRow(row.first.toUtf8().constData()) << row.second;
    }
}

void TestControlServer::programPutValidationRejectsAndPreservesEverything()
{
    QFETCH(QByteArray, body);

    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");

    Program program;
    program.name = "Original";
    program.enabled = true;
    program.dayMode = Program::DayMode::Odd;
    program.dowMask = 0;
    program.intervalDays = 0;

    ProgramStartTime startTime;
    startTime.minutesAfterMidnight = 450;
    startTime.timezone = "America/Chicago";
    QList<ProgramStartTime> startTimes{ startTime };

    ProgramZone zone;
    zone.zoneId = 6;
    zone.sequence = 0;
    zone.durationSeconds = 222;
    QList<ProgramZone> zones{ zone };

    QVERIFY(seedProgramDirect(dbPath, program, startTimes, zones));
    const int originalStartTimeId = startTimes.at(0).id;
    const int originalZoneEntryId = zones.at(0).id;

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = putJson(manager, server.boundPort(),
                                   QString("/admin/programs/%1").arg(program.id), body);
    QCOMPARE(statusCode(reply), 400);

    // Read back on the server's own connection while it is still running: a handler that
    // wrote before returning 400, without a rollback, would show the half-applied data here
    // even though a read after stop() would not (closing the connection rolls it back).
    QNetworkReply* getReply = getJson(manager, server.boundPort(), "/admin/programs");
    const QJsonObject got = QJsonDocument::fromJson(getReply->readAll()).array().at(0).toObject();
    QCOMPARE(got.value("name").toString(), QString("Original"));
    QCOMPARE(got.value("dayMode").toString(), QString("Odd"));
    QCOMPARE(got.value("startTimes").toArray().count(), 1);
    QCOMPARE(got.value("startTimes").toArray().at(0).toObject().value("id").toInt(), originalStartTimeId);
    QCOMPARE(got.value("zones").toArray().count(), 1);
    QCOMPARE(got.value("zones").toArray().at(0).toObject().value("id").toInt(), originalZoneEntryId);

    // Read back through a separate connection while the server is still running.
    {
        IrrigationDataSource verify(dbPath);
        QVERIFY(verify.open());
        const ProgramList programs = verify.allPrograms();
        QCOMPARE(programs.count(), 1);
        QCOMPARE(programs.at(0).id, program.id);
        QCOMPARE(programs.at(0).name, QString("Original"));
        QCOMPARE(programs.at(0).enabled, true);
        QCOMPARE(programs.at(0).dayMode, Program::DayMode::Odd);

        const ProgramStartTimeList verifyStartTimes = verify.startTimesFor(program.id);
        QCOMPARE(verifyStartTimes.count(), 1);
        QCOMPARE(verifyStartTimes.at(0).id, originalStartTimeId);
        QCOMPARE(verifyStartTimes.at(0).minutesAfterMidnight, 450);
        QCOMPARE(verifyStartTimes.at(0).timezone, QString("America/Chicago"));

        const ProgramZoneList verifyZones = verify.zonesFor(program.id);
        QCOMPARE(verifyZones.count(), 1);
        QCOMPARE(verifyZones.at(0).id, originalZoneEntryId);
        QCOMPARE(verifyZones.at(0).zoneId, 6);
        QCOMPARE(verifyZones.at(0).sequence, 0);
        QCOMPARE(verifyZones.at(0).durationSeconds, 222);
    }

    // The connection must still accept a transaction: a rejection that left one open
    // answers 500 here.
    QNetworkReply* secondReply = putJson(manager, server.boundPort(),
                                         QString("/admin/programs/%1").arg(program.id),
                                         QJsonDocument(baseValidProgramBody()).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(secondReply), 200);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));
}

void TestControlServer::programPutMalformedBodyReturns400AndPreservesEverything()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");

    Program program;
    program.name = "Malformed Guard";
    program.dayMode = Program::DayMode::Even;

    ProgramStartTime startTime;
    startTime.minutesAfterMidnight = 610;
    startTime.timezone = "UTC";
    QList<ProgramStartTime> startTimes{ startTime };

    ProgramZone zone;
    zone.zoneId = 5;
    zone.sequence = 0;
    zone.durationSeconds = 88;
    QList<ProgramZone> zones{ zone };

    QVERIFY(seedProgramDirect(dbPath, program, startTimes, zones));
    const int originalStartTimeId = startTimes.at(0).id;
    const int originalZoneEntryId = zones.at(0).id;

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = putJson(manager, server.boundPort(),
                                   QString("/admin/programs/%1").arg(program.id), "not json");
    QCOMPARE(statusCode(reply), 400);

    QNetworkReply* getReply = getJson(manager, server.boundPort(), "/admin/programs");
    const QJsonObject got = QJsonDocument::fromJson(getReply->readAll()).array().at(0).toObject();
    QCOMPARE(got.value("name").toString(), QString("Malformed Guard"));
    QCOMPARE(got.value("dayMode").toString(), QString("Even"));
    QCOMPARE(got.value("startTimes").toArray().at(0).toObject().value("id").toInt(), originalStartTimeId);
    QCOMPARE(got.value("zones").toArray().at(0).toObject().value("id").toInt(), originalZoneEntryId);

    {
        IrrigationDataSource verify(dbPath);
        QVERIFY(verify.open());
        const ProgramList programs = verify.allPrograms();
        QCOMPARE(programs.count(), 1);
        QCOMPARE(programs.at(0).name, QString("Malformed Guard"));
        QCOMPARE(programs.at(0).dayMode, Program::DayMode::Even);

        const ProgramStartTimeList verifyStartTimes = verify.startTimesFor(program.id);
        QCOMPARE(verifyStartTimes.count(), 1);
        QCOMPARE(verifyStartTimes.at(0).id, originalStartTimeId);

        const ProgramZoneList verifyZones = verify.zonesFor(program.id);
        QCOMPARE(verifyZones.count(), 1);
        QCOMPARE(verifyZones.at(0).id, originalZoneEntryId);
    }

    QNetworkReply* secondReply = putJson(manager, server.boundPort(),
                                         QString("/admin/programs/%1").arg(program.id),
                                         QJsonDocument(baseValidProgramBody()).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(secondReply), 200);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));
}

void TestControlServer::programPutWithUnchangedStartTimesPreservesTheirIds()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");

    Program program;
    program.name = "Identity";
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = 5;

    ProgramStartTime a; a.minutesAfterMidnight = 300; a.timezone = "UTC";
    ProgramStartTime b; b.minutesAfterMidnight = 500; b.timezone = "UTC";
    QList<ProgramStartTime> startTimes{ a, b };
    QList<ProgramZone> zones{};

    QVERIFY(seedProgramDirect(dbPath, program, startTimes, zones));
    const int idA = startTimes.at(0).id;
    const int idB = startTimes.at(1).id;
    QVERIFY(idA != idB);

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    const QJsonObject requestBody{
        { "name", "Identity" },
        { "enabled", true },
        { "dayMode", "DaysOfWeek" },
        { "dowMask", 5 },
        { "intervalDays", 0 },
        { "anchorDate", "" },
        { "startTimes", QJsonArray{
            QJsonObject{ { "minutesAfterMidnight", 300 }, { "timezone", "UTC" } },
            QJsonObject{ { "minutesAfterMidnight", 500 }, { "timezone", "UTC" } }
        } },
        { "zones", QJsonArray{} }
    };

    QNetworkAccessManager manager;
    QNetworkReply* reply = putJson(manager, server.boundPort(),
                                   QString("/admin/programs/%1").arg(program.id),
                                   QJsonDocument(requestBody).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 200);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));

    IrrigationDataSource verify(dbPath);
    QVERIFY(verify.open());
    const ProgramStartTimeList result = verify.startTimesFor(program.id);
    QCOMPARE(result.count(), 2);
    QCOMPARE(result.at(0).id, idA);
    QCOMPARE(result.at(0).minutesAfterMidnight, 300);
    QCOMPARE(result.at(1).id, idB);
    QCOMPARE(result.at(1).minutesAfterMidnight, 500);
}

void TestControlServer::programPutChangedStartTimesDeleteUnmatchedInsertUnmatched()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");

    Program program;
    program.name = "Reconcile";
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = 7;

    ProgramStartTime a; a.minutesAfterMidnight = 300; a.timezone = "UTC";
    ProgramStartTime b; b.minutesAfterMidnight = 500; b.timezone = "UTC";
    QList<ProgramStartTime> startTimes{ a, b };
    QList<ProgramZone> zones{};

    QVERIFY(seedProgramDirect(dbPath, program, startTimes, zones));
    const int idA = startTimes.at(0).id;
    const int idB = startTimes.at(1).id;

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    const QJsonObject requestBody{
        { "name", "Reconcile" },
        { "enabled", true },
        { "dayMode", "DaysOfWeek" },
        { "dowMask", 7 },
        { "intervalDays", 0 },
        { "anchorDate", "" },
        { "startTimes", QJsonArray{
            QJsonObject{ { "minutesAfterMidnight", 300 }, { "timezone", "UTC" } },
            QJsonObject{ { "minutesAfterMidnight", 700 }, { "timezone", "UTC" } }
        } },
        { "zones", QJsonArray{} }
    };

    QNetworkAccessManager manager;
    QNetworkReply* reply = putJson(manager, server.boundPort(),
                                   QString("/admin/programs/%1").arg(program.id),
                                   QJsonDocument(requestBody).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 200);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));

    IrrigationDataSource verify(dbPath);
    QVERIFY(verify.open());
    const ProgramStartTimeList result = verify.startTimesFor(program.id);
    QCOMPARE(result.count(), 2);
    QVERIFY(containsStartTimeId(result, idA));
    QVERIFY(containsStartTimeId(result, idB) == false);

    for(const ProgramStartTime& item : result) {
        if(item.id == idA) {
            QCOMPARE(item.minutesAfterMidnight, 300);
        }
        else {
            QCOMPARE(item.minutesAfterMidnight, 700);
            QVERIFY(item.id != idA);
            QVERIFY(item.id != idB);
        }
    }
}

void TestControlServer::programPutDuplicateStartTimesReconcileAsAMultiset()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");

    Program program;
    program.name = "Multiset";
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = 4;

    ProgramStartTime d; d.minutesAfterMidnight = 900; d.timezone = "UTC";
    QList<ProgramStartTime> startTimes{ d };
    QList<ProgramZone> zones{};

    QVERIFY(seedProgramDirect(dbPath, program, startTimes, zones));
    const int idD = startTimes.at(0).id;

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    const QJsonObject requestBody{
        { "name", "Multiset" },
        { "enabled", true },
        { "dayMode", "DaysOfWeek" },
        { "dowMask", 4 },
        { "intervalDays", 0 },
        { "anchorDate", "" },
        { "startTimes", QJsonArray{
            QJsonObject{ { "minutesAfterMidnight", 900 }, { "timezone", "UTC" } },
            QJsonObject{ { "minutesAfterMidnight", 900 }, { "timezone", "UTC" } }
        } },
        { "zones", QJsonArray{} }
    };

    QNetworkAccessManager manager;
    QNetworkReply* reply = putJson(manager, server.boundPort(),
                                   QString("/admin/programs/%1").arg(program.id),
                                   QJsonDocument(requestBody).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 200);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));

    IrrigationDataSource verify(dbPath);
    QVERIFY(verify.open());
    const ProgramStartTimeList result = verify.startTimesFor(program.id);
    QCOMPARE(result.count(), 2);
    QVERIFY(containsStartTimeId(result, idD));
    QCOMPARE(result.at(0).minutesAfterMidnight, 900);
    QCOMPARE(result.at(1).minutesAfterMidnight, 900);
    QVERIFY(result.at(0).id != result.at(1).id);
}

void TestControlServer::programPutReorderedIdenticalStartTimesKeepsIds()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");

    Program program;
    program.name = "Reorder";
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = 6;

    ProgramStartTime e; e.minutesAfterMidnight = 200; e.timezone = "UTC";
    ProgramStartTime f; f.minutesAfterMidnight = 800; f.timezone = "UTC";
    QList<ProgramStartTime> startTimes{ e, f };
    QList<ProgramZone> zones{};

    QVERIFY(seedProgramDirect(dbPath, program, startTimes, zones));
    const int idE = startTimes.at(0).id;
    const int idF = startTimes.at(1).id;

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    // The wire order stays reversed from id order: f before e.
    const QJsonObject requestBody{
        { "name", "Reorder" },
        { "enabled", true },
        { "dayMode", "DaysOfWeek" },
        { "dowMask", 6 },
        { "intervalDays", 0 },
        { "anchorDate", "" },
        { "startTimes", QJsonArray{
            QJsonObject{ { "minutesAfterMidnight", 800 }, { "timezone", "UTC" } },
            QJsonObject{ { "minutesAfterMidnight", 200 }, { "timezone", "UTC" } }
        } },
        { "zones", QJsonArray{} }
    };

    QNetworkAccessManager manager;
    QNetworkReply* reply = putJson(manager, server.boundPort(),
                                   QString("/admin/programs/%1").arg(program.id),
                                   QJsonDocument(requestBody).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 200);

    // The response lists start times in id order, the opposite of the wire order sent above.
    const QJsonArray responseStartTimes = QJsonDocument::fromJson(reply->readAll()).object()
                                               .value("startTimes").toArray();
    QCOMPARE(responseStartTimes.count(), 2);
    QCOMPARE(responseStartTimes.at(0).toObject().value("id").toInt(), idE);
    QCOMPARE(responseStartTimes.at(1).toObject().value("id").toInt(), idF);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));

    IrrigationDataSource verify(dbPath);
    QVERIFY(verify.open());
    const ProgramStartTimeList result = verify.startTimesFor(program.id);
    QCOMPARE(result.count(), 2);
    for(const ProgramStartTime& item : result) {
        if(item.id == idE) {
            QCOMPARE(item.minutesAfterMidnight, 200);
        }
        else if(item.id == idF) {
            QCOMPARE(item.minutesAfterMidnight, 800);
        }
        else {
            QFAIL("unexpected start time id after reorder");
        }
    }
}

void TestControlServer::programPutReplacesProgramZonesAsSent()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");

    Program program;
    program.name = "ZoneReplace";
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = 9;

    QList<ProgramStartTime> startTimes{};
    ProgramZone first; first.zoneId = 3; first.sequence = 0; first.durationSeconds = 100;
    ProgramZone second; second.zoneId = 4; second.sequence = 1; second.durationSeconds = 200;
    QList<ProgramZone> zones{ first, second };

    QVERIFY(seedProgramDirect(dbPath, program, startTimes, zones));
    const int oldFirstId = zones.at(0).id;
    const int oldSecondId = zones.at(1).id;

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    const QJsonObject requestBody{
        { "name", "ZoneReplace" },
        { "enabled", true },
        { "dayMode", "DaysOfWeek" },
        { "dowMask", 9 },
        { "intervalDays", 0 },
        { "anchorDate", "" },
        { "startTimes", QJsonArray{} },
        { "zones", QJsonArray{
            QJsonObject{ { "zoneId", 6 }, { "sequence", 5 }, { "durationSeconds", 750 } }
        } }
    };

    QNetworkAccessManager manager;
    QNetworkReply* reply = putJson(manager, server.boundPort(),
                                   QString("/admin/programs/%1").arg(program.id),
                                   QJsonDocument(requestBody).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 200);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));

    IrrigationDataSource verify(dbPath);
    QVERIFY(verify.open());
    const ProgramZoneList result = verify.zonesFor(program.id);
    QCOMPARE(result.count(), 1);
    QVERIFY(result.at(0).id != oldFirstId);
    QVERIFY(result.at(0).id != oldSecondId);
    QCOMPARE(result.at(0).zoneId, 6);
    QCOMPARE(result.at(0).sequence, 5);
    QCOMPARE(result.at(0).durationSeconds, 750);
}

void TestControlServer::programPutRollsBackOnGenuineDatabaseFailureLeavingOriginalDataIntact()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");

    Program program;
    program.name = "Rollback";
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = 3;

    ProgramStartTime original;
    original.minutesAfterMidnight = 400;
    original.timezone = "UTC";
    QList<ProgramStartTime> startTimes{ original };

    ProgramZone zone;
    zone.zoneId = 8;
    zone.sequence = 0;
    zone.durationSeconds = 111;
    QList<ProgramZone> zones{ zone };

    QVERIFY(seedProgramDirect(dbPath, program, startTimes, zones));
    const int originalStartTimeId = startTimes.at(0).id;
    const int originalZoneEntryId = zones.at(0).id;

    {
        IrrigationDataSource trigger(dbPath);
        QVERIFY(trigger.open());
        bool ok = false;
        trigger.rawQuery(
            "CREATE TRIGGER reject_poison_zone BEFORE INSERT ON program_zones "
            "WHEN NEW.duration_seconds = 999999 "
            "BEGIN SELECT RAISE(ABORT, 'poison zone insert'); END;",
            &ok);
        QVERIFY(ok);
    }

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    // The changed start time forces reconcileStartTimes to delete the original row, and
    // deleteProgramZones always runs, before the poisoned zone insert aborts the write.
    const QJsonObject requestBody{
        { "name", "Rollback Attempt" },
        { "enabled", true },
        { "dayMode", "DaysOfWeek" },
        { "dowMask", 3 },
        { "intervalDays", 0 },
        { "anchorDate", "" },
        { "startTimes", QJsonArray{
            QJsonObject{ { "minutesAfterMidnight", 900 }, { "timezone", "UTC" } }
        } },
        { "zones", QJsonArray{
            QJsonObject{ { "zoneId", 8 }, { "sequence", 0 }, { "durationSeconds", 999999 } }
        } }
    };

    QNetworkAccessManager manager;
    QNetworkReply* reply = putJson(manager, server.boundPort(),
                                   QString("/admin/programs/%1").arg(program.id),
                                   QJsonDocument(requestBody).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 500);

    // Read back on the server's own connection while it is still running: a missing
    // rollbackTransaction() call would leave "Rollback Attempt" visible here, even though a
    // read after stop() would not (closing the connection rolls back whatever was left open).
    QNetworkReply* getReply = getJson(manager, server.boundPort(), "/admin/programs");
    const QJsonObject got = QJsonDocument::fromJson(getReply->readAll()).array().at(0).toObject();
    QCOMPARE(got.value("name").toString(), QString("Rollback"));
    QCOMPARE(got.value("startTimes").toArray().count(), 1);
    QCOMPARE(got.value("startTimes").toArray().at(0).toObject().value("id").toInt(), originalStartTimeId);
    QCOMPARE(got.value("zones").toArray().count(), 1);
    QCOMPARE(got.value("zones").toArray().at(0).toObject().value("id").toInt(), originalZoneEntryId);

    {
        IrrigationDataSource verify(dbPath);
        QVERIFY(verify.open());
        const ProgramList programs = verify.allPrograms();
        QCOMPARE(programs.count(), 1);
        QCOMPARE(programs.at(0).name, QString("Rollback"));
        QCOMPARE(programs.at(0).dowMask, 3);

        const ProgramStartTimeList verifyStartTimes = verify.startTimesFor(program.id);
        QCOMPARE(verifyStartTimes.count(), 1);
        QCOMPARE(verifyStartTimes.at(0).id, originalStartTimeId);
        QCOMPARE(verifyStartTimes.at(0).minutesAfterMidnight, 400);

        const ProgramZoneList verifyZones = verify.zonesFor(program.id);
        QCOMPARE(verifyZones.count(), 1);
        QCOMPARE(verifyZones.at(0).id, originalZoneEntryId);
        QCOMPARE(verifyZones.at(0).durationSeconds, 111);
    }

    // The connection must still accept a transaction: a left-open transaction answers 500
    // here.
    const QJsonObject validBody{
        { "name", "After" }, { "enabled", true }, { "dayMode", "DaysOfWeek" }, { "dowMask", 3 },
        { "intervalDays", 0 }, { "anchorDate", "" },
        { "startTimes", QJsonArray{ QJsonObject{ { "minutesAfterMidnight", 400 }, { "timezone", "UTC" } } } },
        { "zones", QJsonArray{ QJsonObject{ { "zoneId", 8 }, { "sequence", 0 }, { "durationSeconds", 111 } } } }
    };
    QNetworkReply* secondReply = putJson(manager, server.boundPort(),
                                         QString("/admin/programs/%1").arg(program.id),
                                         QJsonDocument(validBody).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(secondReply), 200);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));
}

void TestControlServer::programPutStoresEveryMutableProgramField()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");

    Program program;
    program.name = "Before";
    program.enabled = true;
    program.dayMode = Program::DayMode::Odd;
    program.dowMask = 9;
    QList<ProgramStartTime> startTimes{};
    QList<ProgramZone> zones{};
    QVERIFY(seedProgramDirect(dbPath, program, startTimes, zones));

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    const QJsonObject body{
        { "name", "After" }, { "enabled", false }, { "dayMode", "EveryNDays" }, { "dowMask", 42 },
        { "intervalDays", 1 }, { "anchorDate", "2027-02-03" },
        { "startTimes", QJsonArray{ QJsonObject{ { "minutesAfterMidnight", 610 }, { "timezone", "UTC" } } } },
        { "zones", QJsonArray{} }
    };
    QNetworkReply* reply = putJson(manager, server.boundPort(),
                                   QString("/admin/programs/%1").arg(program.id),
                                   QJsonDocument(body).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 200);
    const QJsonObject response = QJsonDocument::fromJson(reply->readAll()).object();
    const int responseStartTimeId = response.value("startTimes").toArray().at(0).toObject().value("id").toInt();
    QCOMPARE(response.value("name").toString(), QString("After"));
    QCOMPARE(response.value("enabled").toBool(), false);
    QCOMPARE(response.value("dayMode").toString(), QString("EveryNDays"));
    QCOMPARE(response.value("dowMask").toInt(), 42);
    QCOMPARE(response.value("intervalDays").toInt(), 1);
    QCOMPARE(response.value("anchorDate").toString(), QString("2027-02-03"));

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));

    IrrigationDataSource verify(dbPath);
    QVERIFY(verify.open());
    const ProgramList programs = verify.allPrograms();
    QCOMPARE(programs.count(), 1);
    QCOMPARE(programs.at(0).id, program.id);
    QCOMPARE(programs.at(0).name, QString("After"));
    QCOMPARE(programs.at(0).enabled, false);
    QCOMPARE(programs.at(0).dayMode, Program::DayMode::EveryNDays);
    QCOMPARE(programs.at(0).dowMask, 42);
    QCOMPARE(programs.at(0).intervalDays, 1);
    QCOMPARE(programs.at(0).anchorDate, QDate(2027, 2, 3));

    const ProgramStartTimeList stored = verify.startTimesFor(program.id);
    QCOMPARE(stored.count(), 1);
    QCOMPARE(responseStartTimeId, stored.at(0).id);
    QVERIFY(responseStartTimeId > 0);
}

void TestControlServer::programPutTimezoneOnlyChangeReconcilesToANewRow()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");

    Program program;
    program.name = "Tz";
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = 5;
    ProgramStartTime s;
    s.minutesAfterMidnight = 420;
    s.timezone = "UTC";
    QList<ProgramStartTime> startTimes{ s };
    QList<ProgramZone> zones{};
    QVERIFY(seedProgramDirect(dbPath, program, startTimes, zones));
    const int oldId = startTimes.at(0).id;

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    // Same minutes, a different timezone: the reconcile key is the pair, so this must not
    // match the stored row.
    const QJsonObject body{
        { "name", "Tz" }, { "enabled", true }, { "dayMode", "DaysOfWeek" }, { "dowMask", 5 },
        { "intervalDays", 0 }, { "anchorDate", "" },
        { "startTimes", QJsonArray{
            QJsonObject{ { "minutesAfterMidnight", 420 }, { "timezone", "America/Los_Angeles" } }
        } },
        { "zones", QJsonArray{} }
    };
    QNetworkAccessManager manager;
    QNetworkReply* reply = putJson(manager, server.boundPort(),
                                   QString("/admin/programs/%1").arg(program.id),
                                   QJsonDocument(body).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 200);
    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));

    IrrigationDataSource verify(dbPath);
    QVERIFY(verify.open());
    const ProgramStartTimeList result = verify.startTimesFor(program.id);
    QCOMPARE(result.count(), 1);
    QCOMPARE(result.at(0).timezone, QString("America/Los_Angeles"));
    QVERIFY(result.at(0).id != oldId);
}

void TestControlServer::programPutStartTimesSharingMinutesDifferingTimezoneReconcileIndependently()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");

    Program program;
    program.name = "SharedMinutes";
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = 5;
    ProgramStartTime utc;
    utc.minutesAfterMidnight = 420;
    utc.timezone = "UTC";
    ProgramStartTime la;
    la.minutesAfterMidnight = 420;
    la.timezone = "America/Los_Angeles";
    QList<ProgramStartTime> startTimes{ utc, la };
    QList<ProgramZone> zones{};
    QVERIFY(seedProgramDirect(dbPath, program, startTimes, zones));
    const int idUtc = startTimes.at(0).id;
    const int idLa = startTimes.at(1).id;
    QVERIFY(idUtc != idLa);

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    // Only the LA entry is sent back.
    const QJsonObject body{
        { "name", "SharedMinutes" }, { "enabled", true }, { "dayMode", "DaysOfWeek" }, { "dowMask", 5 },
        { "intervalDays", 0 }, { "anchorDate", "" },
        { "startTimes", QJsonArray{
            QJsonObject{ { "minutesAfterMidnight", 420 }, { "timezone", "America/Los_Angeles" } }
        } },
        { "zones", QJsonArray{} }
    };
    QNetworkAccessManager manager;
    QNetworkReply* reply = putJson(manager, server.boundPort(),
                                   QString("/admin/programs/%1").arg(program.id),
                                   QJsonDocument(body).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 200);
    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));

    IrrigationDataSource verify(dbPath);
    QVERIFY(verify.open());
    const ProgramStartTimeList result = verify.startTimesFor(program.id);
    QCOMPARE(result.count(), 1);
    QCOMPARE(result.at(0).id, idLa);
    QCOMPARE(result.at(0).timezone, QString("America/Los_Angeles"));
}

void TestControlServer::programPutStoredDuplicateStartTimesShrinkToOneKeepingTheFirstId()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");

    Program program;
    program.name = "Shrink";
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = 5;
    ProgramStartTime a; a.minutesAfterMidnight = 900; a.timezone = "UTC";
    ProgramStartTime b; b.minutesAfterMidnight = 900; b.timezone = "UTC";
    QList<ProgramStartTime> startTimes{ a, b };
    QList<ProgramZone> zones{};
    QVERIFY(seedProgramDirect(dbPath, program, startTimes, zones));
    const int firstId = startTimes.at(0).id;
    const int secondId = startTimes.at(1).id;
    QVERIFY(firstId != secondId);

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    const QJsonObject body{
        { "name", "Shrink" }, { "enabled", true }, { "dayMode", "DaysOfWeek" }, { "dowMask", 5 },
        { "intervalDays", 0 }, { "anchorDate", "" },
        { "startTimes", QJsonArray{ QJsonObject{ { "minutesAfterMidnight", 900 }, { "timezone", "UTC" } } } },
        { "zones", QJsonArray{} }
    };
    QNetworkReply* reply = putJson(manager, server.boundPort(),
                                   QString("/admin/programs/%1").arg(program.id),
                                   QJsonDocument(body).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 200);
    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));

    IrrigationDataSource verify(dbPath);
    QVERIFY(verify.open());
    const ProgramStartTimeList result = verify.startTimesFor(program.id);
    QCOMPARE(result.count(), 1);
    QCOMPARE(result.at(0).id, firstId);
    QVERIFY(result.at(0).id != secondId);
}

void TestControlServer::programPutAnswers500WhenCommitFailsLeavingOriginalDataIntact()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");

    Program program;
    program.name = "Commit";
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = 3;
    ProgramStartTime original;
    original.minutesAfterMidnight = 400;
    original.timezone = "UTC";
    QList<ProgramStartTime> startTimes{ original };
    ProgramZone zone;
    zone.zoneId = 8;
    zone.sequence = 0;
    zone.durationSeconds = 111;
    QList<ProgramZone> zones{ zone };
    QVERIFY(seedProgramDirect(dbPath, program, startTimes, zones));
    const int originalStartTimeId = startTimes.at(0).id;
    const int originalZoneEntryId = zones.at(0).id;

    // A deferred foreign key, tripped from an AFTER INSERT trigger, makes COMMIT itself fail.
    {
        IrrigationDataSource trigger(dbPath);
        QVERIFY(trigger.open());
        bool ok = false;
        trigger.rawQuery(
            "CREATE TABLE commit_poison (program_ref INTEGER REFERENCES programs(id) "
            "DEFERRABLE INITIALLY DEFERRED)",
            &ok);
        QVERIFY(ok);
        trigger.rawQuery(
            "CREATE TRIGGER poison_commit AFTER INSERT ON program_zones "
            "WHEN NEW.duration_seconds = 777777 "
            "BEGIN INSERT INTO commit_poison VALUES (-1); END;",
            &ok);
        QVERIFY(ok);
    }

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    const QJsonObject body{
        { "name", "Commit Attempt" }, { "enabled", true }, { "dayMode", "DaysOfWeek" }, { "dowMask", 3 },
        { "intervalDays", 0 }, { "anchorDate", "" },
        { "startTimes", QJsonArray{ QJsonObject{ { "minutesAfterMidnight", 900 }, { "timezone", "UTC" } } } },
        { "zones", QJsonArray{ QJsonObject{ { "zoneId", 8 }, { "sequence", 0 }, { "durationSeconds", 777777 } } } }
    };
    QNetworkAccessManager manager;
    QNetworkReply* reply = putJson(manager, server.boundPort(),
                                   QString("/admin/programs/%1").arg(program.id),
                                   QJsonDocument(body).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 500);

    // Read back on the server's own connection while it is still running: a commit-failure
    // path that skips rollbackTransaction() would show "Commit Attempt" here, even though a
    // read after stop() would not (closing the connection rolls back whatever was left open).
    QNetworkReply* getReply = getJson(manager, server.boundPort(), "/admin/programs");
    const QJsonObject got = QJsonDocument::fromJson(getReply->readAll()).array().at(0).toObject();
    QCOMPARE(got.value("name").toString(), QString("Commit"));
    QCOMPARE(got.value("startTimes").toArray().count(), 1);
    QCOMPARE(got.value("startTimes").toArray().at(0).toObject().value("id").toInt(), originalStartTimeId);
    QCOMPARE(got.value("zones").toArray().count(), 1);
    QCOMPARE(got.value("zones").toArray().at(0).toObject().value("id").toInt(), originalZoneEntryId);

    {
        IrrigationDataSource verify(dbPath);
        QVERIFY(verify.open());
        const ProgramList programs = verify.allPrograms();
        QCOMPARE(programs.count(), 1);
        QCOMPARE(programs.at(0).name, QString("Commit"));
        QCOMPARE(verify.startTimesFor(program.id).at(0).id, originalStartTimeId);
        QCOMPARE(verify.zonesFor(program.id).at(0).id, originalZoneEntryId);
    }

    // The connection must still accept a transaction: a left-open transaction answers 500 here.
    const QJsonObject validBody{
        { "name", "After" }, { "enabled", true }, { "dayMode", "DaysOfWeek" }, { "dowMask", 3 },
        { "intervalDays", 0 }, { "anchorDate", "" },
        { "startTimes", QJsonArray{ QJsonObject{ { "minutesAfterMidnight", 400 }, { "timezone", "UTC" } } } },
        { "zones", QJsonArray{ QJsonObject{ { "zoneId", 8 }, { "sequence", 0 }, { "durationSeconds", 111 } } } }
    };
    QNetworkReply* secondReply = putJson(manager, server.boundPort(),
                                         QString("/admin/programs/%1").arg(program.id),
                                         QJsonDocument(validBody).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(secondReply), 200);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));
}

void TestControlServer::programPutAnswers500WhenAStartTimeInsertFailsLeavingOriginalDataIntact()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");

    Program program;
    program.name = "Reconcile Fail";
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = 3;
    ProgramStartTime original;
    original.minutesAfterMidnight = 400;
    original.timezone = "UTC";
    QList<ProgramStartTime> startTimes{ original };
    ProgramZone zone;
    zone.zoneId = 8;
    zone.sequence = 0;
    zone.durationSeconds = 111;
    QList<ProgramZone> zones{ zone };
    QVERIFY(seedProgramDirect(dbPath, program, startTimes, zones));
    const int originalStartTimeId = startTimes.at(0).id;

    // The changed minutes force reconcileStartTimes to delete the original row and insert a
    // new one; the trigger aborts that insert.
    {
        IrrigationDataSource trigger(dbPath);
        QVERIFY(trigger.open());
        bool ok = false;
        trigger.rawQuery(
            "CREATE TRIGGER reject_poison_start_time BEFORE INSERT ON program_start_times "
            "WHEN NEW.minutes_after_midnight = 1234 "
            "BEGIN SELECT RAISE(ABORT, 'poison start time insert'); END;",
            &ok);
        QVERIFY(ok);
    }

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    const QJsonObject body{
        { "name", "Reconcile Attempt" }, { "enabled", true }, { "dayMode", "DaysOfWeek" }, { "dowMask", 3 },
        { "intervalDays", 0 }, { "anchorDate", "" },
        { "startTimes", QJsonArray{ QJsonObject{ { "minutesAfterMidnight", 1234 }, { "timezone", "UTC" } } } },
        { "zones", QJsonArray{ QJsonObject{ { "zoneId", 8 }, { "sequence", 0 }, { "durationSeconds", 222 } } } }
    };
    QNetworkAccessManager manager;
    QNetworkReply* reply = putJson(manager, server.boundPort(),
                                   QString("/admin/programs/%1").arg(program.id),
                                   QJsonDocument(body).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(reply), 500);

    // Read back on the server's own connection while it is still running: a handler that
    // ignores reconcileStartTimes()'s failure would commit the delete with 200 and serve the
    // program with its start time missing.
    QNetworkReply* getReply = getJson(manager, server.boundPort(), "/admin/programs");
    const QJsonObject got = QJsonDocument::fromJson(getReply->readAll()).array().at(0).toObject();
    QCOMPARE(got.value("name").toString(), QString("Reconcile Fail"));
    QCOMPARE(got.value("startTimes").toArray().count(), 1);
    QCOMPARE(got.value("startTimes").toArray().at(0).toObject().value("id").toInt(), originalStartTimeId);
    QCOMPARE(got.value("startTimes").toArray().at(0).toObject().value("minutesAfterMidnight").toInt(), 400);

    {
        IrrigationDataSource verify(dbPath);
        QVERIFY(verify.open());
        const ProgramStartTimeList verifyStartTimes = verify.startTimesFor(program.id);
        QCOMPARE(verifyStartTimes.count(), 1);
        QCOMPARE(verifyStartTimes.at(0).id, originalStartTimeId);
        QCOMPARE(verifyStartTimes.at(0).minutesAfterMidnight, 400);
    }

    // The connection must still accept a transaction: a left-open transaction answers 500 here.
    const QJsonObject validBody{
        { "name", "After" }, { "enabled", true }, { "dayMode", "DaysOfWeek" }, { "dowMask", 3 },
        { "intervalDays", 0 }, { "anchorDate", "" },
        { "startTimes", QJsonArray{ QJsonObject{ { "minutesAfterMidnight", 400 }, { "timezone", "UTC" } } } },
        { "zones", QJsonArray{ QJsonObject{ { "zoneId", 8 }, { "sequence", 0 }, { "durationSeconds", 111 } } } }
    };
    QNetworkReply* secondReply = putJson(manager, server.boundPort(),
                                         QString("/admin/programs/%1").arg(program.id),
                                         QJsonDocument(validBody).toJson(QJsonDocument::Compact));
    QCOMPARE(statusCode(secondReply), 200);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));
}

void TestControlServer::programsGetReportsEmptyNextRunUtcForADisabledProgram()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");

    Program program;
    program.name = "Disabled";
    program.enabled = false;
    program.dayMode = Program::DayMode::EveryNDays;
    program.intervalDays = 1;
    program.anchorDate = QDateTime::currentDateTimeUtc().date().addDays(30);
    ProgramStartTime startTime;
    startTime.minutesAfterMidnight = 360;
    startTime.timezone = "UTC";
    QList<ProgramStartTime> startTimes{ startTime };
    QList<ProgramZone> zones{};
    QVERIFY(seedProgramDirect(dbPath, program, startTimes, zones));

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = getJson(manager, server.boundPort(), "/admin/programs");
    const QJsonArray array = QJsonDocument::fromJson(reply->readAll()).array();
    QCOMPARE(array.count(), 1);
    const QJsonObject body = array.at(0).toObject();
    QCOMPARE(body.value("enabled").toBool(), false);
    QCOMPARE(body.value("nextRunUtc").toString(), QString(""));

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::programDeleteRemovesProgramCascadingStartTimesAndZonesLeavesOtherProgramsIntact()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");

    // A filler program, created and deleted first, gives doomed and survivor ids that do
    // not equal their list position + 1.
    {
        IrrigationDataSource seed(dbPath);
        QVERIFY(seed.open());
        Program filler;
        filler.name = "Filler";
        filler.dayMode = Program::DayMode::DaysOfWeek;
        filler.dowMask = 1;
        QVERIFY(seed.insertProgram(filler));
        QVERIFY(seed.deleteProgram(filler.id));
    }

    Program doomed;
    doomed.name = "Doomed";
    doomed.dayMode = Program::DayMode::DaysOfWeek;
    doomed.dowMask = 1;
    ProgramStartTime doomedStart; doomedStart.minutesAfterMidnight = 100; doomedStart.timezone = "UTC";
    QList<ProgramStartTime> doomedStartTimes{ doomedStart };
    ProgramZone doomedZone; doomedZone.zoneId = 1; doomedZone.sequence = 0; doomedZone.durationSeconds = 50;
    QList<ProgramZone> doomedZones{ doomedZone };
    QVERIFY(seedProgramDirect(dbPath, doomed, doomedStartTimes, doomedZones));
    const int doomedStartTimeId = doomedStartTimes.at(0).id;
    const int doomedZoneEntryId = doomedZones.at(0).id;

    Program survivor;
    survivor.name = "Survivor";
    survivor.dayMode = Program::DayMode::DaysOfWeek;
    survivor.dowMask = 2;
    ProgramStartTime survivorStart; survivorStart.minutesAfterMidnight = 600; survivorStart.timezone = "UTC";
    QList<ProgramStartTime> survivorStartTimes{ survivorStart };
    ProgramZone survivorZone; survivorZone.zoneId = 2; survivorZone.sequence = 0; survivorZone.durationSeconds = 60;
    QList<ProgramZone> survivorZones{ survivorZone };
    QVERIFY(seedProgramDirect(dbPath, survivor, survivorStartTimes, survivorZones));

    // doomed sits at list position 0 (position + 1 == 1) while its real id is one higher;
    // survivor sits at position 1 (position + 1 == 2) while its real id is one higher still.
    QVERIFY(doomed.id != 1);
    QVERIFY(survivor.id != 2);

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = deleteResource(manager, server.boundPort(),
                                          QString("/admin/programs/%1").arg(doomed.id));
    QCOMPARE(statusCode(reply), 204);

    QVERIFY(server.stop(TimeSpan::fromSeconds(5)));

    IrrigationDataSource verify(dbPath);
    QVERIFY(verify.open());
    const ProgramList programs = verify.allPrograms();
    QCOMPARE(programs.count(), 1);
    QCOMPARE(programs.at(0).id, survivor.id);
    QCOMPARE(programs.at(0).name, QString("Survivor"));

    bool ok = false;
    QSqlQuery startTimeCheck = verify.rawQuery(
        QString("SELECT COUNT(*) FROM program_start_times WHERE id = %1").arg(doomedStartTimeId), &ok);
    QVERIFY(ok);
    QVERIFY(startTimeCheck.next());
    QCOMPARE(startTimeCheck.value(0).toInt(), 0);

    QSqlQuery zoneCheck = verify.rawQuery(
        QString("SELECT COUNT(*) FROM program_zones WHERE id = %1").arg(doomedZoneEntryId), &ok);
    QVERIFY(ok);
    QVERIFY(zoneCheck.next());
    QCOMPARE(zoneCheck.value(0).toInt(), 0);

    const ProgramStartTimeList survivorVerifyStartTimes = verify.startTimesFor(survivor.id);
    QCOMPARE(survivorVerifyStartTimes.count(), 1);
    QCOMPARE(survivorVerifyStartTimes.at(0).minutesAfterMidnight, 600);

    const ProgramZoneList survivorVerifyZones = verify.zonesFor(survivor.id);
    QCOMPARE(survivorVerifyZones.count(), 1);
    QCOMPARE(survivorVerifyZones.at(0).durationSeconds, 60);
}

void TestControlServer::programDeleteUnknownIdReturns404()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QNetworkAccessManager manager;
    QNetworkReply* reply = deleteResource(manager, server.boundPort(), "/admin/programs/999");
    QCOMPARE(statusCode(reply), 404);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::programRunEmitsProgramRunRequestedWithTheExactProgramId()
{
    QTemporaryDir dir;
    const QString dbPath = dir.filePath("irrigation.db");

    {
        IrrigationDataSource seed(dbPath);
        QVERIFY(seed.open());
        for(int i = 0; i < 3; i++) {
            Program filler;
            filler.name = QString("Filler %1").arg(i);
            filler.dayMode = Program::DayMode::DaysOfWeek;
            filler.dowMask = 1;
            QVERIFY(seed.insertProgram(filler));
        }
        QVERIFY(seed.deleteProgram(1));
    }

    Program program;
    program.name = "Runnable";
    program.dayMode = Program::DayMode::DaysOfWeek;
    program.dowMask = 1;
    ProgramStartTime startTime; startTime.minutesAfterMidnight = 360; startTime.timezone = "UTC";
    QList<ProgramStartTime> startTimes{ startTime };
    ProgramZone zone; zone.zoneId = 6; zone.sequence = 0; zone.durationSeconds = 200;
    QList<ProgramZone> zones{ zone };
    QVERIFY(seedProgramDirect(dbPath, program, startTimes, zones));

    // The program id must differ from the start-time id, the zone id, the zone number and
    // its own list position plus one.
    QVERIFY(program.id != startTimes.at(0).id);
    QVERIFY(program.id != zones.at(0).zoneId);
    QVERIFY(program.id != 6);
    QVERIFY(program.id != 3);

    IrrigationControlServer server(dbPath);
    QVERIFY(startServerOnLoopback(server));

    QSignalSpy spy(&server, &IrrigationControlServer::programRunRequested);

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(),
                                    QString("/admin/programs/%1/run").arg(program.id), QByteArray());
    QCOMPARE(statusCode(reply), 202);

    if(spy.count() == 0) {
        QVERIFY(spy.wait(5000));
    }
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toInt(), program.id);

    server.stop(TimeSpan::fromSeconds(5));
}

void TestControlServer::programRunUnknownIdReturns404AndEmitsNothing()
{
    QTemporaryDir dir;
    IrrigationControlServer server(dir.filePath("irrigation.db"));
    QVERIFY(startServerOnLoopback(server));

    QSignalSpy spy(&server, &IrrigationControlServer::programRunRequested);

    QNetworkAccessManager manager;
    QNetworkReply* reply = postJson(manager, server.boundPort(), "/admin/programs/999/run", QByteArray());
    QCOMPARE(statusCode(reply), 404);

    spy.wait(200);
    QCOMPARE(spy.count(), 0);

    server.stop(TimeSpan::fromSeconds(5));
}

QTEST_MAIN(TestControlServer)
#include "tst_controlserver.moc"
