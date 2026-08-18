#include "FirmwareUpdateDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QListWidget>
#include <QCloseEvent>
#include <QShowEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QGuiApplication>
#include <QUrl>
#include <QSettings>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>

namespace {

// Local DPI helpers (mirrors MainWindow.cpp / OverlayWidgets.cpp; kept
// file-local to avoid coupling this dialog to MainWindow internals).
float fwDpScale()
{
    static float s_scale = 0.0f;
    if (s_scale == 0.0f) {
        QScreen *screen = QGuiApplication::primaryScreen();
        s_scale = screen ? screen->logicalDotsPerInch() / 96.0f : 1.0f;
        s_scale = qBound(1.0f, s_scale, 4.0f);
    }
    return s_scale;
}
int fwdp(int v) { return qRound(static_cast<float>(v) * fwDpScale()); }
int fwsp(int v) { return qRound(static_cast<float>(v) * fwDpScale()); }

// This app talks to one specific private repo/branch — the same one the
// Jetson used to `git pull` from directly before this feature existed.
const QString kGithubOwner = "harishfaqot";
const QString kGithubRepo = "Skyline-Drone";
const QString kGithubBranch = "Apps";

QString humanSize(qint64 bytes)
{
    if (bytes < 1024) return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

QString shortSha(const QString &sha)
{
    return sha.left(10);
}

QString firstLine(const QString &text)
{
    const int idx = text.indexOf('\n');
    return idx >= 0 ? text.left(idx) : text;
}

} // namespace

FirmwareUpdateDialog::FirmwareUpdateDialog(QWidget *parent)
    : QDialog(parent)
    , m_client(new UpdateClient(this))
    , m_network(new QNetworkAccessManager(this))
{
    setWindowTitle("Firmware Update - Skyline");
    setModal(true);
    setWindowModality(Qt::ApplicationModal);
    setAttribute(Qt::WA_AcceptTouchEvents, true);
    resize(fwdp(440), fwdp(560));

#ifdef Q_OS_ANDROID
    // Same child-widget overlay technique as MainWindow's settings dialog:
    // a native modal QDialog's touch coordinates can desync from a
    // QOpenGLWidget underneath on Android, so this is shown as a frameless
    // child widget of `parent` (the caller passes MainWindow::centralWidget())
    // instead of a real top-level dialog.
    if (parent) {
        setParent(parent);
    }
    setWindowFlags(Qt::FramelessWindowHint | Qt::Widget);
    setModal(false);
    setWindowModality(Qt::NonModal);
    setAttribute(Qt::WA_StyledBackground, true);
    hide();
#endif

    setStyleSheet(QString(R"(
        QDialog {
            background-color: #262624;
        }
        QWidget {
            background-color: #262624;
            color: #e8e8e8;
        }
        QLabel {
            color: #e8e8e8;
        }
        QPushButton {
            background-color: #0075c5;
            color: white;
            border: none;
            padding: 7px 14px;
            border-radius: 6px;
            font-weight: 600;
            font-size: %1px;
            min-height: 30px;
        }
        QPushButton:hover { background-color: #0088e0; }
        QPushButton:pressed { background-color: #005a9c; }
        QPushButton:disabled { background-color: #444444; color: #888888; }
        QLineEdit {
            background-color: rgba(30, 30, 30, 200);
            border: 1px solid rgba(70, 70, 70, 200);
            border-radius: 6px;
            padding: 5px 9px;
            color: #e8e8e8;
            font-size: %1px;
        }
        QLineEdit:focus {
            border: 1px solid #0075c5;
        }
        QListWidget, QPlainTextEdit {
            background-color: rgba(20, 20, 20, 200);
            border: 1px solid rgba(70, 70, 70, 200);
            border-radius: 6px;
            color: #cfcfcf;
            font-size: %2px;
        }
        QProgressBar {
            border: 1px solid rgba(70, 70, 70, 200);
            border-radius: 6px;
            text-align: center;
            color: #e8e8e8;
            background-color: rgba(20, 20, 20, 200);
        }
        QProgressBar::chunk {
            background-color: #0075c5;
            border-radius: 5px;
        }
    )").arg(fwsp(12)).arg(fwsp(11)));

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(fwdp(14), fwdp(14), fwdp(14), fwdp(14));
    layout->setSpacing(fwdp(8));

    QLabel *title = new QLabel("Firmware Update", this);
    title->setStyleSheet("font-size: " + QString::number(fwsp(16)) + "px; font-weight: 700; color: #0075c5;");
    layout->addWidget(title);

    QHBoxLayout *tokenRow = new QHBoxLayout();
    QLabel *tokenLabel = new QLabel("GitHub Token", this);
    tokenLabel->setMinimumWidth(fwdp(90));
    tokenRow->addWidget(tokenLabel);
    m_tokenEdit = new QLineEdit(this);
    m_tokenEdit->setEchoMode(QLineEdit::Password);
    m_tokenEdit->setPlaceholderText("ghp_… (needed once, saved on this phone)");
    m_tokenEdit->setText(githubToken());
    connect(m_tokenEdit, &QLineEdit::editingFinished, this, &FirmwareUpdateDialog::onTokenEdited);
    tokenRow->addWidget(m_tokenEdit, 1);
    layout->addLayout(tokenRow);

    m_currentVersionLabel = new QLabel("Current version (on drone): —", this);
    layout->addWidget(m_currentVersionLabel);

    QHBoxLayout *latestRow = new QHBoxLayout();
    m_latestVersionLabel = new QLabel("Latest version (GitHub): —", this);
    latestRow->addWidget(m_latestVersionLabel, 1);
    m_badgeLabel = new QLabel("Not connected", this);
    m_badgeLabel->setStyleSheet("color: #888888; font-weight: 600;");
    latestRow->addWidget(m_badgeLabel);
    layout->addLayout(latestRow);

    QLabel *logTitle = new QLabel("Changes:", this);
    logTitle->setStyleSheet("font-weight: 600; margin-top: " + QString::number(fwdp(4)) + "px;");
    layout->addWidget(logTitle);

    m_logList = new QListWidget(this);
    m_logList->setMaximumHeight(fwdp(100));
    layout->addWidget(m_logList);

    QHBoxLayout *buttonRow = new QHBoxLayout();
    m_checkButton = new QPushButton("Check for Updates", this);
    connect(m_checkButton, &QPushButton::clicked, this, &FirmwareUpdateDialog::onCheckClicked);
    buttonRow->addWidget(m_checkButton);

    m_updateButton = new QPushButton("Download && Install", this);
    connect(m_updateButton, &QPushButton::clicked, this, &FirmwareUpdateDialog::onUpdateClicked);
    buttonRow->addWidget(m_updateButton);
    layout->addLayout(buttonRow);

    QLabel *progressTitle = new QLabel("Progress:", this);
    progressTitle->setStyleSheet("font-weight: 600; margin-top: " + QString::number(fwdp(6)) + "px;");
    layout->addWidget(progressTitle);

    m_stageLabel = new QLabel("Idle", this);
    layout->addWidget(m_stageLabel);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setFormat("");
    layout->addWidget(m_progressBar);

    m_consoleEdit = new QPlainTextEdit(this);
    m_consoleEdit->setReadOnly(true);
    m_consoleEdit->setMaximumBlockCount(500);
    layout->addWidget(m_consoleEdit, 1);

    m_closeButton = new QPushButton("Close", this);
    connect(m_closeButton, &QPushButton::clicked, this, &FirmwareUpdateDialog::close);
    layout->addWidget(m_closeButton);

    // In-widget message/confirm overlay (see showOverlayMessage/showOverlayConfirm) —
    // a plain child widget, not a native QMessageBox, so it receives touch
    // input correctly on Android.
    m_overlay = new QWidget(this);
    m_overlay->setObjectName("fwOverlay");
    m_overlay->setStyleSheet("QWidget#fwOverlay { background-color: rgba(0,0,0,170); }");
    m_overlay->hide();

    QWidget *overlayPanel = new QWidget(m_overlay);
    overlayPanel->setObjectName("fwOverlayPanel");
    overlayPanel->setStyleSheet(QString(R"(
        QWidget#fwOverlayPanel {
            background-color: #2d2d2b;
            border: 1px solid rgba(0,117,197,120);
            border-radius: 10px;
        }
    )"));
    overlayPanel->setFixedWidth(fwdp(320));

    QVBoxLayout *overlayPanelLayout = new QVBoxLayout(overlayPanel);
    overlayPanelLayout->setContentsMargins(fwdp(16), fwdp(16), fwdp(16), fwdp(16));
    overlayPanelLayout->setSpacing(fwdp(10));

    m_overlayTitleLabel = new QLabel(overlayPanel);
    m_overlayTitleLabel->setWordWrap(true);
    m_overlayTitleLabel->setStyleSheet("font-weight: 700; font-size: " + QString::number(fwsp(14)) + "px; color: #0075c5;");
    overlayPanelLayout->addWidget(m_overlayTitleLabel);

    m_overlayMessageLabel = new QLabel(overlayPanel);
    m_overlayMessageLabel->setWordWrap(true);
    overlayPanelLayout->addWidget(m_overlayMessageLabel);

    QHBoxLayout *overlayButtonRow = new QHBoxLayout();
    overlayButtonRow->addStretch();
    m_overlayCancelButton = new QPushButton("No", overlayPanel);
    m_overlayCancelButton->setStyleSheet("background-color: #444444;");
    connect(m_overlayCancelButton, &QPushButton::clicked, this, &FirmwareUpdateDialog::hideOverlay);
    overlayButtonRow->addWidget(m_overlayCancelButton);

    m_overlayConfirmButton = new QPushButton("OK", overlayPanel);
    connect(m_overlayConfirmButton, &QPushButton::clicked, this, [this]() {
        std::function<void()> callback = m_overlayOnConfirm;
        hideOverlay();
        if (callback) callback();
    });
    overlayButtonRow->addWidget(m_overlayConfirmButton);
    overlayPanelLayout->addLayout(overlayButtonRow);

    QVBoxLayout *overlayOuterLayout = new QVBoxLayout(m_overlay);
    overlayOuterLayout->addStretch();
    QHBoxLayout *overlayCenterRow = new QHBoxLayout();
    overlayCenterRow->addStretch();
    overlayCenterRow->addWidget(overlayPanel);
    overlayCenterRow->addStretch();
    overlayOuterLayout->addLayout(overlayCenterRow);
    overlayOuterLayout->addStretch();

    connect(m_client, &UpdateClient::connected, this, &FirmwareUpdateDialog::onClientConnected);
    connect(m_client, &UpdateClient::disconnected, this, &FirmwareUpdateDialog::onClientDisconnected);
    connect(m_client, &UpdateClient::error, this, &FirmwareUpdateDialog::onClientError);
    connect(m_client, &UpdateClient::checkResult, this, &FirmwareUpdateDialog::onCheckResult);
    connect(m_client, &UpdateClient::uploadReady, this, &FirmwareUpdateDialog::onUploadReady);
    connect(m_client, &UpdateClient::uploadProgress, this, &FirmwareUpdateDialog::onUploadProgress);
    connect(m_client, &UpdateClient::requestFailed, this, &FirmwareUpdateDialog::onRequestFailed);
    connect(m_client, &UpdateClient::statusUpdate, this, &FirmwareUpdateDialog::onStatusUpdate);

    refreshUpdateButtonState();
}

void FirmwareUpdateDialog::setDroneHost(const QString &host)
{
    if (host == m_droneHost) return;
    m_droneHost = host;

    m_client->disconnect();
    m_agentConnected = false;
    m_badgeLabel->setText("Connecting…");
    m_badgeLabel->setStyleSheet("color: #ffcc00; font-weight: 600;");
    refreshUpdateButtonState();

    if (!m_droneHost.isEmpty()) {
        m_client->connectToServer(QString("ws://%1:9091").arg(m_droneHost));
    }
}

void FirmwareUpdateDialog::setDroneArmed(bool armed)
{
    m_armed = armed;
    refreshUpdateButtonState();
}

void FirmwareUpdateDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (!m_droneHost.isEmpty() && !m_client->isConnected()) {
        m_client->connectToServer(QString("ws://%1:9091").arg(m_droneHost));
    }
}

void FirmwareUpdateDialog::closeEvent(QCloseEvent *event)
{
    if (m_busy) {
        showOverlayMessage("Update in progress",
            "An update is currently being downloaded/applied. Please wait "
            "for it to finish before closing this window.");
        event->ignore();
        return;
    }
    QDialog::closeEvent(event);
}

void FirmwareUpdateDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    if (m_overlay) {
        m_overlay->setGeometry(rect());
    }
}

// ============================================================================
// UpdateClient signal handlers (drone-side agent connection)
// ============================================================================

void FirmwareUpdateDialog::onClientConnected()
{
    m_agentConnected = true;
    refreshUpdateButtonState();
    m_client->requestCheck();
}

void FirmwareUpdateDialog::onClientDisconnected()
{
    m_agentConnected = false;
    m_badgeLabel->setText("Not connected");
    m_badgeLabel->setStyleSheet("color: #888888; font-weight: 600;");
    refreshUpdateButtonState();
}

void FirmwareUpdateDialog::onClientError(const QString &errorString)
{
    appendLogLine("Drone connection error: " + errorString);
    refreshUpdateButtonState();
}

void FirmwareUpdateDialog::onCheckResult(UpdateVersionInfo current)
{
    m_currentVersion = current;
    if (current.valid) {
        m_currentVersionLabel->setText(QString("Current version (on drone): %1 — %2 (%3)")
            .arg(shortSha(current.commit), current.subject, current.date));
    } else {
        m_currentVersionLabel->setText("Current version (on drone): unknown (no prior update installed)");
    }
    maybeCompareVersions();
}

void FirmwareUpdateDialog::onUploadReady()
{
    setProgress("Uploading to drone…", 40);
    appendLogLine("Drone ready, uploading archive…");
}

void FirmwareUpdateDialog::onUploadProgress(qint64 sent, qint64 total)
{
    if (total <= 0) return;
    const int pct = 40 + static_cast<int>((sent * 30) / total); // 40-70% band
    setProgress(QString("Uploading to drone… %1 / %2").arg(humanSize(sent), humanSize(total)),
                qMin(pct, 70));
}

void FirmwareUpdateDialog::onRequestFailed(const QString &reason)
{
    if (m_awaitingUpload) {
        m_awaitingUpload = false;
        setBusy(false);
        showOverlayMessage("Update not started", reason);
    } else {
        appendLogLine("Drone check failed: " + reason);
    }
    refreshUpdateButtonState();
}

void FirmwareUpdateDialog::onStatusUpdate(const QString &stage, const QString &message, const QString &line)
{
    if (stage == "error") {
        setBusy(false);
        m_progressBar->setStyleSheet("QProgressBar::chunk { background-color: #ff3333; border-radius: 5px; }");
        m_stageLabel->setText("Error: " + message);
        appendLogLine("ERROR: " + message);
        refreshUpdateButtonState();
        return;
    }

    m_progressBar->setStyleSheet(QString());
    if (stage == "verify") setProgress(message, 72);
    else if (stage == "extract") setProgress(message, 80);
    else if (stage == "build") setProgress(message, 90);
    else if (stage == "done") setProgress(message, 100);

    if (!line.isEmpty()) appendLogLine(line);

    if (stage == "done") {
        setBusy(false);
        appendLogLine("Update finished. The drone link will reconnect automatically.");
        refreshUpdateButtonState();
        m_client->requestCheck();
    }
}

// ============================================================================
// GitHub — latest version / changelog / archive download
// ============================================================================

QString FirmwareUpdateDialog::githubToken() const
{
    QSettings settings("Skyline", "SkylineApp");
    return settings.value("github/token").toString();
}

void FirmwareUpdateDialog::onTokenEdited()
{
    QSettings settings("Skyline", "SkylineApp");
    settings.setValue("github/token", m_tokenEdit->text().trimmed());
}

QNetworkRequest FirmwareUpdateDialog::githubRequest(const QString &url) const
{
    QNetworkRequest request{QUrl(url)};
    const QString token = githubToken();
    if (!token.isEmpty()) {
        request.setRawHeader("Authorization", ("Bearer " + token).toUtf8());
    }
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setRawHeader("User-Agent", "Skyline-App");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return request;
}

void FirmwareUpdateDialog::fetchLatestFromGithub()
{
    if (githubToken().isEmpty()) {
        m_badgeLabel->setText("Missing GitHub token");
        m_badgeLabel->setStyleSheet("color: #ff5555; font-weight: 600;");
        return;
    }

    m_badgeLabel->setText("Checking GitHub…");
    m_badgeLabel->setStyleSheet("color: #888888; font-weight: 600;");

    const QString url = QString("https://api.github.com/repos/%1/%2/commits/%3")
        .arg(kGithubOwner, kGithubRepo, kGithubBranch);
    QNetworkReply *reply = m_network->get(githubRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        handleLatestCommitReply(reply);
        reply->deleteLater();
    });
}

void FirmwareUpdateDialog::handleLatestCommitReply(QNetworkReply *reply)
{
    const QByteArray body = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        m_badgeLabel->setText("GitHub check failed");
        m_badgeLabel->setStyleSheet("color: #ff5555; font-weight: 600;");
        QJsonObject errObj = QJsonDocument::fromJson(body).object();
        appendLogLine("GitHub error: " + errObj.value("message").toString(reply->errorString()));
        return;
    }

    QJsonObject root = QJsonDocument::fromJson(body).object();
    UpdateVersionInfo latest;
    latest.commit = root.value("sha").toString();
    latest.subject = firstLine(root.value("commit").toObject().value("message").toString());
    latest.date = root.value("commit").toObject().value("author").toObject().value("date").toString();
    latest.valid = !latest.commit.isEmpty();

    m_latestVersion = latest;
    if (latest.valid) {
        m_latestVersionLabel->setText(QString("Latest version (GitHub): %1 — %2 (%3)")
            .arg(shortSha(latest.commit), latest.subject, latest.date));
    }
    maybeCompareVersions();
}

void FirmwareUpdateDialog::maybeCompareVersions()
{
    if (!m_latestVersion.valid) return;

    if (!m_currentVersion.valid) {
        // Nothing installed via this mechanism yet (or no local git HEAD
        // either) — nothing to diff against, just offer to install latest.
        m_commitsBehind = 1;
        m_badgeLabel->setText("Update available");
        m_badgeLabel->setStyleSheet("color: #ffcc00; font-weight: 600;");
        refreshUpdateButtonState();
        return;
    }

    if (m_currentVersion.commit == m_latestVersion.commit) {
        m_commitsBehind = 0;
        m_badgeLabel->setText("Up to date");
        m_badgeLabel->setStyleSheet("color: #00cc44; font-weight: 600;");
        m_logList->clear();
        refreshUpdateButtonState();
        return;
    }

    const QString url = QString("https://api.github.com/repos/%1/%2/compare/%3...%4")
        .arg(kGithubOwner, kGithubRepo, m_currentVersion.commit, m_latestVersion.commit);
    QNetworkReply *reply = m_network->get(githubRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        handleCompareReply(reply);
        reply->deleteLater();
    });
}

