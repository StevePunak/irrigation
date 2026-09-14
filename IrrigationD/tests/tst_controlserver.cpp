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
#include <QStringList>
#include <QTcpServer>
#include <QTimeZone>
#include <QUrl>

#include <Kanoop/timespan.h>

#include "database/irrigationdatasource.h"
#include "irrigationcontrolserver.h"
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

QTEST_MAIN(TestControlServer)
#include "tst_controlserver.moc"
