#ifndef ROSIMAGECLIENT_H
#define ROSIMAGECLIENT_H

#include <QObject>
#include <QWebSocket>
#include <QImage>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFutureWatcher>

/**
 * @brief ROS Image Client using rosbridge_server protocol
 *
 * Connects to rosbridge WebSocket (default port 9090) and subscribes to:
 *   - /image_raw   (sensor_msgs/Image)   → imageReceived
 *
 * rosbridge encodes the "data" field of sensor_msgs/Image as base64.
 * The image is reconstructed from the raw pixel buffer using the
 * encoding, width, height, and step fields in the message.
 *
 * Common encodings supported:
 *   "rgb8"  – 3 bytes/pixel, R G B
 *   "bgr8"  – 3 bytes/pixel, B G R
 *   "mono8" – 1 byte/pixel, greyscale
 *   "rgba8" – 4 bytes/pixel
 */

class RosImageClient : public QObject
{
    Q_OBJECT

public:
    explicit RosImageClient(QObject *parent = nullptr);
    ~RosImageClient();

    void connectToServer(const QString &url);
    void disconnect();
    bool isConnected() const;

signals:
    void imageReceived(const QImage &image);

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

    // Parses the full rosbridge JSON envelope (the expensive part for
    // megabyte-scale base64 image payloads) and decodes the embedded
    // sensor_msgs/Image into a QImage. Pure function, safe to run on a
    // worker thread via QtConcurrent.
    static QImage decodeImage(const QByteArray &rawMessageUtf8);

    // Kicks off a decode of m_pendingMsg if one isn't already running.
    // Called after a new message arrives and after a decode finishes, so
    // that decoding always jumps straight to the newest message instead of
    // working through a backlog in order.
    void startNextDecode();

    QWebSocket m_webSocket;

    // LATENCY FIX: previously every /image_raw message was base64-decoded,
    // JSON-parsed into a QImage, and smooth-scaled synchronously on the GUI
    // thread, one at a time, in arrival order. If that per-frame work took
    // longer than the ~100ms publish interval (easily the case for
    // megabyte-scale base64 JSON on a phone CPU), messages piled up faster
    // than they could be processed and the app kept rendering older and
    // older frames — a backlog that grows over time, not network latency.
    // Now: only the single newest message is ever decoded (older ones
    // received while a decode is in flight are dropped), and decoding runs
    // off the GUI thread so the OpenGL point-cloud repaint can't stall it.
    QByteArray m_pendingMsg;
    bool m_hasPending = false;
    bool m_decoding = false;
    QFutureWatcher<QImage> m_decodeWatcher;
};

#endif // ROSIMAGECLIENT_H