void FirmwareUpdateDialog::handleCompareReply(QNetworkReply *reply)
{
    const QByteArray body = reply->readAll();
    if (reply->error() != QNetworkReply::NoError) {
        // Compare failing (e.g. current commit not found on GitHub anymore)
        // shouldn't block updating — just skip the changelog/count.
        m_commitsBehind = 1;
        m_badgeLabel->setText("Update available");
        m_badgeLabel->setStyleSheet("color: #ffcc00; font-weight: 600;");
        refreshUpdateButtonState();
        return;
    }

    QJsonObject root = QJsonDocument::fromJson(body).object();
    m_commitsBehind = root.value("ahead_by").toInt(1);

    m_logList->clear();
    for (const QJsonValue &v : root.value("commits").toArray()) {
        const QString subject = firstLine(v.toObject().value("commit").toObject().value("message").toString());
        m_logList->addItem(subject);
    }

    if (m_commitsBehind <= 0) {
        m_badgeLabel->setText("Up to date");
        m_badgeLabel->setStyleSheet("color: #00cc44; font-weight: 600;");
    } else {
        m_badgeLabel->setText(QString("%1 commit(s) behind").arg(m_commitsBehind));
        m_badgeLabel->setStyleSheet("color: #ffcc00; font-weight: 600;");
    }
    refreshUpdateButtonState();
}

