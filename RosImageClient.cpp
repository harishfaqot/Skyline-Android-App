#include "RosImageClient.h"
#include <QDebug>
#include <QByteArray>
#include <QtConcurrent/QtConcurrent>

RosImageClient::RosImageClient(QObject *parent)
    : QObject(parent)
{
    connect(&m_webSocket, &QWebSocket::connected,
            this, &RosImageClient::onConnected);
    connect(&m_webSocket, &QWebSocket::disconnected,
            this, &RosImageClient::onDisconnected);
    connect(&m_webSocket, &QWebSocket::textMessageReceived,
            this, &RosImageClient::onTextMessageReceived);
    connect(&m_webSocket,
            QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &RosImageClient::onError);

    connect(&m_decodeWatcher, &QFutureWatcher<QImage>::finished, this, [this]() {
        QImage img = m_decodeWatcher.result();
        m_decoding = false;
        if (!img.isNull())
            emit imageReceived(img);
        // Jump straight to whatever is newest now, dropping anything that
        // arrived and was superseded while this decode was running.
        startNextDecode();
    });
}

RosImageClient::~RosImageClient()
{
    disconnect();
}

void RosImageClient::connectToServer(const QString &url)
{
    qDebug() << "RosImageClient: connecting to rosbridge at" << url;
    m_webSocket.open(QUrl(url));
}

void RosImageClient::disconnect()
{
    if (m_webSocket.state() == QAbstractSocket::ConnectedState)
        m_webSocket.close();
}

bool RosImageClient::isConnected() const
{
    return m_webSocket.state() == QAbstractSocket::ConnectedState;
}

// ============================================================================
// Slots
// ============================================================================

void RosImageClient::onConnected()
{
    qDebug() << "RosImageClient: connected – subscribing to image topics";
    // Primary path: compressed (JPEG) frames. This is what image_transport
    // publishes on <topic>/compressed on a real robot, and what
    // rosbridge_mock.py now sends too. A JPEG frame is typically 10-20x
    // smaller than the equivalent raw rgb8 frame, which is what actually
    // lets us hit high fps over a constrained/emulated network link instead
    // of building up a multi-second backlog.
    sendSubscribe("/image_raw/compressed", "sensor_msgs/CompressedImage");
    // Fallback: some robots only publish raw (no compressed transport
    // enabled). If both arrive we just render whichever frame is newest.
    sendSubscribe("/image_raw", "sensor_msgs/Image");
    emit connected();
}

void RosImageClient::onDisconnected()
{
    qDebug() << "RosImageClient: disconnected";
    emit disconnected();
}

void RosImageClient::onTextMessageReceived(const QString &message)
{
    // Cheap pre-filter so we don't burn GUI-thread time on topics we don't
    // care about — full JSON parsing (the expensive step for a ~1MB+
    // base64-encoded frame) happens on a worker thread in decodeImage().
    if (!message.contains(QLatin1String("/image_raw")))
        return; // matches both "/image_raw" and "/image_raw/compressed"

    // Always keep only the newest message. If a decode is already running,
    // this just overwrites m_pendingMsg — the in-flight decode still gets
    // emitted (it's already almost done), but any frame(s) that arrive
    // while it's running are dropped rather than queued, so we never fall
    // further behind arrival rate. This is what actually caps the latency:
    // previously every frame was parsed+decoded on the GUI thread in strict
    // arrival order with nothing dropped, so if a single frame's JSON
    // parse + base64 decode + smooth-scale took longer than the ~100ms
    // publish interval, a growing backlog built up over time.
    m_pendingMsg = message.toUtf8();
    m_hasPending = true;
    startNextDecode();
}

void RosImageClient::startNextDecode()
{
    if (m_decoding || !m_hasPending) return;

    QByteArray raw = m_pendingMsg;
    m_hasPending = false;
    m_decoding = true;

    m_decodeWatcher.setFuture(QtConcurrent::run(&RosImageClient::decodeImage, raw));
}

void RosImageClient::onError(QAbstractSocket::SocketError err)
{
    QString errStr = m_webSocket.errorString();
    qWarning() << "RosImageClient: WebSocket error:" << err << "-" << errStr;
    emit error(errStr);
}

// ============================================================================
// Helpers
// ============================================================================

void RosImageClient::sendSubscribe(const QString &topic, const QString &type)
{
    QJsonObject sub;
    sub["op"]           = "subscribe";
    sub["topic"]        = topic;
    sub["type"]         = type;
    // Was 100ms (~10 Hz), which silently capped fps regardless of how fast
    // the publisher/network could actually go. 16ms (~60 Hz) removes that
    // artificial ceiling; queue_length=1 tells rosbridge itself to drop
    // backlog rather than queue it server-side too (mirrors the drop-stale
    // logic in rosbridge_mock.py and in this client's decode path below).
    sub["throttle_rate"] = 16;
    sub["queue_length"]  = 1;

    m_webSocket.sendTextMessage(
        QString::fromUtf8(QJsonDocument(sub).toJson(QJsonDocument::Compact)));
}

