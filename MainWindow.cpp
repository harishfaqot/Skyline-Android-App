#include "MainWindow.h"
#include "PointCloudWidget.h"
#include "ImageViewerWidget.h"
#include "RosPointCloudClient.h"
#include "RosImageClient.h"
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
    const int titleBarH = dp(48);
    const int btnSize   = titleBarH;    // full bar height = comfortable tap area
    const int circleSize = dp(32);      // ← tune this to change visual circle size
    const int iconFontSize = sp(12);    // ← tune this to change symbol size

    QWidget *titleBar = new QWidget(this);
    titleBar->setStyleSheet(QString("background-color: #1a1a18; border-radius: %1px %1px 0 0; border: none;")
                                .arg(dp(6)));
    titleBar->setFixedHeight(titleBarH);

    QHBoxLayout *titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(dp(10), 0, dp(4), 0);
    titleLayout->setSpacing(dp(4));

    m_titleLabel = new QLabel("Camera Feed", titleBar);
    m_titleLabel->setStyleSheet(QString("color: #e0e0e0; font-weight: 600; font-size: %1px; border: none; background: transparent;")
                                    .arg(sp(11)));
    titleLayout->addWidget(m_titleLabel);
    titleLayout->addStretch();

    // ── Button design: transparent QPushButton (full bar height = tap area)
    //    with a smaller colored circle QLabel centered behind the Unicode symbol.
    //    Tune circleSize and iconFontSize independently of the tap target size.

    // Swap button
    {
        QWidget *container = new QWidget(titleBar);
        container->setFixedSize(btnSize, btnSize);

        QLabel *circle = new QLabel(container);
        circle->setFixedSize(circleSize, circleSize);
        circle->move((btnSize - circleSize) / 2, (btnSize - circleSize) / 2);
        circle->setStyleSheet(QString("background-color: #0075c5; border-radius: %1px;")
                                  .arg(circleSize / 2));
        circle->setAttribute(Qt::WA_TransparentForMouseEvents);

        m_swapButton = new QPushButton("⇄", container);
        m_swapButton->setFixedSize(btnSize, btnSize);
        m_swapButton->move(0, -2);
        m_swapButton->setStyleSheet(QString(R"(
            QPushButton {
                background-color: transparent;
                border: none;
                color: white;
                font-size: %1px;
                font-weight: bold;
            }
        )").arg(iconFontSize));
        m_swapButton->setToolTip("Swap views");
        connect(m_swapButton, &QPushButton::clicked, this, &MiniPlayerWidget::requestSwap);

        // Hover effect on the circle
        connect(m_swapButton, &QPushButton::pressed,  circle, [circle](){ circle->setStyleSheet(circle->styleSheet().replace("#0075c5","#005a9c")); });
        connect(m_swapButton, &QPushButton::released, circle, [circle](){ circle->setStyleSheet(circle->styleSheet().replace("#005a9c","#0075c5")); });

        titleLayout->addWidget(container);
    }

    // Close button
    {
        QWidget *container = new QWidget(titleBar);
        container->setFixedSize(btnSize, btnSize);

        QLabel *circle = new QLabel(container);
        circle->setFixedSize(circleSize, circleSize);
        circle->move((btnSize - circleSize) / 2, (btnSize - circleSize) / 2);
        circle->setStyleSheet(QString("background-color: rgba(196,69,105,200); border-radius: %1px;")
                                  .arg(circleSize / 2));
        circle->setAttribute(Qt::WA_TransparentForMouseEvents);

        m_closeButton = new QPushButton("X", container);
        m_closeButton->setFixedSize(btnSize, btnSize);
        m_closeButton->move(0, 0);
        m_closeButton->setStyleSheet(QString(R"(
            QPushButton {
                background-color: transparent;
                border: none;
                color: white;
                font-size: %1px;
                font-weight: bold;
            }
        )").arg(iconFontSize));
        m_closeButton->setToolTip("Close mini player");
        connect(m_closeButton, &QPushButton::clicked, this, &MiniPlayerWidget::closePlayer);

        connect(m_closeButton, &QPushButton::pressed,  circle, [circle](){ circle->setStyleSheet(circle->styleSheet().replace("rgba(196,69,105,200)","rgba(150,40,80,220)")); });
        connect(m_closeButton, &QPushButton::released, circle, [circle](){ circle->setStyleSheet(circle->styleSheet().replace("rgba(150,40,80,220)","rgba(196,69,105,200)")); });

        titleLayout->addWidget(container);
    }

    mainLayout->addWidget(titleBar);

    // Content area container
    m_contentContainer = new QWidget(this);
    m_contentContainer->setStyleSheet(QString("background-color: rgba(25, 25, 25, 250); border-radius: 0 0 %1px %1px; border: none;")
                                          .arg(dp(6)));
    m_contentLayout = new QVBoxLayout(m_contentContainer);
    m_contentLayout->setContentsMargins(dp(2), dp(2), dp(2), dp(2));
    m_contentLayout->setSpacing(0);
    mainLayout->addWidget(m_contentContainer, 1);

    // FIX: mini player default size scaled with dp()
    resize(dp(320), dp(240));
    raise();
}

MiniPlayerWidget::~MiniPlayerWidget()
{
}

void MiniPlayerWidget::setContentWidget(QWidget *widget)
{
    m_contentWidget = widget;
}

void MiniPlayerWidget::setTitle(const QString &title)
{
    m_titleLabel->setText(title);
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

    // Auto-connect on startup
    QTimer::singleShot(500, this, [this]() {
        QString url = m_rosUrlEdit->text().trimmed();
        if (!url.isEmpty()) {
            m_statusLabel->setText("🔄 Auto-connecting...");
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
    mainLayout->setContentsMargins(dp(12), dp(12), dp(12), dp(32));
    mainLayout->setSpacing(dp(12));

    // Top bar — FIX: height via dp() so it fits content on all densities
    QWidget *topBar = new QWidget(this);
    topBar->setStyleSheet(R"(
        QWidget {
            background-color: #1a1a18;
            border-bottom: 1px solid #0075c5;
        }
    )");
    topBar->setFixedHeight(dp(60));  // FIX: was 60px hardcoded

    QHBoxLayout *topLayout = new QHBoxLayout(topBar);
    topLayout->setContentsMargins(dp(16), dp(8), dp(16), dp(8));
    topLayout->setSpacing(dp(16));

    // Logo
    QLabel *logoLabel = new QLabel(topBar);
    QPixmap logo(":/android/res/drawable/skyline.png");
    if (logo.isNull()) {
        logo = QPixmap("android/res/drawable/skyline.png");
    }
    if (!logo.isNull()) {
        // FIX: scale logo with dp() so it stays proportional on all screens
        logoLabel->setPixmap(logo.scaled(dp(36), dp(36), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        logoLabel->setText("🛸");
        logoLabel->setStyleSheet(QString("font-size: %1px; background: transparent;").arg(sp(28)));
    }
    topLayout->addWidget(logoLabel);

    QLabel *appName = new QLabel("SKYLINE", topBar);
    appName->setStyleSheet(QString(R"(
        color: #0075c5;
        font-size: %1px;
        font-weight: 700;
        letter-spacing: 1px;
        background: transparent;
    )").arg(sp(24)));  // FIX: was 24px hardcoded
    topLayout->addWidget(appName);

    topLayout->addSpacing(dp(24));

    // Telemetry labels — FIX: all sizes via dp()/sp()
    auto makeTelemetryLabel = [&](QWidget *parent) -> QLabel* {
        QLabel *lbl = new QLabel(parent);
        lbl->setStyleSheet(QString(R"(
            QLabel {
                background-color: rgba(0, 0, 0, 120);
                border: 1px solid rgba(0, 117, 197, 80);
                border-radius: %1px;
                color: #00ff00;
                font-weight: 700;
                font-size: %2px;
                padding: %3px %4px;
                min-width: %5px;
            }
        )").arg(dp(4)).arg(sp(14)).arg(dp(6)).arg(dp(12)).arg(dp(85)));
        return lbl;
    };

    m_batteryLabel = makeTelemetryLabel(topBar);
    topLayout->addWidget(m_batteryLabel);

    m_signalLabel = makeTelemetryLabel(topBar);
    topLayout->addWidget(m_signalLabel);

    m_modeLabel = new QLabel(topBar);
    m_modeLabel->setStyleSheet(QString(R"(
        QLabel {
            background-color: rgba(0, 0, 0, 120);
            border: 1px solid rgba(0, 117, 197, 80);
            border-radius: %1px;
            color: #0075c5;
            font-weight: 700;
            font-size: %2px;
            padding: %3px %4px;
            min-width: %5px;
        }
    )").arg(dp(4)).arg(sp(14)).arg(dp(6)).arg(dp(12)).arg(dp(90)));
    topLayout->addWidget(m_modeLabel);

    topLayout->addStretch();

    // Helper lambda to make a top-bar button with consistent DPI-aware sizing
    auto makeTopButton = [&](const QString &text, const QString &bg, const QString &hoverBg,
                              const QString &color = "white") -> QPushButton* {
        QPushButton *btn = new QPushButton(text, topBar);
        btn->setStyleSheet(QString(R"(
            QPushButton {
                background-color: %1;
                color: %4;
                border: 1px solid rgba(0, 117, 197, 100);
                padding: %5px %6px;
                border-radius: %7px;
                font-weight: 600;
                font-size: %8px;
                min-height: %9px;
                min-width: %10px;
            }
            QPushButton:hover { background-color: %2; border: 1px solid #0075c5; }
            QPushButton:pressed { background-color: %3; }
        )").arg(bg, hoverBg, "#005a9c", color)
           .arg(dp(6)).arg(dp(14)).arg(dp(4)).arg(sp(13)).arg(dp(32)).arg(dp(60)));
        return btn;
    };

    // Recording button
    m_recordButton = new QPushButton("⏺  REC", topBar);
    m_recordButton->setCheckable(true);
    m_recordButton->setToolTip("Start/Stop Recording");
    m_recordButton->setStyleSheet(QString(R"(
        QPushButton {
            background-color: #cc0000;
            color: white;
            border: none;
            padding: %1px %2px;
            border-radius: %3px;
            font-weight: 600;
            font-size: %4px;
            min-height: %5px;
            min-width: %6px;
        }
        QPushButton:hover   { background-color: #ff0000; }
        QPushButton:checked { background-color: #ff0000; border: 1px solid white; }
    )").arg(dp(6)).arg(dp(14)).arg(dp(4)).arg(sp(13)).arg(dp(32)).arg(dp(60)));
    connect(m_recordButton, &QPushButton::clicked, this, &MainWindow::onToggleRecording);
    topLayout->addWidget(m_recordButton);

    m_swapButton = makeTopButton("⇄  Swap", "rgba(50,50,50,200)", "rgba(70,70,70,220)");
    m_swapButton->setToolTip("Swap main and mini views");
    m_swapButton->setStyleSheet(m_swapButton->styleSheet() +
        "QPushButton:pressed { background-color: #0075c5; }");
    connect(m_swapButton, &QPushButton::clicked, this, &MainWindow::onSwapViews);
    topLayout->addWidget(m_swapButton);

    m_splitButton = new QPushButton("Split", topBar);
    m_splitButton->setToolTip("Toggle split view");
    m_splitButton->setCheckable(true);
    m_splitButton->setStyleSheet(QString(R"(
        QPushButton {
            background-color: rgba(50, 50, 50, 200);
            color: #e8e8e8;
            border: 1px solid rgba(0, 117, 197, 100);
            padding: %1px %2px;
            border-radius: %3px;
            font-weight: 600;
            font-size: %4px;
            min-height: %5px;
            min-width: %6px;
        }
        QPushButton:hover   { background-color: rgba(70,70,70,220); border: 1px solid #0075c5; }
        QPushButton:checked { background-color: #0075c5; border: 1px solid #0088e0; }
    )").arg(dp(6)).arg(dp(14)).arg(dp(4)).arg(sp(13)).arg(dp(32)).arg(dp(92)));
    connect(m_splitButton, &QPushButton::clicked, this, &MainWindow::onToggleSplitView);
    topLayout->addWidget(m_splitButton);

    m_clearButton = new QPushButton("🗑  Clear", topBar);
    m_clearButton->setToolTip("Clear map");
    m_clearButton->setStyleSheet(QString(R"(
        QPushButton {
            background-color: rgba(80, 30, 30, 180);
            color: #ff6b6b;
            border: 1px solid rgba(196, 69, 105, 120);
            padding: %1px %2px;
            border-radius: %3px;
            font-weight: 600;
            font-size: %4px;
            min-height: %5px;
            min-width: %6px;
        }
        QPushButton:hover  { background-color: rgba(100,40,40,200); border: 1px solid rgba(213,95,130,180); color: #ff8888; }
        QPushButton:pressed { background-color: rgba(196,69,105,200); color: white; }
    )").arg(dp(6)).arg(dp(14)).arg(dp(4)).arg(sp(13)).arg(dp(32)).arg(dp(60)));
    connect(m_clearButton, &QPushButton::clicked, this, &MainWindow::onClearMap);
    topLayout->addWidget(m_clearButton);

    m_settingsButton = makeTopButton("⚙Settings", "rgba(50,50,50,200)", "rgba(70,70,70,220)");
    m_settingsButton->setToolTip("Open settings");
    connect(m_settingsButton, &QPushButton::clicked, this, &MainWindow::onShowSettings);
    topLayout->addWidget(m_settingsButton);

    // Restore mini player button (initially hidden)
    m_restoreMiniButton = new QPushButton("🔳  Restore", topBar);
    m_restoreMiniButton->setToolTip("Restore mini player");
    m_restoreMiniButton->setStyleSheet(QString(R"(
        QPushButton {
            background-color: rgba(80, 60, 20, 180);
            color: #ffcc00;
            border: 1px solid rgba(200, 160, 60, 120);
            padding: %1px %2px;
            border-radius: %3px;
            font-weight: 600;
            font-size: %4px;
            min-height: %5px;
            min-width: %6px;
        }
        QPushButton:hover { background-color: rgba(100,80,30,200); border: 1px solid rgba(220,180,80,180); }
    )").arg(dp(6)).arg(dp(14)).arg(dp(4)).arg(sp(13)).arg(dp(32)).arg(dp(60)));
    m_restoreMiniButton->hide();
    connect(m_restoreMiniButton, &QPushButton::clicked, [this]() {
        if (m_miniPlayer) {
            m_miniPlayer->restorePlayer();
            m_restoreMiniButton->hide();
        }
    });
    topLayout->addWidget(m_restoreMiniButton);

    mainLayout->addWidget(topBar);

    // Main view area
    m_mainViewContainer = new QWidget(this);
    QVBoxLayout *mainViewLayout = new QVBoxLayout(m_mainViewContainer);
    mainViewLayout->setContentsMargins(0, 0, 0, 0);

    // Create widgets
    m_pointCloudWidget = new PointCloudWidget(this);
    m_imageViewerWidget = new ImageViewerWidget(this);

    mainViewLayout->addWidget(m_pointCloudWidget);
    mainViewLayout->addWidget(m_imageViewerWidget);

    mainLayout->addWidget(m_mainViewContainer, 1);

    // Status bar
    m_statusLabel = new QLabel("🔄 Initializing - Auto-connecting to stream...", this);
    m_statusLabel->setObjectName("statusLabel");
    mainLayout->addWidget(m_statusLabel);

    setCentralWidget(centralWidget);

    // Create mini player
    m_miniPlayer = new MiniPlayerWidget(m_mainViewContainer);
    connect(m_miniPlayer, &MiniPlayerWidget::requestSwap, this, &MainWindow::onSwapViews);
    connect(m_miniPlayer, &MiniPlayerWidget::closed, this, &MainWindow::onMiniPlayerClosed);
    connect(m_miniPlayer, &MiniPlayerWidget::minimized, [this]() {
        if (m_restoreMiniButton) m_restoreMiniButton->show();
    });
    connect(m_miniPlayer, &MiniPlayerWidget::restored, [this]() {
        if (m_restoreMiniButton) m_restoreMiniButton->hide();
    });

    // Hidden URL input (will be in settings)
    m_rosUrlEdit = new QLineEdit("ws://192.168.20.128:9090");
    m_rosUrlEdit->hide();
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

    QLabel *title = new QLabel("⚙️  Settings", m_settingsDialog);
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
    m_rosToggleButton = new QPushButton("⚡ Connect", m_settingsDialog);
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

void MainWindow::updateViewMode()
{
    if (m_splitButton && m_splitButton->isChecked()) {
        // === SPLIT VIEW MODE ===
        m_miniPlayer->hide();

        // Clear mini player content first
        if (m_miniPlayer->findChildren<QWidget*>().count() > 0) {
            m_miniPlayer->setProperty("hasContent", false);
        }

        // Remove widgets from any parent and hide
        m_pointCloudWidget->setParent(nullptr);
        m_pointCloudWidget->hide();
        m_imageViewerWidget->setParent(nullptr);
        m_imageViewerWidget->hide();

        // Clear existing layout
        QLayoutItem *item;
        while ((item = m_mainViewContainer->layout()->takeAt(0)) != nullptr) {
            if (item->widget() && item->widget() != m_miniPlayer) {
                QSplitter *splitter = qobject_cast<QSplitter*>(item->widget());
                if (splitter) {
                    splitter->deleteLater();
                } else if (item->widget() != m_pointCloudWidget &&
                           item->widget() != m_imageViewerWidget) {
                    item->widget()->deleteLater();
                }
            }
            delete item;
        }

        // Create splitter
        QSplitter *splitter = new QSplitter(Qt::Horizontal, m_mainViewContainer);
        splitter->addWidget(m_pointCloudWidget);
        splitter->addWidget(m_imageViewerWidget);
        splitter->setStretchFactor(0, 1);
        splitter->setStretchFactor(1, 1);
        splitter->setStyleSheet("QSplitter::handle { background-color: rgba(13, 115, 119, 100); width: 2px; }");

        m_mainViewContainer->layout()->addWidget(splitter);
        m_pointCloudWidget->show();
        m_imageViewerWidget->show();

    } else {
        // === SINGLE VIEW WITH MINI PLAYER ===

        // Determine which widget goes where FIRST
        QWidget *mainWidget = nullptr;
        QWidget *miniWidget = nullptr;
        QString miniTitle;

        if (m_currentViewMode == PointCloudMain) {
            mainWidget = m_pointCloudWidget;
            miniWidget = m_imageViewerWidget;
            miniTitle = "📷 Camera";
        } else { // CameraMain
            mainWidget = m_imageViewerWidget;
            miniWidget = m_pointCloudWidget;
            miniTitle = "🗺️ 3D Map";
        }

        // Step 1: Setup mini player FIRST (so it takes ownership of miniWidget)
        // Remove old content from mini player without hiding it
        if (m_miniPlayer->findChildren<QWidget*>().contains(mainWidget)) {
            // Main widget is currently in mini player, remove it
            QLayoutItem *item;
            while ((item = m_miniPlayer->layout()->itemAt(1)->widget()->layout()->takeAt(0)) != nullptr) {
                if (item->widget() == mainWidget) {
                    item->widget()->setParent(nullptr);
                }
                delete item;
            }
        }

        // Step 2: Clear main container
        QLayoutItem *item;
        while ((item = m_mainViewContainer->layout()->takeAt(0)) != nullptr) {
            QWidget *widget = item->widget();
            if (widget && widget != m_miniPlayer) {
                QSplitter *splitter = qobject_cast<QSplitter*>(widget);
                if (splitter) {
                    // Remove widgets from splitter
                    for (int i = splitter->count() - 1; i >= 0; --i) {
                        QWidget *w = splitter->widget(i);
                        if (w) {
                            w->setParent(nullptr);
                        }
                    }
                    splitter->deleteLater();
                } else if (widget != mainWidget && widget != miniWidget) {
                    widget->setParent(nullptr);
                }
            }
            delete item;
        }

        // Step 3: Add main widget to main container
        mainWidget->setParent(m_mainViewContainer);
        m_mainViewContainer->layout()->addWidget(mainWidget);
        mainWidget->show();
        mainWidget->raise();

        // Step 4: NOW add mini widget to mini player (after main is secured)
        miniWidget->setParent(m_miniPlayer);
        if (m_miniPlayer->layout()->itemAt(1) && m_miniPlayer->layout()->itemAt(1)->widget()) {
            QVBoxLayout *contentLayout = qobject_cast<QVBoxLayout*>(
                m_miniPlayer->layout()->itemAt(1)->widget()->layout());
            if (contentLayout) {
                // Clear old content
                QLayoutItem *oldItem;
                while ((oldItem = contentLayout->takeAt(0)) != nullptr) {
                    delete oldItem;
                }
                // Add new content
                contentLayout->addWidget(miniWidget);
                miniWidget->show();
            }
        }

        m_miniPlayer->setTitle(miniTitle);

        // Step 5: Show mini player if not minimized
        if (!m_miniPlayer->isMinimized()) {
            m_miniPlayer->show();
            m_miniPlayer->raise();

            // Position mini player
            QTimer::singleShot(100, this, [this]() {
                if (m_miniPlayer->isVisible() && m_mainViewContainer) {
                    int x = m_mainViewContainer->width() - m_miniPlayer->width() - 20;
                    int y = m_mainViewContainer->height() - m_miniPlayer->height() - 20;
                    x = qMax(0, qMin(x, m_mainViewContainer->width() - m_miniPlayer->width()));
                    y = qMax(0, qMin(y, m_mainViewContainer->height() - m_miniPlayer->height()));
                    m_miniPlayer->move(x, y);
                }
            });
        }

        // Step 6: Force updates
        m_mainViewContainer->layout()->activate();
        mainWidget->update();
        m_mainViewContainer->update();
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
        m_statusLabel->setText("⚪ Manually disconnected");
    } else {
        m_manualDisconnect = false;  // User clicked connect
        QString url = m_rosUrlEdit->text().trimmed();
        if (url.isEmpty()) {
            QMessageBox::warning(this, "Connection Error", "Please enter a valid WebSocket URL");
            return;
        }
        m_rosClient->connectToServer(url);
        m_imageClient->connectToServer(url);
        m_statusLabel->setText("⏳ Connecting...");
    }
}

void MainWindow::onClearMap()
{
    m_pointCloudWidget->m_renderer.clearAccumulatedPoints();
    m_pointCloudWidget->update();
    m_statusLabel->setText("🗑️  Map cleared");
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
    m_rosToggleButton->setText("⚡ Disconnect");
    if (m_loadButton) {
        m_loadButton->setEnabled(false);
    }
    m_statusLabel->setText("🟢 Connected & Streaming");
}

void MainWindow::onRosDisconnected()
{
    m_rosStreamActive = false;
    m_rosToggleButton->setText("⚡ Connect");
    if (m_loadButton) {
        m_loadButton->setEnabled(true);
    }

    // Auto-reconnect if not manually disconnected
    if (!m_manualDisconnect) {
        QString url = m_rosUrlEdit->text().trimmed();
        if (!url.isEmpty()) {
            m_statusLabel->setText("🔄 Auto-reconnecting...");
            QTimer::singleShot(2000, this, [this, url]() {
                if (!m_rosStreamActive && !m_manualDisconnect) {
                    m_rosClient->connectToServer(url);
                    m_imageClient->connectToServer(url);
                }
            });
        } else {
            m_statusLabel->setText("⚪ Disconnected");
        }
    } else {
        m_statusLabel->setText("⚪ Disconnected");
    }
}

void MainWindow::onRosPointCloudReceived(const std::vector<float> &points)
{
    m_pointCloudWidget->m_renderer.updatePointCloud(points);
    m_pointCloudWidget->update();

    static int frameCount = 0;
    if (++frameCount % 30 == 0) {
        size_t numPoints = m_pointCloudWidget->m_renderer.getPointCount();
        m_statusLabel->setText(QString("📡 Streaming - %1k points").arg(numPoints / 1000));
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
}

void MainWindow::onRosRobotPoseReceived(const RobotPose &pose)
{
    m_pointCloudWidget->updateRobotPose(pose.position, pose.orientation);
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

void MainWindow::onTelemetryReceived(int battery, int signal, const QString &mode)
{
    m_batteryLevel = battery;
    m_signalStrength = signal;
    m_flightMode = mode;

    // Show telemetry labels when we have data
    m_batteryLabel->show();
    m_signalLabel->show();
    m_modeLabel->show();

    // Update battery with color coding
    QString batteryText = QString("🔋 %1%").arg(battery);
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
    QString signalText = QString("📡 %1%").arg(signal);
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
}

void MainWindow::onToggleRecording()
{
    m_isRecording = !m_isRecording;

    if (m_isRecording) {
        m_recordButton->setToolTip("Stop Recording");
        m_statusLabel->setText("🔴 Recording...");
    } else {
        m_recordButton->setToolTip("Start Recording");
        // Don't update status label if streaming
        if (m_rosStreamActive) {
            size_t numPoints = m_pointCloudWidget->m_renderer.getPointCount();
            m_statusLabel->setText(QString("📡 Streaming - %1k points").arg(numPoints / 1000));
        }
    }
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
