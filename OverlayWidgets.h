#ifndef OVERLAYWIDGETS_H
#define OVERLAYWIDGETS_H

#include <QWidget>
#include <QVector3D>
#include <QDateTime>
#include <QMap>
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

private:
    void rebuild();

    QColor  m_dotColor;
    bool    m_showDot = false;
    QIcon   m_icon;
    QString m_label;
    QString m_value;
    QColor  m_valueColor;

    QHBoxLayout *m_layout = nullptr;
    QLabel *m_dotLabel = nullptr;
    QLabel *m_iconLabel = nullptr;
    QLabel *m_textLabel = nullptr;
};

// ============================================================================
// SidebarButton — one icon+label item in the sidebar rail. Built from plain
// QLabels in a centered QVBoxLayout instead of QToolButton's built-in
// icon/text layout — on Android's minimal style, a stylesheet'd QToolButton
// does not reliably center ToolButtonTextUnderIcon content, which is what
// made the sidebar icons look pinned to the left instead of centered.
// ============================================================================
class SidebarButton : public QWidget
{
    Q_OBJECT
public:
    explicit SidebarButton(const QIcon &icon, const QString &label, QWidget *parent = nullptr);

    void setChecked(bool checked);
    bool isChecked() const { return m_checked; }

signals:
    void clicked();

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void restyle();

    QIcon   m_icon;
    QLabel *m_iconLabel = nullptr;
    QLabel *m_textLabel = nullptr;
    bool    m_checked = false;
    bool    m_hovered = false;
};

// ============================================================================
// SidebarWidget — left vertical icon rail (3D / TOP / MAP / WP / SCAN / SETTINGS)
// ============================================================================
class SidebarWidget : public QWidget
{
    Q_OBJECT
public:
    explicit SidebarWidget(QWidget *parent = nullptr);

    void addItem(const QString &id, const QIcon &icon, const QString &label);
    void setActive(const QString &id);

signals:
    void itemClicked(const QString &id);

private:
    QVBoxLayout *m_layout = nullptr;
    QMap<QString, SidebarButton*> m_buttons;
    QString m_activeId;
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
    float m_min = -5.0f;
    float m_max = 5.0f;
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

private:
    QLabel *m_valueLabel = nullptr;
    SparklineWidget *m_sparkline = nullptr;
    QString m_unit;
};

#endif // OVERLAYWIDGETS_H