void FirmwareUpdateDialog::startDownload()
{
    setProgress("Downloading update from GitHub…", 0);
    appendLogLine("Downloading " + shortSha(m_latestVersion.commit) + " from GitHub…");

    const QString url = QString("https://api.github.com/repos/%1/%2/zipball/%3")
        .arg(kGithubOwner, kGithubRepo, m_latestVersion.commit);
    QNetworkReply *reply = m_network->get(githubRequest(url));
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (total <= 0) return;
        const int pct = static_cast<int>((received * 40) / total); // 0-40% band
        setProgress(QString("Downloading update… %1 / %2").arg(humanSize(received), humanSize(total)),
                    qMin(pct, 40));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        handleDownloadReply(reply);
        reply->deleteLater();
    });
}

void FirmwareUpdateDialog::handleDownloadReply(QNetworkReply *reply)
{
    if (reply->error() != QNetworkReply::NoError) {
        setBusy(false);
        m_stageLabel->setText("Download failed");
        appendLogLine("Download failed: " + reply->errorString());
        refreshUpdateButtonState();
        return;
    }

    m_downloadedArchive = reply->readAll();
    appendLogLine(QString("Downloaded %1.").arg(humanSize(m_downloadedArchive.size())));

    const QString sha256 = QString::fromLatin1(
        QCryptographicHash::hash(m_downloadedArchive, QCryptographicHash::Sha256).toHex());

    m_awaitingUpload = true;
    setProgress("Sending to drone…", 40);
    m_client->uploadArchive(m_downloadedArchive, sha256,
                             m_latestVersion.commit, m_latestVersion.date, m_latestVersion.subject);
    m_downloadedArchive.clear();
}

