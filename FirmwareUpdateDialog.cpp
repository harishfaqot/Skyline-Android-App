#include "FirmwareUpdateDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QListWidget>
#include <QScrollArea>
#include <QFrame>
#include <QCloseEvent>
#include <QShowEvent>
#include <QResizeEvent>
#include <QScreen>
#include <QGuiApplication>
#include <QUrl>
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
    resize(fwdp(380), fwdp(360));

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
        QScrollArea { border: none; }
    )").arg(fwsp(12)).arg(fwsp(11)));

    QVBoxLayout *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    // Everything lives inside a scroll area — the dialog is shown at a
    // fixed size on Android (see MainWindow::onShowFirmwareUpdate), and
    // expanding "Details" below shouldn't be able to clip content that
    // doesn't fit; it just scrolls instead.
    QScrollArea *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    outerLayout->addWidget(scrollArea);

    QWidget *content = new QWidget(scrollArea);
    scrollArea->setWidget(content);

    QVBoxLayout *layout = new QVBoxLayout(content);
    layout->setContentsMargins(fwdp(16), fwdp(16), fwdp(16), fwdp(16));
    layout->setSpacing(fwdp(10));

    QLabel *title = new QLabel("Firmware Update", content);
    title->setStyleSheet("font-size: " + QString::number(fwsp(16)) + "px; font-weight: 700; color: #0075c5;");
    layout->addWidget(title);

    // Status row: colored dot + one-line status + manual refresh.
    // The dot is a plain CSS-styled box (same technique as StatusPill in
    // OverlayWidgets.cpp), not a unicode glyph — glyph characters like "●"
    // render as a missing-glyph "tofu" box on Android builds/devices
    // without that exact character in their font, which is the whole
    // reason IconRenderer.h exists (see its header comment).
    QHBoxLayout *statusRow = new QHBoxLayout();
    m_statusDot = new QLabel(content);
    m_statusDot->setFixedSize(fwdp(10), fwdp(10));
    m_statusDot->setStyleSheet(QString("background-color: #888888; border-radius: %1px;").arg(fwdp(5)));
    statusRow->addWidget(m_statusDot);
    m_statusLabel = new QLabel("Connecting…", content);
    m_statusLabel->setWordWrap(true);
    statusRow->addWidget(m_statusLabel, 1);
    m_refreshButton = new QPushButton("Refresh", content);
    m_refreshButton->setToolTip("Check again");
    m_refreshButton->setStyleSheet(QString(
        "background-color: transparent; color: #888888; font-weight: 600; font-size: %1px; min-height: %2px; padding: 4px 8px;"
    ).arg(fwsp(12)).arg(fwdp(30)));
    connect(m_refreshButton, &QPushButton::clicked, this, &FirmwareUpdateDialog::onRefreshClicked);
    statusRow->addWidget(m_refreshButton);
    layout->addLayout(statusRow);

    // Single primary action button — its label/enabled state tracks
    // whatever the current state actually calls for (see refreshUpdateButtonState).
    m_primaryButton = new QPushButton("Checking…", content);
    m_primaryButton->setMinimumHeight(fwdp(44));
    connect(m_primaryButton, &QPushButton::clicked, this, &FirmwareUpdateDialog::onPrimaryButtonClicked);
    layout->addWidget(m_primaryButton);

    // Progress — replaces the primary button while an update is running.
    m_progressContainer = new QWidget(content);
    QVBoxLayout *progressLayout = new QVBoxLayout(m_progressContainer);
    progressLayout->setContentsMargins(0, 0, 0, 0);
    progressLayout->setSpacing(fwdp(6));
    m_stageLabel = new QLabel(m_progressContainer);
    m_stageLabel->setWordWrap(true);
    m_stageLabel->setStyleSheet(QString("color: #cfcfcf; font-size: %1px;").arg(fwsp(12)));
    progressLayout->addWidget(m_stageLabel);
    m_progressBar = new QProgressBar(m_progressContainer);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setFormat("");
    progressLayout->addWidget(m_progressBar);
    layout->addWidget(m_progressContainer);
    m_progressContainer->hide();

    // Collapsed-by-default details: changelog + raw build log, for anyone
    // who wants to see what's actually happening (most users won't need to).
    m_detailsToggle = new QPushButton("Show Details", content);
    m_detailsToggle->setStyleSheet(QString(
        "background-color: transparent; color: #888888; text-align: left; "
        "font-weight: 500; font-size: %1px; padding: 2px; min-height: %2px;"
    ).arg(fwsp(12)).arg(fwdp(24)));
    connect(m_detailsToggle, &QPushButton::clicked, this, &FirmwareUpdateDialog::onDetailsToggled);
    layout->addWidget(m_detailsToggle);

    m_detailsPanel = new QWidget(content);
    QVBoxLayout *detailsLayout = new QVBoxLayout(m_detailsPanel);
    detailsLayout->setContentsMargins(0, 0, 0, 0);
    detailsLayout->setSpacing(fwdp(6));

    QLabel *changesLabel = new QLabel("Changes:", m_detailsPanel);
    changesLabel->setStyleSheet(QString("font-weight: 600; font-size: %1px;").arg(fwsp(12)));
    detailsLayout->addWidget(changesLabel);
    m_logList = new QListWidget(m_detailsPanel);
    m_logList->setMaximumHeight(fwdp(90));
    detailsLayout->addWidget(m_logList);

    QLabel *consoleLabel = new QLabel("Log:", m_detailsPanel);
    consoleLabel->setStyleSheet(QString("font-weight: 600; font-size: %1px;").arg(fwsp(12)));
    detailsLayout->addWidget(consoleLabel);
    m_consoleEdit = new QPlainTextEdit(m_detailsPanel);
    m_consoleEdit->setReadOnly(true);
    m_consoleEdit->setMaximumBlockCount(500);
    m_consoleEdit->setMinimumHeight(fwdp(120));
    detailsLayout->addWidget(m_consoleEdit);

    layout->addWidget(m_detailsPanel);
    m_detailsPanel->hide();

    m_closeButton = new QPushButton("Close", content);
    connect(m_closeButton, &QPushButton::clicked, this, &FirmwareUpdateDialog::close);
    layout->addWidget(m_closeButton);

    // Without this, the scroll area's content widget (stretched to fill the
    // dialog's fixed height — see setWidgetResizable(true) above) has more
    // vertical space than the collapsed content needs, and QVBoxLayout
    // distributes that leftover among widgets by their own Preferred size
    // policy instead of leaving it all in one place — which is what was
    // producing a stray gap in the middle of the layout instead of blank
    // space at the bottom where you'd expect it.
    layout->addStretch();

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
            background-color: #33332f;
            border: 1px solid rgba(0,117,197,140);
            border-radius: 12px;
        }
    )"));
    // Sized to content within a range, rather than a hard fixed width, so
    // short messages don't leave an oddly wide empty box and long ones
    // still wrap comfortably instead of stretching edge-to-edge.
    overlayPanel->setMinimumWidth(fwdp(230));
    overlayPanel->setMaximumWidth(fwdp(300));

    QVBoxLayout *overlayPanelLayout = new QVBoxLayout(overlayPanel);
    overlayPanelLayout->setContentsMargins(fwdp(20), fwdp(20), fwdp(20), fwdp(18));
    overlayPanelLayout->setSpacing(fwdp(14));

    m_overlayTitleLabel = new QLabel(overlayPanel);
    m_overlayTitleLabel->setWordWrap(true);
    m_overlayTitleLabel->setStyleSheet("font-weight: 700; font-size: " + QString::number(fwsp(15)) + "px; color: #0075c5;");
    overlayPanelLayout->addWidget(m_overlayTitleLabel);

    m_overlayMessageLabel = new QLabel(overlayPanel);
    m_overlayMessageLabel->setWordWrap(true);
    m_overlayMessageLabel->setStyleSheet("font-size: " + QString::number(fwsp(12)) + "px; color: #cfcfcf;");
    overlayPanelLayout->addWidget(m_overlayMessageLabel);

    QHBoxLayout *overlayButtonRow = new QHBoxLayout();
    overlayButtonRow->setSpacing(fwdp(10));
    overlayButtonRow->addStretch();
    m_overlayCancelButton = new QPushButton("No", overlayPanel);
    m_overlayCancelButton->setMinimumWidth(fwdp(72));
    m_overlayCancelButton->setStyleSheet("background-color: #4a4a48;");
    connect(m_overlayCancelButton, &QPushButton::clicked, this, &FirmwareUpdateDialog::hideOverlay);
    overlayButtonRow->addWidget(m_overlayCancelButton);

    m_overlayConfirmButton = new QPushButton("OK", overlayPanel);
    m_overlayConfirmButton->setMinimumWidth(fwdp(72));
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
    fetchLatestFromGithub();
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
        m_statusDot->setStyleSheet(QString("background-color: #ff5555; border-radius: %1px;").arg(fwdp(5)));
        m_statusLabel->setText("Update failed — see Details");
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
#ifdef SKYLINE_BUILTIN_GITHUB_TOKEN
    return QStringLiteral(SKYLINE_BUILTIN_GITHUB_TOKEN);
