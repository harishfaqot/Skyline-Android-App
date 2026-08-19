#include "PointCloudClient.h"
#include <QDebug>
#include <QUrl>
#include <QTimer>
#include <cstring>
#include <cmath>
#include <cstdint>

// Must match pointcloud_ws_server.py's HEADER struct ("<BIIII") exactly.
namespace {
constexpr int kHeaderSize = 1 + 4 * 4; // uint8 + 4x uint32
constexpr quint8 kWireVersion = 1;

quint32 readU32LE(const char *p)
{
    return static_cast<quint32>(static_cast<quint8>(p[0]))
         | (static_cast<quint32>(static_cast<quint8>(p[1])) << 8)
         | (static_cast<quint32>(static_cast<quint8>(p[2])) << 16)
         | (static_cast<quint32>(static_cast<quint8>(p[3])) << 24);
}
} // namespace

PointCloudClient::PointCloudClient(QObject *parent)
    : QObject(parent)
{
    connect(&m_webSocket, &QWebSocket::connected,
            this, &PointCloudClient::onConnected);
    connect(&m_webSocket, &QWebSocket::disconnected,
            this, &PointCloudClient::onDisconnected);
    connect(&m_webSocket, &QWebSocket::binaryMessageReceived,
            this, &PointCloudClient::onBinaryMessageReceived);
    connect(&m_webSocket,
            QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &PointCloudClient::onError);
    connect(&m_webSocket, &QWebSocket::pong, this, &PointCloudClient::onPong);

    m_pingTimer = new QTimer(this);
    m_pingTimer->setInterval(2000);
    connect(m_pingTimer, &QTimer::timeout, this, &PointCloudClient::onPingTimer);

    m_statsTimer = new QTimer(this);
    m_statsTimer->setInterval(1000);
    connect(m_statsTimer, &QTimer::timeout, this, &PointCloudClient::onStatsTimer);
}

PointCloudClient::~PointCloudClient()
{
    disconnect();
}

void PointCloudClient::connectToServer(const QString &url)
{
    const QString host = QUrl(url).host();
    if (host.isEmpty()) {
        qWarning() << "PointCloudClient: no host in URL" << url;
        return;
    }

    const QString pcUrl = QStringLiteral("ws://%1:%2").arg(host).arg(kPort);
    qDebug() << "PointCloudClient: connecting to" << pcUrl;
    m_webSocket.open(QUrl(pcUrl));
}

void PointCloudClient::disconnect()
{
    if (m_webSocket.state() == QAbstractSocket::ConnectedState)
        m_webSocket.close();
}

bool PointCloudClient::isConnected() const
{
    return m_webSocket.state() == QAbstractSocket::ConnectedState;
}

void PointCloudClient::onConnected()
{
    qDebug() << "PointCloudClient: connected";
    m_bytesReceivedAccum = 0;
    m_pingTimer->start();
    m_statsTimer->start();
    onPingTimer();
    emit connected();
}

void PointCloudClient::onDisconnected()
{
    qDebug() << "PointCloudClient: disconnected";
    m_pingTimer->stop();
    m_statsTimer->stop();
    emit latencyMeasured(-1);
    emit bandwidthUpdated(0.0, 0.0);
    emit disconnected();
}

void PointCloudClient::onBinaryMessageReceived(const QByteArray &message)
{
    m_bytesReceivedAccum += message.size();

    if (message.size() < kHeaderSize) {
        qWarning() << "PointCloudClient: frame too small for header," << message.size() << "bytes";
        return;
    }

    const char *data = message.constData();
    const quint8 version = static_cast<quint8>(data[0]);
    if (version != kWireVersion) {
        qWarning() << "PointCloudClient: unexpected wire version" << version;
        return;
    }
    const quint32 numPoints = readU32LE(data + 13); // skip version(1) + seq(4) + secs(4) + nsecs(4)

    const qint64 expectedSize = static_cast<qint64>(kHeaderSize) + static_cast<qint64>(numPoints) * 3 * sizeof(float);
    if (message.size() < expectedSize) {
        qWarning() << "PointCloudClient: frame truncated, expected" << expectedSize << "got" << message.size();
        return;
    }

    std::vector<float> points;
    points.reserve(numPoints * 3);

    // kHeaderSize (17) isn't 4-byte aligned, so the xyz payload right after
    // it isn't guaranteed float-aligned either — reinterpret_casting a
    // pointer into it and dereferencing would be undefined behavior (and a
    // real fault, not just a lint nit, on ARM/Android). memcpy per float
    // works regardless of alignment.
    const char *cursor = data + kHeaderSize;
    for (quint32 i = 0; i < numPoints; ++i) {
        float x, y, z;
        std::memcpy(&x, cursor + 0, sizeof(float));
        std::memcpy(&y, cursor + 4, sizeof(float));
        std::memcpy(&z, cursor + 8, sizeof(float));
        cursor += 3 * sizeof(float);
        // Cheap safety net in case of an encoding bug upstream — the server
        // already filters non-finite points before sending.
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
        points.push_back(x);
        points.push_back(y);
        points.push_back(z);
    }

    if (!points.empty())
        emit pointCloudReceived(points);
}

void PointCloudClient::onError(QAbstractSocket::SocketError err)
{
    const QString errStr = m_webSocket.errorString();
    qWarning() << "PointCloudClient: WebSocket error:" << err << "-" << errStr;
    emit error(errStr);
}

void PointCloudClient::onPong(quint64 elapsedTime, const QByteArray &payload)
{
    Q_UNUSED(payload);
    emit latencyMeasured(static_cast<qint64>(elapsedTime));
}

void PointCloudClient::onPingTimer()
{
    if (m_webSocket.state() == QAbstractSocket::ConnectedState) {
        m_webSocket.ping();
    }
}

void PointCloudClient::onStatsTimer()
{
    const double downKbps = (m_bytesReceivedAccum * 8) / 1000.0;
    m_bytesReceivedAccum = 0;
    emit bandwidthUpdated(downKbps, 0.0); // receive-only link, no meaningful uplink to track
}
