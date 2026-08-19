#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "RosPointCloudClient.h"
#include <QMainWindow>
#include <QElapsedTimer>
#include <QVector3D>
#include <vector>

// Forward declarations
class PointCloudWidget;
class ImageViewerWidget;
class RosPointCloudClient;
class RtspVideoClient;
class PointCloudClient;
class QPushButton;
class QLabel;
class QLineEdit;
class QCheckBox;
class QDoubleSpinBox;
class QSpinBox;
class QTabWidget;
class QSlider;
class QComboBox;
class QVBoxLayout;
class QDialog;
class QSplitter;
class QTimer;
class QListWidget;
class FirmwareUpdateDialog;

// New dashboard overlay widgets (see OverlayWidgets.h)
class StatusPill;
class CameraPanelWidget;
class ElevationLegendWidget;
class CompassWidget;
class MiniMapWidget;
class StatItemWidget;
class FollowSwitchWidget;
class CaptureControlWidget;

// Forward decl for the corner resize handle (defined in MainWindow.cpp,
// tightly coupled to MiniPlayerWidget so it doesn't need its own header).
class MiniPlayerResizeGrip;

// Mini player widget for picture-in-picture view
class MiniPlayerWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MiniPlayerWidget(QWidget *parent = nullptr);
    ~MiniPlayerWidget();

    void setContentWidget(QWidget *widget);
    bool isMinimized() const { return m_minimized; }

    // Called by MiniPlayerResizeGrip while the user drags it, so a manual
    // resize survives a minimize/restore round-trip the same way a drag does.
    void setCustomSize(const QSize &size) { m_customSize = size; }

    // Native width/height of the live camera frame currently docked here
    // (e.g. 1280/720), so the resize grip can preserve it instead of
    // stretching the video. Returns <= 0 when there's no frame to match
    // (pointcloud content, or no frame received yet) — resize is then
    // unconstrained.
    double contentAspectRatio() const;

public slots:
    void minimizePlayer();
    void restorePlayer();

signals:
    // Emitted on a plain tap (no drag) anywhere on the mini player — see
    // handleRelease(). There's no title bar or button to trigger this
    // otherwise; it's the only way to swap main/mini, matching QGC's
    // chrome-less floating preview.
    void requestSwap();
    void minimized();
    void restored();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

    // Content (camera/pointcloud) widgets keep their own mouse handling for
    // pan/orbit when shown full-size as the main view; while docked as the
    // mini player's preview we intercept their mouse events here instead so
    // a tap swaps main/mini and a drag repositions the mini player, exactly
    // like clicking the QGC picture-in-picture window does.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void handlePress(const QPoint &pos);
    void handleMove(const QPoint &pos);
    void handleRelease();

    QWidget *m_contentWidget;
    QVBoxLayout *m_contentLayout;
    QWidget *m_contentContainer;
    MiniPlayerResizeGrip *m_resizeGrip;

    QPoint m_dragPosition;
    bool m_dragging;
    bool m_moved;      // press moved past the click threshold -> not a tap
    bool m_minimized;

    QSize m_customSize;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onLoadFile();
    void onToggleRosStream();
    void onClearMap();
    void onSwapViews();
    void onToggleSplitView();
    void onShowSettings();
    void onShowFirmwareUpdate();

    // Decay control slots
    void onDecayEnabledChanged(int state);
    void onDecayTimeChanged(double value);
    void onMaxPointsChanged(int value);
    
    // ROS client callbacks
    void onRosConnected();
    void onRosDisconnected();
    void onRosPointCloudReceived(const std::vector<float> &points);
    void onRosPathReceived(const std::vector<QVector3D> &path);
    void onRosRobotPoseReceived(const RobotPose &pose);
    void onRosError(const QString &errorString);
    
    // Image client callbacks
    void onImageConnected();
    void onImageDisconnected();
    void onImageError(const QString &errorString);

    // Point-cloud client callbacks (own connection, see PointCloudClient.h)
    void onPointCloudConnected();
    void onPointCloudDisconnected();
    void onPointCloudError(const QString &errorString);

    // Telemetry callbacks
    void onTelemetryReceived(int battery, int signal, const QString &mode,
                              double voltageV, double currentA, bool armed);

    // Recording
    void onToggleRecording();
    void onCapturePhoto();
    void onShutterPressed(bool videoMode);

    // New dashboard interactions
    void onUiUpdateTick();
    void onRecTimerTick();
    void onFollowSwitchToggled(bool following);

