#ifndef FIRMWAREUPDATEDIALOG_H
#define FIRMWAREUPDATEDIALOG_H

#include <QDialog>
#include <QByteArray>
#include <QNetworkRequest>
#include "UpdateClient.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QProgressBar;
class QPlainTextEdit;
class QListWidget;
class QNetworkAccessManager;
class QNetworkReply;

/**
 * @brief "Firmware Update" dialog — DJI-style in-app OTA update for the drone.
 *
 * The Jetson has no internet access, so this mirrors how DJI's app updates
 * aircraft firmware: the PHONE downloads the new version from GitHub (it
 * has internet), then pushes the downloaded archive to the drone over the
 * same local WiFi link the app already uses for telemetry/video. The
 * drone-side agent (update_agent/agent.py, default port 9091) never talks
 * to GitHub itself — see UpdateClient for that connection.
 *
 * Requires a GitHub personal access token (the repo is private) entered
 * once here and stored locally via QSettings.
 *
 * The update button is disabled while the drone is armed (see setDroneArmed),
 * and the dialog refuses to close while a download/upload/apply is active.
 */
class FirmwareUpdateDialog : public QDialog
{
    Q_OBJECT

public:
    explicit FirmwareUpdateDialog(QWidget *parent = nullptr);

    // host is the same host the app uses for the rosbridge link (ws://host:9090);
    // the update agent listens on the same host, port 9091.
    void setDroneHost(const QString &host);
    void setDroneArmed(bool armed);

protected:
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private slots:
    // UpdateClient (drone-side agent connection)
    void onClientConnected();
    void onClientDisconnected();
    void onClientError(const QString &errorString);
    void onCheckResult(UpdateVersionInfo current);
    void onUploadReady();
    void onUploadProgress(qint64 sent, qint64 total);
    void onRequestFailed(const QString &reason);
    void onStatusUpdate(const QString &stage, const QString &message, const QString &line);

    // UI actions
    void onCheckClicked();
    void onUpdateClicked();
    void onTokenEdited();

private:
    void fetchLatestFromGithub();
    void handleLatestCommitReply(QNetworkReply *reply);
    void maybeCompareVersions();
    void handleCompareReply(QNetworkReply *reply);
    void startDownload();
    void handleDownloadReply(QNetworkReply *reply);

    void refreshUpdateButtonState();
    void appendLogLine(const QString &text);
    void setProgress(const QString &phaseLabel, int percent);
    void setBusy(bool busy);

    QString githubToken() const;
    QNetworkRequest githubRequest(const QString &url) const;

    UpdateClient *m_client;
    QNetworkAccessManager *m_network;
    QString m_droneHost;

    bool m_agentConnected = false;
    bool m_armed = false;
    bool m_busy = false; // true from confirm-click through done/error (download+upload+apply)
    bool m_awaitingUpload = false; // true between uploadArchive() and uploadReady()/requestFailed()

    UpdateVersionInfo m_currentVersion;
    UpdateVersionInfo m_latestVersion;
    int m_commitsBehind = -1; // -1 = unknown until both versions are known
    QByteArray m_downloadedArchive;

    QLabel *m_currentVersionLabel;
    QLabel *m_latestVersionLabel;
    QLabel *m_badgeLabel;
    QListWidget *m_logList;

    QLineEdit *m_tokenEdit;
    QPushButton *m_checkButton;
    QPushButton *m_updateButton;
    QPushButton *m_closeButton;

    QLabel *m_stageLabel;
    QProgressBar *m_progressBar;
    QPlainTextEdit *m_consoleEdit;
};

#endif // FIRMWAREUPDATEDIALOG_H
