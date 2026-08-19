#ifndef TELEMETRYCLIENT_H
#define TELEMETRYCLIENT_H

#include <QObject>
#include <QWebSocket>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

/**
 * @brief Telemetry downlink client — own connection, own port.
 *
 * Talks to skyline_app.py's dedicated telemetry websocket on :9093, split
 * off from the /path + /Odometry connection on :9090 (see skyline_app.py's
 * module docstring). /path can resend its entire recent pose history every
 * message and /Odometry streams at up to 20Hz with full covariance arrays —
 * fine on a fast link, but on a constrained real RF downlink that traffic
 * can saturate the pipe. Since TCP is strictly in-order, small/cheap
 * telemetry messages sharing that same socket would queue up behind an
 * ever-refilling backlog of pose data and could go stale indefinitely (the
 * app would still show "Connected & Streaming" since /path keeps arriving,
 * just delayed, while battery/mode/GPS never update). A dedicated
 * connection means telemetry can never be head-of-line blocked by
 * path/odometry traffic, no matter how congested the link gets.
 *
 * connectToServer() takes the same "ws://<drone-ip>:9090" URL used
 * everywhere else in the app — only the host is reused, the port is
 * swapped to 9093 (same pattern PointCloudClient uses for 9092,
 * RtspVideoClient for 8554, and UpdateClient for 9091).
 *
 * Wire format: raw JSON text, one object per message, no rosbridge
 * "op"/"topic"/"msg" wrapper and no subscribe handshake — this server only
 * ever pushes one message type to every connected client:
 *   {"battery": 87, "signal": 71, "mode": "AUTO", "voltage": 11.4,
 *    "current": 3.2, "armed": true, ...}
 * battery/signal/voltage/current are -1 when not yet known; mode is the
 * only field required to be present and non-empty.
 */
class TelemetryClient : public QObject
{
    Q_OBJECT

public:
    explicit TelemetryClient(QObject *parent = nullptr);
    ~TelemetryClient();

    void connectToServer(const QString &url);
    void disconnect();
    bool isConnected() const;

signals:
    void telemetryReceived(int battery, int signal, const QString &mode,
                            double voltageV, double currentA, bool armed);

    void connected();
    void disconnected();
    void error(const QString &errorString);

    // Round-trip time of the last WebSocket ping/pong, in milliseconds.
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
    static const quint16 kPort = 9093;

    QWebSocket m_webSocket;

    QTimer *m_pingTimer = nullptr;
    QTimer *m_statsTimer = nullptr;
    qint64 m_bytesReceivedAccum = 0;
};

#endif // TELEMETRYCLIENT_H