#ifdef Q_OS_ANDROID
private:
    void requestStoragePermissions();
    // Inserts a JPEG into the shared MediaStore Pictures collection (so it
    // shows up in the system Gallery app), via JNI ContentResolver calls.
    // Returns false on any failure — caller should fall back to app-scoped
    // storage rather than silently losing the photo.
    bool saveJpegToGallery(const QByteArray &jpegBytes, const QString &displayName);
#endif

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateStatusLabel();
    void updateNetworkStats(); // refreshes the LATENCY/DOWNLINK/UPLINK MORE-popover stats
    void setupUI();
    void createSettingsDialog();
    void updateViewMode();
    void positionOverlays();
    void detachViewWidget(QWidget *widget);
    void clearMainContainer();
    void showSidePanel(const QString &id);
    void hideSidePanel();
    void toggleOverflowPanel();
    void hideOverflowPanel();
    void activateView(const QString &id);
    void toggleBatteryDetailsPanel();
    void hideBatteryDetailsPanel();
    void toggleMoreStatsPanel();
    void hideMoreStatsPanel();
    void scheduleImageReconnect();
    void schedulePointCloudReconnect();
    void showToast(const QString &text); // brief on-screen confirmation, e.g. "Photo saved"

    PointCloudWidget *m_pointCloudWidget;
    ImageViewerWidget *m_imageViewerWidget;
    RosPointCloudClient *m_rosClient;
    RtspVideoClient *m_imageClient;
    PointCloudClient *m_pointCloudClient;

    MiniPlayerWidget *m_miniPlayer;
    QWidget *m_mainViewContainer;
    QWidget *m_splitViewContainer;
    
    QPushButton *m_loadButton;
    QPushButton *m_rosToggleButton;
    QPushButton *m_clearButton;
    QPushButton *m_swapButton;
    QPushButton *m_splitButton;
    QPushButton *m_settingsButton;
    QLineEdit *m_rosUrlEdit;
    QLabel *m_statusLabel;
    QLabel *m_toastLabel = nullptr;
    QLabel *m_batteryLabel;
    QLabel *m_signalLabel;
    QLabel *m_modeLabel;
    
    // Settings (hidden in dialog)
    QCheckBox *m_decayEnabledCheckbox;
    QDoubleSpinBox *m_decayTimeSpinBox;
    QSpinBox *m_maxPointsSpinBox;
    QDialog *m_settingsDialog;
    FirmwareUpdateDialog *m_firmwareDialog = nullptr;

    enum ViewMode { PointCloudMain, CameraMain, SplitView };
    ViewMode m_currentViewMode;
    
    bool m_rosStreamActive;
    bool m_imageStreamActive;
    bool m_imageReconnectPending = false; // guards scheduleImageReconnect() against double-scheduling
    bool m_pointCloudStreamActive = false;
    bool m_pointCloudReconnectPending = false; // guards schedulePointCloudReconnect() against double-scheduling
    bool m_isRecording;
    bool m_manualDisconnect;
    bool m_droneArmed = false;
    int m_batteryLevel;
    int m_signalStrength;
    QString m_flightMode;

    // --- New dashboard widgets ---
    CameraPanelWidget *m_cameraPanel = nullptr;
    ElevationLegendWidget *m_elevationLegend = nullptr;
    CompassWidget *m_compassWidget = nullptr;
    MiniMapWidget *m_miniMapWidget = nullptr;

    StatusPill *m_connectedPill = nullptr;
    StatusPill *m_batteryPill = nullptr;
    StatusPill *m_signalPill = nullptr;
    StatusPill *m_voltagePill = nullptr;
    StatusPill *m_currentPill = nullptr;
    StatusPill *m_armedPill = nullptr;
    StatusPill *m_modePill = nullptr;
    QLabel *m_recTimeLabel = nullptr;

    // Battery pill's details popover — houses Voltage/Current, which used to
    // be their own always-visible top-bar pills (RC Signal stays visible).
    QWidget *m_batteryDetailsPanel = nullptr;

    // Floating ORBIT/FOLLOW switch over the 3D view — toggles whether the
    // camera target tracks the drone's live pose (see onFollowSwitchToggled).
    FollowSwitchWidget *m_followSwitch = nullptr;

    // Floating photo/video capture cluster (replaces the old top-bar
    // REC/photo buttons) — see onShutterPressed().
    CaptureControlWidget *m_captureControl = nullptr;

    QWidget *m_bottomStatsBar = nullptr;
    StatItemWidget *m_altitudeStat = nullptr;
    StatItemWidget *m_speedStat = nullptr;
    StatItemWidget *m_distanceStat = nullptr;
    StatItemWidget *m_flownDistStat = nullptr;
    StatItemWidget *m_pointsStat = nullptr;
    StatItemWidget *m_fpsStat = nullptr;
    StatItemWidget *m_timeStat = nullptr;
    StatItemWidget *m_uptimeStat = nullptr;
    StatItemWidget *m_flightTimeStat = nullptr;
    StatItemWidget *m_latencyStat = nullptr;
    StatItemWidget *m_downlinkStat = nullptr;
    StatItemWidget *m_uplinkStat = nullptr;

    // "MORE" popover — houses the stats that used to sit directly in the
    // bottom bar (Flown Dist / Points / FPS / Time), plus network stats.
    QWidget *m_moreStatsPanel = nullptr;
    QPushButton *m_moreStatsButton = nullptr;

    // Latest network readings, combined from both clients (see
    // updateNetworkStats()) — ROS WebSocket link (telemetry/pointcloud) for
    // latency and its own small up/down traffic, RTSP client for video
    // downlink, which dominates total bandwidth.
    qint64 m_wsLatencyMs = -1;
    double m_wsDownKbps = 0.0;
    double m_wsUpKbps = 0.0;
    double m_videoDownKbps = 0.0;
    double m_pointCloudDownKbps = 0.0;

    // --- Overflow-menu-driven slide-in context panel (WP / SCAN) ---
    QWidget *m_sidePanel = nullptr;
    QLabel *m_sidePanelTitleLabel = nullptr;
    QWidget *m_sidePanelBody = nullptr;
    QVBoxLayout *m_sidePanelBodyLayout = nullptr;
    QString m_sidePanelActiveId;
    class QListWidget *m_waypointListWidget = nullptr;
    int m_waypointCounter = 0;
    QPushButton *m_scanToggleButton = nullptr;
    QLabel *m_scanStatusLabel = nullptr;

    // --- Custom "⋮" overflow dropdown (replaces native QMenu — a native
    // QMenu popup was not reliably receiving touch input on Android) ---
    QWidget *m_overflowPanel = nullptr;
    QPushButton *m_overflowButton = nullptr;
    QPushButton *m_overflowSplitBtn = nullptr;

    // The former sidebar's view switches (3D/Top/WP/Scan/Settings), now the
    // top "VIEW" section of the overflow menu — kept exclusive by hand since
    // they're plain checkable QPushButtons, not a QButtonGroup.
    std::vector<QPushButton*> m_viewRowButtons;
    QString m_activeViewId = "3d";

    QTimer *m_uiUpdateTimer = nullptr; // drives fps/time/render animation
    QTimer *m_recTimer = nullptr;      // drives REC elapsed time label
    QElapsedTimer m_recElapsed;

    // Uptime: counts from the first successful drone connection this
    // session (we have no way to know true device power-on time, so this is
    // the closest proxy). Flight time: counts while armed, freezes at its
    // last value on disarm, restarts from zero on the next arm.
    QElapsedTimer m_uptimeElapsed;
    bool m_uptimeRunning = false;
    QElapsedTimer m_flightElapsed;
    bool m_flightActive = false;
    qint64 m_lastFlightMs = 0;

    // Flight-derived telemetry (approximated from robot pose stream)
    bool m_hasHomePosition = false;
    QVector3D m_homePosition;
    bool m_hasLastPosition = false;
    QVector3D m_lastPosition;
    qint64 m_lastPositionTimeMs = 0;
    double m_flownDistanceAccum = 0.0;
    double m_currentSpeed = 0.0;
    double m_currentAltitude = 0.0;
    double m_currentDistance = 0.0;
    float m_currentHeading = 0.0f;
    std::vector<QVector3D> m_flightPath;
};

#endif // MAINWINDOW_H
