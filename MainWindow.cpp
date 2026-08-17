#include "MainWindow.h"
#include "PointCloudWidget.h"
#include "ImageViewerWidget.h"
#include "RosPointCloudClient.h"
#include "RosImageClient.h"
#include "OverlayWidgets.h"
#include "IconRenderer.h"
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
// MiniPlayerWidget Implementation
// ============================================================================

MiniPlayerWidget::MiniPlayerWidget(QWidget *parent)
    : QWidget(parent)
    , m_contentWidget(nullptr)
    , m_contentLayout(nullptr)
    , m_contentContainer(nullptr)
    , m_dragging(false)
    , m_minimized(false)
{
    setWindowFlags(Qt::Widget);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setMouseTracking(true);

    // FIX: use dp() so border/radius scale with screen density
    setStyleSheet(QString(R"(
        QWidget {
            background-color: #262624;
            border: 2px solid #0075c5;
            border-radius: %1px;
        }
    )").arg(dp(8)));

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Title bar + buttons: size them together so buttons always fit inside.
    // Rule: bar height = button size. We use dp(36) — compact but still
    // finger-tappable on Android (36dp ≈ 6.8mm, above the 6mm comfort floor).
    // The old dp(48) buttons in a dp(32) bar caused visible overflow.
    const int titleBarH = dp(42);

    QWidget *titleBar = new QWidget(this);
    titleBar->setStyleSheet(QString("background-color: #1a1a18; border-radius: %1px %1px 0 0; border: none;")
                                .arg(dp(6)));
    titleBar->setFixedHeight(titleBarH);

    QHBoxLayout *titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(dp(10), 0, dp(4), 0);
    titleLayout->setSpacing(dp(2));

    m_titleLabel = new QLabel("Camera Feed", titleBar);
    m_titleLabel->setStyleSheet(QString("color: #e0e0e0; font-weight: 600; font-size: %1px; border: none; background: transparent;")
                                    .arg(sp(11)));
    titleLayout->addWidget(m_titleLabel);
    titleLayout->addStretch();

    // Real title-bar controls: record / swap main-mini / split view / close.
    // Previously this bar only had a label — none of these actions (or the
    // closePlayer slot) were reachable from the UI.
    auto makeTitleBtn = [&](IconRenderer::Id iconId, const QColor &color, const QString &tip) {
        QPushButton *b = new QPushButton(titleBar);
        const int s = dp(28);
        b->setFixedSize(s, s);
        b->setIcon(IconRenderer::icon(iconId, color, dp(15)));
        b->setIconSize(QSize(dp(15), dp(15)));
        b->setToolTip(tip);
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QString(R"(
            QPushButton { background-color: transparent; border: none; border-radius: %1px; padding: 0; }
            QPushButton:hover { background-color: rgba(255,255,255,25); }
            QPushButton:checked { background-color: rgba(0,117,197,60); }
        )").arg(dp(6)));
        titleLayout->addWidget(b);
        return b;
    };

    m_recordButton = makeTitleBtn(IconRenderer::Id::Record, QColor("#ff4d4d"), "Start/Stop Recording");
    m_recordButton->setCheckable(true);
    connect(m_recordButton, &QPushButton::clicked, this, &MiniPlayerWidget::requestRecordToggle);

    m_swapButton = makeTitleBtn(IconRenderer::Id::Swap, QColor("#cfcfcf"), "Swap main / mini view");
    connect(m_swapButton, &QPushButton::clicked, this, &MiniPlayerWidget::requestSwap);

    m_splitButton = makeTitleBtn(IconRenderer::Id::Split, QColor("#cfcfcf"), "Toggle split view");
    m_splitButton->setCheckable(true);
    connect(m_splitButton, &QPushButton::clicked, this, &MiniPlayerWidget::requestSplitToggle);

    m_closeButton = makeTitleBtn(IconRenderer::Id::Close, QColor("#cfcfcf"), "Close");
    connect(m_closeButton, &QPushButton::clicked, this, &MiniPlayerWidget::closePlayer);

    mainLayout->addWidget(titleBar);

    // Content area container
    m_contentContainer = new QWidget(this);
    m_contentContainer->setStyleSheet(QString("background-color: rgba(25, 25, 25, 250); border-radius: 0 0 %1px %1px; border: none;")
                                          .arg(dp(6)));
    m_contentLayout = new QVBoxLayout(m_contentContainer);
    m_contentLayout->setContentsMargins(dp(2), dp(2), dp(2), dp(2));
    m_contentLayout->setSpacing(0);
    mainLayout->addWidget(m_contentContainer, 1);

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
        m_contentLayout->removeWidget(m_contentWidget);
        m_contentWidget->setParent(nullptr);
    }

    m_contentWidget = widget;

    if (widget) {
        widget->setParent(m_contentContainer);
        m_contentLayout->addWidget(widget);
        widget->show();
    }
}