// ============================================================================
// UI actions
// ============================================================================

void FirmwareUpdateDialog::onCheckClicked()
{
    fetchLatestFromGithub();
    if (m_agentConnected) {
        m_client->requestCheck();
    }
}

void FirmwareUpdateDialog::onUpdateClicked()
{
    if (!m_agentConnected || m_armed || m_busy || m_commitsBehind <= 0 || !m_latestVersion.valid) {
        return;
    }
    if (githubToken().isEmpty()) {
        showOverlayMessage("GitHub token required", "Enter a GitHub personal access token above first.");
        return;
    }

    showOverlayConfirm("Update drone firmware",
        "This will download the latest software from GitHub, send it to the "
        "drone over WiFi, and rebuild/restart its onboard stack. The video "
        "and telemetry link will drop briefly and reconnect automatically. Continue?",
        [this]() {
            setBusy(true);
            m_consoleEdit->clear();
            startDownload();
        });
}

// ============================================================================
// Helpers
// ============================================================================

void FirmwareUpdateDialog::refreshUpdateButtonState()
{
    bool canUpdate = m_agentConnected && !m_armed && !m_busy && m_commitsBehind > 0 && m_latestVersion.valid;
    m_updateButton->setEnabled(canUpdate);
    m_checkButton->setEnabled(!m_busy);
    m_closeButton->setEnabled(!m_busy);

    if (m_armed) {
        m_updateButton->setToolTip("Disarm the drone before updating its firmware.");
    } else if (m_busy) {
        m_updateButton->setToolTip("An update is already in progress.");
    } else if (!m_agentConnected) {
        m_updateButton->setToolTip("Not connected to the drone's update agent.");
    } else if (m_commitsBehind <= 0) {
        m_updateButton->setToolTip("Already up to date.");
    } else {
        m_updateButton->setToolTip(QString());
    }
}