#else
    return QString();
#endif
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
        m_statusLabel->setText("Update check unavailable (no token in this build)");
        m_statusDot->setStyleSheet(QString("background-color: #ff5555; border-radius: %1px;").arg(fwdp(5)));
        return;
    }

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
        QJsonObject errObj = QJsonDocument::fromJson(body).object();
        appendLogLine("Update check error: " + errObj.value("message").toString(reply->errorString()));
        refreshUpdateButtonState();
        m_statusLabel->setText("Couldn't check for updates — see Details");
        m_statusDot->setStyleSheet(QString("background-color: #ff5555; border-radius: %1px;").arg(fwdp(5)));
        return;
    }

    QJsonObject root = QJsonDocument::fromJson(body).object();
    UpdateVersionInfo latest;
    latest.commit = root.value("sha").toString();
    latest.subject = firstLine(root.value("commit").toObject().value("message").toString());
    latest.date = root.value("commit").toObject().value("author").toObject().value("date").toString();
    latest.valid = !latest.commit.isEmpty();

    m_latestVersion = latest;
    maybeCompareVersions();
}

void FirmwareUpdateDialog::maybeCompareVersions()
{
    if (!m_latestVersion.valid) return;

    if (!m_currentVersion.valid) {
        // Nothing installed via this mechanism yet (or no local git HEAD
        // either) — nothing to diff against, just offer to install latest.
        m_commitsBehind = 1;
        m_logList->clear();
        m_logList->addItem(m_latestVersion.subject);
        refreshUpdateButtonState();
        return;
    }

    if (m_currentVersion.commit == m_latestVersion.commit) {
        m_commitsBehind = 0;
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
    refreshUpdateButtonState();
}

void FirmwareUpdateDialog::startDownload()
{
    setProgress("Downloading update…", 0);
    appendLogLine("Downloading " + shortSha(m_latestVersion.commit) + "…");

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

void FirmwareUpdateDialog::onRefreshClicked()
{
    fetchLatestFromGithub();
    if (m_agentConnected) {
        m_client->requestCheck();
    }
}

void FirmwareUpdateDialog::onDetailsToggled()
{
    m_detailsExpanded = !m_detailsExpanded;
    m_detailsPanel->setVisible(m_detailsExpanded);
    m_detailsToggle->setText(m_detailsExpanded ? "Hide Details" : "Show Details");
}

void FirmwareUpdateDialog::onPrimaryButtonClicked()
{
    if (!m_agentConnected || m_armed || m_busy || m_commitsBehind <= 0 || !m_latestVersion.valid) {
        return;
    }
    if (githubToken().isEmpty()) {
        showOverlayMessage("Update unavailable", "This build has no update credentials configured. Contact the app developer.");
        return;
    }

    showOverlayConfirm("Update drone firmware",
        "The drone will restart and briefly lose connection during this. Continue?",
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
    QString dotColor = "#888888";
    QString statusText;

    if (!m_agentConnected) {
        statusText = "Connecting to drone…";
    } else if (m_busy) {
        dotColor = "#0075c5";
        statusText = "Updating…";
    } else if (m_commitsBehind < 0) {
        statusText = "Checking for updates…";
    } else if (m_commitsBehind == 0) {
        dotColor = "#00cc44";
        statusText = m_currentVersion.valid
            ? QString("Up to date (%1)").arg(shortSha(m_currentVersion.commit))
            : "Up to date";
    } else {
        dotColor = "#ffcc00";
        statusText = QString("Update available — %1 commit%2 behind")
            .arg(m_commitsBehind).arg(m_commitsBehind == 1 ? "" : "s");
    }
    m_statusDot->setStyleSheet(QString("background-color: %1; border-radius: %2px;").arg(dotColor).arg(fwdp(5)));
    m_statusLabel->setText(statusText);

    m_primaryButton->setVisible(!m_busy);
    m_progressContainer->setVisible(m_busy);

    bool canUpdate = m_agentConnected && !m_armed && !m_busy && m_commitsBehind > 0 && m_latestVersion.valid;
    if (m_armed) {
        m_primaryButton->setText("Disarm Drone to Update");
        m_primaryButton->setEnabled(false);
    } else if (!m_agentConnected) {
        m_primaryButton->setText("Connecting…");
        m_primaryButton->setEnabled(false);
    } else if (m_commitsBehind < 0) {
        m_primaryButton->setText("Checking…");
        m_primaryButton->setEnabled(false);
    } else if (m_commitsBehind == 0) {
        m_primaryButton->setText("Up to Date");
        m_primaryButton->setEnabled(false);
    } else {
        m_primaryButton->setText("Update Now");
        m_primaryButton->setEnabled(canUpdate);
    }

    m_refreshButton->setEnabled(m_agentConnected && !m_busy);
    m_closeButton->setEnabled(!m_busy);
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