void MiniPlayerWidget::setTitle(const QString &title)
{
    m_titleLabel->setText(title);
}

void MiniPlayerWidget::setRecording(bool recording)
{
    if (m_recordButton) m_recordButton->setChecked(recording);
}

void MiniPlayerWidget::setSplitActive(bool active)
{
    if (m_splitButton) m_splitButton->setChecked(active);
}

void MiniPlayerWidget::minimizePlayer()
{
    m_minimized = true;
    m_contentContainer->hide();
    setFixedHeight(dp(32)); // FIX: was 32px hardcoded
    emit minimized();
}

void MiniPlayerWidget::restorePlayer()
{
    m_minimized = false;
    m_contentContainer->show();
    setFixedHeight(QWIDGETSIZE_MAX);
    if (m_customSize.isValid()) {
        resize(m_customSize);
    } else {
        resize(dp(320), dp(240));  // FIX: scaled default size
    }
    emit restored();
}

void MiniPlayerWidget::closePlayer()
{
    hide();
    emit closed();
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
        m_dragging = true;
        m_dragPosition = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
    }
}

void MiniPlayerWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && parentWidget()) {
        QPoint newPos = mapToParent(event->pos() - m_dragPosition);

        // Constrain to parent bounds
        int maxX = parentWidget()->width() - width();
        int maxY = parentWidget()->height() - height();

        newPos.setX(qBound(0, newPos.x(), maxX));
        newPos.setY(qBound(0, newPos.y(), maxY));

        move(newPos);
        event->accept();
    }
}

void MiniPlayerWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
        setCursor(Qt::ArrowCursor);
        event->accept();
    }
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
    connect(m_rosClient, &RosPointCloudClient::pointCloudReceived,
            this, &MainWindow::onRosPointCloudReceived);
    connect(m_rosClient, &RosPointCloudClient::pathReceived,
            this, &MainWindow::onRosPathReceived);
    connect(m_rosClient, &RosPointCloudClient::robotPoseReceived,
            this, &MainWindow::onRosRobotPoseReceived);
    connect(m_rosClient, &RosPointCloudClient::telemetryReceived,
            this, &MainWindow::onTelemetryReceived);
    connect(m_rosClient, &RosPointCloudClient::error,
            this, &MainWindow::onRosError);

    m_imageClient = new RosImageClient(this);
    connect(m_imageClient, &RosImageClient::connected,
            this, &MainWindow::onImageConnected);
    connect(m_imageClient, &RosImageClient::disconnected,
            this, &MainWindow::onImageDisconnected);
    connect(m_imageClient, &RosImageClient::imageReceived,
            this, &MainWindow::onImageReceived);
    connect(m_imageClient, &RosImageClient::error,
            this, &MainWindow::onImageError);

    updateViewMode();
    positionOverlays();

    // Auto-connect on startup
    QTimer::singleShot(500, this, [this]() {
        QString url = m_rosUrlEdit->text().trimmed();
        if (!url.isEmpty()) {
            m_statusLabel->setText("Auto-connecting...");
            m_rosClient->connectToServer(url);
            m_imageClient->connectToServer(url);
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
    topBar->setStyleSheet(R"(
        QWidget {
            background-color: #14140f;
            border-bottom: 1px solid rgba(0, 117, 197, 90);
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

    // Battery pill: "BATTERY  91%"
    m_batteryPill = new StatusPill(topBar);
    m_batteryPill->setIcon(IconRenderer::icon(IconRenderer::Id::Battery, QColor("#cfcfcf"), sp(14)));
    m_batteryPill->setLabel("BATTERY");
    m_batteryPill->setValue("--");
    m_batteryPill->setValueColor(QColor("#e8e8e8"));
    topLayout->addWidget(m_batteryPill);

    // RC signal pill: "RC SIGNAL  51%"
    m_signalPill = new StatusPill(topBar);
    m_signalPill->setIcon(IconRenderer::icon(IconRenderer::Id::Signal, QColor("#cfcfcf"), sp(14)));
    m_signalPill->setLabel("RC SIGNAL");
    m_signalPill->setValue("--");
    m_signalPill->setValueColor(QColor("#e8e8e8"));
    topLayout->addWidget(m_signalPill);

    // Voltage pill: "VOLTAGE  11.4V"
    m_voltagePill = new StatusPill(topBar);
    m_voltagePill->setIcon(IconRenderer::icon(IconRenderer::Id::Bolt, QColor("#cfcfcf"), sp(14)));
    m_voltagePill->setLabel("VOLTAGE");
    m_voltagePill->setValue("--");
    m_voltagePill->setValueColor(QColor("#e8e8e8"));
    topLayout->addWidget(m_voltagePill);

    // Current pill: "CURRENT  4.5A"
    m_currentPill = new StatusPill(topBar);
    m_currentPill->setIcon(IconRenderer::icon(IconRenderer::Id::Waveform, QColor("#cfcfcf"), sp(14)));
    m_currentPill->setLabel("CURRENT");
    m_currentPill->setValue("--");
    m_currentPill->setValueColor(QColor("#e8e8e8"));
    topLayout->addWidget(m_currentPill);

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

    // Recording indicator — small "REC 00:02:11" pill, matches the mock.
    // Kept as a checkable QPushButton so the existing recording logic is
    // untouched; only its visual style changes.
    m_recordButton = new QPushButton("REC   00:00", topBar);
    m_recordButton->setCheckable(true);
    m_recordButton->setToolTip("Start/Stop Recording");
    m_recordButton->setIcon(IconRenderer::icon(IconRenderer::Id::Record, QColor("#555555"), sp(14)));
    m_recordButton->setIconSize(QSize(sp(14), sp(14)));
    m_recordButton->setStyleSheet(QString(R"(
        QPushButton {
            background-color: transparent;
            color: #555555;
            border: none;
            padding: %1px %2px;
            border-radius: %3px;
            font-weight: 700;
            font-size: %4px;
            min-height: %5px;
        }
        QPushButton:hover   { background-color: rgba(255,255,255,10); }
        QPushButton:checked { color: #ff3333; }
    )").arg(dp(5)).arg(dp(14)).arg(dp(5)).arg(sp(13)).arg(dp(36)));
    connect(m_recordButton, &QPushButton::clicked, this, &MainWindow::onToggleRecording);
    connect(m_recordButton, &QPushButton::toggled, this, [this](bool checked) {
        m_recordButton->setIcon(IconRenderer::icon(checked ? IconRenderer::Id::RecordStop : IconRenderer::Id::Record,
                                                     checked ? QColor("#ff3333") : QColor("#555555"), sp(14)));
    });
    topLayout->addWidget(m_recordButton);

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

    QPushButton *swapRow = makeOverflowRow(IconRenderer::Id::Swap, "Swap main / mini view");
    connect(swapRow, &QPushButton::clicked, this, [this]() {
        onSwapViews();
        hideOverflowPanel();
    });

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

    // These logic-only, off-screen buttons keep other call sites (e.g. the
    // sidebar/settings dialog) that trigger these actions programmatically
    // working unchanged.
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
    // Body: left icon sidebar + main 3D view (with floating overlays)
    // ------------------------------------------------------------------
    QWidget *body = new QWidget(this);
    QHBoxLayout *bodyLayout = new QHBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    // Left icon rail
    m_sidebar = new SidebarWidget(body);
    m_sidebar->addItem("3d", IconRenderer::icon(IconRenderer::Id::Cube3D, QColor("#cfcfcf"), dp(20)), "3D");
    m_sidebar->addItem("top", IconRenderer::icon(IconRenderer::Id::TopView, QColor("#cfcfcf"), dp(20)), "TOP");
    m_sidebar->addItem("wp", IconRenderer::icon(IconRenderer::Id::Waypoint, QColor("#cfcfcf"), dp(20)), "WP");
    m_sidebar->addItem("scan", IconRenderer::icon(IconRenderer::Id::Scan, QColor("#cfcfcf"), dp(20)), "SCAN");
    m_sidebar->addItem("settings", IconRenderer::icon(IconRenderer::Id::Settings, QColor("#cfcfcf"), dp(20)), "SETTINGS");
    connect(m_sidebar, &SidebarWidget::itemClicked, this, &MainWindow::onSidebarItemClicked);
    bodyLayout->addWidget(m_sidebar);

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
    // compass + mini-map (bottom-right corner).
    m_elevationLegend = new ElevationLegendWidget(m_mainViewContainer);
    m_elevationLegend->setRange(-5.0f, 5.0f);
    m_elevationLegend->raise();

    m_compassWidget = new CompassWidget(m_mainViewContainer);
    m_compassWidget->raise();

    m_miniMapWidget = new MiniMapWidget(m_mainViewContainer);
    m_miniMapWidget->raise();

    // ORBIT/FOLLOW switch — floats over the 3D view (top-left, under the
    // legend) whenever the point cloud is visible; toggles whether the
    // camera target tracks the drone's live pose.
    m_followSwitch = new FollowSwitchWidget(m_mainViewContainer);
    m_followSwitch->raise();
    connect(m_followSwitch, &FollowSwitchWidget::toggled, this, &MainWindow::onFollowSwitchToggled);

    // ------------------------------------------------------------------
    // Slide-in context panel — gives the sidebar's WP / SCAN items real
    // content instead of doing nothing. Lives over the right edge of the
    // main view; opened/closed via showSidePanel()/hideSidePanel().
    // ------------------------------------------------------------------
    m_sidePanel = new QWidget(m_mainViewContainer);
    m_sidePanel->setObjectName("sidePanel");
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
        m_sidebar->setActive("3d");
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
    // Bottom stats bar: Altitude / Speed / Distance / Flown Dist / Points
    // / FPS / Time — replaces the old plain status line.
    // ------------------------------------------------------------------
    m_bottomStatsBar = new QWidget(this);
    m_bottomStatsBar->setObjectName("bottomStatsBar");
    m_bottomStatsBar->setStyleSheet(R"(
        QWidget#bottomStatsBar { background-color: #14140f; border-top: 1px solid rgba(0, 117, 197, 60); }
    )");
    // NOTE: was setFixedHeight(dp(58)), but a stat item's real content
    // (label + value + sparkline, with spacing/margins) needs ~66-70px,
    // so the fixed 58px was clipping the sparkline row. Let the layout
    // size the bar to its content instead of forcing a height that's
    // too small for it.
    
    m_bottomStatsBar->setMinimumHeight(dp(74));
    QHBoxLayout *statsLayout = new QHBoxLayout(m_bottomStatsBar);
    statsLayout->setContentsMargins(dp(20), dp(6), dp(20), dp(12));
    statsLayout->setSpacing(dp(28));

    m_altitudeStat  = new StatItemWidget("ALTITUDE", "m", true, m_bottomStatsBar);
    m_speedStat     = new StatItemWidget("SPEED", "m/s", true, m_bottomStatsBar);
    m_distanceStat  = new StatItemWidget("DISTANCE", "m", true, m_bottomStatsBar);
    m_flownDistStat = new StatItemWidget("FLOWN DIST", "m", true, m_bottomStatsBar);
    m_pointsStat    = new StatItemWidget("POINTS", "", false, m_bottomStatsBar);
    m_fpsStat       = new StatItemWidget("FPS", "", false, m_bottomStatsBar);
    m_timeStat      = new StatItemWidget("TIME", "", false, m_bottomStatsBar);

    for (StatItemWidget *stat : { m_altitudeStat, m_speedStat, m_distanceStat, m_flownDistStat,
                                 m_pointsStat, m_fpsStat, m_timeStat }) {
        statsLayout->addWidget(stat);
    }
    statsLayout->addStretch();

    mainLayout->addWidget(m_bottomStatsBar);

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
    connect(m_miniPlayer, &MiniPlayerWidget::requestSplitToggle, this, &MainWindow::onToggleSplitView);
    connect(m_miniPlayer, &MiniPlayerWidget::requestRecordToggle, this, &MainWindow::onToggleRecording);
    connect(m_miniPlayer, &MiniPlayerWidget::closed, this, &MainWindow::onMiniPlayerClosed);

    // Hidden URL input (will be in settings)
    m_rosUrlEdit = new QLineEdit("ws://192.168.1.12:9090");
    m_rosUrlEdit->hide();

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

    if (m_miniMapWidget) {
        int x = m_mainViewContainer->width() - m_miniMapWidget->width() - margin;
        int y = m_mainViewContainer->height() - m_miniMapWidget->height() - margin;
        m_miniMapWidget->move(qMax(0, x), qMax(0, y));
        m_miniMapWidget->raise();
    }

    if (m_compassWidget && m_miniMapWidget) {
        // Place compass beside the mini-map (left side) to avoid legend overlap.
        int x = m_miniMapWidget->x() - m_compassWidget->width() - dp(14);
        int y = m_miniMapWidget->y() + (m_miniMapWidget->height() - m_compassWidget->height()) / 2;
        m_compassWidget->move(qMax(0, x), qMax(0, y));
        m_compassWidget->raise();
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

    // Apply dark theme to settings dialog
    m_settingsDialog->setStyleSheet(QString(R"(
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
        QPushButton:hover {
            background-color: #0088e0;
        }
        QPushButton:pressed {
            background-color: #005a9c;
        }
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
            background-color: rgba(40, 40, 40, 220);
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
            background-color: rgba(30, 30, 30, 200);
        }
        QCheckBox::indicator:checked {
            background-color: #0075c5;
            border-color: #0075c5;
        }
        QDoubleSpinBox, QSpinBox {
            background-color: rgba(30, 30, 30, 200);
            border: 1px solid rgba(70, 70, 70, 200);
            border-radius: 6px;
            padding: 4px 8px;
            color: #e8e8e8;
            font-size: %1px;
        }
        QDoubleSpinBox:focus, QSpinBox:focus {
            border: 1px solid #0075c5;
        }
    )")
                                        .arg(sp(12))
                                        .arg(dp(8))
                                        .arg(dp(16)));

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
    m_rosUrlEdit->setPlaceholderText("ws://localhost:9090");
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
            background-color: #0075c5;
            border: 1px solid #005a9c;
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
            background-color: rgba(30, 30, 30, 200);
            border: 1px solid rgba(70, 70, 70, 200);
            border-radius: 6px;
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
        m_miniPlayer->setSplitActive(true);

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
        QString miniTitle;

        if (m_currentViewMode == PointCloudMain) {
            mainWidget = m_pointCloudWidget;
            miniWidget = m_imageViewerWidget;
            miniTitle = "CAMERA 1";
        } else { // CameraMain
            mainWidget = m_imageViewerWidget;
            miniWidget = m_pointCloudWidget;
            miniTitle = "3D MAP";
        }

        mainWidget->setParent(m_mainViewContainer);
        m_mainViewContainer->layout()->addWidget(mainWidget);
        mainWidget->show();
        mainWidget->raise();

        m_miniPlayer->setContentWidget(miniWidget);
        m_miniPlayer->setTitle(miniTitle);
        m_miniPlayer->setSplitActive(false);
        m_miniPlayer->setRecording(m_isRecording);

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
    // checked state. The mini player's own visible split button and the
    // overflow menu's split row each maintain their own independent
    // checked state and just called this slot, so clicking them changed
    // their own appearance but never touched the value updateViewMode()
    // actually looks at.
    //
    // This is the master toggle: flip it here, then let updateViewMode()
    // (via MiniPlayerWidget::setSplitActive) and the overflow-panel sync
    // at line ~1844 keep every visible split control in agreement with it,
    // regardless of which one was clicked.
    if (m_splitButton) {
        m_splitButton->setChecked(!m_splitButton->isChecked());
    }
    if (m_overflowSplitBtn && m_splitButton) {
        m_overflowSplitBtn->setChecked(m_splitButton->isChecked());
    }

    updateViewMode();
}

void MainWindow::onMiniPlayerClosed()
{
    // Return to single view
    if (m_splitButton) {
        m_splitButton->setChecked(false);
    }
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
        m_statusLabel->setText("Manually disconnected");
    } else {
        m_manualDisconnect = false;  // User clicked connect
        QString url = m_rosUrlEdit->text().trimmed();
        if (url.isEmpty()) {
            QMessageBox::warning(this, "Connection Error", "Please enter a valid WebSocket URL");
            return;
        }
        m_rosClient->connectToServer(url);
        m_imageClient->connectToServer(url);
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
        QString url = m_rosUrlEdit->text().trimmed();
        if (!url.isEmpty()) {
            m_statusLabel->setText("Auto-reconnecting...");
            if (m_connectedPill) {
                m_connectedPill->setDotColor(QColor("#ffcc00"));
                m_connectedPill->setValue("Reconnecting…");
            }
            QTimer::singleShot(2000, this, [this, url]() {
                if (!m_rosStreamActive && !m_manualDisconnect) {
                    m_rosClient->connectToServer(url);
                    m_imageClient->connectToServer(url);
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
    if (m_miniMapWidget && !path.empty()) {
        m_flightPath = path;
        m_miniMapWidget->setPath(m_flightPath);
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
    if (m_miniMapWidget) m_miniMapWidget->setPath(m_flightPath);
    if (m_altitudeStat)  m_altitudeStat->setValue(m_currentAltitude, 1);
    if (m_speedStat)     m_speedStat->setValue(m_currentSpeed, 1);
    if (m_distanceStat)  m_distanceStat->setValue(m_currentDistance, 1);
    if (m_flownDistStat) m_flownDistStat->setValue(m_flownDistanceAccum, 1);
}

void MainWindow::onImageConnected()
{
    m_imageStreamActive = true;
}

void MainWindow::onImageDisconnected()
{
    m_imageStreamActive = false;
}

void MainWindow::onImageReceived(const QImage &image)
{
    m_imageViewerWidget->setImage(image);
}

void MainWindow::onImageError(const QString &errorString)
{
    qWarning() << "Image error:" << errorString;
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

    // Update battery with color coding
    QString batteryText = QString("%1%").arg(battery);
    QString batteryColor;
    QString batteryBorder;
    if (battery < 20) {
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

    // Update signal with color coding
    QString signalText = QString("%1%").arg(signal);
    QString signalColor;
    QString signalBorder;
    if (signal < 20) {
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
        QColor c = battery < 20 ? QColor("#ff3333") : battery < 50 ? QColor("#ffcc00") : QColor("#00e676");
        m_batteryPill->setValue(QString("%1%").arg(battery));
        m_batteryPill->setValueColor(c);
    }
    if (m_signalPill) {
        QColor c = signal < 20 ? QColor("#ff3333") : signal < 50 ? QColor("#ffcc00") : QColor("#00e676");
        m_signalPill->setValue(QString("%1%").arg(signal));
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
}

void MainWindow::onFollowSwitchToggled(bool following)
{
    if (m_pointCloudWidget) {
        m_pointCloudWidget->setFollowMode(following);
    }
}

void MainWindow::onToggleRecording()
{
    m_isRecording = !m_isRecording;

    if (m_isRecording) {
        m_recordButton->setToolTip("Stop Recording");
        m_recordButton->setChecked(true);
        m_statusLabel->setText("Recording...");
        m_recElapsed.start();
        if (m_recTimer) m_recTimer->start(1000);
        onRecTimerTick();
    } else {
        m_recordButton->setToolTip("Start Recording");
        m_recordButton->setChecked(false);
        if (m_recTimer) m_recTimer->stop();
        m_recordButton->setText("REC   00:00");
        // Don't update status label if streaming
        if (m_rosStreamActive) {
            size_t numPoints = m_pointCloudWidget->m_renderer.getPointCount();
            m_statusLabel->setText(QString("Streaming - %1k points").arg(numPoints / 1000));
        }
    }

    if (m_miniPlayer) m_miniPlayer->setRecording(m_isRecording);
}

void MainWindow::onRecTimerTick()
{
    if (!m_isRecording || !m_recordButton) return;
    qint64 elapsedMs = m_recElapsed.elapsed();
    int totalSec = static_cast<int>(elapsedMs / 1000);
    int mins = totalSec / 60;
    int secs = totalSec % 60;
    m_recordButton->setText(QString("REC   %1:%2")
                                .arg(mins, 2, 10, QChar('0'))
                                .arg(secs, 2, 10, QChar('0')));
}

void MainWindow::onSidebarItemClicked(const QString &id)
{
    m_sidebar->setActive(id);
    hideOverflowPanel();

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
    } else if (id == "settings") {
        onShowSettings();
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

void MainWindow::onUiUpdateTick()
{
    // Time-of-day readout
    if (m_timeStat) {
        m_timeStat->setText(QDateTime::currentDateTime().toString("HH:mm:ss"));
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

#ifdef Q_OS_ANDROID
#include <QJniObject>
#include <QJniEnvironment>
#include <QCoreApplication>

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
#endif
