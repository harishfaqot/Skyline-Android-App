#include "RosPointCloudClient.h"
#include <QDebug>
#include <QByteArray>
#include <cstring>
#include <cmath>

// ============================================================================
// rosbridge JSON protocol helpers
//
// PointCloud2 data arrives as a base64-encoded binary blob.  The blob is the
// raw byte stream of the sensor_msgs/PointCloud2 "data" field.  We need the
// fields array to know how to parse it (offset, datatype, step/point_step).
//
// For /cloud_registered published by FAST-LIO / LIO-SAM the typical layout is:
//   fields: x(float32,0), y(float32,4), z(float32,8)
//   point_step: 16 or 32 (depends on additional fields like intensity/ring)
//
// We extract x,y,z from the correct byte offsets inside each point.
// ============================================================================

static QByteArray base64Decode(const QString &b64)
{
    return QByteArray::fromBase64(b64.toUtf8());
}

// rosbridge datatype codes (matches sensor_msgs/PointField constants)
static float readFieldFloat(const QByteArray &data, int byteOffset, int datatype)
{
    if (byteOffset + 4 > data.size()) return 0.0f;
    switch (datatype) {
        case 7: { // FLOAT32
            float v; memcpy(&v, data.constData() + byteOffset, 4); return v;
        }
        case 8: { // FLOAT64
            if (byteOffset + 8 > data.size()) return 0.0f;
            double v; memcpy(&v, data.constData() + byteOffset, 8); return (float)v;
        }
        case 4: { // INT32
            int32_t v; memcpy(&v, data.constData() + byteOffset, 4); return (float)v;
        }
        case 6: { // UINT32
            uint32_t v; memcpy(&v, data.constData() + byteOffset, 4); return (float)v;
        }
        default:
            return 0.0f;
    }
}

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

    sendSubscribe("/cloud_registered", "sensor_msgs/PointCloud2");
    sendSubscribe("/path",             "nav_msgs/Path");
    sendSubscribe("/Odometry",         "nav_msgs/Odometry");
    sendSubscribe("/telemetry",        "std_msgs/String");

    emit connected();
}

void RosPointCloudClient::onDisconnected()
{
    qDebug() << "RosPointCloudClient: disconnected";
    emit disconnected();
}

void RosPointCloudClient::onTextMessageReceived(const QString &message)
{
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

    if (topic == "/cloud_registered") {
        processPointCloud2(msg);
    } else if (topic == "/path") {
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

    m_webSocket.sendTextMessage(
        QString::fromUtf8(QJsonDocument(sub).toJson(QJsonDocument::Compact)));
}

// ============================================================================
// sensor_msgs/PointCloud2  →  std::vector<float> [x,y,z, ...]
// ============================================================================
void RosPointCloudClient::processPointCloud2(const QJsonObject &msg)
{
    // rosbridge encodes the "data" field as base64
    QString b64data = msg["data"].toString();
    if (b64data.isEmpty()) {
        qWarning() << "RosPointCloudClient: PointCloud2 data field is empty";
        return;
    }

    QByteArray raw = base64Decode(b64data);

    // point_step = bytes per point
    int pointStep = msg["point_step"].toInt(0);
    int width     = msg["width"].toInt(0);
    int height    = msg["height"].toInt(1);

    if (pointStep <= 0 || width == 0) {
        qWarning() << "RosPointCloudClient: invalid PointCloud2 point_step/width";
        return;
    }

    // Parse field offsets and datatypes from the "fields" array
    QJsonArray fields = msg["fields"].toArray();
    int xOffset = -1, yOffset = -1, zOffset = -1;
    int xType = 7, yType = 7, zType = 7;  // default FLOAT32

    for (const QJsonValue &fv : fields) {
        QJsonObject f = fv.toObject();
        QString name = f["name"].toString();
        int offset   = f["offset"].toInt();
        int datatype = f["datatype"].toInt(7);
        if (name == "x") { xOffset = offset; xType = datatype; }
        else if (name == "y") { yOffset = offset; yType = datatype; }
        else if (name == "z") { zOffset = offset; zType = datatype; }
    }

    if (xOffset < 0 || yOffset < 0 || zOffset < 0) {
        qWarning() << "RosPointCloudClient: PointCloud2 missing x/y/z fields";
        return;
    }

    int numPoints = width * height;
    std::vector<float> points;
    points.reserve(numPoints * 3);

    for (int i = 0; i < numPoints; ++i) {
        int base = i * pointStep;
        if (base + pointStep > raw.size()) break;

        float x = readFieldFloat(raw, base + xOffset, xType);
        float y = readFieldFloat(raw, base + yOffset, yType);
        float z = readFieldFloat(raw, base + zOffset, zType);

        // Skip NaN / Inf points that LiDAR drivers sometimes emit
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;

        points.push_back(x);
        points.push_back(y);
        points.push_back(z);
    }

    if (!points.empty())
        emit pointCloudReceived(points);
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
