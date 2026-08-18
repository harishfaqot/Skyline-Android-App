#include "UpdateClient.h"
#include <QDebug>
#include <QJsonDocument>
#include <qmath.h>

namespace {
constexpr qint64 kUploadChunkSize = 256 * 1024;
}

UpdateClient::UpdateClient(QObject *parent)
    : QObject(parent)
{
    connect(&m_webSocket, &QWebSocket::connected,
            this, &UpdateClient::onConnected);
    connect(&m_webSocket, &QWebSocket::disconnected,
            this, &UpdateClient::onDisconnected);
    connect(&m_webSocket, &QWebSocket::textMessageReceived,
            this, &UpdateClient::onTextMessageReceived);
    connect(&m_webSocket, &QWebSocket::bytesWritten,
            this, &UpdateClient::onBytesWritten);
    connect(&m_webSocket,
            QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &UpdateClient::onError);
}

UpdateClient::~UpdateClient()
{
    disconnect();
}

void UpdateClient::connectToServer(const QString &url)
{
    qDebug() << "UpdateClient: connecting to update agent at" << url;
    m_webSocket.open(QUrl(url));
}

void UpdateClient::disconnect()
{
    if (m_webSocket.state() == QAbstractSocket::ConnectedState)
        m_webSocket.close();
}

bool UpdateClient::isConnected() const
{
    return m_webSocket.state() == QAbstractSocket::ConnectedState;
}

void UpdateClient::requestCheck()
{
    QJsonObject req;
    req["op"] = "check";
    m_webSocket.sendTextMessage(
        QString::fromUtf8(QJsonDocument(req).toJson(QJsonDocument::Compact)));
}

void UpdateClient::uploadArchive(const QByteArray &data, const QString &sha256,
                                  const QString &commit, const QString &date, const QString &subject)
{
    m_uploadData = data;
    m_uploadOffset = 0;
    m_uploadAcked = 0;

    QJsonObject req;
    req["op"] = "upload_begin";
    req["size"] = data.size();
    req["sha256"] = sha256;
    req["commit"] = commit;
    req["date"] = date;
    req["subject"] = subject;
    m_webSocket.sendTextMessage(
        QString::fromUtf8(QJsonDocument(req).toJson(QJsonDocument::Compact)));
}

// ============================================================================
// Slots
// ============================================================================

void UpdateClient::onConnected()
{
    qDebug() << "UpdateClient: connected to update agent";
    emit connected();
}

void UpdateClient::onDisconnected()
{
    qDebug() << "UpdateClient: disconnected from update agent";
    emit disconnected();
}

void UpdateClient::onTextMessageReceived(const QString &message)
{
    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &parseErr);
    if (parseErr.error != QJsonParseError::NoError) {
        qWarning() << "UpdateClient: JSON parse error:" << parseErr.errorString();
        return;
    }

    QJsonObject root = doc.object();
    const QString op = root.value("op").toString();

    if (op == "check_result") {
        emit checkResult(parseVersionInfo(root.value("current").toObject()));
    } else if (op == "upload_ready") {
        emit uploadReady();
        sendNextChunk();
    } else if (op == "status") {
        const QString stage = root.value("stage").toString();
        const QString message2 = root.value("message").toString();
        const QString line = root.value("line").isNull() ? QString() : root.value("line").toString();
        emit statusUpdate(stage, message2, line);
    } else if (op == "error") {
        emit requestFailed(root.value("message").toString());
    }
}

void UpdateClient::onBytesWritten(qint64 bytes)
{
    if (m_uploadAcked <= 0 && bytes <= 0) return;
    m_uploadAcked += bytes;
    emit uploadProgress(m_uploadAcked, m_uploadOffset);
}

void UpdateClient::onError(QAbstractSocket::SocketError err)
{
    QString errStr = m_webSocket.errorString();
    qWarning() << "UpdateClient: WebSocket error:" << err << "-" << errStr;
    emit error(errStr);
}

// ============================================================================
// Helpers
// ============================================================================

void UpdateClient::sendNextChunk()
{
    const qint64 total = m_uploadData.size();
    while (m_uploadOffset < total) {
        const qint64 len = qMin<qint64>(kUploadChunkSize, total - m_uploadOffset);
        m_webSocket.sendBinaryMessage(m_uploadData.mid(m_uploadOffset, len));
        m_uploadOffset += len;
    }

    QJsonObject end;
    end["op"] = "upload_end";
    m_webSocket.sendTextMessage(
        QString::fromUtf8(QJsonDocument(end).toJson(QJsonDocument::Compact)));

    // Everything has been handed to the socket's write queue (and copied
    // into it) — our own copy is no longer needed.
    m_uploadData.clear();
}

UpdateVersionInfo UpdateClient::parseVersionInfo(const QJsonObject &obj)
{
    UpdateVersionInfo info;
    if (obj.isEmpty()) return info;
    info.commit = obj.value("commit").toString();
    info.date = obj.value("date").toString();
    info.subject = obj.value("subject").toString();
    info.source = obj.value("source").toString();
    info.valid = !info.commit.isEmpty();
    return info;
}
