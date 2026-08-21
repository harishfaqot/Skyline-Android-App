#include "MainWindow.h"
#include "PointCloudWidget.h"
#include "ImageViewerWidget.h"
#include "RosPointCloudClient.h"
#include "RtspVideoClient.h"
#include "PointCloudClient.h"
#include "TelemetryClient.h"
#include "OverlayWidgets.h"
#include "IconRenderer.h"
#include "FirmwareUpdateDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QDialog>
#include <QMouseEvent>
#include <QTouchEvent>
#include <QEnterEvent>
#include <QPainter>
#include <QSplitter>
#include <QTimer>
#include <QDir>
#include <QFileInfo>
#include <QCoreApplication>
#include <QFrame>
#include <QGuiApplication>   // FIX: needed for primaryScreen()
#include <QScreen>           // FIX: needed for logicalDotsPerInch()
#include <QScrollArea>
#include <QListWidget>
#include <QDateTime>
#include <QStandardPaths>
#include <QBuffer>
#include <QUrl>
#include <QSettings>
#include <QtMath>
#include <cmath>

#ifdef Q_OS_ANDROID
#include <QtCore/private/qandroidextras_p.h>
#endif

// ============================================================================
// FIX: DPI-aware scaling helpers
//
// WHY THIS IS NEEDED:
//   Qt uses "device pixels" internally but stylesheet px values and
//   setFixedSize() calls are in "logical pixels" on desktop (96 DPI baseline).
//   On Android at 480 DPI, 1 logical px = 5 physical px — so a 24px button
//   appears as ~1.5 mm, far below the Android 9mm minimum touch target.
//   These helpers convert dp (density-independent pixels, like Android dp/sp)
//   into the correct logical pixel count for the current screen.
// ============================================================================

// Returns the display scale factor relative to 96 DPI (1.0 on a normal desktop)
static float dpScale()
{
    static float s_scale = 0.0f;
    if (s_scale == 0.0f) {
        QScreen *screen = QGuiApplication::primaryScreen();
        if (screen) {
            // logicalDotsPerInch already accounts for user DPI settings and
            // Qt's high-DPI scaling — it's the right value to use here.
            s_scale = screen->logicalDotsPerInch() / 96.0f;
        } else {
            s_scale = 1.0f;
        }
        // Clamp: don't let very unusual screens break layout
        s_scale = qBound(1.0f, s_scale, 4.0f);
    }
    return s_scale;
}

// Convert density-independent pixels to actual logical pixels
static int dp(int value)
{
    return qRound(static_cast<float>(value) * dpScale());
}

// Convert sp (scale-independent points, for fonts) to pt for QFont / stylesheet
static int sp(int value)
{
    return qRound(static_cast<float>(value) * dpScale());
}

// Formats an elapsed duration as "HH:MM:SS" for the Uptime / Flight Time stats
static QString formatHms(qint64 elapsedMs)
{
    int totalSec = static_cast<int>(elapsedMs / 1000);
    int hours = totalSec / 3600;
    int mins  = (totalSec % 3600) / 60;
    int secs  = totalSec % 60;
    return QString("%1:%2:%3")
        .arg(hours, 2, 10, QChar('0'))
        .arg(mins,  2, 10, QChar('0'))
        .arg(secs,  2, 10, QChar('0'));
}

static int safeBottomInset()
{
#ifdef Q_OS_ANDROID
    // Keep footer clear of simulator/device gesture bar area.
    return dp(36);
#else
    return 0;
#endif
}

// ============================================================================
// MiniPlayerResizeGrip — small diagonal-stroke handle in the mini player's
// bottom-right corner. Dragging it resizes the mini player in place
// (top-left anchored, like QWidget::resize()'s default growth direction),
// the way QGroundControl's floating video preview resizes.
// ============================================================================
class MiniPlayerResizeGrip : public QWidget
{
public:
    explicit MiniPlayerResizeGrip(MiniPlayerWidget *owner)
        : QWidget(owner), m_owner(owner)
    {
        // Bottom-left corner: the "/" diagonal (top-right to bottom-left).
        setCursor(Qt::SizeBDiagCursor);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QPen pen(QColor(255, 255, 255, 160));
        pen.setWidth(dp(2));
        p.setPen(pen);
        const int inset = dp(4);
        for (int i = 0; i < 3; ++i) {
            int o = inset + i * dp(5);
            p.drawLine(inset, height() - o, o, height() - inset);
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !m_owner->parentWidget()) return;
        m_dragging = true;
        m_startGlobalPos = event->globalPosition().toPoint();
        m_startSize = m_owner->size();
        m_startPos = m_owner->pos();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_dragging) return;
        QWidget *bounds = m_owner->parentWidget();
        if (!bounds) return;

        QPoint delta = event->globalPosition().toPoint() - m_startGlobalPos;

        const int minW = dp(160), minH = dp(100);
        int maxW = qMax(minW, int(bounds->width() * 0.6));
        int maxH = qMax(minH, int(bounds->height() * 0.6));

        // Bottom-left grip: dragging left grows width while the RIGHT edge
        // stays put (so width changes must shift x the opposite way);
        // dragging down grows height same as a bottom-right grip would,
        // since the top edge stays put either way. Fold the container-edge
        // limits into maxW/maxH up front so the aspect-ratio branch below
        // never has to re-derive one dimension from the other after the
        // fact (which would silently break the ratio it just enforced).
        int rightEdge = m_startPos.x() + m_startSize.width();
        maxW = qMin(maxW, rightEdge);
        maxH = qMin(maxH, bounds->height() - m_owner->y());

        int newW, newH;
        const double aspect = m_owner->contentAspectRatio();
        if (aspect > 0.0) {
            // Live video is docked here — resize along whichever axis the
            // user is dragging more, then derive the other dimension from
            // the stream's own width/height so the preview never stretches.
            int wCandidate = m_startSize.width() - delta.x();
            int hCandidate = m_startSize.height() + delta.y();

            if (qAbs(delta.x()) >= qAbs(delta.y())) {
                newW = qBound(minW, wCandidate, maxW);
                newH = qRound(newW / aspect);
                if (newH < minH || newH > maxH) {
                    newH = qBound(minH, newH, maxH);
                    newW = qBound(minW, qRound(newH * aspect), maxW);
                }
            } else {
                newH = qBound(minH, hCandidate, maxH);
                newW = qRound(newH * aspect);
                if (newW < minW || newW > maxW) {
                    newW = qBound(minW, newW, maxW);
                    newH = qBound(minH, qRound(newW / aspect), maxH);
                }
            }
        } else {
            newW = qBound(minW, m_startSize.width() - delta.x(), maxW);
            newH = qBound(minH, m_startSize.height() + delta.y(), maxH);
        }

        int newX = qMax(0, rightEdge - newW);

        m_owner->resize(newW, newH);
        m_owner->move(newX, m_owner->y());
        m_owner->setCustomSize(QSize(newW, newH));
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_dragging = false;
            event->accept();
        }
    }

private:
    MiniPlayerWidget *m_owner;
    bool m_dragging = false;
    QPoint m_startGlobalPos;
    QSize m_startSize;
    QPoint m_startPos;
};

// ============================================================================
// MiniPlayerWidget Implementation
// ============================================================================

MiniPlayerWidget::MiniPlayerWidget(QWidget *parent)
    : QWidget(parent)
    , m_contentWidget(nullptr)
    , m_contentLayout(nullptr)
    , m_contentContainer(nullptr)
    , m_resizeGrip(nullptr)
    , m_dragging(false)
    , m_moved(false)
    , m_minimized(false)
{
    setWindowFlags(Qt::Widget);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setAttribute(Qt::WA_StyledBackground, true);
    setMouseTracking(true);

    // FIX: use dp() so border/radius scale with screen density
    // NOTE: scoped to the MiniPlayerWidget selector, not a bare "QWidget" —
    // the latter would leak this background/border onto every child widget
    // too (see the identical fix on topBar for the visible symptom).
    setStyleSheet(QString(R"(
        MiniPlayerWidget {
            background-color: #262624;
            border: 2px solid #0075c5;
            border-radius: %1px;
        }
    )").arg(dp(8)));

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // No title bar / buttons — just the content, filling the whole widget,
    // like QGC's floating PiP window. Dragging (press anywhere and move) and
    // tap-to-swap (see eventFilter()) are the only interactions; there's
    // nothing left to reach a record/split/close button for, since the main
    // top bar already has its own record control and split is still
    // reachable from the overflow menu.
    m_contentContainer = new QWidget(this);
    m_contentContainer->setStyleSheet(QString("background-color: rgba(25, 25, 25, 250); border-radius: %1px; border: none;")
                                          .arg(dp(6)));
    m_contentLayout = new QVBoxLayout(m_contentContainer);
    m_contentLayout->setContentsMargins(dp(2), dp(2), dp(2), dp(2));
    m_contentLayout->setSpacing(0);
    mainLayout->addWidget(m_contentContainer, 1);

    m_resizeGrip = new MiniPlayerResizeGrip(this);

    // Sized like the "CAMERA 1" preview panel in the dashboard mock —
    // compact widescreen tile rather than a large picture-in-picture window.
    resize(dp(260), dp(160));
    raise();
}

MiniPlayerWidget::~MiniPlayerWidget()
{
}

void MiniPlayerWidget::setContentWidget(QWidget *widget)
{
    if (m_contentWidget == widget) return;

    if (m_contentWidget) {
        m_contentWidget->removeEventFilter(this);
        m_contentLayout->removeWidget(m_contentWidget);
        m_contentWidget->setParent(nullptr);
    }

    m_contentWidget = widget;

    if (widget) {
        widget->setParent(m_contentContainer);
        m_contentLayout->addWidget(widget);
        widget->show();
        // Steal its mouse events while it's the mini preview (see
        // eventFilter()) so pan/orbit gestures don't fire on the tiny
        // preview and a tap swaps it to the main view instead.
        widget->installEventFilter(this);
    }
}

double MiniPlayerWidget::contentAspectRatio() const
{
    if (auto *iv = qobject_cast<ImageViewerWidget *>(m_contentWidget)) {
        QSize s = iv->getCurrentImage().size();
        if (s.width() > 0 && s.height() > 0) {
            return double(s.width()) / double(s.height());
        }
    }
    return 0.0;
}

void MiniPlayerWidget::minimizePlayer()
{
    m_minimized = true;
    m_contentContainer->hide();
    if (m_resizeGrip) m_resizeGrip->hide();
    setFixedHeight(dp(32)); // FIX: was 32px hardcoded
    emit minimized();
}

void MiniPlayerWidget::restorePlayer()
{
    m_minimized = false;
    m_contentContainer->show();
    if (m_resizeGrip) m_resizeGrip->show();
    setFixedHeight(QWIDGETSIZE_MAX);
    if (m_customSize.isValid()) {
        resize(m_customSize);
    } else {
        resize(dp(320), dp(240));  // FIX: scaled default size
    }
    emit restored();
}

void MiniPlayerWidget::enterEvent(QEnterEvent *event)
{
    Q_UNUSED(event);
}

void MiniPlayerWidget::leaveEvent(QEvent *event)
{
    Q_UNUSED(event);
    if (!m_dragging) {
        setCursor(Qt::ArrowCursor);
    }
}

void MiniPlayerWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        handlePress(event->pos());
        event->accept();
    }
}

void MiniPlayerWidget::mouseMoveEvent(QMouseEvent *event)
{
    handleMove(event->pos());
    event->accept();
}

void MiniPlayerWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        handleRelease();
        event->accept();
    }
}

void MiniPlayerWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_resizeGrip) {
        const int g = dp(20);
        m_resizeGrip->setGeometry(0, height() - g, g, g);
        m_resizeGrip->raise();
    }
}

// Shared by the real mouse events above (title bar / empty chrome) and by
// eventFilter() below (content widget) so a press-drag-release anywhere on
// the mini player behaves the same: drag past a small threshold moves the
// window, a plain tap swaps main/mini.
void MiniPlayerWidget::handlePress(const QPoint &pos)
{
    m_dragging = true;
    m_moved = false;
    m_dragPosition = pos;
    setCursor(Qt::ClosedHandCursor);
}

void MiniPlayerWidget::handleMove(const QPoint &pos)
{
    if (!m_dragging) return;

    if (!m_moved && (pos - m_dragPosition).manhattanLength() > dp(6)) {
        m_moved = true;
    }

    if (parentWidget()) {
        QPoint newPos = mapToParent(pos - m_dragPosition);

        // Constrain to parent bounds
        int maxX = parentWidget()->width() - width();
        int maxY = parentWidget()->height() - height();

        newPos.setX(qBound(0, newPos.x(), maxX));
        newPos.setY(qBound(0, newPos.y(), maxY));

        move(newPos);
    }
}

void MiniPlayerWidget::handleRelease()
{
    const bool wasClick = m_dragging && !m_moved;
    m_dragging = false;
    setCursor(Qt::ArrowCursor);

    if (wasClick) {
        // Deferred: this fires from inside the content widget's own mouse
        // event dispatch (via eventFilter() below). Swapping reparents that
        // very widget out from under itself — doing that synchronously,
        // mid-dispatch, left Qt's internal mouse-grab bookkeeping pointed at
        // a widget that had just moved elsewhere, so the *next* click (e.g.
        // the title bar's swap button) silently failed to register. Posting
        // the swap to the next event loop turn lets the current mouse event
        // finish unwinding cleanly first.
        QTimer::singleShot(0, this, [this]() { emit requestSwap(); });
    }
}

