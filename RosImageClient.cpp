#include "RosImageClient.h"
#include <QDebug>
#include <QByteArray>

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
    qDebug() << "RosImageClient: connected – subscribing to /image_raw";
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
    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError) {
        qWarning() << "RosImageClient: JSON parse error:" << parseErr.errorString();
        return;
    }

    QJsonObject root = doc.object();
    if (root["op"].toString() != "publish") return;
    if (root["topic"].toString() != "/image_raw") return;

    processImage(root["msg"].toObject());
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
    sub["throttle_rate"] = 100;  // ~10 Hz max to avoid flooding
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
void RosImageClient::processImage(const QJsonObject &msg)
{
    QString encoding = msg["encoding"].toString();
    int width        = msg["width"].toInt(0);
    int height       = msg["height"].toInt(0);
    int step         = msg["step"].toInt(0);
    QString b64data  = msg["data"].toString();

    if (width <= 0 || height <= 0 || b64data.isEmpty()) {
        qWarning() << "RosImageClient: invalid image message (w=" << width
                   << " h=" << height << " data_empty=" << b64data.isEmpty() << ")";
        return;
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
        emit imageReceived(img.copy());
        return;
    } else {
        qWarning() << "RosImageClient: unsupported encoding:" << encoding
                   << "– trying rgb8 fallback";
        fmt = QImage::Format_RGB888;
    }

    if (raw.size() < step * height) {
        qWarning() << "RosImageClient: data too short for image dimensions";
        return;
    }

    QImage img(reinterpret_cast<const uchar*>(raw.constData()),
               width, height, step, fmt);

    if (needSwapRB)
        img = img.rgbSwapped();

    // .copy() detaches from the raw buffer so the QByteArray can be freed
    emit imageReceived(img.copy());
}