void FirmwareUpdateDialog::appendLogLine(const QString &text)
{
    m_consoleEdit->appendPlainText(text);
}

void FirmwareUpdateDialog::setProgress(const QString &phaseLabel, int percent)
{
    m_stageLabel->setText(phaseLabel);
    m_progressBar->setValue(qBound(0, percent, 100));
}

void FirmwareUpdateDialog::setBusy(bool busy)
{
    m_busy = busy;
    refreshUpdateButtonState();
}

void FirmwareUpdateDialog::showOverlayMessage(const QString &title, const QString &message)
{
    m_overlayTitleLabel->setText(title);
    m_overlayMessageLabel->setText(message);
    m_overlayCancelButton->hide();
    m_overlayConfirmButton->setText("OK");
    m_overlayOnConfirm = nullptr;
    m_overlay->setGeometry(rect());
    m_overlay->show();
    m_overlay->raise();
}

void FirmwareUpdateDialog::showOverlayConfirm(const QString &title, const QString &message, std::function<void()> onConfirm)
{
    m_overlayTitleLabel->setText(title);
    m_overlayMessageLabel->setText(message);
    m_overlayCancelButton->show();
    m_overlayConfirmButton->setText("Yes");
    m_overlayOnConfirm = std::move(onConfirm);
    m_overlay->setGeometry(rect());
    m_overlay->show();
    m_overlay->raise();
}

void FirmwareUpdateDialog::hideOverlay()
{
    m_overlay->hide();
    m_overlayOnConfirm = nullptr;
}