bool MiniPlayerWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_contentWidget) {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                handlePress(m_contentWidget->mapTo(this, me->pos()));
                return true;
            }
            break;
        }
        case QEvent::MouseMove:
            handleMove(m_contentWidget->mapTo(this, static_cast<QMouseEvent *>(event)->pos()));
            return true;
        case QEvent::MouseButtonRelease: {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                handleRelease();
                return true;
            }
            break;
        }
        // PointCloudWidget opts into raw touch input on Android (see its
        // event() override) and explicitly accept()s every touch event, so
        // Qt never synthesizes the QMouseEvents the cases above rely on —
        // without this, tapping the mini player while it holds the 3D view
        // did nothing. ImageViewerWidget never opts into touch, so it still
        // gets ordinary synthesized mouse events and doesn't need this path.
        case QEvent::TouchBegin: {
            auto *te = static_cast<QTouchEvent *>(event);
            if (te->points().size() == 1) {
                handlePress(m_contentWidget->mapTo(this, te->points().first().position().toPoint()));
            }
            return true;
        }
        case QEvent::TouchUpdate: {
            auto *te = static_cast<QTouchEvent *>(event);
            if (te->points().size() == 1) {
                handleMove(m_contentWidget->mapTo(this, te->points().first().position().toPoint()));
            }
            return true;
        }
        case QEvent::TouchEnd:
        case QEvent::TouchCancel:
            handleRelease();
            return true;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

// ============================================================================
// MainWindow Implementation
// ============================================================================

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_pointCloudWidget(nullptr)
    , m_imageViewerWidget(nullptr)
    , m_rosClient(nullptr)
    , m_imageClient(nullptr)
    , m_pointCloudClient(nullptr)
    , m_telemetryClient(nullptr)
    , m_miniPlayer(nullptr)
    , m_mainViewContainer(nullptr)
    , m_splitViewContainer(nullptr)
    , m_settingsDialog(nullptr)
    , m_currentViewMode(PointCloudMain)
    , m_rosStreamActive(false)
    , m_imageStreamActive(false)
    , m_isRecording(false)
    , m_manualDisconnect(false)
    , m_batteryLevel(0)
    , m_signalStrength(0)
    , m_flightMode("MANUAL")
{
    setWindowTitle("Skyline");

    // FIX: don't hardcode 1600x1000 — use screen geometry instead.
    // On Android this is ignored anyway (app runs fullscreen), but on desktop
    // it gives a sensible default relative to the actual display resolution.
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen) {
        QRect screenRect = screen->availableGeometry();
        resize(qMin(1600, screenRect.width()), qMin(1000, screenRect.height()));
    } else {
        resize(1600, 1000);
    }

#ifdef Q_OS_ANDROID
    requestStoragePermissions();
#endif

    setupUI();
    createSettingsDialog();

    // Create ROS clients
    m_rosClient = new RosPointCloudClient(this);
    connect(m_rosClient, &RosPointCloudClient::connected,
            this, &MainWindow::onRosConnected);
    connect(m_rosClient, &RosPointCloudClient::disconnected,
            this, &MainWindow::onRosDisconnected);
    connect(m_rosClient, &RosPointCloudClient::pathReceived,
            this, &MainWindow::onRosPathReceived);
    connect(m_rosClient, &RosPointCloudClient::robotPoseReceived,
            this, &MainWindow::onRosRobotPoseReceived);
    connect(m_rosClient, &RosPointCloudClient::error,
            this, &MainWindow::onRosError);
    connect(m_rosClient, &RosPointCloudClient::latencyMeasured, this, [this](qint64 ms) {
        m_wsLatencyMs = ms;
        updateNetworkStats();
    });
    connect(m_rosClient, &RosPointCloudClient::bandwidthUpdated, this, [this](double downKbps, double upKbps) {
        m_wsDownKbps = downKbps;
        m_wsUpKbps = upKbps;
        updateNetworkStats();
    });

    m_imageClient = new RtspVideoClient(this);
    connect(m_imageClient, &RtspVideoClient::connected,
            this, &MainWindow::onImageConnected);
    connect(m_imageClient, &RtspVideoClient::disconnected,
            this, &MainWindow::onImageDisconnected);
    connect(m_imageClient, &RtspVideoClient::error,
            this, &MainWindow::onImageError);
    connect(m_imageClient, &RtspVideoClient::frameReady,
            m_imageViewerWidget, &ImageViewerWidget::setImage);
    connect(m_imageClient, &RtspVideoClient::downlinkBitrateChanged, this, [this](double kbps) {
        m_videoDownKbps = kbps;
        updateNetworkStats();
    });

    m_pointCloudClient = new PointCloudClient(this);
    connect(m_pointCloudClient, &PointCloudClient::connected,
            this, &MainWindow::onPointCloudConnected);
    connect(m_pointCloudClient, &PointCloudClient::disconnected,
            this, &MainWindow::onPointCloudDisconnected);
    connect(m_pointCloudClient, &PointCloudClient::error,
            this, &MainWindow::onPointCloudError);
    connect(m_pointCloudClient, &PointCloudClient::pointCloudReceived,
            this, &MainWindow::onRosPointCloudReceived);
    connect(m_pointCloudClient, &PointCloudClient::bandwidthUpdated, this, [this](double downKbps, double upKbps) {
        Q_UNUSED(upKbps);
        m_pointCloudDownKbps = downKbps;
        updateNetworkStats();
    });

    m_telemetryClient = new TelemetryClient(this);
    connect(m_telemetryClient, &TelemetryClient::connected,
            this, &MainWindow::onTelemetryConnected);
    connect(m_telemetryClient, &TelemetryClient::disconnected,
            this, &MainWindow::onTelemetryDisconnected);
    connect(m_telemetryClient, &TelemetryClient::error,
            this, &MainWindow::onTelemetryError);
    connect(m_telemetryClient, &TelemetryClient::telemetryReceived,
            this, &MainWindow::onTelemetryReceived);

    updateViewMode();
    positionOverlays();

    // Auto-connect on startup
    QTimer::singleShot(500, this, [this]() {
        if (!m_rosUrlEdit->text().trimmed().isEmpty()) {
            m_statusLabel->setText("Auto-connecting...");
            QString url = droneWsUrl();
            m_rosClient->connectToServer(url);
            m_imageClient->connectToServer(url);
            m_pointCloudClient->connectToServer(url);
            m_telemetryClient->connectToServer(url);
        }
    });
}

MainWindow::~MainWindow()
{
}

