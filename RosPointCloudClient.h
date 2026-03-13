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

/**
 * @brief ROS Point Cloud Client using rosbridge_server protocol
 *
 * Connects to rosbridge WebSocket (default port 9090) and subscribes to:
 *   - /cloud_registered  (sensor_msgs/PointCloud2)  → pointCloudReceived
 *   - /path              (nav_msgs/Path)             → pathReceived
 *   - /Odometry          (nav_msgs/Odometry)         → robotPoseReceived
 *   - /telemetry         (custom JSON object)        → telemetryReceived
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
    void pointCloudReceived(const std::vector<float> &points);
    void pathReceived(const std::vector<QVector3D> &poses);
    void robotPoseReceived(const RobotPose &pose);
    // telemetry signal kept for API compatibility – not used with rosbridge
    void telemetryReceived(int battery, int signal, const QString &mode);

    void connected();
    void disconnected();
    void error(const QString &errorString);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString &message);
    void onError(QAbstractSocket::SocketError error);

private:
    void sendSubscribe(const QString &topic, const QString &type);
    void processPointCloud2(const QJsonObject &msg);
    void processPath(const QJsonObject &msg);
    void processOdometry(const QJsonObject &msg);
    void processTelemetry(const QJsonObject &msg);

    QWebSocket m_webSocket;
};

#endif // ROSPOINTCLOUDCLIENT_H
