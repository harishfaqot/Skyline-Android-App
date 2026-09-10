#include "TelemetryClient.h"
#include <QDebug>
#include <QUrl>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>

TelemetryClient::TelemetryClient(QObject *parent)
    : QObject(parent)
{
    connect(&m_webSocket, &QWebSocket::connected,
            this, &TelemetryClient::onConnected);
    connect(&m_webSocket, &QWebSocket::disconnected,
            this, &TelemetryClient::onDisconnected);
    connect(&m_webSocket, &QWebSocket::textMessageReceived,
            this, &TelemetryClient::onTextMessageReceived);
    connect(&m_webSocket,
            QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &TelemetryClient::onError);
    connect(&m_webSocket, &QWebSocket::pong, this, &TelemetryClient::onPong);

    m_pingTimer = new QTimer(this);
    m_pingTimer->setInterval(2000);
    connect(m_pingTimer, &QTimer::timeout, this, &TelemetryClient::onPingTimer);

    m_statsTimer = new QTimer(this);
    m_statsTimer->setInterval(1000);
    connect(m_statsTimer, &QTimer::timeout, this, &TelemetryClient::onStatsTimer);
}

TelemetryClient::~TelemetryClient()
{
    disconnect();
}

void TelemetryClient::connectToServer(const QString &url)
{
    const QString host = QUrl(url).host();
    if (host.isEmpty()) {
        qWarning() << "TelemetryClient: no host in URL" << url;
        return;
    }

    const QString telUrl = QStringLiteral("ws://%1:%2").arg(host).arg(kPort);
    qDebug() << "TelemetryClient: connecting to" << telUrl;
    m_webSocket.open(QUrl(telUrl));
}

void TelemetryClient::disconnect()
{
    if (m_webSocket.state() == QAbstractSocket::ConnectedState)
        m_webSocket.close();
}

bool TelemetryClient::isConnected() const
{
    return m_webSocket.state() == QAbstractSocket::ConnectedState;
}

void TelemetryClient::setRecording(bool enabled)
{
    if (m_webSocket.state() != QAbstractSocket::ConnectedState) {
        qWarning() << "TelemetryClient: setRecording() called while disconnected, dropped";
        return;
    }
    QJsonObject cmd;
    cmd["cmd"] = "set_recording";
    cmd["enabled"] = enabled;
    m_webSocket.sendTextMessage(QJsonDocument(cmd).toJson(QJsonDocument::Compact));
}

void TelemetryClient::onConnected()
{
    qDebug() << "TelemetryClient: connected";
    m_bytesReceivedAccum = 0;
    m_pingTimer->start();
    m_statsTimer->start();
    onPingTimer();
    emit connected();
}

void TelemetryClient::onDisconnected()
{
    qDebug() << "TelemetryClient: disconnected";
    m_pingTimer->stop();
    m_statsTimer->stop();
    emit latencyMeasured(-1);
    emit bandwidthUpdated(0.0, 0.0);
    emit disconnected();
}

void TelemetryClient::onTextMessageReceived(const QString &message)
{
    m_bytesReceivedAccum += message.toUtf8().size();

    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        qWarning() << "TelemetryClient: JSON parse error:" << parseErr.errorString();
        return;
    }

    const QJsonObject payload = doc.object();

    const int battery = payload.value("battery").toInt(-1);
    const int signal  = payload.value("signal").toInt(-1);
    const QString mode = payload.value("mode").toString();
    const double voltage = payload.value("voltage").toDouble(-1.0);
    const double current = payload.value("current").toDouble(-1.0);
    const bool armed = payload.value("armed").toBool(false);

    if (mode.isEmpty()) {
        return;
    }

    emit telemetryReceived(battery, signal, mode, voltage, current, armed);

    if (payload.contains("recording")) {
        emit recordingStateChanged(payload.value("recording").toBool(false));
    }

    const QJsonArray messages = payload.value("messages").toArray();
    for (const QJsonValue &v : messages) {
        const QJsonObject m = v.toObject();
        emit statusMessageReceived(m.value("severity").toInt(6), m.value("text").toString());
    }
}

void TelemetryClient::onError(QAbstractSocket::SocketError err)
{
    const QString errStr = m_webSocket.errorString();
    qWarning() << "TelemetryClient: WebSocket error:" << err << "-" << errStr;
    emit error(errStr);
}

void TelemetryClient::onPong(quint64 elapsedTime, const QByteArray &payload)
{
    Q_UNUSED(payload);
    emit latencyMeasured(static_cast<qint64>(elapsedTime));
}

void TelemetryClient::onPingTimer()
{
    if (m_webSocket.state() == QAbstractSocket::ConnectedState) {
        m_webSocket.ping();
    }
}

void TelemetryClient::onStatsTimer()
{
    const double downKbps = (m_bytesReceivedAccum * 8) / 1000.0;
    m_bytesReceivedAccum = 0;
    emit bandwidthUpdated(downKbps, 0.0); // receive-only link, no meaningful uplink to track
}
