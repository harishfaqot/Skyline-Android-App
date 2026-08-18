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
class RosImageClient;
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

// Mini player widget for picture-in-picture view
class MiniPlayerWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MiniPlayerWidget(QWidget *parent = nullptr);
    ~MiniPlayerWidget();

    void setContentWidget(QWidget *widget);
    void setTitle(const QString &title);
    void setRecording(bool recording);
    void setSplitActive(bool active);
    bool isMinimized() const { return m_minimized; }
    
public slots:
    void closePlayer();
    void minimizePlayer();
    void restorePlayer();

signals:
    void requestSwap();
    void requestSplitToggle();
    void requestRecordToggle();
    void closed();
    void minimized();
    void restored();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QWidget *m_contentWidget;
    QVBoxLayout *m_contentLayout;
    QWidget *m_contentContainer;
    QLabel *m_titleLabel;
    QPushButton *m_swapButton;
    QPushButton *m_recordButton;
    QPushButton *m_splitButton;
    QPushButton *m_closeButton;
    
    QPoint m_dragPosition;
    bool m_dragging;
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
    void onMiniPlayerClosed();
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
    void onImageReceived(const QImage &image);
    void onImageError(const QString &errorString);
    
    // Telemetry callbacks
    void onTelemetryReceived(int battery, int signal, const QString &mode,
                              double voltageV, double currentA, bool armed);

    // Recording
    void onToggleRecording();

    // New dashboard interactions
    void onUiUpdateTick();
    void onRecTimerTick();
    void onFollowSwitchToggled(bool following);

#ifdef Q_OS_ANDROID
private:
    void requestStoragePermissions();
#endif

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateStatusLabel();
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

    PointCloudWidget *m_pointCloudWidget;
    ImageViewerWidget *m_imageViewerWidget;
    RosPointCloudClient *m_rosClient;
    RosImageClient *m_imageClient;
    
    MiniPlayerWidget *m_miniPlayer;
    QWidget *m_mainViewContainer;
    QWidget *m_splitViewContainer;
    
    QPushButton *m_loadButton;
    QPushButton *m_rosToggleButton;
    QPushButton *m_clearButton;
    QPushButton *m_swapButton;
    QPushButton *m_splitButton;
    QPushButton *m_settingsButton;
    QPushButton *m_recordButton;
    QLineEdit *m_rosUrlEdit;
    QLabel *m_statusLabel;
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

    // "MORE" popover — houses the stats that used to sit directly in the
    // bottom bar (Flown Dist / Points / FPS / Time).
    QWidget *m_moreStatsPanel = nullptr;
    QPushButton *m_moreStatsButton = nullptr;

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
