#ifndef ROSPOINTCLOUDCLIENT_H
#define ROSPOINTCLOUDCLIENT_H

#include <QObject>
#include <QWebSocket>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
#include <vector>
#include <QVector3D>
#include <QQuaternion>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

/**
 * @brief ROS Point Cloud Client using rosbridge_server protocol
 *
 * Connects to rosbridge WebSocket (default port 9090) and subscribes to:
 *   - /path              (nav_msgs/Path)             → pathReceived
 *   - /Odometry          (nav_msgs/Odometry)         → robotPoseReceived
 *
 * Point cloud data (/cloud_registered) is NOT handled here — it moved to
 * its own binary connection, see PointCloudClient.h. Telemetry
 * (/telemetry) is NOT handled here either — see TelemetryClient.h: sharing
 * this connection let big /path/Odometry payloads head-of-line block small,
 * latency-sensitive telemetry on constrained real links. Same reasoning as
 * RtspVideoClient: an isolated process/port/wire-format so a heavy stream
 * never contends with another one's small, latency-sensitive traffic.
 *
 * rosbridge protocol (JSON text messages):
 *   Subscribe:  {"op":"subscribe","topic":"/topic","type":"msg_type"}
 *   Receive:    {"op":"publish","topic":"/topic","msg":{...}}
 */

struct RobotPose {
    QVector3D   position;
    QQuaternion orientation;
    RobotPose() : position(0,0,0), orientation(1,0,0,0) {}
};

class RosPointCloudClient : public QObject
{
    Q_OBJECT

public:
    explicit RosPointCloudClient(QObject *parent = nullptr);
    ~RosPointCloudClient();

    void connectToServer(const QString &url);
    void disconnect();
    bool isConnected() const;

signals:
    void pathReceived(const std::vector<QVector3D> &poses);
    void robotPoseReceived(const RobotPose &pose);

    void connected();
    void disconnected();
    void error(const QString &errorString);

    // Round-trip time of the last WebSocket ping/pong, in milliseconds.
    // Uses the WebSocket protocol's own ping frames (RFC 6455), not a
    // custom rosbridge message, so this works against any WS server —
    // the mock and a real rosbridge alike — without protocol changes.
    void latencyMeasured(qint64 ms);

    // Measured over the previous ~1s window; kbps (kilobits/sec).
    void bandwidthUpdated(double downKbps, double upKbps);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString &message);
    void onError(QAbstractSocket::SocketError error);
    void onPong(quint64 elapsedTime, const QByteArray &payload);
    void onPingTimer();
    void onStatsTimer();

private:
    void sendSubscribe(const QString &topic, const QString &type);
    void processPath(const QJsonObject &msg);
    void processOdometry(const QJsonObject &msg);

    QWebSocket m_webSocket;

    QTimer *m_pingTimer = nullptr;   // periodic RTT probe while connected
    QTimer *m_statsTimer = nullptr;  // 1Hz bandwidth window
    qint64 m_bytesReceivedAccum = 0;
    qint64 m_bytesSentAccum = 0;
};

#endif // ROSPOINTCLOUDCLIENT_H
