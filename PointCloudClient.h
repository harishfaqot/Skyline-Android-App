#ifndef POINTCLOUDCLIENT_H
#define POINTCLOUDCLIENT_H

#include <QObject>
#include <QWebSocket>
#include <vector>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

/**
 * @brief Point-cloud downlink client — binary WebSocket frames, own port.
 *
 * Talks to pointcloud_ws_server.py (skyline_ws/src/skyline/src/), a
 * standalone process separate from skyline_app.py's rosbridge-style
 * websocket on :9090 — same reasoning as RtspVideoClient being separate
 * from the telemetry link: an isolated process/port means a big point
 * cloud can never contend (CPU or bandwidth) with small, latency-sensitive
 * telemetry/path traffic, or with the RTSP video stream sharing the same
 * physical downlink.
 *
 * connectToServer() takes the same "ws://<drone-ip>:9090" URL used
 * everywhere else in the app — only the host is reused, the port is
 * swapped to the point-cloud server's 9092 (same pattern RtspVideoClient
 * already uses for 8554, and UpdateClient for 9091).
 *
 * Wire format (binary WebSocket frames, little-endian) — must match
 * pointcloud_ws_server.py's HEADER struct exactly:
 *   uint8  version   (1)
 *   uint32 seq
 *   uint32 stamp_secs
 *   uint32 stamp_nsecs
 *   uint32 num_points
 *   float32[num_points * 3]   x,y,z interleaved
 *
 * No JSON, no base64, no subscribe handshake — the server only ever
 * broadcasts this one message type to every connected client.
 */
class PointCloudClient : public QObject
{
    Q_OBJECT

public:
    explicit PointCloudClient(QObject *parent = nullptr);
    ~PointCloudClient();

    void connectToServer(const QString &url);
    void disconnect();
    bool isConnected() const;

signals:
    void pointCloudReceived(const std::vector<float> &points);

    void connected();
    void disconnected();
    void error(const QString &errorString);

    // Round-trip time of the last WebSocket ping/pong, in milliseconds.
    void latencyMeasured(qint64 ms);

    // Measured over the previous ~1s window; kbps (kilobits/sec). Folded
    // into MainWindow's total downlink figure alongside the RTSP stream.
    void bandwidthUpdated(double downKbps, double upKbps);

private slots:
    void onConnected();
    void onDisconnected();
    void onBinaryMessageReceived(const QByteArray &message);
    void onError(QAbstractSocket::SocketError error);
    void onPong(quint64 elapsedTime, const QByteArray &payload);
    void onPingTimer();
    void onStatsTimer();

private:
    static const quint16 kPort = 9092;

    QWebSocket m_webSocket;

    QTimer *m_pingTimer = nullptr;
    QTimer *m_statsTimer = nullptr;
    qint64 m_bytesReceivedAccum = 0;
};

#endif // POINTCLOUDCLIENT_H