// ============================================================================
// sensor_msgs/Image parsing
//
// rosbridge sends:
// {
//   "encoding": "rgb8",   // or "bgr8", "mono8", "rgba8", "bgra8", etc.
//   "width":  640,
//   "height": 480,
//   "step":   1920,       // bytes per row
//   "data":   "<base64>"
// }
// ============================================================================
// Runs on a worker thread (via QtConcurrent::run) — must not touch any
// GUI/widget state, only pure data in -> QImage out.
QImage RosImageClient::decodeImage(const QByteArray &rawMessageUtf8)
{
    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(rawMessageUtf8, &parseErr);
    if (parseErr.error != QJsonParseError::NoError) {
        qWarning() << "RosImageClient: JSON parse error:" << parseErr.errorString();
        return QImage();
    }

    QJsonObject root = doc.object();
    if (root["op"].toString() != "publish") return QImage();
    QString topic = root["topic"].toString();
    if (topic != "/image_raw" && topic != "/image_raw/compressed") return QImage();

    QJsonObject msg = root["msg"].toObject();

    // sensor_msgs/CompressedImage: {header, format: "jpeg"/"png", data}.
    // No width/height/step/encoding fields — Qt's JPEG/PNG decoder infers
    // dimensions from the compressed data itself, and this path is both
    // far cheaper (no manual pixel unpacking) and far smaller over the wire
    // than the raw path below.
    if (topic == "/image_raw/compressed" || msg.contains("format")) {
        QByteArray compressed = QByteArray::fromBase64(msg["data"].toString().toUtf8());
        if (compressed.isEmpty()) {
            qWarning() << "RosImageClient: empty compressed image payload";
            return QImage();
        }
        QImage img = QImage::fromData(compressed); // auto-detects jpeg/png
        if (img.isNull())
            qWarning() << "RosImageClient: failed to decode compressed image"
                       << "(format=" << msg["format"].toString() << ")";
        return img;
    }

    QString encoding = msg["encoding"].toString();
    int width        = msg["width"].toInt(0);
    int height       = msg["height"].toInt(0);
    int step         = msg["step"].toInt(0);
    QString b64data  = msg["data"].toString();

    if (width <= 0 || height <= 0 || b64data.isEmpty()) {
        qWarning() << "RosImageClient: invalid image message (w=" << width
                   << " h=" << height << " data_empty=" << b64data.isEmpty() << ")";
        return QImage();
    }

    QByteArray raw = QByteArray::fromBase64(b64data.toUtf8());

    QImage::Format fmt = QImage::Format_Invalid;
    bool needSwapRB = false;

    if (encoding == "rgb8" || encoding == "RGB8") {
        fmt = QImage::Format_RGB888;
    } else if (encoding == "bgr8" || encoding == "BGR8") {
        fmt = QImage::Format_RGB888;
        needSwapRB = true;
    } else if (encoding == "rgba8" || encoding == "RGBA8") {
        fmt = QImage::Format_RGBA8888;
    } else if (encoding == "bgra8" || encoding == "BGRA8") {
        fmt = QImage::Format_RGBA8888;
        needSwapRB = true;
    } else if (encoding == "mono8" || encoding == "8UC1") {
        fmt = QImage::Format_Grayscale8;
    } else if (encoding == "mono16" || encoding == "16UC1") {
        // Convert 16-bit mono to 8-bit greyscale
        fmt = QImage::Format_Grayscale8;
        int numPixels = width * height;
        QByteArray buf(numPixels, '\0');
        const uint16_t *src = reinterpret_cast<const uint16_t*>(raw.constData());
        uint8_t *dst = reinterpret_cast<uint8_t*>(buf.data());
        for (int i = 0; i < numPixels && (i * 2 + 1) < raw.size(); ++i)
            dst[i] = (uint8_t)(src[i] >> 8);
        QImage img(reinterpret_cast<const uchar*>(buf.constData()),
                   width, height, width, QImage::Format_Grayscale8);
        return img.copy();
    } else {
        qWarning() << "RosImageClient: unsupported encoding:" << encoding
                   << "– trying rgb8 fallback";
        fmt = QImage::Format_RGB888;
    }

    if (raw.size() < step * height) {
        qWarning() << "RosImageClient: data too short for image dimensions";
        return QImage();
    }

    QImage img(reinterpret_cast<const uchar*>(raw.constData()),
               width, height, step, fmt);

    if (needSwapRB)
        img = img.rgbSwapped();

    // .copy() detaches from the raw buffer so it's safe once this
    // function's local QByteArray goes out of scope.
    return img.copy();
}
