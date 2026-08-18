#ifndef UPDATECLIENT_H
#define UPDATECLIENT_H

#include <QObject>
#include <QWebSocket>
#include <QJsonObject>
#include <QString>
#include <QByteArray>

/**
 * @brief Client for the Skyline OTA update agent (see update_agent/agent.py)
 *
 * Connects to a small websocket daemon that runs on the drone's Jetson HOST
 * (outside the ROS/Docker stack, default port 9091). This is a separate
 * connection from the rosbridge link (RosPointCloudClient/RosImageClient)
 * because that link's server process lives inside the very container an
 * update rebuilds and restarts.
 *
 * The Jetson has no internet access, so unlike a typical OTA client this
 * class doesn't ask the agent to fetch anything — it only reports the
 * drone's currently-installed version, and receives an already-downloaded
 * update archive pushed to it (see FirmwareUpdateDialog, which downloads
 * the archive from GitHub using the phone's own internet connection, the
 * same way DJI's app fetches firmware before pushing it to the aircraft).
 *
 * Wire protocol (JSON control messages + raw binary frames), mirrors agent.py:
 *   Check:  {"op":"check"} -> {"op":"check_result","current":{...}|null}
 *   Upload: {"op":"upload_begin","size":N,"sha256":...,"commit":...,"date":...,"subject":...}
 *           -> {"op":"upload_ready"} or {"op":"error","message":...}
 *           <N bytes as binary frames>
 *           {"op":"upload_end"}
 *           -> repeated {"op":"status","stage":...,"message":...,"line":...}
 */

struct UpdateVersionInfo {
    QString commit;
    QString date;
    QString subject;
    QString source; // "ota" | "git" | empty if unknown
    bool valid = false;
};

class UpdateClient : public QObject
{
    Q_OBJECT

public:
    explicit UpdateClient(QObject *parent = nullptr);
    ~UpdateClient();

    void connectToServer(const QString &url);
    void disconnect();
    bool isConnected() const;

    void requestCheck();

    // Begin pushing an already-downloaded update archive to the drone.
    // `data` is the raw zip bytes (already downloaded from GitHub);
    // sha256 is the lowercase hex digest of `data`, used by the agent to
    // verify the transfer wasn't corrupted over WiFi.
    void uploadArchive(const QByteArray &data, const QString &sha256,
                        const QString &commit, const QString &date, const QString &subject);

signals:
    void connected();
    void disconnected();
    void error(const QString &errorString); // transport-level (socket) errors

    void checkResult(UpdateVersionInfo current);

    void uploadReady();
    void uploadProgress(qint64 sent, qint64 total);
    // Server-reported failure of whichever request is currently pending
    // (upload rejected, e.g. "already in progress", or a post-transfer
    // verify/extract/build failure surfaced as a "status" error stage).
    void requestFailed(const QString &reason);
    void statusUpdate(const QString &stage, const QString &message, const QString &line);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString &message);
    void onBytesWritten(qint64 bytes);
    void onError(QAbstractSocket::SocketError error);

private:
    static UpdateVersionInfo parseVersionInfo(const QJsonObject &obj);
    void sendNextChunk();

    QWebSocket m_webSocket;

    // Chunked binary upload state. m_uploading gates onBytesWritten() so
    // that writes unrelated to the archive transfer (e.g. a later
    // requestCheck() call) don't get misreported as more upload progress —
    // bytesWritten() fires for *every* socket write, not just upload chunks.
    bool m_uploading = false;
    QByteArray m_uploadData;
    qint64 m_uploadOffset = 0;
    qint64 m_uploadAcked = 0; // bytes Qt has confirmed written to the socket
};

#endif // UPDATECLIENT_H
