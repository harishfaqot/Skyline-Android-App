#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "RosPointCloudClient.h"
#include <QMainWindow>

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
class QMenu;
class QAction;
class QVBoxLayout;
class QDialog;
class QSplitter;

// Mini player widget for picture-in-picture view
class MiniPlayerWidget : public QWidget
{
    Q_OBJECT

public:
    explicit MiniPlayerWidget(QWidget *parent = nullptr);
    ~MiniPlayerWidget();

    void setContentWidget(QWidget *widget);
    void setTitle(const QString &title);
    bool isMinimized() const { return m_minimized; }
    
public slots:
    void closePlayer();
    void minimizePlayer();
    void restorePlayer();

signals:
    void requestSwap();
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
    void onTelemetryReceived(int battery, int signal, const QString &mode);
    
    // Recording
    void onToggleRecording();

#ifdef Q_OS_ANDROID
private:
    void requestStoragePermissions();
#endif

private:
    void updateStatusLabel();
    void setupUI();
    void createSettingsDialog();
    void updateViewMode();
    
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
    QPushButton *m_restoreMiniButton;
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
    
    enum ViewMode { PointCloudMain, CameraMain, SplitView };
    ViewMode m_currentViewMode;
    
    bool m_rosStreamActive;
    bool m_imageStreamActive;
    bool m_isRecording;
    bool m_manualDisconnect;
    int m_batteryLevel;
    int m_signalStrength;
    QString m_flightMode;
};

#endif // MAINWINDOW_H
