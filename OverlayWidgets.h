#ifndef OVERLAYWIDGETS_H
#define OVERLAYWIDGETS_H

#include <QWidget>
#include <QVector3D>
#include <QDateTime>
#include <QString>
#include <QColor>
#include <QIcon>
#include <vector>
#include <deque>

class QLabel;
class QPushButton;
class QToolButton;
class QVBoxLayout;
class QHBoxLayout;
class QMouseEvent;
class QResizeEvent;
class QEnterEvent;

// ============================================================================
// StatusPill — small rounded pill used for "Connected", Battery, Signal, Mode
// ============================================================================
class StatusPill : public QWidget
{
    Q_OBJECT
public:
    explicit StatusPill(QWidget *parent = nullptr);

    void setDotColor(const QColor &color);   // pass invalid color to hide the dot
    void setIcon(const QIcon &icon);
    void setLabel(const QString &text);
    void setValue(const QString &text);
    void setValueColor(const QColor &color);

    // Lets a pill (e.g. Battery) act as a disclosure trigger for a details
    // popover — shows a pointer cursor and emits clicked() on tap/click.
    void setClickable(bool clickable);
    void setChevronVisible(bool visible);

signals:
    void clicked();

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void rebuild();

    QColor  m_dotColor;
    bool    m_showDot = false;
    QIcon   m_icon;
    QString m_label;
    QString m_value;
    QColor  m_valueColor;
    bool    m_clickable = false;

    QHBoxLayout *m_layout = nullptr;
    QLabel *m_dotLabel = nullptr;
    QLabel *m_iconLabel = nullptr;
    QLabel *m_textLabel = nullptr;
    QLabel *m_chevronLabel = nullptr;
};

// ============================================================================
// CameraPanelWidget — floating "CAMERA 1" preview panel with expand/snapshot
// ============================================================================
class CameraPanelWidget : public QWidget
{
    Q_OBJECT
public:
    explicit CameraPanelWidget(QWidget *parent = nullptr);

    void setContentWidget(QWidget *widget);
    void setLive(bool live);

signals:
    void expandClicked();
    void snapshotClicked();

private:
    QWidget *m_contentContainer = nullptr;
    QVBoxLayout *m_contentLayout = nullptr;
    QLabel *m_dotLabel = nullptr;
};

// ============================================================================
// ElevationLegendWidget — vertical rainbow gradient bar with tick labels
// ============================================================================
class ElevationLegendWidget : public QWidget
{
    Q_OBJECT
public:
    explicit ElevationLegendWidget(QWidget *parent = nullptr);

    void setRange(float minVal, float maxVal);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    float m_min = -10.0f;
    float m_max = 10.0f;
};

// ============================================================================
// CompassWidget — circular heading indicator
// ============================================================================
class CompassWidget : public QWidget
{
    Q_OBJECT
public:
    explicit CompassWidget(QWidget *parent = nullptr);

    void setHeading(float degrees); // 0 = North, clockwise

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    static QString cardinal(float degrees);
    float m_heading = 0.0f;
};

// ============================================================================
// MiniMapWidget — small top-down map showing the flown path
// ============================================================================
class MiniMapWidget : public QWidget
{
    Q_OBJECT
public:
    explicit MiniMapWidget(QWidget *parent = nullptr);

    void setPath(const std::vector<QVector3D> &path);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void positionZoomButtons();

    std::vector<QVector3D> m_path;
    QPushButton *m_zoomInBtn = nullptr;
    QPushButton *m_zoomOutBtn = nullptr;
    float m_zoom = 1.0f;
};

// ============================================================================
// FollowSwitchWidget — two-state sliding pill switch ("ORBIT" / "FOLLOW")
// that toggles whether the 3D camera target tracks the drone's live pose.
// ============================================================================
class FollowSwitchWidget : public QWidget
{
    Q_OBJECT
public:
    explicit FollowSwitchWidget(QWidget *parent = nullptr);

    void setFollowing(bool following); // sets state without emitting toggled()
    bool isFollowing() const { return m_following; }

signals:
    void toggled(bool following);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void animateTo(qreal target);

    bool m_following = false;
    qreal m_slide = 0.0; // 0 = ORBIT (left), 1 = FOLLOW (right)
    class QVariantAnimation *m_anim = nullptr;
};

// ============================================================================
// SparklineWidget — tiny trend line used under bottom stat values
// ============================================================================
class SparklineWidget : public QWidget
{
    Q_OBJECT
public:
    explicit SparklineWidget(QWidget *parent = nullptr);
    void pushValue(float value);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    std::deque<float> m_values;
};

// ============================================================================
// StatItemWidget — one "ALTITUDE / 2.3 m" block with sparkline for bottom bar
// ============================================================================
class StatItemWidget : public QWidget
{
    Q_OBJECT
public:
    explicit StatItemWidget(const QString &label, const QString &unit,
                             bool withSparkline, QWidget *parent = nullptr);

    void setValue(double value, int decimals = 1);
    void setText(const QString &text);
    void setValueColor(const QColor &color);

private:
    QLabel *m_valueLabel = nullptr;
    SparklineWidget *m_sparkline = nullptr;
    QString m_unit;
};

// ============================================================================
// CaptureModeSwitch — two-state sliding pill switch (video/photo), the same
// visual pattern as FollowSwitchWidget above but with icons instead of text.
// ============================================================================
class CaptureModeSwitch : public QWidget
{
    Q_OBJECT
public:
    explicit CaptureModeSwitch(QWidget *parent = nullptr);

    void setVideoMode(bool video); // sets state without emitting toggled()
    bool isVideoMode() const { return m_videoMode; }

signals:
    void toggled(bool videoMode);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void animateTo(qreal target);

    bool m_videoMode = true;
    qreal m_slide = 0.0; // 0 = video (left), 1 = photo (right)
    class QVariantAnimation *m_anim = nullptr;
};

// ============================================================================
// ShutterButton — big circular capture button. Custom-painted (drawEllipse)
// rather than a QPushButton with a QSS border-radius: Android's Qt style
// plugin doesn't reliably respect a stylesheet border-radius on QPushButton,
// which is what made this render as a square before — every other circular
// overlay in this file (CompassWidget, FollowSwitchWidget's knob, etc.)
// already avoids that by painting itself directly, so this follows suit.
// ============================================================================
class ShutterButton : public QWidget
{
    Q_OBJECT
public:
    explicit ShutterButton(QWidget *parent = nullptr);

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    bool m_pressed = false;
};

// ============================================================================
// CaptureControlWidget — floating photo/video capture cluster (QGC-style):
// a sliding video/photo mode switch above a big shutter button. The elapsed-
// recording readout lives in the top bar instead (see MainWindow), not here.
// ============================================================================
class CaptureControlWidget : public QWidget
{
    Q_OBJECT
public:
    explicit CaptureControlWidget(QWidget *parent = nullptr);

    // Updates the shutter's color/icon (red dot = video idle, red square =
    // video recording, blue = photo mode) and locks the mode switch while
    // recording, since you can't change capture mode mid-recording.
    void setRecording(bool recording);
    bool isVideoMode() const { return m_videoMode; }

signals:
    // Emitted when the shutter is tapped; videoMode reflects whichever mode
    // was selected in the switch at the time of the tap.
    void shutterPressed(bool videoMode);

private:
    CaptureModeSwitch *m_modeSwitch = nullptr;
    ShutterButton *m_shutterBtn = nullptr;
    bool m_videoMode = true;
    bool m_recording = false;
};

#endif // OVERLAYWIDGETS_H
