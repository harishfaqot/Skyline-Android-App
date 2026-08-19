#include "RosPointCloudClient.h"
#include <QDebug>
#include <QByteArray>
#include <QTimer>

// ============================================================================
RosPointCloudClient::RosPointCloudClient(QObject *parent)
    : QObject(parent)
{
    connect(&m_webSocket, &QWebSocket::connected,
            this, &RosPointCloudClient::onConnected);
    connect(&m_webSocket, &QWebSocket::disconnected,
            this, &RosPointCloudClient::onDisconnected);
    connect(&m_webSocket, &QWebSocket::textMessageReceived,
            this, &RosPointCloudClient::onTextMessageReceived);
    connect(&m_webSocket,
            QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &RosPointCloudClient::onError);
    connect(&m_webSocket, &QWebSocket::pong, this, &RosPointCloudClient::onPong);

    m_pingTimer = new QTimer(this);
    m_pingTimer->setInterval(2000);
    connect(m_pingTimer, &QTimer::timeout, this, &RosPointCloudClient::onPingTimer);

    m_statsTimer = new QTimer(this);
    m_statsTimer->setInterval(1000);
    connect(m_statsTimer, &QTimer::timeout, this, &RosPointCloudClient::onStatsTimer);
}

RosPointCloudClient::~RosPointCloudClient()
{
    disconnect();
}

void RosPointCloudClient::connectToServer(const QString &url)
{
    qDebug() << "RosPointCloudClient: connecting to rosbridge at" << url;
    m_webSocket.open(QUrl(url));
}

void RosPointCloudClient::disconnect()
{
    if (m_webSocket.state() == QAbstractSocket::ConnectedState)
        m_webSocket.close();
}

bool RosPointCloudClient::isConnected() const
{
    return m_webSocket.state() == QAbstractSocket::ConnectedState;
}

// ============================================================================
// Slots
// ============================================================================

void RosPointCloudClient::onConnected()
{
    qDebug() << "RosPointCloudClient: connected – subscribing to topics";

    sendSubscribe("/path",             "nav_msgs/Path");
    sendSubscribe("/Odometry",         "nav_msgs/Odometry");
    sendSubscribe("/telemetry",        "std_msgs/String");

    m_bytesReceivedAccum = 0;
    m_bytesSentAccum = 0;
    m_pingTimer->start();
    m_statsTimer->start();
    onPingTimer(); // first probe immediately rather than waiting 2s

    emit connected();
}

void RosPointCloudClient::onDisconnected()
{
    qDebug() << "RosPointCloudClient: disconnected";
    m_pingTimer->stop();
    m_statsTimer->stop();
    emit latencyMeasured(-1);
    emit bandwidthUpdated(0.0, 0.0);
    emit disconnected();
}

void RosPointCloudClient::onTextMessageReceived(const QString &message)
{
    m_bytesReceivedAccum += message.toUtf8().size();

    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError) {
        qWarning() << "RosPointCloudClient: JSON parse error:" << parseErr.errorString();
        return;
    }

    QJsonObject root = doc.object();
    QString op    = root["op"].toString();
    QString topic = root["topic"].toString();

    if (op != "publish") return;

    QJsonObject msg = root["msg"].toObject();

    if (topic == "/path") {
        processPath(msg);
    } else if (topic == "/Odometry") {
        processOdometry(msg);
    } else if (topic == "/telemetry") {
        processTelemetry(msg);
    }
}

void RosPointCloudClient::onError(QAbstractSocket::SocketError err)
{
    QString errStr = m_webSocket.errorString();
    qWarning() << "RosPointCloudClient: WebSocket error:" << err << "-" << errStr;
    emit error(errStr);
}

void RosPointCloudClient::onPong(quint64 elapsedTime, const QByteArray &payload)
{
    Q_UNUSED(payload);
    emit latencyMeasured(static_cast<qint64>(elapsedTime));
}

void RosPointCloudClient::onPingTimer()
{
    if (m_webSocket.state() == QAbstractSocket::ConnectedState) {
        m_webSocket.ping();
    }
}