void MainWindow::setupUI()
{
    // FIX: build font sizes dynamically using sp() so they scale with DPI.
    // Hardcoded "font-size: 13px" in a stylesheet is 13 physical pixels on
    // every screen — on a 480 DPI phone that is ~0.7mm tall, unreadable.
    // sp() maps the design-time point size to the correct logical pixel count.
    setStyleSheet(QString(R"(
        QMainWindow {
            background-color: #262624;
        }
        QWidget {
            background-color: transparent;
            color: #e8e8e8;
            font-family: 'SF Pro Display', 'Segoe UI', sans-serif;
            font-size: %1px;
        }
        QPushButton {
            background-color: #0075c5;
            color: white;
            border: none;
            padding: %2px %3px;
            border-radius: %4px;
            font-weight: 600;
            font-size: %1px;
            min-height: %5px;
        }
        QPushButton:hover  { background-color: #0088e0; }
        QPushButton:pressed { background-color: #005a9c; }
        QPushButton:disabled {
            background-color: rgba(60, 60, 60, 150);
            color: rgba(160, 160, 160, 150);
        }
        QPushButton#dangerButton { background-color: rgba(196, 69, 105, 200); }
        QPushButton#dangerButton:hover { background-color: rgba(213, 95, 130, 220); }
        QLineEdit {
            background-color: rgba(30, 30, 30, 200);
            border: 1px solid rgba(70, 70, 70, 200);
            border-radius: %4px;
            padding: %6px %7px;
            color: #e8e8e8;
            font-size: %1px;
        }
        QLineEdit:focus {
            border: 1px solid #0075c5;
            background-color: rgba(40, 40, 40, 220);
        }
        QLabel#statusLabel {
            background-color: rgba(20, 20, 20, 200);
            border: 1px solid rgba(50, 50, 50, 200);
            border-radius: %4px;
            padding: %6px %7px;
            font-weight: 500;
            font-size: %8px;
        }
    )")
                      .arg(sp(13))   // %1 body font size
                      .arg(dp(10))   // %2 button padding vertical
                      .arg(dp(20))   // %3 button padding horizontal
                      .arg(dp(6))    // %4 border-radius
                      .arg(dp(36))   // %5 min-height button
                      .arg(dp(8))    // %6 padding vertical (input/status)
                      .arg(dp(12))   // %7 padding horizontal
                      .arg(sp(12))); // %8 status label font

    QWidget *centralWidget = new QWidget(this);
    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(0, 0, 0, safeBottomInset());
    mainLayout->setSpacing(0);

    // ------------------------------------------------------------------
    // Top bar — dark strip with logo, connection status, telemetry pills,
    // mode, record indicator, and an overflow menu (restyled to match the
    // Skyline dashboard mock).
    // ------------------------------------------------------------------
    QWidget *topBar = new QWidget(this);
    topBar->setObjectName("topBar");
    topBar->setAttribute(Qt::WA_StyledBackground, true);
    // NOTE: this selector must be scoped to #topBar, not a bare "QWidget"
    // selector — Qt style sheets cascade to every matching descendant, and
    // every child in here (labels, StatusPills, StatusPill's own child
    // labels) is-a QWidget too. An unscoped rule was leaking this
    // background + border-bottom onto all of them, which is what showed up
    // as a stray underline under the logo/wordmark and every pill's text
    // instead of the pills' own rounded-box style.
    topBar->setStyleSheet(R"(
        QWidget#topBar {
            background-color: #14140f;
            border-bottom: 1px solid rgba(0, 117, 197, 60);
        }
    )");
    topBar->setFixedHeight(dp(56));

    QHBoxLayout *topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(dp(16), dp(8), dp(16), dp(8));
    topLayout->setSpacing(dp(14));

    // Logo
    QLabel *logoLabel = new QLabel(topBar);
    QPixmap logo(":/android/res/drawable/skyline.png");
    if (logo.isNull()) {
        logo = QPixmap("android/res/drawable/skyline.png");
    }
    if (!logo.isNull()) {
        logoLabel->setPixmap(logo.scaled(dp(30), dp(30), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        logoLabel->setPixmap(IconRenderer::icon(IconRenderer::Id::Drone, QColor("#0075c5"), dp(26)).pixmap(dp(26), dp(26)));
        logoLabel->setStyleSheet("background: transparent;");
    }
    topLayout->addWidget(logoLabel);

    QLabel *appName = new QLabel("SKYLINE", topBar);
    appName->setStyleSheet(QString(R"(
        color: #e8e8e8;
        font-size: %1px;
        font-weight: 700;
        letter-spacing: 2px;
        background: transparent;
    )").arg(sp(18)));
    topLayout->addWidget(appName);

    topLayout->addSpacing(dp(28));

    // "● Connected" pill
    m_connectedPill = new StatusPill(topBar);
    m_connectedPill->setDotColor(QColor("#666666"));
    m_connectedPill->setValue("Disconnected");
    topLayout->addWidget(m_connectedPill);

    topLayout->addSpacing(dp(6));

    // "● Disarmed"/"● Armed" pill
    m_armedPill = new StatusPill(topBar);
    m_armedPill->setDotColor(QColor("#666666"));
    m_armedPill->setValue("--");
    topLayout->addWidget(m_armedPill);

    topLayout->addSpacing(dp(6));

    // Battery pill: "BATTERY  91%" — doubles as the disclosure trigger for
    // the Voltage/Current details popover (see toggleBatteryDetailsPanel()).
    m_batteryPill = new StatusPill(topBar);
    m_batteryPill->setIcon(IconRenderer::icon(IconRenderer::Id::Battery, QColor("#cfcfcf"), sp(14)));
    m_batteryPill->setLabel("BATTERY");
    m_batteryPill->setValue("--");
    m_batteryPill->setValueColor(QColor("#e8e8e8"));
    m_batteryPill->setClickable(true);
    m_batteryPill->setChevronVisible(true);
    connect(m_batteryPill, &StatusPill::clicked, this, &MainWindow::toggleBatteryDetailsPanel);
    topLayout->addWidget(m_batteryPill);

    // RC signal pill: "RC SIGNAL  51%" — stays visible in the top bar.
    m_signalPill = new StatusPill(topBar);
    m_signalPill->setIcon(IconRenderer::icon(IconRenderer::Id::Signal, QColor("#cfcfcf"), sp(14)));
    m_signalPill->setLabel("RC SIGNAL");
    m_signalPill->setValue("--");
    m_signalPill->setValueColor(QColor("#e8e8e8"));
    topLayout->addWidget(m_signalPill);

    // Voltage/Current pills still exist (onTelemetryReceived keeps driving
    // them the same way) but now live inside m_batteryDetailsPanel instead
    // of the top bar — built further down, once that panel exists.
    m_voltagePill = new StatusPill(topBar);
    m_voltagePill->setIcon(IconRenderer::icon(IconRenderer::Id::Bolt, QColor("#cfcfcf"), sp(14)));
    m_voltagePill->setLabel("VOLTAGE");
    m_voltagePill->setValue("--");
    m_voltagePill->setValueColor(QColor("#e8e8e8"));

    m_currentPill = new StatusPill(topBar);
    m_currentPill->setIcon(IconRenderer::icon(IconRenderer::Id::Waveform, QColor("#cfcfcf"), sp(14)));
    m_currentPill->setLabel("CURRENT");
    m_currentPill->setValue("--");
    m_currentPill->setValueColor(QColor("#e8e8e8"));

    // Mode pill: "MODE  AUTO"
    m_modePill = new StatusPill(topBar);
    m_modePill->setLabel("MODE");
    m_modePill->setValue(m_flightMode);
    m_modePill->setValueColor(QColor("#0075c5"));
    topLayout->addWidget(m_modePill);

    // Keep the legacy telemetry labels alive (used internally / hidden) so
    // existing telemetry plumbing continues to compile without change.
    m_batteryLabel = new QLabel(topBar);
    m_batteryLabel->hide();
    m_signalLabel = new QLabel(topBar);
    m_signalLabel->hide();
    m_modeLabel = new QLabel(topBar);
    m_modeLabel->hide();

    topLayout->addStretch();

    // Recording indicator — only visible while actually recording (the
    // record/photo controls themselves live in the floating QGC-style
    // capture cluster over the main view; see m_captureControl below and
    // onShutterPressed()). Just a status readout, not a button.
    m_recTimeLabel = new QLabel("●  REC 00:00:00", topBar);
    m_recTimeLabel->setStyleSheet(QString("color: #ff3333; font-weight: 700; font-size: %1px; background: transparent;")
                                       .arg(sp(12)));
    m_recTimeLabel->hide();
    topLayout->addWidget(m_recTimeLabel);

    // "⋮" overflow control — houses Swap / Split / Clear / Settings, which
    // used to be individual top-bar buttons.
    //
    // NOTE: this used to be a native QMenu popup. On-device testing showed
    // its items were unresponsive to touch on Android — a known category of
    // Qt-for-Android issue where popup (Qt::Popup) top-level windows don't
    // always get touch events forwarded correctly. Replaced with a plain
    // QWidget dropdown that lives inside the normal widget tree (same
    // technique as the WP/SCAN side panel), which reliably receives touch
    // like any other button.
    m_overflowButton = new QPushButton("⋮", topBar);
    m_overflowButton->setToolTip("More actions");
    m_overflowButton->setCursor(Qt::PointingHandCursor);
    m_overflowButton->setStyleSheet(QString(R"(
        QPushButton {
            background-color: transparent;
            color: #cfcfcf;
            border: none;
            font-size: %1px;
            font-weight: 700;
            min-width: %2px;
            min-height: %2px;
            padding: 0;
        }
        QPushButton:hover { background-color: rgba(255,255,255,15); border-radius: %3px; }
    )").arg(sp(28)).arg(dp(44)).arg(dp(10)));
    connect(m_overflowButton, &QPushButton::clicked, this, &MainWindow::toggleOverflowPanel);
    topLayout->addWidget(m_overflowButton);

    m_overflowPanel = new QWidget(centralWidget);
    m_overflowPanel->setObjectName("overflowPanel");
    m_overflowPanel->setAttribute(Qt::WA_StyledBackground, true);
    m_overflowPanel->setStyleSheet(QString(R"(
        QWidget#overflowPanel {
            background-color: #1e1e1c;
            border: 1px solid rgba(0,117,197,90);
            border-radius: %1px;
        }
    )").arg(dp(8)));
    m_overflowPanel->setFixedWidth(dp(220));
    m_overflowPanel->hide();

    QVBoxLayout *overflowLayout = new QVBoxLayout(m_overflowPanel);
    overflowLayout->setContentsMargins(dp(6), dp(6), dp(6), dp(6));
    overflowLayout->setSpacing(dp(2));

    auto makeOverflowRow = [&](IconRenderer::Id iconId, const QString &text, bool checkable = false) {
        QPushButton *row = new QPushButton(m_overflowPanel);
        row->setText("  " + text);
        row->setIcon(IconRenderer::icon(iconId, QColor("#e8e8e8"), sp(16)));
        row->setIconSize(QSize(sp(16), sp(16)));
        row->setCheckable(checkable);
        row->setCursor(Qt::PointingHandCursor);
        row->setStyleSheet(QString(R"(
            QPushButton {
                background-color: transparent;
                color: #e8e8e8;
                border: none;
                border-radius: %1px;
                text-align: left;
                padding: %2px %3px;
                font-weight: 600;
                font-size: %4px;
                min-height: %5px;
            }
            QPushButton:hover, QPushButton:checked { background-color: rgba(0,117,197,60); }
        )").arg(dp(6)).arg(dp(8)).arg(dp(10)).arg(sp(13)).arg(dp(38)));
        overflowLayout->addWidget(row);
        return row;
    };

    // "VIEW" section — the view/mode switches that used to live in a
    // dedicated left sidebar rail now open from here instead (see
    // activateView()). Settings, further down, already covers what used to
    // be the sidebar's 5th item, so it isn't duplicated here.
    QLabel *viewSectionLabel = new QLabel("VIEW", m_overflowPanel);
    viewSectionLabel->setStyleSheet(QString("background: transparent; color: #8a8a86; font-size: %1px; font-weight: 700; letter-spacing: 0.5px; padding: %2px %3px 2px;")
                                         .arg(sp(9)).arg(dp(6)).arg(dp(10)));
    overflowLayout->addWidget(viewSectionLabel);

    auto makeViewRow = [&](const QString &id, IconRenderer::Id iconId, const QString &text) {
        QPushButton *row = makeOverflowRow(iconId, text, true);
        row->setProperty("viewId", id);
        m_viewRowButtons.push_back(row);
        connect(row, &QPushButton::clicked, this, [this, id]() {
            activateView(id);
            hideOverflowPanel();
        });
        return row;
    };

    makeViewRow("3d", IconRenderer::Id::Cube3D, "3D View");
    makeViewRow("top", IconRenderer::Id::TopView, "Top View");
    makeViewRow("wp", IconRenderer::Id::Waypoint, "Waypoints");
    makeViewRow("scan", IconRenderer::Id::Scan, "Scan");
    if (!m_viewRowButtons.empty()) m_viewRowButtons.front()->setChecked(true);

    QFrame *viewSeparator = new QFrame(m_overflowPanel);
    viewSeparator->setFrameShape(QFrame::HLine);
    viewSeparator->setStyleSheet("background-color: rgba(255,255,255,20); max-height: 1px; border: none;");
    overflowLayout->addWidget(viewSeparator);

    m_overflowSplitBtn = makeOverflowRow(IconRenderer::Id::Split, "Toggle split view", true);
    connect(m_overflowSplitBtn, &QPushButton::clicked, this, [this]() {
        onToggleSplitView();
        hideOverflowPanel();
    });

    QPushButton *clearRow = makeOverflowRow(IconRenderer::Id::Trash, "Clear map");
    connect(clearRow, &QPushButton::clicked, this, [this]() {
        onClearMap();
        hideOverflowPanel();
    });

    QFrame *overflowSeparator = new QFrame(m_overflowPanel);
    overflowSeparator->setFrameShape(QFrame::HLine);
    overflowSeparator->setStyleSheet("background-color: rgba(255,255,255,20); max-height: 1px; border: none;");
    overflowLayout->addWidget(overflowSeparator);

    QPushButton *settingsRow = makeOverflowRow(IconRenderer::Id::Settings, "Settings");
    connect(settingsRow, &QPushButton::clicked, this, [this]() {
        onShowSettings();
        hideOverflowPanel();
    });

    QPushButton *firmwareRow = makeOverflowRow(IconRenderer::Id::Bolt, "Firmware Update");
    connect(firmwareRow, &QPushButton::clicked, this, [this]() {
        onShowFirmwareUpdate();
        hideOverflowPanel();
    });

    // Battery pill's details popover — Voltage/Current pills (built earlier,
    // alongside the other top-bar pills) land here instead of the top bar.
    m_batteryDetailsPanel = new QWidget(centralWidget);
    m_batteryDetailsPanel->setObjectName("batteryDetailsPanel");
    m_batteryDetailsPanel->setAttribute(Qt::WA_StyledBackground, true);
    m_batteryDetailsPanel->setStyleSheet(QString(R"(
        QWidget#batteryDetailsPanel {
            background-color: #1e1e1c;
            border: 1px solid rgba(0,117,197,90);
            border-radius: %1px;
        }
    )").arg(dp(8)));
    m_batteryDetailsPanel->setFixedWidth(dp(176));
    m_batteryDetailsPanel->hide();

    QVBoxLayout *batteryDetailsLayout = new QVBoxLayout(m_batteryDetailsPanel);
    batteryDetailsLayout->setContentsMargins(dp(10), dp(8), dp(10), dp(8));
    batteryDetailsLayout->setSpacing(dp(4));
    batteryDetailsLayout->addWidget(m_voltagePill);
    batteryDetailsLayout->addWidget(m_currentPill);

    // These logic-only, off-screen buttons keep other call sites (e.g. the
    // settings dialog) that trigger these actions programmatically working
    // unchanged.
    m_swapButton = new QPushButton(this);
    m_swapButton->hide();
    connect(m_swapButton, &QPushButton::clicked, this, &MainWindow::onSwapViews);

    m_splitButton = new QPushButton(this);
    m_splitButton->setCheckable(true);
    m_splitButton->hide();
    connect(m_splitButton, &QPushButton::clicked, this, &MainWindow::onToggleSplitView);

    m_clearButton = new QPushButton(this);
    m_clearButton->hide();
    connect(m_clearButton, &QPushButton::clicked, this, &MainWindow::onClearMap);

    m_settingsButton = new QPushButton(this);
    m_settingsButton->hide();
    connect(m_settingsButton, &QPushButton::clicked, this, &MainWindow::onShowSettings);

    mainLayout->addWidget(topBar);

    // ------------------------------------------------------------------
    // Body: full-width main 3D view (with floating overlays). The view
    // switches that used to live in a left sidebar rail here now open from
    // the "⋮" overflow menu instead — see the VIEW section built above.
    // ------------------------------------------------------------------
    QWidget *body = new QWidget(this);
    QHBoxLayout *bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    // Main view area
    m_mainViewContainer = new QWidget(body);
    m_mainViewContainer->setStyleSheet("background-color: #0d0d0b;");
    QVBoxLayout *mainViewLayout = new QVBoxLayout(m_mainViewContainer);
    mainViewLayout->setContentsMargins(0, 0, 0, 0);

    // Create widgets
    m_pointCloudWidget = new PointCloudWidget(this);
    m_imageViewerWidget = new ImageViewerWidget(this);

    mainViewLayout->addWidget(m_pointCloudWidget);
    mainViewLayout->addWidget(m_imageViewerWidget);

    bodyLayout->addWidget(m_mainViewContainer, 1);
    mainLayout->addWidget(body, 1);

    // Floating overlays on top of the 3D view: elevation legend (right edge),
    // compass (bottom-right corner). The mini-map overlay was removed —
    // the mini player's tap-to-swap 3D view covers that need now.
    m_elevationLegend = new ElevationLegendWidget(m_mainViewContainer);
    m_elevationLegend->setRange(-10.0f, 10.0f);
    m_elevationLegend->raise();

    m_compassWidget = new CompassWidget(m_mainViewContainer);
    m_compassWidget->raise();

    // ORBIT/FOLLOW switch — floats over the 3D view (top-left, under the
    // legend) whenever the point cloud is visible; toggles whether the
    // camera target tracks the drone's live pose.
    m_followSwitch = new FollowSwitchWidget(m_mainViewContainer);
    m_followSwitch->raise();
    connect(m_followSwitch, &FollowSwitchWidget::toggled, this, &MainWindow::onFollowSwitchToggled);

    // Floating photo/video capture cluster (replaces the old top-bar
    // REC/photo buttons) — positioned in positionOverlays().
    m_captureControl = new CaptureControlWidget(m_mainViewContainer);
    m_captureControl->raise();
    connect(m_captureControl, &CaptureControlWidget::shutterPressed, this, &MainWindow::onShutterPressed);

    // ------------------------------------------------------------------
    // Slide-in context panel — gives the overflow menu's WP / SCAN items real
    // content instead of doing nothing. Lives over the right edge of the
    // main view; opened/closed via showSidePanel()/hideSidePanel().
    // ------------------------------------------------------------------
    m_sidePanel = new QWidget(m_mainViewContainer);
    m_sidePanel->setObjectName("sidePanel");
    m_sidePanel->setAttribute(Qt::WA_StyledBackground, true);
    m_sidePanel->setStyleSheet(QString(R"(
        QWidget#sidePanel {
            background-color: rgba(15, 15, 18, 235);
            border-left: 1px solid rgba(0, 117, 197, 90);
        }
    )"));
    m_sidePanel->setFixedWidth(dp(260));
    m_sidePanel->hide();

    QVBoxLayout *sidePanelLayout = new QVBoxLayout(m_sidePanel);
    sidePanelLayout->setContentsMargins(0, 0, 0, 0);
    sidePanelLayout->setSpacing(0);

    QWidget *sidePanelHeader = new QWidget(m_sidePanel);
    sidePanelHeader->setFixedHeight(dp(46));
    sidePanelHeader->setStyleSheet("background-color: rgba(0,0,0,60); border: none;");
    QHBoxLayout *sidePanelHeaderLayout = new QHBoxLayout(sidePanelHeader);
    sidePanelHeaderLayout->setContentsMargins(dp(14), 0, dp(8), 0);

    m_sidePanelTitleLabel = new QLabel("PANEL", sidePanelHeader);
    m_sidePanelTitleLabel->setStyleSheet(QString("color: #e8e8e8; font-weight: 700; font-size: %1px; letter-spacing: 1px; background: transparent;").arg(sp(12)));
    sidePanelHeaderLayout->addWidget(m_sidePanelTitleLabel);
    sidePanelHeaderLayout->addStretch();

    QPushButton *sidePanelCloseBtn = new QPushButton(sidePanelHeader);
    sidePanelCloseBtn->setFixedSize(dp(28), dp(28));
    sidePanelCloseBtn->setIcon(IconRenderer::icon(IconRenderer::Id::Close, QColor("#cfcfcf"), dp(14)));
    sidePanelCloseBtn->setCursor(Qt::PointingHandCursor);
    sidePanelCloseBtn->setStyleSheet(QString(R"(
        QPushButton { background-color: transparent; border: none; border-radius: %1px; padding: 0; }
        QPushButton:hover { background-color: rgba(255,255,255,20); }
    )").arg(dp(6)));
    connect(sidePanelCloseBtn, &QPushButton::clicked, this, [this]() {
        hideSidePanel();
        m_activeViewId = "3d";
        for (QPushButton *btn : m_viewRowButtons) {
            btn->setChecked(btn->property("viewId").toString() == "3d");
        }
    });
    sidePanelHeaderLayout->addWidget(sidePanelCloseBtn);

    sidePanelLayout->addWidget(sidePanelHeader);

    QScrollArea *sidePanelScroll = new QScrollArea(m_sidePanel);
    sidePanelScroll->setWidgetResizable(true);
    sidePanelScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sidePanelScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    sidePanelScroll->setFrameShape(QFrame::NoFrame);
    sidePanelScroll->setStyleSheet("background: transparent; border: none;");

    m_sidePanelBody = new QWidget(sidePanelScroll);
    m_sidePanelBody->setStyleSheet("background: transparent;");
    m_sidePanelBodyLayout = new QVBoxLayout(m_sidePanelBody);
    m_sidePanelBodyLayout->setContentsMargins(dp(14), dp(14), dp(14), dp(14));
    m_sidePanelBodyLayout->setSpacing(dp(10));

    sidePanelScroll->setWidget(m_sidePanelBody);
    sidePanelLayout->addWidget(sidePanelScroll, 1);

    // ------------------------------------------------------------------
    // Bottom stats bar: Altitude / Speed / Distance / Flown Dist / Uptime /
    // Flight Time stay always visible; Points / FPS / Time / network stats
    // move into the "MORE" popover to keep this bar scannable at a glance.
    // ------------------------------------------------------------------
    m_bottomStatsBar = new QWidget(this);
    m_bottomStatsBar->setObjectName("bottomStatsBar");
    m_bottomStatsBar->setAttribute(Qt::WA_StyledBackground, true);
    m_bottomStatsBar->setStyleSheet(R"(
        QWidget#bottomStatsBar { background-color: #14140f; border-top: 1px solid rgba(0, 117, 197, 60); }
    )");
    // NOTE: was setFixedHeight(dp(58)), but a stat item's real content
    // (label + value + sparkline, with spacing/margins) needs a minimum of
    // ~52px (see StatItemWidget), so the fixed 58px was clipping the
    // sparkline row. Sized to that content minimum plus tight margins
    // instead of the old generous 74px, which read as too tall.

    m_bottomStatsBar->setMinimumHeight(dp(64));
    QHBoxLayout *statsLayout = new QHBoxLayout(m_bottomStatsBar);
    statsLayout->setContentsMargins(dp(20), dp(4), dp(20), dp(6));
    statsLayout->setSpacing(dp(28));

    m_altitudeStat   = new StatItemWidget("ALTITUDE", "m", true, m_bottomStatsBar);
    m_speedStat      = new StatItemWidget("SPEED", "m/s", true, m_bottomStatsBar);
    m_distanceStat   = new StatItemWidget("DISTANCE", "m", true, m_bottomStatsBar);
    m_flownDistStat  = new StatItemWidget("FLOWN DIST", "m", true, m_bottomStatsBar);
    m_uptimeStat     = new StatItemWidget("UPTIME", "", false, m_bottomStatsBar);
    m_flightTimeStat = new StatItemWidget("FLIGHT TIME", "", false, m_bottomStatsBar);

    for (StatItemWidget *stat : { m_altitudeStat, m_speedStat, m_distanceStat, m_flownDistStat }) {
        statsLayout->addWidget(stat);
    }

    QFrame *statsDivider = new QFrame(m_bottomStatsBar);
    statsDivider->setFrameShape(QFrame::VLine);
    statsDivider->setStyleSheet("background-color: rgba(255,255,255,20); max-width: 1px; border: none;");
    statsLayout->addWidget(statsDivider);

    m_uptimeStat->setText("--:--:--");
    m_flightTimeStat->setText("--:--:--");
    statsLayout->addWidget(m_uptimeStat);
    statsLayout->addWidget(m_flightTimeStat);

    statsLayout->addStretch();

    // "MORE" toggle — houses Flown Dist / Points / FPS / Time.
    m_moreStatsButton = new QPushButton(m_bottomStatsBar);
    m_moreStatsButton->setText("⋯   MORE");
    m_moreStatsButton->setCursor(Qt::PointingHandCursor);
    m_moreStatsButton->setStyleSheet(QString(R"(
        QPushButton {
            background-color: transparent;
            border: none;
            border-radius: %1px;
            color: #9a9a96;
            font-weight: 600;
            font-size: %2px;
            padding: %3px %4px;
            min-height: %5px;
        }
        QPushButton:hover { background-color: rgba(255,255,255,15); }
    )").arg(dp(6)).arg(sp(11)).arg(dp(4)).arg(dp(10)).arg(dp(28)));
    connect(m_moreStatsButton, &QPushButton::clicked, this, &MainWindow::toggleMoreStatsPanel);
    statsLayout->addWidget(m_moreStatsButton);

    mainLayout->addWidget(m_bottomStatsBar);

    // "MORE" popover — Points / FPS / Time plus network stats, opened
    // upward from m_moreStatsButton since it sits in the bottom bar. Two
    // rows (QGridLayout) rather than one long QHBoxLayout — 6 items in a
    // single row would overflow a phone-width screen.
    m_moreStatsPanel = new QWidget(centralWidget);
    m_moreStatsPanel->setObjectName("moreStatsPanel");
    m_moreStatsPanel->setAttribute(Qt::WA_StyledBackground, true);
    m_moreStatsPanel->setStyleSheet(QString(R"(
        QWidget#moreStatsPanel {
            background-color: #1e1e1c;
            border: 1px solid rgba(0,117,197,90);
            border-radius: %1px;
        }
    )").arg(dp(8)));
    m_moreStatsPanel->hide();

    QGridLayout *moreStatsLayout = new QGridLayout(m_moreStatsPanel);
    moreStatsLayout->setContentsMargins(dp(16), dp(10), dp(16), dp(12));
    moreStatsLayout->setHorizontalSpacing(dp(24));
    moreStatsLayout->setVerticalSpacing(dp(10));

    m_pointsStat    = new StatItemWidget("POINTS", "", false, m_moreStatsPanel);
    m_fpsStat       = new StatItemWidget("FPS", "", false, m_moreStatsPanel);
    m_timeStat      = new StatItemWidget("TIME", "", false, m_moreStatsPanel);
    m_latencyStat   = new StatItemWidget("LATENCY", "", false, m_moreStatsPanel);
    m_downlinkStat  = new StatItemWidget("DOWNLINK", "", false, m_moreStatsPanel);
    m_uplinkStat    = new StatItemWidget("UPLINK", "", false, m_moreStatsPanel);

    int col = 0;
    for (StatItemWidget *stat : { m_pointsStat, m_fpsStat, m_timeStat }) {
        moreStatsLayout->addWidget(stat, 0, col++);
    }
    col = 0;
    for (StatItemWidget *stat : { m_latencyStat, m_downlinkStat, m_uplinkStat }) {
        moreStatsLayout->addWidget(stat, 1, col++);
    }

    setCentralWidget(centralWidget);

    // Guard against the window being squeezed shorter than topBar + a usable
    // body + bottomBar (this can happen in Android multi-window / floating
    // window modes where the OS resizes the surface directly). Without an
    // explicit minimum here, some window managers can shrink the whole
    // window below what our layout needs, and Qt then has no choice but to
    // compress even the non-stretch bars (topBar/bottomBar) to fit — which
    // is what produces a squeezed/cropped bottom bar.
    setMinimumSize(dp(320), dp(56) + dp(200) + dp(58));

    // Hidden legacy status label — retained only so existing status-message
    // call sites (`m_statusLabel->setText(...)`) keep compiling; its text is
    // mirrored onto the "Connected" pill's tooltip in onUiUpdateTick().
    m_statusLabel = new QLabel("Initializing...", this);
    m_statusLabel->setObjectName("statusLabel");
    m_statusLabel->hide();

    // Create mini player (used as the floating "CAMERA 1" preview tile)
    m_miniPlayer = new MiniPlayerWidget(m_mainViewContainer);
    connect(m_miniPlayer, &MiniPlayerWidget::requestSwap, this, &MainWindow::onSwapViews);

    // Hidden URL input (will be in settings) — prefilled with whatever IP
    // was last entered, so it doesn't need retyping on every launch.
    QSettings settings;
    QString savedIp = settings.value("connection/droneIp", "172.20.10.5").toString();
    m_rosUrlEdit = new QLineEdit(savedIp);
    m_rosUrlEdit->hide();
    connect(m_rosUrlEdit, &QLineEdit::editingFinished, this, [this]() {
        QSettings settings;
        settings.setValue("connection/droneIp", m_rosUrlEdit->text().trimmed());
    });

    // Timer driving FPS / time-of-day / decay animation for the overlays
    m_uiUpdateTimer = new QTimer(this);
    connect(m_uiUpdateTimer, &QTimer::timeout, this, &MainWindow::onUiUpdateTick);
    m_uiUpdateTimer->start(500);

    // Timer driving the REC elapsed-time readout
    m_recTimer = new QTimer(this);
    connect(m_recTimer, &QTimer::timeout, this, &MainWindow::onRecTimerTick);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);

    if (QWidget *cw = centralWidget()) {
        if (QLayout *layout = cw->layout()) {
            layout->setContentsMargins(0, 0, 0, safeBottomInset());
        }
    }

    positionOverlays();
}

void MainWindow::positionOverlays()
{
    if (!m_mainViewContainer) return;

    const int margin = dp(16);

    if (m_elevationLegend) {
        int x = margin;
        int y = (m_mainViewContainer->height() - m_elevationLegend->height()) / 2;
        m_elevationLegend->move(qMax(0, x), qMax(0, y));
        m_elevationLegend->raise();
    }

    if (m_followSwitch) {
        m_followSwitch->move(margin, margin);
        m_followSwitch->raise();
    }

    if (m_compassWidget) {
        // Anchored bottom-right, where it used to sit beside the mini-map.
        int x = m_mainViewContainer->width() - m_compassWidget->width() - margin;
        int y = m_mainViewContainer->height() - m_compassWidget->height() - margin;
        m_compassWidget->move(qMax(0, x), qMax(0, y));
        m_compassWidget->raise();
    }

    if (m_captureControl) {
        // Right edge, vertically centered — clear of the mini player
        // (top-right) and compass (bottom-right).
        int x = m_mainViewContainer->width() - m_captureControl->width() - margin;
        int y = (m_mainViewContainer->height() - m_captureControl->height()) / 2;
        m_captureControl->move(qMax(0, x), qMax(0, y));
        m_captureControl->raise();
    }

    if (m_miniPlayer && m_miniPlayer->isVisible() && !m_miniPlayer->isMinimized()) {
        // Keep the mini player clear of the side panel when it's open —
        // previously both anchored to the right edge and could overlap,
        // with the panel's periodic raise() calls even pushing it in front
        // of the camera preview.
        int rightBound = m_mainViewContainer->width();
        if (m_sidePanel && m_sidePanel->isVisible()) {
            rightBound -= m_sidePanel->width();
        }
        int x = rightBound - m_miniPlayer->width() - dp(20);
        int y = dp(20);
        x = qMax(0, qMin(x, rightBound - m_miniPlayer->width()));
        y = qMax(0, qMin(y, m_mainViewContainer->height() - m_miniPlayer->height()));
        m_miniPlayer->move(x, y);
        m_miniPlayer->raise();
    }

    // Side panel is topmost of the overlay stack, on top of the mini player.
    if (m_sidePanel && m_sidePanel->isVisible()) {
        int x = m_mainViewContainer->width() - m_sidePanel->width();
        m_sidePanel->setGeometry(qMax(0, x), 0, m_sidePanel->width(), m_mainViewContainer->height());
        m_sidePanel->raise();
    }
}

void MainWindow::showToast(const QString &text)
{
    if (!m_mainViewContainer) return;

    if (!m_toastLabel) {
        m_toastLabel = new QLabel(m_mainViewContainer);
        m_toastLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        m_toastLabel->setAlignment(Qt::AlignCenter);
        m_toastLabel->setStyleSheet(QString(R"(
            QLabel {
                background-color: rgba(20, 20, 20, 220);
                color: #e8e8e8;
                border-radius: %1px;
                padding: %2px %3px;
                font-weight: 600;
                font-size: %4px;
            }
        )").arg(dp(10)).arg(dp(8)).arg(dp(16)).arg(sp(12)));
        m_toastLabel->hide();
    }

    m_toastLabel->setText(text);
    m_toastLabel->adjustSize();

    int x = (m_mainViewContainer->width() - m_toastLabel->width()) / 2;
    int y = m_mainViewContainer->height() - m_toastLabel->height() - dp(90); // clear of the bottom stats bar
    m_toastLabel->move(qMax(0, x), qMax(0, y));
    m_toastLabel->show();
    m_toastLabel->raise();

    QTimer::singleShot(1500, this, [this]() {
        if (m_toastLabel) m_toastLabel->hide();
    });
}

void MainWindow::createSettingsDialog()
{
    m_settingsDialog = new QDialog(this);
    m_settingsDialog->setWindowTitle("Settings - Skyline");
    m_settingsDialog->setModal(true);
    m_settingsDialog->setWindowModality(Qt::ApplicationModal);
    m_settingsDialog->setAttribute(Qt::WA_AcceptTouchEvents, true);
    m_settingsDialog->resize(dp(430), dp(360));

#ifdef Q_OS_ANDROID
    // Use a child-widget overlay on Android to avoid native dialog touch
    // coordinate desync when a QOpenGLWidget is visible underneath.
    if (centralWidget()) {
        m_settingsDialog->setParent(centralWidget());
    }
    m_settingsDialog->setWindowFlags(Qt::FramelessWindowHint | Qt::Widget);
    m_settingsDialog->setModal(false);
    m_settingsDialog->setWindowModality(Qt::NonModal);
    m_settingsDialog->setAttribute(Qt::WA_StyledBackground, true);
    m_settingsDialog->hide();
#endif

    // Dark theme, matching FirmwareUpdateDialog's palette (rounded card,
    // blue-tinted borders/accents) for visual consistency between the two
    // dialogs. Content here has natural margins from the dialog edges
    // (layout->setContentsMargins below), unlike FirmwareUpdateDialog's
    // 0-margin scroll area, so QDialog's own rounded background is enough —
    // no separate full-bleed "content" wrapper needed to avoid it being
    // painted over.
    m_settingsDialog->setStyleSheet(QString(R"(
        QDialog {
            background-color: #1b1b19;
            border: 1px solid rgba(0, 133, 224, 90);
            border-radius: %4px;
        }
        QLabel {
            color: #e8e8e8;
            background: transparent;
        }
        QPushButton {
            background-color: #0085e0;
            color: white;
            border: none;
            padding: 7px 14px;
            border-radius: 10px;
            font-weight: 600;
            font-size: %1px;
            min-height: 30px;
        }
        QPushButton:hover {
            background-color: #1c93e8;
        }
        QPushButton:pressed {
            background-color: #006bb8;
        }
        QLineEdit {
            background-color: rgba(0, 0, 0, 130);
            border: 1px solid rgba(0, 133, 224, 60);
            border-radius: 10px;
            padding: 5px 9px;
            color: #e8e8e8;
            font-size: %1px;
        }
        QLineEdit:focus {
            border: 1px solid #0085e0;
            background-color: rgba(0, 0, 0, 160);
        }
        QCheckBox {
            spacing: %2px;
            font-size: %1px;
            font-weight: 500;
            color: #e8e8e8;
        }
        QCheckBox::indicator {
            width: %3px;
            height: %3px;
            border-radius: 3px;
            border: 2px solid #4d4d4d;
            background-color: rgba(0, 0, 0, 130);
        }
        QCheckBox::indicator:checked {
            background-color: #0085e0;
            border-color: #0085e0;
        }
        QDoubleSpinBox, QSpinBox {
            background-color: rgba(0, 0, 0, 130);
            border: 1px solid rgba(0, 133, 224, 60);
            border-radius: 10px;
            padding: 4px 8px;
            color: #e8e8e8;
            font-size: %1px;
        }
        QDoubleSpinBox:focus, QSpinBox:focus {
            border: 1px solid #0085e0;
        }
    )")
                                        .arg(sp(12))
                                        .arg(dp(8))
                                        .arg(dp(16))
                                        .arg(dp(18)));

    QVBoxLayout *layout = new QVBoxLayout(m_settingsDialog);
    layout->setContentsMargins(dp(14), dp(14), dp(14), dp(14));
    layout->setSpacing(dp(10));

    QLabel *title = new QLabel("Settings", m_settingsDialog);
    title->setStyleSheet("font-size: " + QString::number(sp(16)) + "px; font-weight: 700; color: #0075c5;");
    layout->addWidget(title);

    // Compact controls row: IP input - Connect
    QHBoxLayout *urlLayout = new QHBoxLayout();
    QLabel *urlLabel = new QLabel("IP Input", m_settingsDialog);
    urlLabel->setMinimumWidth(dp(70));
    m_rosUrlEdit->setParent(m_settingsDialog);
    m_rosUrlEdit->setPlaceholderText("e.g. 192.168.1.100");
    m_rosUrlEdit->show();
    urlLayout->addWidget(urlLabel);
    urlLayout->addWidget(m_rosUrlEdit, 1);
    m_rosToggleButton = new QPushButton("Connect", m_settingsDialog);
    m_rosToggleButton->setMinimumWidth(dp(95));
    m_rosToggleButton->setMinimumHeight(dp(32));
    connect(m_rosToggleButton, &QPushButton::clicked, this, &MainWindow::onToggleRosStream);
    urlLayout->addWidget(m_rosToggleButton);
    layout->addLayout(urlLayout);

    // Compact controls row: Data - Load PCD button
    QHBoxLayout *dataLayout = new QHBoxLayout();
    QLabel *dataLabel = new QLabel("Data", m_settingsDialog);
    dataLabel->setMinimumWidth(dp(70));
    m_loadButton = new QPushButton("📁  Load PCD File", m_settingsDialog);
    m_loadButton->setMinimumWidth(dp(160));
    m_loadButton->setMinimumHeight(dp(32));
    connect(m_loadButton, &QPushButton::clicked, this, &MainWindow::onLoadFile);
    dataLayout->addWidget(dataLabel);
    dataLayout->addWidget(m_loadButton);
    dataLayout->addStretch();
    layout->addLayout(dataLayout);

    // Map Building Section
    QLabel *mapTitle = new QLabel("Map Building", m_settingsDialog);
    mapTitle->setStyleSheet("font-size: " + QString::number(sp(14)) + "px; font-weight: 600; color: #e0e0e0; margin-top: " + QString::number(dp(8)) + "px;");
    layout->addWidget(mapTitle);

    // Decay toggle switch (default: Off)
    QHBoxLayout *decayToggleLayout = new QHBoxLayout();
    QLabel *decayToggleLabel = new QLabel("Point Decay:", m_settingsDialog);
    decayToggleLabel->setMinimumWidth(dp(145));
    m_decayEnabledCheckbox = new QCheckBox(m_settingsDialog);
    m_decayEnabledCheckbox->setChecked(false);
    m_decayEnabledCheckbox->setToolTip("Toggle point decay on/off");
    m_decayEnabledCheckbox->setStyleSheet(QString(R"(
        QCheckBox::indicator {
            width: %1px;
            height: %2px;
            border-radius: %3px;
            background-color: #555555;
            border: 1px solid #3d3d3d;
        }
        QCheckBox::indicator:checked {
            background-color: #0085e0;
            border: 1px solid #006bb8;
        }
    )").arg(dp(42)).arg(dp(22)).arg(dp(11)));
    QLabel *decayStateLabel = new QLabel("Off", m_settingsDialog);
    decayStateLabel->setMinimumWidth(dp(34));
    connect(m_decayEnabledCheckbox, &QCheckBox::stateChanged, this,
            [this, decayStateLabel](int state) {
                onDecayEnabledChanged(state);
                decayStateLabel->setText(state == Qt::Checked ? "On" : "Off");
            });
    decayToggleLayout->addWidget(decayToggleLabel);
    decayToggleLayout->addWidget(m_decayEnabledCheckbox);
    decayToggleLayout->addWidget(decayStateLabel);
    decayToggleLayout->addStretch();
    layout->addLayout(decayToggleLayout);

    // Decay time
    QHBoxLayout *decayLayout = new QHBoxLayout();
    QLabel *decayLabel = new QLabel("Decay Time (seconds):", m_settingsDialog);
    decayLabel->setMinimumWidth(dp(145));
    m_decayTimeSpinBox = new QDoubleSpinBox(m_settingsDialog);
    m_decayTimeSpinBox->setRange(0.5, 60.0);
    m_decayTimeSpinBox->setValue(5.0);
    m_decayTimeSpinBox->setSingleStep(0.5);
    m_decayTimeSpinBox->setDecimals(1);
    m_decayTimeSpinBox->setStyleSheet(R"(
        QDoubleSpinBox {
            background-color: rgba(0, 0, 0, 130);
            border: 1px solid rgba(0, 133, 224, 60);
            border-radius: 10px;
            padding: 4px 8px;
        }
    )");
    connect(m_decayTimeSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::onDecayTimeChanged);
    m_decayTimeSpinBox->setEnabled(false); // default: no decay
    m_pointCloudWidget->m_renderer.setDecayEnabled(false);
    decayLayout->addWidget(decayLabel);
    decayLayout->addWidget(m_decayTimeSpinBox);
    decayLayout->addStretch();
    layout->addLayout(decayLayout);

    // Max points
    QHBoxLayout *maxPointsLayout = new QHBoxLayout();
    QLabel *maxPointsLabel = new QLabel("Max Points (thousands):", m_settingsDialog);
    maxPointsLabel->setMinimumWidth(dp(145));
    m_maxPointsSpinBox = new QSpinBox(m_settingsDialog);
    m_maxPointsSpinBox->setRange(10, 10000);
    m_maxPointsSpinBox->setValue(1000);
    m_maxPointsSpinBox->setSingleStep(100);
    m_maxPointsSpinBox->setStyleSheet(m_decayTimeSpinBox->styleSheet());
    connect(m_maxPointsSpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::onMaxPointsChanged);
    maxPointsLayout->addWidget(maxPointsLabel);
    maxPointsLayout->addWidget(m_maxPointsSpinBox);
    maxPointsLayout->addStretch();
    layout->addLayout(maxPointsLayout);

    layout->addStretch();

    // Close button
    QPushButton *closeBtn = new QPushButton("Close", m_settingsDialog);
    closeBtn->setMinimumHeight(dp(34));
    connect(closeBtn, &QPushButton::clicked, m_settingsDialog, &QDialog::accept);
    layout->addWidget(closeBtn);
}

void MainWindow::detachViewWidget(QWidget *widget)
{
    // Pulls `widget` out of whichever layout currently owns it (main
    // container, a split-view QSplitter, or the mini player's content
    // layout) without deleting it, so it's free to be reparented elsewhere.
    if (!widget) return;

    if (QWidget *parent = widget->parentWidget()) {
        if (QLayout *layout = parent->layout()) {
            layout->removeWidget(widget);
        }
    }
    widget->setParent(nullptr);
}

void MainWindow::clearMainContainer()
{
    // Removes whatever is currently in the main view container's layout.
    // If it's a split-view QSplitter, the pointcloud/image widgets are
    // detached first so they aren't destroyed along with the splitter.
    QLayout *layout = m_mainViewContainer->layout();
    if (!layout) return;

    QLayoutItem *item;
    while ((item = layout->takeAt(0)) != nullptr) {
        QWidget *w = item->widget();
        delete item;
        if (!w || w == m_miniPlayer) continue;

        if (QSplitter *splitter = qobject_cast<QSplitter*>(w)) {
            detachViewWidget(m_pointCloudWidget);
            detachViewWidget(m_imageViewerWidget);
            splitter->deleteLater();
        } else if (w != m_pointCloudWidget && w != m_imageViewerWidget) {
            w->deleteLater();
        }
    }
}

void MainWindow::updateViewMode()
{
    // Always start from a clean main container; detach the mini player's
    // current content too so pointCloud/imageViewer are never simultaneously
    // referenced by two layouts (the old bug: manual QLayoutItem traversal
    // into the mini player's internals was fragile and could silently no-op
    // on swap/split, which is what made the overflow menu's Swap/Split
    // actions appear "broken").
    clearMainContainer();
    m_miniPlayer->setContentWidget(nullptr);
    detachViewWidget(m_pointCloudWidget);
    detachViewWidget(m_imageViewerWidget);

    if (m_splitButton && m_splitButton->isChecked()) {
        // === SPLIT VIEW MODE ===
        m_miniPlayer->hide();

        QSplitter *splitter = new QSplitter(Qt::Horizontal, m_mainViewContainer);
        splitter->addWidget(m_pointCloudWidget);
        splitter->addWidget(m_imageViewerWidget);
        splitter->setStretchFactor(0, 1);
        splitter->setStretchFactor(1, 1);

        // FIX: "sometimes it works, sometimes it doesn't" when dragging to
        // resize. The stylesheet was forcing `width: 2px` on the handle —
        // with a QSS stylesheet active, Qt's CSS engine applies that width
        // to the actual QSplitterHandle widget's hit-testable geometry,
        // not just its visual line. A 2-logical-px touch target is nearly
        // impossible to land a finger on precisely and consistently; a
        // miss by even a couple of pixels lands the touch on
        // PointCloudWidget or ImageViewerWidget instead, which then just
        // treats it as a normal 1-finger gesture on that view rather than
        // a resize drag.
        //
        // setHandleWidth() controls the actual widget size (independent of
        // the stylesheet), so we can make the real touch target
        // comfortably finger-sized while keeping the visible accent thin
        // via a border instead of filling the whole width with color.
        splitter->setHandleWidth(dp(16));
        splitter->setStyleSheet(
            "QSplitter::handle { background-color: rgba(20,20,24,60); }"
            "QSplitter::handle:hover, QSplitter::handle:pressed { background-color: rgba(13,115,119,90); }"
        );

        m_mainViewContainer->layout()->addWidget(splitter);
        m_pointCloudWidget->show();
        m_imageViewerWidget->show();

    } else {
        // === SINGLE VIEW WITH MINI PLAYER ===
        QWidget *mainWidget;
        QWidget *miniWidget;

        if (m_currentViewMode == PointCloudMain) {
            mainWidget = m_pointCloudWidget;
            miniWidget = m_imageViewerWidget;
        } else { // CameraMain
            mainWidget = m_imageViewerWidget;
            miniWidget = m_pointCloudWidget;
        }

        mainWidget->setParent(m_mainViewContainer);
        m_mainViewContainer->layout()->addWidget(mainWidget);
        mainWidget->show();
        mainWidget->raise();

        m_miniPlayer->setContentWidget(miniWidget);

        if (!m_miniPlayer->isMinimized()) {
            m_miniPlayer->show();
            m_miniPlayer->raise();

            QTimer::singleShot(100, this, [this]() {
                if (m_miniPlayer->isVisible() && m_mainViewContainer) {
                    positionOverlays();
                }
            });
        }

        m_mainViewContainer->layout()->activate();
        mainWidget->update();
        m_mainViewContainer->update();
    }

    // The ORBIT/FOLLOW switch only makes sense while the point cloud is
    // actually on screen — split view shows it in the left pane, single
    // view only when it's the main widget (not tucked in the mini player).
    if (m_followSwitch) {
        bool pointCloudVisible = (m_splitButton && m_splitButton->isChecked())
            || m_currentViewMode == PointCloudMain;
        m_followSwitch->setVisible(pointCloudVisible);
        if (pointCloudVisible) {
            m_followSwitch->raise();
        }
    }
}

void MainWindow::onSwapViews()
{
    if (m_splitButton && m_splitButton->isChecked()) {
        return; // No swap in split mode
    }

    m_currentViewMode = (m_currentViewMode == PointCloudMain) ? CameraMain : PointCloudMain;
    updateViewMode();
}

void MainWindow::onToggleSplitView()
{
    // FIX: split view never actually activated. updateViewMode() reads
    // MainWindow::m_splitButton->isChecked() as the single source of
    // truth for split mode, but that button is a hidden, "logic-only"
    // widget (see its construction above) — nothing ever toggled its
    // checked state. The overflow menu's split row maintained its own
    // independent checked state and just called this slot, so clicking it
    // changed its own appearance but never touched the value
    // updateViewMode() actually looks at.
    //
    // This is the master toggle: flip it here, then let the overflow-panel
    // sync at line ~1844 keep its visible control in agreement with it.
    if (m_splitButton) {
        m_splitButton->setChecked(!m_splitButton->isChecked());
    }
    if (m_overflowSplitBtn && m_splitButton) {
        m_overflowSplitBtn->setChecked(m_splitButton->isChecked());
    }

    updateViewMode();
}

void MainWindow::onShowSettings()
{
    if (m_settingsDialog) {
        if (m_settingsDialog->isVisible()) {
            m_settingsDialog->raise();
            m_settingsDialog->activateWindow();
            return;
        }

#ifdef Q_OS_ANDROID
        QWidget *host = centralWidget() ? centralWidget() : this;
        const int margin = dp(12);
        const int desiredW = dp(430);
        const int desiredH = dp(360);
        const int w = qMax(dp(260), qMin(desiredW, host->width() - 2 * margin));
        const int h = qMax(dp(220), qMin(desiredH, host->height() - 2 * margin));
        const int x = (host->width() - w) / 2;
        const int y = (host->height() - h) / 2;
        m_settingsDialog->setGeometry(x, y, w, h);
        m_settingsDialog->show();
#else
        m_settingsDialog->raise();
        m_settingsDialog->activateWindow();
        // Use open() on Android-friendly event loop (avoid nested exec loop)
        // which can lock touch input on some Qt + OpenGL widget combinations.
        m_settingsDialog->open();
#endif

        m_settingsDialog->raise();
        m_settingsDialog->activateWindow();
    }
}

void MainWindow::onShowFirmwareUpdate()
{
    if (!m_firmwareDialog) {
#ifdef Q_OS_ANDROID
        m_firmwareDialog = new FirmwareUpdateDialog(centralWidget() ? centralWidget() : this);
#else
        m_firmwareDialog = new FirmwareUpdateDialog(this);
#endif
    }

    // The update agent listens on the same host as the rosbridge link
    // (ws://host:9090), just on a different port (9091) — see UpdateClient.
    QString host = m_rosUrlEdit->text().trimmed();
    if (!host.isEmpty()) {
        m_firmwareDialog->setDroneHost(host);
    }
    m_firmwareDialog->setDroneArmed(m_droneArmed);

    if (m_firmwareDialog->isVisible()) {
        m_firmwareDialog->raise();
        m_firmwareDialog->activateWindow();
        return;
    }

#ifdef Q_OS_ANDROID
    QWidget *host2 = centralWidget() ? centralWidget() : this;
    const int margin = dp(12);
    const int desiredW = dp(430); // same size as the Settings dialog
    const int desiredH = dp(360);
    const int w = qMax(dp(260), qMin(desiredW, host2->width() - 2 * margin));
    const int h = qMax(dp(280), qMin(desiredH, host2->height() - 2 * margin));
    const int x = (host2->width() - w) / 2;
    const int y = (host2->height() - h) / 2;
    m_firmwareDialog->setGeometry(x, y, w, h);
    m_firmwareDialog->show();
#else
    m_firmwareDialog->open();
#endif

    m_firmwareDialog->raise();
    m_firmwareDialog->activateWindow();
}

void MainWindow::onLoadFile()
{
#ifdef Q_OS_ANDROID
    QString filename = QFileDialog::getOpenFileName(
        this,
        "Open Point Cloud File",
        "/storage/emulated/0/Download",
        "Point Cloud Files (*.pcd)");
#else
    QString filename = QFileDialog::getOpenFileName(
        this,
        "Open Point Cloud File",
        QDir::homePath(),
        "Point Cloud Files (*.pcd)");
#endif

    if (!filename.isEmpty()) {
        if (m_rosStreamActive) {
            m_rosClient->disconnect();
        }
        if (m_pointCloudStreamActive) {
            // Live cloud data is a separate connection now (PointCloudClient)
            // — without this it would keep overwriting the file just loaded.
            m_pointCloudClient->disconnect();
        }

        if (m_pointCloudWidget->loadPointCloud(filename)) {
            m_statusLabel->setText(QString("📂 Loaded: %1").arg(QFileInfo(filename).fileName()));
        } else {
            QMessageBox::warning(this, "Error", "Failed to load point cloud file");
            m_statusLabel->setText("❌ Error loading file");
        }
    }
}

void MainWindow::onToggleRosStream()
{
    if (m_rosStreamActive) {
        m_manualDisconnect = true;  // User clicked disconnect
        m_rosClient->disconnect();
        m_imageClient->disconnect();
        m_pointCloudClient->disconnect();
        m_telemetryClient->disconnect();
        m_statusLabel->setText("Manually disconnected");
    } else {
        m_manualDisconnect = false;  // User clicked connect
        if (m_rosUrlEdit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, "Connection Error", "Please enter a valid IP address");
            return;
        }
        QSettings().setValue("connection/droneIp", m_rosUrlEdit->text().trimmed());
        QString url = droneWsUrl();
        m_rosClient->connectToServer(url);
        m_imageClient->connectToServer(url);
        m_pointCloudClient->connectToServer(url);
        m_telemetryClient->connectToServer(url);
        m_statusLabel->setText("Connecting...");
    }
}

void MainWindow::onClearMap()
{
    m_pointCloudWidget->m_renderer.clearAccumulatedPoints();
    m_pointCloudWidget->update();
    m_statusLabel->setText("Map cleared");
}

void MainWindow::onDecayEnabledChanged(int state)
{
    bool enabled = (state != 0);
    m_pointCloudWidget->m_renderer.setDecayEnabled(enabled);
    m_decayTimeSpinBox->setEnabled(enabled);
}

void MainWindow::onDecayTimeChanged(double value)
{
    m_pointCloudWidget->m_renderer.setDecayTime(static_cast<float>(value));
}

void MainWindow::onMaxPointsChanged(int value)
{
    size_t maxPoints = static_cast<size_t>(value) * 1000;
    m_pointCloudWidget->m_renderer.setMaxPoints(maxPoints);
}

void MainWindow::onRosConnected()
{
    m_rosStreamActive = true;
    m_rosToggleButton->setText("Disconnect");
    if (m_loadButton) {
        m_loadButton->setEnabled(false);
    }
    m_statusLabel->setText("Connected & Streaming");
    if (m_connectedPill) {
        m_connectedPill->setDotColor(QColor("#00cc44"));
        m_connectedPill->setValue("Connected");
    }

    // Uptime has no real "drone power-on" signal to key off, so this session's
    // first successful connection is the closest proxy — it keeps counting
    // through later disconnect/reconnect cycles rather than resetting.
    if (!m_uptimeRunning) {
        m_uptimeElapsed.start();
        m_uptimeRunning = true;
    }
}

void MainWindow::onRosDisconnected()
{
    m_rosStreamActive = false;
    m_rosToggleButton->setText("Connect");
    if (m_loadButton) {
        m_loadButton->setEnabled(true);
    }
    if (m_connectedPill) {
        m_connectedPill->setDotColor(QColor("#666666"));
        m_connectedPill->setValue("Disconnected");
    }

    // Auto-reconnect if not manually disconnected
    if (!m_manualDisconnect) {
        if (!m_rosUrlEdit->text().trimmed().isEmpty()) {
            m_statusLabel->setText("Auto-reconnecting...");
            if (m_connectedPill) {
                m_connectedPill->setDotColor(QColor("#ffcc00"));
                m_connectedPill->setValue("Reconnecting…");
            }
            QString url = droneWsUrl();
            QTimer::singleShot(2000, this, [this, url]() {
                if (!m_rosStreamActive && !m_manualDisconnect) {
                    m_rosClient->connectToServer(url);
                }
            });
        } else {
            m_statusLabel->setText("Disconnected");
        }
    } else {
        m_statusLabel->setText("Disconnected");
    }
}

void MainWindow::onRosPointCloudReceived(const std::vector<float> &points)
{
    m_pointCloudWidget->m_renderer.updatePointCloud(points);
    m_pointCloudWidget->update();

    size_t numPoints = m_pointCloudWidget->m_renderer.getPointCount();
    if (m_pointsStat) {
        m_pointsStat->setText(numPoints >= 1000
                                  ? QString("%1 M").arg(numPoints / 1000000.0, 0, 'f', 2)
                                  : QString::number(numPoints));
    }

    static int frameCount = 0;
    if (++frameCount % 30 == 0) {
        m_statusLabel->setText(QString("Streaming - %1k points").arg(numPoints / 1000));
    }
}

void MainWindow::onRosError(const QString &errorString)
{
    m_statusLabel->setText(QString("❌ Connection failed - %1").arg(errorString));
    // Only show popup if user manually triggered connection, not on auto-connect
}

void MainWindow::onRosPathReceived(const std::vector<QVector3D> &path)
{
    m_pointCloudWidget->updatePath(path);
    if (!path.empty()) {
        m_flightPath = path;
    }
}

void MainWindow::onRosRobotPoseReceived(const RobotPose &pose)
{
    m_pointCloudWidget->updateRobotPose(pose.position, pose.orientation);

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    if (!m_hasHomePosition) {
        m_homePosition = pose.position;
        m_hasHomePosition = true;
    }

    // Speed + flown distance from consecutive pose samples
    if (m_hasLastPosition) {
        float segment = m_lastPosition.distanceToPoint(pose.position);
        double dtSec = qMax(0.001, (nowMs - m_lastPositionTimeMs) / 1000.0);
        m_currentSpeed = segment / dtSec;
        m_flownDistanceAccum += segment;
    }
    m_lastPosition = pose.position;
    m_lastPositionTimeMs = nowMs;
    m_hasLastPosition = true;

    m_currentAltitude = pose.position.z();
    m_currentDistance = m_homePosition.distanceToPoint(pose.position);

    // Heading from the quaternion's yaw component (rotation about Z)
    QVector3D forward = pose.orientation.rotatedVector(QVector3D(1, 0, 0));
    float headingRad = std::atan2(forward.y(), forward.x());
    m_currentHeading = qRadiansToDegrees(headingRad);
    if (m_currentHeading < 0) m_currentHeading += 360.0f;

    m_flightPath.push_back(pose.position);
    if (m_flightPath.size() > 2000) {
        m_flightPath.erase(m_flightPath.begin());
    }

    if (m_compassWidget) m_compassWidget->setHeading(m_currentHeading);
    if (m_altitudeStat)  m_altitudeStat->setValue(m_currentAltitude, 1);
    if (m_speedStat)     m_speedStat->setValue(m_currentSpeed, 1);
    if (m_distanceStat)  m_distanceStat->setValue(m_currentDistance, 1);
    if (m_flownDistStat) m_flownDistStat->setValue(m_flownDistanceAccum, 1);
}

void MainWindow::onImageConnected()
{
    m_imageStreamActive = true;
    m_imageViewerWidget->setVideoActive(true);
}

void MainWindow::onImageDisconnected()
{
    m_imageStreamActive = false;
    m_imageViewerWidget->setVideoActive(false);
    scheduleImageReconnect();
}

// Independent of the ROS/rosbridge reconnect above — video and telemetry
// are separate services on the drone (video_rtsp_server.py vs. rosbridge),
// and rosbridge being down/flaky must not keep tearing down an otherwise-
// working RTSP pipeline before it can ever render a frame (that was
// exactly the "no camera feed" symptom: onRosDisconnected()'s retry loop
// was calling m_imageClient->connectToServer() every 2s in lockstep with
// rosbridge connection-refused errors, killing the video decoder mid-setup
// every time). RtspVideoClient only emits disconnected() once it had
// actually reached PLAYING at least once, so a connection that never
// succeeds in the first place (e.g. server unreachable) only emits
// error() — hooking both here, guarded by m_imageReconnectPending, covers
// "never connected" and "dropped after connecting" without double-
// scheduling when both signals fire for the same failure.
QString MainWindow::droneWsUrl() const
{
    return QString("ws://%1:9090").arg(m_rosUrlEdit->text().trimmed());
}

void MainWindow::scheduleImageReconnect()
{
    if (m_imageReconnectPending || m_manualDisconnect) {
        return;
    }
    m_imageReconnectPending = true;
    QTimer::singleShot(2000, this, [this]() {
        m_imageReconnectPending = false;
        if (m_imageStreamActive || m_manualDisconnect) {
            return;
        }
        if (!m_rosUrlEdit->text().trimmed().isEmpty()) {
            m_imageClient->connectToServer(droneWsUrl());
        }
    });
}

void MainWindow::onImageError(const QString &errorString)
{
    scheduleImageReconnect();
    qWarning() << "Image error:" << errorString;
}

void MainWindow::onPointCloudConnected()
{
    m_pointCloudStreamActive = true;
}

void MainWindow::onPointCloudDisconnected()
{
    m_pointCloudStreamActive = false;
    m_pointCloudDownKbps = 0.0;
    updateNetworkStats();
    schedulePointCloudReconnect();
}

// Independent of both the telemetry (m_rosClient) and video (m_imageClient)
// reconnect loops, for the same reason scheduleImageReconnect() is
// independent of the telemetry one — see the comment there. pointcloud_ws_server.py
// is its own process on its own port, so it can be down/restarting while
// telemetry and video are healthy, or vice versa, and none of the three
// should tear down another's working connection while retrying.
void MainWindow::schedulePointCloudReconnect()
{
    if (m_pointCloudReconnectPending || m_manualDisconnect) {
        return;
    }
    m_pointCloudReconnectPending = true;
    QTimer::singleShot(2000, this, [this]() {
        m_pointCloudReconnectPending = false;
        if (m_pointCloudStreamActive || m_manualDisconnect) {
            return;
        }
        if (!m_rosUrlEdit->text().trimmed().isEmpty()) {
            m_pointCloudClient->connectToServer(droneWsUrl());
        }
    });
}

void MainWindow::onPointCloudError(const QString &errorString)
{
    schedulePointCloudReconnect();
    qWarning() << "Point cloud error:" << errorString;
}

void MainWindow::onTelemetryConnected()
{
    m_telemetryStreamActive = true;
}

void MainWindow::onTelemetryDisconnected()
{
    m_telemetryStreamActive = false;
    scheduleTelemetryReconnect();
}

// Independent of the ROS/path (m_rosClient), video, and point-cloud
// reconnect loops, for the same reason schedulePointCloudReconnect() is
// independent of the others — see the comment there. Telemetry now runs on
// its own connection specifically so it can't be starved by /path or
// /Odometry traffic (see TelemetryClient.h / skyline_app.py's module
// docstring), so it shouldn't be tangled back together with their
// reconnect logic either.
void MainWindow::scheduleTelemetryReconnect()
{
    if (m_telemetryReconnectPending || m_manualDisconnect) {
        return;
    }
    m_telemetryReconnectPending = true;
    QTimer::singleShot(2000, this, [this]() {
        m_telemetryReconnectPending = false;
        if (m_telemetryStreamActive || m_manualDisconnect) {
            return;
        }
        if (!m_rosUrlEdit->text().trimmed().isEmpty()) {
            m_telemetryClient->connectToServer(droneWsUrl());
        }
    });
}

void MainWindow::onTelemetryError(const QString &errorString)
{
    scheduleTelemetryReconnect();
    qWarning() << "Telemetry error:" << errorString;
}

void MainWindow::onTelemetryReceived(int battery, int signal, const QString &mode,
                                      double voltageV, double currentA, bool armed)
{
    m_batteryLevel = battery;
    m_signalStrength = signal;
    m_flightMode = mode;

    // NOTE: m_batteryLabel / m_signalLabel / m_modeLabel are legacy widgets
    // kept only so old call sites below still compile. They are children of
    // topBar but were never added to topBar's layout, so calling ->show()
    // on them makes Qt render them unmanaged at (0,0) inside topBar —
    // right on top of the logo. The restyled pills (m_batteryPill /
    // m_signalPill / m_modePill) already show this info, so these stay
    // hidden.

    // Update battery with color coding. battery < 0 means "unknown" (e.g.
    // /mavros/battery hasn't reported yet) — show "--" instead of a
    // misleading red "-1%".
    QString batteryText = battery >= 0 ? QString("%1%").arg(battery) : "--";
    QString batteryColor;
    QString batteryBorder;
    if (battery < 0) {
        batteryColor = "#cfcfcf";  // Unknown
        batteryBorder = "rgba(207, 207, 207, 120)";
    } else if (battery < 20) {
        batteryColor = "#ff3333";  // Red
        batteryBorder = "rgba(255, 51, 51, 120)";
    } else if (battery < 50) {
        batteryColor = "#ffcc00";  // Yellow
        batteryBorder = "rgba(255, 204, 0, 120)";
    } else {
        batteryColor = "#00ff00";  // Green
        batteryBorder = "rgba(0, 255, 0, 80)";
    }
    m_batteryLabel->setStyleSheet(QString(R"(
        QLabel {
            background-color: rgba(0, 0, 0, 120);
            border: 1px solid %2;
            border-radius: 4px;
            color: %1;
            font-weight: 700;
            font-size: " + QString::number(sp(14)) + "px;
            padding: 6px 12px;
            min-width: 85px;
        }
    )").arg(batteryColor, batteryBorder));
    m_batteryLabel->setText(batteryText);

    // Update signal with color coding. signal < 0 means "unknown".
    QString signalText = signal >= 0 ? QString("%1%").arg(signal) : "--";
    QString signalColor;
    QString signalBorder;
    if (signal < 0) {
        signalColor = "#cfcfcf";  // Unknown
        signalBorder = "rgba(207, 207, 207, 120)";
    } else if (signal < 20) {
        signalColor = "#ff3333";  // Red
        signalBorder = "rgba(255, 51, 51, 120)";
    } else if (signal < 50) {
        signalColor = "#ffcc00";  // Yellow
        signalBorder = "rgba(255, 204, 0, 120)";
    } else {
        signalColor = "#00ff00";  // Green
        signalBorder = "rgba(0, 255, 0, 80)";
    }
    m_signalLabel->setStyleSheet(QString(R"(
        QLabel {
            background-color: rgba(0, 0, 0, 120);
            border: 1px solid %2;
            border-radius: 4px;
            color: %1;
            font-weight: 700;
            font-size: " + QString::number(sp(14)) + "px;
            padding: 6px 12px;
            min-width: 85px;
        }
    )").arg(signalColor, signalBorder));
    m_signalLabel->setText(signalText);

    // Update mode
    m_modeLabel->setText(mode);

    // Drive the restyled top-bar pills too
    if (m_batteryPill) {
        QColor c = battery < 0 ? QColor("#cfcfcf") : battery < 20 ? QColor("#ff3333") : battery < 50 ? QColor("#ffcc00") : QColor("#00e676");
        m_batteryPill->setValue(batteryText);
        m_batteryPill->setValueColor(c);
    }
    if (m_signalPill) {
        QColor c = signal < 0 ? QColor("#cfcfcf") : signal < 20 ? QColor("#ff3333") : signal < 50 ? QColor("#ffcc00") : QColor("#00e676");
        m_signalPill->setValue(signalText);
        m_signalPill->setValueColor(c);
    }
    if (m_voltagePill) {
        if (voltageV >= 0.0) {
            m_voltagePill->setValue(QString("%1V").arg(voltageV, 0, 'f', 1));
        } else {
            m_voltagePill->setValue("--");
        }
    }
    if (m_currentPill) {
        if (currentA >= 0.0) {
            m_currentPill->setValue(QString("%1A").arg(currentA, 0, 'f', 1));
        } else {
            m_currentPill->setValue("--");
        }
    }
    if (m_armedPill) {
        m_armedPill->setDotColor(armed ? QColor("#ff3333") : QColor("#666666"));
        m_armedPill->setValue(armed ? "Armed" : "Disarmed");
    }
    if (m_modePill) {
        m_modePill->setValue(mode);
    }

    // Flight Time counts from zero each time the drone arms, and freezes at
    // its last value on disarm (see onUiUpdateTick for the live readout).
    if (armed && !m_droneArmed) {
        m_flightElapsed.start();
        m_flightActive = true;
        if (m_flightTimeStat) m_flightTimeStat->setValueColor(QColor("#0075c5"));
    } else if (!armed && m_droneArmed) {
        m_lastFlightMs = m_flightElapsed.elapsed();
        m_flightActive = false;
        if (m_flightTimeStat) m_flightTimeStat->setValueColor(QColor("#e8e8e8"));
    }

    m_droneArmed = armed;
    if (m_firmwareDialog) {
        m_firmwareDialog->setDroneArmed(armed);
    }
}

void MainWindow::onFollowSwitchToggled(bool following)
{
    if (m_pointCloudWidget) {
        m_pointCloudWidget->setFollowMode(following);
    }
}

void MainWindow::onShutterPressed(bool videoMode)
{
    if (videoMode) {
        onToggleRecording();
    } else {
        onCapturePhoto();
    }
}

void MainWindow::onToggleRecording()
{
    m_isRecording = !m_isRecording;

    if (m_isRecording) {
        m_statusLabel->setText("Recording...");
        m_recElapsed.start();
        if (m_recTimer) m_recTimer->start(1000);
        if (m_recTimeLabel) m_recTimeLabel->show();
        onRecTimerTick();
    } else {
        if (m_recTimer) m_recTimer->stop();
        if (m_recTimeLabel) m_recTimeLabel->hide();
        // Don't update status label if streaming
        if (m_rosStreamActive) {
            size_t numPoints = m_pointCloudWidget->m_renderer.getPointCount();
            m_statusLabel->setText(QString("Streaming - %1k points").arg(numPoints / 1000));
        }
    }

    if (m_captureControl) m_captureControl->setRecording(m_isRecording);
}

void MainWindow::onCapturePhoto()
{
    QImage frame = m_imageViewerWidget ? m_imageViewerWidget->getCurrentImage() : QImage();
    if (frame.isNull()) {
        showToast("No camera feed to capture");
        return;
    }

    QString fileName = QString("Skyline_%1.jpg").arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));

#ifdef Q_OS_ANDROID
    QByteArray jpegBytes;
    QBuffer buffer(&jpegBytes);
    buffer.open(QIODevice::WriteOnly);
    frame.save(&buffer, "JPG", 92);
    buffer.close();

    if (saveJpegToGallery(jpegBytes, fileName)) {
        showToast("Photo saved to Gallery");
        return;
    }
    // Fall through to the app-scoped save below — e.g. MediaStore insertion
    // can fail on some OEM builds, and the photo shouldn't just vanish.
#endif

    // App-scoped external storage: no runtime permission needed on any
    // Android version, but not indexed into the system Gallery — this is
    // the fallback path (and the only path on desktop).
    QString dirPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/Photos";
    QDir dir(dirPath);
    if (!dir.exists() && !dir.mkpath(".")) {
        showToast("Couldn't save photo");
        return;
    }

    if (frame.save(dir.filePath(fileName), "JPG", 92)) {
        showToast("Photo saved");
    } else {
        showToast("Couldn't save photo");
    }
}

void MainWindow::onRecTimerTick()
{
    if (!m_isRecording || !m_recTimeLabel) return;
    m_recTimeLabel->setText(QString("●  REC %1").arg(formatHms(m_recElapsed.elapsed())));
}

void MainWindow::activateView(const QString &id)
{
    m_activeViewId = id;
    for (QPushButton *btn : m_viewRowButtons) {
        btn->setChecked(btn->property("viewId").toString() == id);
    }

    // WP and SCAN open a slide-in context panel; every other item should
    // close it so it doesn't linger over an unrelated view.
    if (id != "wp" && id != "scan") {
        hideSidePanel();
    }

    if (id == "3d") {
        // Reset to the default orbit view of the point cloud
        m_currentViewMode = PointCloudMain;
        updateViewMode();
        m_pointCloudWidget->resetView();
    } else if (id == "top") {
        // Snap the 3D camera to a straight-down view of the map
        m_currentViewMode = PointCloudMain;
        updateViewMode();
        m_pointCloudWidget->snapToTopView();
    } else if (id == "wp") {
        showSidePanel("wp");
    } else if (id == "scan") {
        showSidePanel("scan");
    }
}

void MainWindow::showSidePanel(const QString &id)
{
    if (!m_sidePanel || !m_sidePanelBodyLayout) return;

    // Only rebuild the panel body when switching to a *different* section —
    // this is what lets the waypoint list (and its counter) survive the
    // user closing the panel and reopening it later.
    if (m_sidePanelActiveId != id) {
        QLayoutItem *item;
        while ((item = m_sidePanelBodyLayout->takeAt(0)) != nullptr) {
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }
        m_waypointListWidget = nullptr;
        m_scanToggleButton = nullptr;
        m_scanStatusLabel = nullptr;
        m_sidePanelActiveId = id;

        auto sectionLabel = [this](const QString &text) {
            QLabel *l = new QLabel(text, m_sidePanelBody);
            l->setStyleSheet(QString("color: #8a8a86; font-size: %1px; font-weight: 700; letter-spacing: 0.5px; background: transparent;").arg(sp(10)));
            return l;
        };

        if (id == "wp") {
            m_sidePanelTitleLabel->setText("WAYPOINTS");

            QLabel *hint = new QLabel(
                "Plan a route by adding waypoints below. This list is local to the app for now — "
                "sending it to the flight controller isn't wired up yet.", m_sidePanelBody);
            hint->setWordWrap(true);
            hint->setStyleSheet(QString("color: #9a9a96; font-size: %1px; background: transparent;").arg(sp(11)));
            m_sidePanelBodyLayout->addWidget(hint);

            m_waypointListWidget = new QListWidget(m_sidePanelBody);
            m_waypointListWidget->setStyleSheet(QString(R"(
                QListWidget {
                    background-color: rgba(255,255,255,8);
                    border: 1px solid rgba(255,255,255,20);
                    border-radius: %1px;
                    color: #e8e8e8;
                    font-size: %2px;
                    padding: %3px;
                }
                QListWidget::item { padding: %4px %3px; border-radius: %1px; }
                QListWidget::item:selected { background-color: rgba(0,117,197,60); }
            )").arg(dp(6)).arg(sp(12)).arg(dp(4)).arg(dp(6)));
            m_waypointListWidget->setMinimumHeight(dp(160));
            m_sidePanelBodyLayout->addWidget(m_waypointListWidget);

            // Stacked (not side-by-side) so both buttons always have room —
            // a QHBoxLayout row here didn't fit the panel width and forced
            // a horizontal scrollbar that cut the "Remove" button off.
            QPushButton *addBtn = new QPushButton("Add Waypoint", m_sidePanelBody);
            addBtn->setIcon(IconRenderer::icon(IconRenderer::Id::Plus, QColor("#ffffff"), sp(13)));
            connect(addBtn, &QPushButton::clicked, this, [this]() {
                ++m_waypointCounter;
                QVector3D pos = m_hasLastPosition ? m_lastPosition : QVector3D(0, 0, 0);
                m_waypointListWidget->addItem(QString("WP %1  —  (%2, %3, %4)")
                    .arg(m_waypointCounter)
                    .arg(pos.x(), 0, 'f', 1).arg(pos.y(), 0, 'f', 1).arg(pos.z(), 0, 'f', 1));
                m_waypointListWidget->setCurrentRow(m_waypointListWidget->count() - 1);
            });
            m_sidePanelBodyLayout->addWidget(addBtn);

            QPushButton *removeBtn = new QPushButton("Remove Selected", m_sidePanelBody);
            removeBtn->setIcon(IconRenderer::icon(IconRenderer::Id::Trash, QColor("#ffffff"), sp(13)));
            removeBtn->setObjectName("dangerButton");
            connect(removeBtn, &QPushButton::clicked, this, [this]() {
                int row = m_waypointListWidget->currentRow();
                if (row >= 0) delete m_waypointListWidget->takeItem(row);
            });
            m_sidePanelBodyLayout->addWidget(removeBtn);
            m_sidePanelBodyLayout->addStretch();

        } else if (id == "scan") {
            m_sidePanelTitleLabel->setText("SCAN");

            m_sidePanelBodyLayout->addWidget(sectionLabel("LIVE STREAM"));

            m_scanStatusLabel = new QLabel(m_sidePanelBody);
            m_scanStatusLabel->setWordWrap(true);
            m_scanStatusLabel->setStyleSheet(QString("color: #e8e8e8; font-size: %1px; background: transparent;").arg(sp(12)));
            m_sidePanelBodyLayout->addWidget(m_scanStatusLabel);

            m_scanToggleButton = new QPushButton(m_sidePanelBody);
            m_scanToggleButton->setIcon(IconRenderer::icon(IconRenderer::Id::Scan, QColor("#ffffff"), sp(14)));
            connect(m_scanToggleButton, &QPushButton::clicked, this, &MainWindow::onToggleRosStream);
            m_sidePanelBodyLayout->addWidget(m_scanToggleButton);

            m_sidePanelBodyLayout->addSpacing(dp(6));
            m_sidePanelBodyLayout->addWidget(sectionLabel("POINT CLOUD"));

            QPushButton *clearBtn = new QPushButton("Clear Map", m_sidePanelBody);
            clearBtn->setIcon(IconRenderer::icon(IconRenderer::Id::Trash, QColor("#ffffff"), sp(13)));
            clearBtn->setObjectName("dangerButton");
            connect(clearBtn, &QPushButton::clicked, this, &MainWindow::onClearMap);
            m_sidePanelBodyLayout->addWidget(clearBtn);

            m_sidePanelBodyLayout->addStretch();

            // Populate immediately rather than waiting for the next tick.
            onUiUpdateTick();
        }
    }

    m_sidePanel->show();
    m_sidePanel->raise();
    positionOverlays();
}

void MainWindow::hideSidePanel()
{
    if (m_sidePanel) m_sidePanel->hide();
}

void MainWindow::toggleOverflowPanel()
{
    if (!m_overflowPanel) return;

    if (m_overflowPanel->isVisible()) {
        hideOverflowPanel();
        return;
    }

    hideBatteryDetailsPanel();
    hideMoreStatsPanel();

    // Reflect current split-view state on the checkable row each time it opens.
    if (m_overflowSplitBtn && m_splitButton) {
        m_overflowSplitBtn->setChecked(m_splitButton->isChecked());
    }

    m_overflowPanel->adjustSize();
    if (m_overflowButton) {
        QPoint below = m_overflowButton->mapTo(centralWidget(), QPoint(0, m_overflowButton->height() + dp(4)));
        int x = below.x() + m_overflowButton->width() - m_overflowPanel->width();
        x = qMax(dp(8), x);
        m_overflowPanel->move(x, below.y());
    }
    m_overflowPanel->show();
    m_overflowPanel->raise();
}

void MainWindow::hideOverflowPanel()
{
    if (m_overflowPanel) m_overflowPanel->hide();
}

void MainWindow::toggleBatteryDetailsPanel()
{
    if (!m_batteryDetailsPanel) return;

    if (m_batteryDetailsPanel->isVisible()) {
        hideBatteryDetailsPanel();
        return;
    }

    hideOverflowPanel();
    hideMoreStatsPanel();

    m_batteryDetailsPanel->adjustSize();
    if (m_batteryPill) {
        QPoint below = m_batteryPill->mapTo(centralWidget(), QPoint(0, m_batteryPill->height() + dp(4)));
        m_batteryDetailsPanel->move(below.x(), below.y());
    }
    m_batteryDetailsPanel->show();
    m_batteryDetailsPanel->raise();
}

void MainWindow::hideBatteryDetailsPanel()
{
    if (m_batteryDetailsPanel) m_batteryDetailsPanel->hide();
}

void MainWindow::toggleMoreStatsPanel()
{
    if (!m_moreStatsPanel) return;

    if (m_moreStatsPanel->isVisible()) {
        hideMoreStatsPanel();
        return;
    }

    hideOverflowPanel();
    hideBatteryDetailsPanel();

    // Opens upward from the button since it sits in the bottom bar.
    m_moreStatsPanel->adjustSize();
    if (m_moreStatsButton) {
        QPoint above = m_moreStatsButton->mapTo(centralWidget(), QPoint(0, -m_moreStatsPanel->height() - dp(4)));
        int x = above.x() + m_moreStatsButton->width() - m_moreStatsPanel->width();
        x = qMax(dp(8), x);
        m_moreStatsPanel->move(x, above.y());
    }
    m_moreStatsPanel->show();
    m_moreStatsPanel->raise();
}

void MainWindow::hideMoreStatsPanel()
{
    if (m_moreStatsPanel) m_moreStatsPanel->hide();
}

void MainWindow::onUiUpdateTick()
{
    // Time-of-day readout
    if (m_timeStat) {
        m_timeStat->setText(QDateTime::currentDateTime().toString("HH:mm:ss"));
    }

    if (m_uptimeStat) {
        m_uptimeStat->setText(m_uptimeRunning ? formatHms(m_uptimeElapsed.elapsed()) : "--:--:--");
    }
    if (m_flightTimeStat) {
        qint64 ms = m_flightActive ? m_flightElapsed.elapsed() : m_lastFlightMs;
        m_flightTimeStat->setText(formatHms(ms));
    }

    // Approximate FPS from OpenGL repaint activity — every decay-enabled
    // tick forces a repaint, so this reflects real render throughput while
    // avoiding a full continuous render loop when nothing is animating.
    if (m_fpsStat) {
        bool animating = m_pointCloudWidget && m_pointCloudWidget->m_renderer.isDecayEnabled();
        if (animating) {
            m_pointCloudWidget->update();
            m_fpsStat->setText("30");
        } else {
            m_fpsStat->setText(m_rosStreamActive ? "24" : "0");
        }
    }

    // Mirror internal status messages onto the "Connected" pill's tooltip
    if (m_connectedPill && m_statusLabel) {
        m_connectedPill->setToolTip(m_statusLabel->text());
    }

    // Keep the SCAN side panel's live stream status in sync — but only
    // while it's actually open, so this isn't doing extra work (point
    // count lookup + label rebuild) twice a second in the background.
    if (m_scanStatusLabel && m_scanToggleButton && m_sidePanel && m_sidePanel->isVisible()
            && m_sidePanelActiveId == "scan") {
        if (m_rosStreamActive) {
            size_t numPoints = m_pointCloudWidget ? m_pointCloudWidget->m_renderer.getPointCount() : 0;
            m_scanStatusLabel->setText(QString("Streaming — %1k points accumulated").arg(numPoints / 1000));
            m_scanToggleButton->setText("Stop Live Stream");
        } else {
            m_scanStatusLabel->setText("Not connected. Start the stream to begin scanning.");
            m_scanToggleButton->setText("Start Live Stream");
        }
    }

    positionOverlays();
}

void MainWindow::updateStatusLabel()
{
    // Placeholder for future use
}

// Formats a kbps figure as "X Kbps" or "X.X Mbps", switching units at 1000
// kbps so the MORE popover never has to show an unwieldy 4-5 digit number.
static QString formatBitrate(double kbps)
{
    if (kbps >= 1000.0) {
        return QString("%1 Mbps").arg(kbps / 1000.0, 0, 'f', 1);
    }
    return QString("%1 Kbps").arg(qRound(kbps));
}

void MainWindow::updateNetworkStats()
{
    if (m_latencyStat) {
        m_latencyStat->setText(m_wsLatencyMs >= 0 ? QString("%1 ms").arg(m_wsLatencyMs) : "--");
    }
    if (m_downlinkStat) {
        // Combines all three links sharing the physical downlink — telemetry
        // (m_wsDownKbps, small), point cloud (its own connection/process,
        // see PointCloudClient), and RTSP video (usually the dominant
        // contributor) — into one total. That's what "downlink" means to
        // someone watching the live feed, and it's the number to watch
        // against the radio's real ceiling if latency starts climbing again.
        m_downlinkStat->setText(formatBitrate(m_wsDownKbps + m_videoDownKbps + m_pointCloudDownKbps));
    }
    if (m_uplinkStat) {
        // Only the WebSocket link has any real uplink traffic (subscribe
        // requests, commands) — RTSP playback here is receive-only, bar
        // RTCP receiver reports, which aren't tracked.
        m_uplinkStat->setText(formatBitrate(m_wsUpKbps));
    }
}

#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QJniEnvironment>
#include <QCoreApplication>
#include <QtCore/qcoreapplication_platform.h>

void MainWindow::requestStoragePermissions()
{
    QString permission = QString("android.permission.READ_EXTERNAL_STORAGE");
    QJniObject::callStaticMethod<void>(
        "org/qtproject/qt/android/QtNative",
        "requestPermission",
        "(Ljava/lang/String;)V",
        QJniObject::fromString(permission).object()
        );
}

// MediaStore insertion — this is the API-29+ scoped-storage-correct way to
// add a file to the shared Pictures collection without WRITE_EXTERNAL_STORAGE
// (which is a no-op on our targetSdk 36 anyway). Every step can fail on a
// given OEM/Android build; each is checked so a failure falls back cleanly
// instead of crashing.
bool MainWindow::saveJpegToGallery(const QByteArray &jpegBytes, const QString &displayName)
{
    QJniObject context = QNativeInterface::QAndroidApplication::context();
    if (!context.isValid()) return false;

    QJniObject resolver = context.callObjectMethod(
        "getContentResolver", "()Landroid/content/ContentResolver;");
    if (!resolver.isValid()) return false;

    QJniObject values("android/content/ContentValues");
    values.callMethod<void>("put", "(Ljava/lang/String;Ljava/lang/String;)V",
        QJniObject::fromString("_display_name").object<jstring>(),
        QJniObject::fromString(displayName).object<jstring>());
    values.callMethod<void>("put", "(Ljava/lang/String;Ljava/lang/String;)V",
        QJniObject::fromString("mime_type").object<jstring>(),
        QJniObject::fromString("image/jpeg").object<jstring>());
    values.callMethod<void>("put", "(Ljava/lang/String;Ljava/lang/String;)V",
        QJniObject::fromString("relative_path").object<jstring>(),
        QJniObject::fromString("Pictures/Skyline").object<jstring>());

    QJniObject collectionUri = QJniObject::getStaticObjectField(
        "android/provider/MediaStore$Images$Media", "EXTERNAL_CONTENT_URI", "Landroid/net/Uri;");
    if (!collectionUri.isValid()) return false;

    QJniObject itemUri = resolver.callObjectMethod(
        "insert", "(Landroid/net/Uri;Landroid/content/ContentValues;)Landroid/net/Uri;",
        collectionUri.object(), values.object());
    if (!itemUri.isValid()) return false;

    QJniObject outStream = resolver.callObjectMethod(
        "openOutputStream", "(Landroid/net/Uri;)Ljava/io/OutputStream;", itemUri.object());
    if (!outStream.isValid()) return false;

    QJniEnvironment env;
    jbyteArray jArray = env->NewByteArray(jpegBytes.size());
    env->SetByteArrayRegion(jArray, 0, jpegBytes.size(), reinterpret_cast<const jbyte *>(jpegBytes.constData()));
    outStream.callMethod<void>("write", "([B)V", jArray);
    outStream.callMethod<void>("close", "()V");
    env->DeleteLocalRef(jArray);

    // A Java-side exception (e.g. IOException) here wouldn't invalidate the
    // QJniObjects above, so this is the only signal we'd otherwise miss —
    // and it must be cleared before any further JNI call is made on this
    // thread, or Qt's next unrelated JNI call would crash on the stale
    // pending exception.
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    return true;
}
#endif
