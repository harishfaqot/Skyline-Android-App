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
 *    "current": 3.2, "armed": true, "recording": false,
 *    "messages": [{"severity": 4, "text": "EKF3 IMU0 forced reset"}],
 *    "file_events": [{"type": "video", "path": "/mnt/skyline/.../video.avi"}], ...}
 * battery/signal/voltage/current are -1 when not yet known; mode is the
 * only field required to be present and non-empty. "recording" is the
 * server's authoritative recording state (see setRecording() below);
 * "messages" is the list of new ArduPilot STATUSTEXT lines (QGC "Messages"
 * panel equivalent) since the previous packet — usually empty; "file_events"
 * is the list of video/photo saves recording_manager.py reported since the
 * previous packet (see fileSaved() below) — also usually empty.
 *
 * This is the one connection in the app that isn't purely a downlink: it
 * also carries the REC button's command uplink (see setRecording()) back to
 * skyline_app.py, which republishes it onto ROS. Piggybacking on an
 * already-open, low-traffic socket was simpler than opening a dedicated
 * port for one small, infrequent, latency-insensitive command.
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

    // Sends {"cmd":"set_recording","enabled":enabled} upstream. Fire-and-
    // forget — the actual recording state is only known for sure once it
    // comes back in a subsequent telemetryReceived()'s "recording" field
    // (see recordingStateChanged()), not from this call succeeding.
    void setRecording(bool enabled);

    // Sends {"cmd":"capture_photo"} upstream — recording_manager.py saves
    // one native-resolution frame straight from the Jetson's camera (see
    // that script's capture_photo_cb) to its own /mnt/skyline/photos/
    // file. Fire-and-forget, same as setRecording() but with no
    // confirmation path back at all (there's no ROS link from
    // video_rtsp_server.py, which does the actual write, back to
    // recording_manager.py — see that script's module docstring), so
    // there's nothing to wire a "photo saved" signal here to.
    void capturePhoto();

signals:
    void telemetryReceived(int battery, int signal, const QString &mode,
                            double voltageV, double currentA, bool armed);

    // Emitted whenever a packet carries a "recording" field — the server's
    // confirmed recording state (see recording_manager.py), not just an
    // echo of what the app last asked for.
    void recordingStateChanged(bool recording);

    // One emission per new ArduPilot STATUSTEXT line (QGC "Messages" panel
    // equivalent). severity follows MAVLink MAV_SEVERITY (0=EMERGENCY ...
    // 7=DEBUG, lower is more severe).
    void statusMessageReceived(int severity, const QString &text);

    // One emission per file_events entry — the Jetson-side path
    // recording_manager.py wrote (or, for video, will mux to — see that
    // script's module docstring) a video or photo to. type is "video" or
    // "photo".
    void fileSaved(const QString &type, const QString &path);

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