void RosPointCloudClient::onStatsTimer()
{
    // Accumulated over the last ~1s (timer interval) — close enough to a
    // true kbps rate without needing a precise elapsed-time measurement.
    const double downKbps = (m_bytesReceivedAccum * 8) / 1000.0;
    const double upKbps   = (m_bytesSentAccum * 8) / 1000.0;
    m_bytesReceivedAccum = 0;
    m_bytesSentAccum = 0;
    emit bandwidthUpdated(downKbps, upKbps);
}

// ============================================================================
// Helpers
// ============================================================================

void RosPointCloudClient::sendSubscribe(const QString &topic, const QString &type)
{
    QJsonObject sub;
    sub["op"]    = "subscribe";
    sub["topic"] = topic;
    sub["type"]  = type;
    // Request throttled delivery to avoid flooding the UI on high-rate topics
    // (rosbridge ignores unknown keys, so this is safe even for older versions)
    sub["throttle_rate"] = 100;   // ms between messages, ~10 Hz max
    sub["queue_length"]  = 1;     // only keep latest

    const QByteArray payload = QJsonDocument(sub).toJson(QJsonDocument::Compact);
    m_bytesSentAccum += payload.size();
    m_webSocket.sendTextMessage(QString::fromUtf8(payload));
}

// ============================================================================
// nav_msgs/Path  →  std::vector<QVector3D>
// ============================================================================
void RosPointCloudClient::processPath(const QJsonObject &msg)
{
    QJsonArray posesArr = msg["poses"].toArray();
    if (posesArr.isEmpty()) return;

    std::vector<QVector3D> poses;
    poses.reserve(posesArr.size());

    for (const QJsonValue &pv : posesArr) {
        QJsonObject position =
            pv.toObject()["pose"].toObject()["position"].toObject();

        float x = (float)position["x"].toDouble();
        float y = (float)position["y"].toDouble();
        float z = (float)position["z"].toDouble();
        poses.emplace_back(x, y, z);
    }

    qDebug() << "RosPointCloudClient: path received," << poses.size() << "poses";
    emit pathReceived(poses);
}

// ============================================================================
// nav_msgs/Odometry  →  RobotPose  (used as the robot axis/marker)
// ============================================================================
void RosPointCloudClient::processOdometry(const QJsonObject &msg)
{
    QJsonObject poseObj     = msg["pose"].toObject()["pose"].toObject();
    QJsonObject positionObj = poseObj["position"].toObject();
    QJsonObject orientObj   = poseObj["orientation"].toObject();

    RobotPose pose;
    pose.position = QVector3D(
        (float)positionObj["x"].toDouble(),
        (float)positionObj["y"].toDouble(),
        (float)positionObj["z"].toDouble()
    );
    // QQuaternion(scalar/w, x, y, z)
    pose.orientation = QQuaternion(
        (float)orientObj["w"].toDouble(),
        (float)orientObj["x"].toDouble(),
        (float)orientObj["y"].toDouble(),
        (float)orientObj["z"].toDouble()
    );

    emit robotPoseReceived(pose);
}

// ============================================================================
// telemetry custom JSON  →  telemetryReceived(battery, signal, mode, voltage, current, armed)
// Expected payload example:
//   {"battery": 87, "signal": 71, "mode": "AUTO", "voltage": 11.4, "current": 3.2, "armed": true}
// Also supports std_msgs/String wrapper:
//   {"data": "{\"battery\":87,\"signal\":71,\"mode\":\"AUTO\"}"}
// voltage/current/armed are optional — battery/signal/mode remain the only
// required fields so older publishers that don't send them still work.
// ============================================================================
void RosPointCloudClient::processTelemetry(const QJsonObject &msg)
{
    QJsonObject payload = msg;

    if (msg.contains("data") && msg.value("data").isString()) {
        QJsonParseError err;
        const QByteArray data = msg.value("data").toString().toUtf8();
        const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            payload = doc.object();
        }
    }

    const int battery = payload.value("battery").toInt(-1);
    const int signal  = payload.value("signal").toInt(-1);
    const QString mode = payload.value("mode").toString();
    const double voltage = payload.value("voltage").toDouble(-1.0);
    const double current = payload.value("current").toDouble(-1.0);
    const bool armed = payload.value("armed").toBool(false);

    if (battery < 0 || signal < 0 || mode.isEmpty()) {
        return;
    }

    emit telemetryReceived(battery, signal, mode, voltage, current, armed);
}
