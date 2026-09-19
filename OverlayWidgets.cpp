#include "OverlayWidgets.h"
#include "IconRenderer.h"

#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QConicalGradient>
#include <QtMath>
#include <QMap>
#include <QGuiApplication>
#include <QScreen>
#include <QSizePolicy>
#include <QStyle>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QEnterEvent>
#include <QVariantAnimation>
#include <QEasingCurve>
#include <algorithm>
#include <cmath>

// Local DPI helpers (mirrors the ones in MainWindow.cpp; kept file-local
// here to avoid coupling these reusable widgets to MainWindow internals).
static float ow_dpScale()
{
    static float s_scale = 0.0f;
    if (s_scale == 0.0f) {
        QScreen *screen = QGuiApplication::primaryScreen();
        s_scale = screen ? screen->logicalDotsPerInch() / 96.0f : 1.0f;
        s_scale = qBound(1.0f, s_scale, 4.0f);
    }
    return s_scale;
}
static int owdp(int v) { return qRound(static_cast<float>(v) * ow_dpScale()); }
static int owsp(int v) { return qRound(static_cast<float>(v) * ow_dpScale()); }

// ============================================================================
// StatusPill
// ============================================================================
StatusPill::StatusPill(QWidget *parent)
    : QWidget(parent)
    , m_valueColor(QColor("#e8e8e8"))
{
    // Plain QWidget subclasses don't paint their own stylesheet's
    // background/border unless this is set.
    setAttribute(Qt::WA_StyledBackground, true);
    // Needed for the ":hover" rule below to actually repaint on a custom
    // QWidget (Qt only tracks hover state for widgets that opt in).
    setAttribute(Qt::WA_Hover, true);

    // Flat by default — no box/border, matching the borderless REC button
    // and "⋮" overflow control already in the top bar. Only a pill that's
    // actually clickable (Battery, via setClickable()) gets a soft hover
    // highlight, as a hint that it's tappable.
    setStyleSheet(QString(R"(
        StatusPill {
            background-color: transparent;
            border: none;
            border-radius: %1px;
        }
        StatusPill[clickable="true"]:hover {
            background-color: rgba(255, 255, 255, 15);
        }
    )").arg(owdp(6)));

    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(owdp(10), owdp(4), owdp(10), owdp(4));
    m_layout->setSpacing(owdp(6));

    m_dotLabel = new QLabel(this);
    m_dotLabel->setFixedSize(owdp(8), owdp(8));
    m_dotLabel->hide();
    m_layout->addWidget(m_dotLabel);

    m_iconLabel = new QLabel(this);
    m_iconLabel->setStyleSheet("background: transparent;");
    m_iconLabel->setFixedSize(owdp(14), owdp(14));
    m_iconLabel->hide();
    m_layout->addWidget(m_iconLabel);

    m_textLabel = new QLabel(this);
    m_textLabel->setStyleSheet(QString("background: transparent; font-size: %1px; font-weight: 600; color: #e8e8e8;").arg(owsp(12)));
    m_layout->addWidget(m_textLabel);

    m_chevronLabel = new QLabel(this);
    m_chevronLabel->setStyleSheet("background: transparent;");
    m_chevronLabel->setFixedSize(owdp(11), owdp(11));
    m_chevronLabel->setPixmap(IconRenderer::icon(IconRenderer::Id::ChevronDown, QColor("#0075c5"), owdp(11)).pixmap(owdp(11), owdp(11)));
    m_chevronLabel->hide();
    m_layout->addWidget(m_chevronLabel);

    setMinimumHeight(owdp(28));
}

void StatusPill::rebuild()
{
    QString text;
    if (!m_label.isEmpty()) {
        text += m_label + "  ";
    }
    text += QString("<span style=\"color:%1; font-weight:700;\">%2</span>")
                .arg(m_valueColor.name(), m_value.toHtmlEscaped());
    m_textLabel->setText(text);
}

void StatusPill::setDotColor(const QColor &color)
{
    m_showDot = color.isValid();
    m_dotLabel->setVisible(m_showDot);
    if (m_showDot) {
        m_dotLabel->setStyleSheet(QString("background-color: %1; border-radius: %2px;")
                                       .arg(color.name()).arg(owdp(4)));
    }
}

void StatusPill::setIcon(const QIcon &icon)
{
    m_icon = icon;
    const int size = owdp(14);
    m_iconLabel->setPixmap(icon.pixmap(size, size));
    m_iconLabel->setVisible(!icon.isNull());
}

void StatusPill::setLabel(const QString &text)
{
    m_label = text;
    rebuild();
}

void StatusPill::setValue(const QString &text)
{
    m_value = text;
    rebuild();
}

void StatusPill::setValueColor(const QColor &color)
{
    m_valueColor = color;
    rebuild();
}

void StatusPill::setClickable(bool clickable)
{
    m_clickable = clickable;
    setCursor(clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);

    // Drives the "StatusPill[clickable=\"true\"]:hover" rule above — a
    // repolish is needed for a dynamic property change to affect an
    // already-applied style sheet.
    setProperty("clickable", clickable);
    style()->unpolish(this);
    style()->polish(this);
}

void StatusPill::setChevronVisible(bool visible)
{
    m_chevronLabel->setVisible(visible);
}

void StatusPill::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_clickable && event->button() == Qt::LeftButton && rect().contains(event->pos())) {
        emit clicked();
    }
    QWidget::mouseReleaseEvent(event);
}

// ============================================================================
// CameraPanelWidget
// ============================================================================
CameraPanelWidget::CameraPanelWidget(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QString(R"(
        CameraPanelWidget {
            background-color: rgba(10, 10, 10, 220);
            border: 1px solid rgba(0, 117, 197, 90);
            border-radius: %1px;
        }
    )").arg(owdp(8)));

    QVBoxLayout *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Header
    QWidget *header = new QWidget(this);
    header->setFixedHeight(owdp(42));
    header->setStyleSheet("background: transparent;");
    QHBoxLayout *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(owdp(10), owdp(4), owdp(6), owdp(4));
    headerLayout->setSpacing(owdp(6));

    m_dotLabel = new QLabel(header);
    m_dotLabel->setFixedSize(owdp(8), owdp(8));
    m_dotLabel->setStyleSheet(QString("background-color: #00ff00; border-radius: %1px;").arg(owdp(4)));
    headerLayout->addWidget(m_dotLabel);

    QLabel *title = new QLabel("CAMERA 1", header);
    title->setStyleSheet(QString("background: transparent; color: #cfcfcf; font-size: %1px; font-weight: 700; letter-spacing: 1px;").arg(owsp(11)));
    headerLayout->addWidget(title);
    headerLayout->addStretch();

    auto makeIconBtn = [&](IconRenderer::Id iconId) {
        QPushButton *b = new QPushButton(header);
        b->setFixedSize(owdp(34), owdp(34));
        b->setIcon(IconRenderer::icon(iconId, QColor("#e8e8e8"), owdp(17)));
        b->setIconSize(QSize(owdp(17), owdp(17)));
        b->setCursor(Qt::PointingHandCursor);
        b->setStyleSheet(QString(R"(
            QPushButton { background-color: rgba(255,255,255,15);
                          border: none; border-radius: %1px; padding: 0; }
            QPushButton:hover { background-color: rgba(255,255,255,35); }
        )").arg(owdp(8)));
        return b;
    };

    QPushButton *expandBtn = makeIconBtn(IconRenderer::Id::Expand);
    connect(expandBtn, &QPushButton::clicked, this, &CameraPanelWidget::expandClicked);
    headerLayout->addWidget(expandBtn);

    QPushButton *snapBtn = makeIconBtn(IconRenderer::Id::Camera);
    connect(snapBtn, &QPushButton::clicked, this, &CameraPanelWidget::snapshotClicked);
    headerLayout->addWidget(snapBtn);

    outer->addWidget(header);

    m_contentContainer = new QWidget(this);
    m_contentContainer->setStyleSheet("background-color: black;");
    m_contentLayout = new QVBoxLayout(m_contentContainer);
    m_contentLayout->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(m_contentContainer, 1);
}

void CameraPanelWidget::setContentWidget(QWidget *widget)
{
    QLayoutItem *item;
    while ((item = m_contentLayout->takeAt(0)) != nullptr) {
        delete item;
    }
    if (widget) {
        widget->setParent(m_contentContainer);
        m_contentLayout->addWidget(widget);
        widget->show();
    }
}

void CameraPanelWidget::setLive(bool live)
{
    m_dotLabel->setStyleSheet(QString("background-color: %1; border-radius: %2px;")
                                   .arg(live ? "#00ff00" : "#666666").arg(owdp(4)));
}

// ============================================================================
// ElevationLegendWidget
// ============================================================================
ElevationLegendWidget::ElevationLegendWidget(QWidget *parent)
    : QWidget(parent)
{
    setFixedWidth(owdp(72));
    setMinimumHeight(owdp(260));

    // Purely decorative (paintEvent only, no interactive children) but it
    // sits over the 3D viewport at the right edge — exactly where a pinch
    // gesture's second finger tends to land. Without this, any touch point
    // that lands on this widget never reaches PointCloudWidget, so the
    // widget only ever sees 1 active finger and pinch-zoom silently fails
    // unless both fingers happen to land outside this rect.
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
}

void ElevationLegendWidget::setRange(float minVal, float maxVal)
{
    m_min = minVal;
    m_max = maxVal;
    update();
}

void ElevationLegendWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int titleH = owdp(16);
    const int barW = owdp(14);
    const int barX = owdp(4);
    const int barY = titleH + owdp(6);
    const int barH = height() - barY - owdp(6);

    // Title
    p.setPen(QColor("#9a9a96"));
    QFont f = p.font();
    f.setPixelSize(owsp(9));
    f.setBold(true);
    p.setFont(f);
    p.drawText(QRect(0, 0, width(), titleH), Qt::AlignLeft | Qt::AlignVCenter, "ELEVATION (m)");

    // Rainbow "jet"-like gradient bar: red(top) -> yellow -> green -> cyan -> blue(bottom)
    QLinearGradient grad(0, barY, 0, barY + barH);
    grad.setColorAt(0.0, QColor(220, 30, 20));
    grad.setColorAt(0.25, QColor(240, 200, 30));
    grad.setColorAt(0.5, QColor(40, 200, 90));
    grad.setColorAt(0.75, QColor(30, 140, 220));
    grad.setColorAt(1.0, QColor(30, 40, 170));

    QRectF barRect(barX, barY, barW, barH);
    p.setPen(QPen(QColor(255, 255, 255, 40), 1));
    p.setBrush(grad);
    p.drawRoundedRect(barRect, 2, 2);

    // Tick labels
    p.setPen(QColor("#cfcfcf"));
    QFont tf = p.font();
    tf.setPixelSize(owsp(9));
    tf.setBold(false);
    p.setFont(tf);

    const int ticks = 5;
    for (int i = 0; i < ticks; ++i) {
        float t = static_cast<float>(i) / (ticks - 1); // 0 at top
        float val = m_max - t * (m_max - m_min);
        int y = barY + qRound(t * barH);
        p.drawLine(barX + barW, y, barX + barW + owdp(3), y);
        QRect textRect(barX + barW + owdp(6), y - owdp(7), width() - (barX + barW + owdp(6)), owdp(14));
        p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, QString::number(val, 'f', 1));
    }
}

// ============================================================================
// CompassWidget
// ============================================================================
CompassWidget::CompassWidget(QWidget *parent)
    : QWidget(parent)
{
    // Enlarged from the original 96x110 so the dial, heading readout, and
    // cardinal label are all comfortably legible at a glance in flight.
    // Height trimmed since the heading readout is now a single line
    // ("96° E") instead of two stacked lines.
    setFixedSize(owdp(136), owdp(134));

    // Same reasoning as ElevationLegendWidget: this is a paint-only widget
    // with no buttons, but it sits right next to the mini-map in the
    // bottom-right corner — a common spot for a pinch gesture's outer
    // finger. Make it pass touch/mouse events through to the 3D view.
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
}

void CompassWidget::setHeading(float degrees)
{
    // Normalize to [0, 360)
    degrees = std::fmod(degrees, 360.0f);
    if (degrees < 0) degrees += 360.0f;
    m_heading = degrees;
    update();
}

QString CompassWidget::cardinal(float degrees)
{
    static const char *names[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    int idx = static_cast<int>(std::round(degrees / 45.0f)) % 8;
    if (idx < 0) idx += 8;
    return QString(names[idx]);
}

void CompassWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int dialSize = owdp(104);
    QRectF dialRect((width() - dialSize) / 2.0, 0, dialSize, dialSize);
    QPointF center = dialRect.center();
    float radius = dialSize / 2.0f;

    // Outer ring
    p.setPen(QPen(QColor(0, 117, 197, 120), 2.0));
    p.setBrush(QColor(10, 10, 10, 160));
    p.drawEllipse(dialRect);

    // Cardinal labels on the ring (N/E/S/W) and they rotate with the dial.
    p.save();
    p.translate(center);
    p.rotate(-m_heading);

    struct CardinalMark { float ang; const char *label; QColor color; };
    const std::vector<CardinalMark> marks = {
        {0, "N", QColor("#ff4d4d")},
        {90, "E", QColor("#cfcfcf")},
        {180, "S", QColor("#cfcfcf")},
        {270, "W", QColor("#cfcfcf")},
    };

    QFont cf = p.font();
    cf.setPixelSize(owsp(12));
    cf.setBold(true);
    p.setFont(cf);

    for (const auto &m : marks) {
        p.save();
        p.rotate(m.ang);
        p.setPen(m.color);
        QRectF tr(-owdp(12), -radius + owdp(6), owdp(24), owdp(14));
        p.drawText(tr, Qt::AlignCenter, QString::fromLatin1(m.label));
        p.restore();
    }
    p.restore();

    // Heading needle (points up, fixed — the dial rotates instead)
    p.save();
    p.translate(center);
    QPainterPath needle;
    float nLen = radius - owdp(20);
    needle.moveTo(0, -nLen);
    needle.lineTo(owdp(7), owdp(6));
    needle.lineTo(0, 0);
    needle.lineTo(-owdp(7), owdp(6));
    needle.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#0075c5"));
    p.drawPath(needle);
    p.setBrush(QColor("#0a0a0a"));
    p.drawEllipse(QPointF(0, 0), owdp(4), owdp(4));
    p.restore();

    // Heading readout below dial.
    QFont hf = p.font();
    hf.setPixelSize(owsp(17));
    hf.setBold(true);
    p.setFont(hf);
    p.setPen(QColor("#0075c5"));
    QRect degRect(0, dialSize + owdp(4), width(), owdp(22));
    p.drawText(degRect, Qt::AlignHCenter | Qt::AlignTop,
               QString("%1°  %2").arg(qRound(m_heading)).arg(cardinal(m_heading)));
}

// ============================================================================
// MiniMapWidget
// ============================================================================
// FIX: buttons were 14dp with border-radius = half their size, i.e. a
// perfect circle. Two small circles of the same dark color sitting close
// together read visually as one merged, elongated capsule/pill rather than
// two distinct buttons — that's what looked like "overlap". 14dp is also
// well under a comfortable touch-target size. Sizing them up and using a
// modest border-radius (a rounded square rather than a full circle) fixes
// both the visual merging and the touch target in one change.
static constexpr int kZoomBtnSize   = 20; // dp, up from 14
static constexpr int kZoomBtnRadius = 5;  // dp — rounded square, not a circle

MiniMapWidget::MiniMapWidget(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(owdp(150), owdp(130));

    // Previously these used plain "+"/"−" text on a near-transparent
    // (~8% opacity) background, which was very easy to miss against the
    // busy grid/path drawing underneath. Solid dark chips + vector icons
    // make them clearly visible regardless of what's drawn behind them.
    m_zoomInBtn = new QPushButton(this);
    m_zoomOutBtn = new QPushButton(this);
    m_zoomInBtn->setText("+");
    m_zoomOutBtn->setText("-");
    for (QPushButton *b : { m_zoomInBtn, m_zoomOutBtn }) {
        b->setFixedSize(owdp(kZoomBtnSize), owdp(kZoomBtnSize));
        QFont f = b->font();
        f.setPixelSize(owsp(12));
        f.setBold(true);
        b->setFont(f);
        b->setCursor(Qt::PointingHandCursor);
        // FIX: MainWindow applies a global stylesheet with
        // "QPushButton { min-height: 36dp; }" that cascades down to every
        // QPushButton in the app (Qt style sheet inheritance). That
        // ancestor rule was overriding setFixedSize()'s height here — the
        // width stayed at kZoomBtnSize since there's no matching
        // min-width rule, but the height kept getting forced to 36dp,
        // which is exactly why changing kZoomBtnSize only visibly changed
        // the width. Explicitly declaring min/max-height AND min/max-width
        // in this button's own (more specific, locally-set) stylesheet
        // wins the cascade and locks the real size.
        b->setStyleSheet(QString(R"(
            QPushButton { color: #e8e8e8; background-color: rgba(10,10,12,180);
                          border: 1px solid rgba(255,255,255,30); border-radius: %1px; padding: 0;
                          min-width: %2px; max-width: %2px; min-height: %2px; max-height: %2px; }
            QPushButton:hover { background-color: rgba(30,30,34,220); border-color: #0075c5; }
        )").arg(owdp(kZoomBtnRadius)).arg(owdp(kZoomBtnSize)));
    }
    connect(m_zoomInBtn, &QPushButton::clicked, this, [this]() {
        m_zoom = qMin(4.0f, m_zoom * 1.25f);
        update();
    });
    connect(m_zoomOutBtn, &QPushButton::clicked, this, [this]() {
        m_zoom = qMax(0.25f, m_zoom / 1.25f);
        update();
    });

    // FIX: position the zoom buttons here, once, instead of inside
    // paintEvent(). The widget uses setFixedSize() above, so its
    // width()/height() are already final at this point — there's no need
    // to recompute their position on every single repaint. Repeatedly
    // calling move() from paintEvent() (which fires on nearly every frame
    // while flight-path data streams in) is an anti-pattern: it isn't
    // reliably applied/composited on every platform, which is what caused
    // the two buttons to visually overlap instead of sitting neatly
    // stacked top-right. resizeEvent() (below) also calls this, as a
    // defensive fallback in case anything ever resizes this widget.
    positionZoomButtons();
}

void MiniMapWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    positionZoomButtons();
}

void MiniMapWidget::positionZoomButtons()
{
    if (!m_zoomInBtn || !m_zoomOutBtn) return;

    const int btnSize = owdp(kZoomBtnSize);
    const int rightPad = owdp(4);
    const int topPad = owdp(5);
    const int gap = owdp(8); // clear visual gap between the two distinct chips
    const int zoomBtnX = width() - rightPad - btnSize;
    m_zoomInBtn->move(zoomBtnX, topPad);
    m_zoomOutBtn->move(zoomBtnX, topPad + btnSize + gap);
    m_zoomInBtn->raise();
    m_zoomOutBtn->raise();
}

void MiniMapWidget::setPath(const std::vector<QVector3D> &path)
{
    m_path = path;
    update();
}

void MiniMapWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    QRectF full = rect();
    p.setPen(QPen(QColor(0, 117, 197, 90), 1));
    p.setBrush(QColor(15, 15, 18, 210));
    p.drawRoundedRect(full.adjusted(0.5, 0.5, -0.5, -0.5), owdp(6), owdp(6));

    // Grid
    p.setPen(QPen(QColor(255, 255, 255, 18), 1));
    const int gridStep = owdp(20);
    for (int x = gridStep; x < width(); x += gridStep) p.drawLine(x, 0, x, height());
    for (int y = gridStep; y < height(); y += gridStep) p.drawLine(0, y, width(), y);

    // Path
    if (m_path.size() >= 2) {
        float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
        for (const auto &v : m_path) {
            minX = std::min(minX, v.x()); maxX = std::max(maxX, v.x());
            minY = std::min(minY, v.y()); maxY = std::max(maxY, v.y());
        }
        float spanX = std::max(0.001f, maxX - minX);
        float spanY = std::max(0.001f, maxY - minY);
        float pad = owdp(14);
        float availW = (width() - 2 * pad) * m_zoom;
        float availH = (height() - 2 * pad) * m_zoom;
        float scale = std::min(availW / spanX, availH / spanY);
        float cx = width() / 2.0f;
        float cy = height() / 2.0f;
        float midX = (minX + maxX) / 2.0f;
        float midY = (minY + maxY) / 2.0f;

        QPainterPath path;
        for (size_t i = 0; i < m_path.size(); ++i) {
            float px = cx + (m_path[i].x() - midX) * scale;
            float py = cy - (m_path[i].y() - midY) * scale;
            if (i == 0) path.moveTo(px, py);
            else path.lineTo(px, py);
        }
        p.setPen(QPen(QColor("#0075c5"), 2));
        p.drawPath(path);

        // Arrow at last point
        QPointF last = path.currentPosition();
        p.setBrush(QColor("#0088e0"));
        p.setPen(Qt::NoPen);
        p.drawEllipse(last, owdp(3), owdp(3));
    } else {
        p.setPen(QColor("#5a5a56"));
        QFont f = p.font();
        f.setPixelSize(owsp(9));
        p.setFont(f);
        p.drawText(full, Qt::AlignCenter, "No path yet");
    }

    // Scale bar
    p.setPen(QColor("#9a9a96"));
    QFont sf = p.font();
    sf.setPixelSize(owsp(8));
    p.setFont(sf);
    int barY = height() - owdp(6);
    p.drawLine(width() - owdp(38), barY, width() - owdp(8), barY);
    p.drawText(QRect(width() - owdp(46), barY - owdp(14), owdp(44), owdp(12)),
               Qt::AlignRight, "10 m");
}

// ============================================================================
// FollowSwitchWidget
// ============================================================================
FollowSwitchWidget::FollowSwitchWidget(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(owdp(148), owdp(32));
    setCursor(Qt::PointingHandCursor);
    setToolTip("Toggle camera follow mode");
}

void FollowSwitchWidget::setFollowing(bool following)
{
    if (m_following == following) return;
    m_following = following;
    animateTo(m_following ? 1.0 : 0.0);
}

void FollowSwitchWidget::animateTo(qreal target)
{
    if (!m_anim) {
        m_anim = new QVariantAnimation(this);
        m_anim->setDuration(180);
        m_anim->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            m_slide = v.toReal();
            update();
        });
    }
    m_anim->stop();
    m_anim->setStartValue(m_slide);
    m_anim->setEndValue(target);
    m_anim->start();
}

void FollowSwitchWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
        m_following = !m_following;
        animateTo(m_following ? 1.0 : 0.0);
        emit toggled(m_following);
    }
    QWidget::mouseReleaseEvent(event);
}

void FollowSwitchWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF full(0, 0, width(), height());
    const qreal radius = height() / 2.0;

    // Track
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(20, 20, 15, 210));
    p.drawRoundedRect(full, radius, radius);

    // Sliding highlight, half the track width, eased between ORBIT/FOLLOW
    const qreal halfW = width() / 2.0;
    QRectF knob(halfW * m_slide, 0, halfW, height());
    knob.adjust(owdp(2), owdp(2), -owdp(2), -owdp(2));
    QColor knobColor = QColor(0, 117, 197, 255);
    p.setBrush(knobColor);
    p.drawRoundedRect(knob, radius - owdp(2), radius - owdp(2));

    // Labels
    QFont f = p.font();
    f.setPixelSize(owsp(10));
    f.setBold(true);
    p.setFont(f);

    QRectF orbitRect(0, 0, halfW, height());
    QRectF followRect(halfW, 0, halfW, height());

    p.setPen(m_slide < 0.5 ? QColor("#ffffff") : QColor("#8a8a86"));
    p.drawText(orbitRect, Qt::AlignCenter, "ORBIT");

    p.setPen(m_slide >= 0.5 ? QColor("#ffffff") : QColor("#8a8a86"));
    p.drawText(followRect, Qt::AlignCenter, "FOLLOW");
}

// ============================================================================
// SparklineWidget
// ============================================================================
SparklineWidget::SparklineWidget(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(owdp(18));
    setMinimumWidth(owdp(60));
}

void SparklineWidget::pushValue(float value)
{
    m_values.push_back(value);
    while (m_values.size() > 40) m_values.pop_front();
    update();
}

void SparklineWidget::paintEvent(QPaintEvent *)
{
    if (m_values.size() < 2) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    float minV = *std::min_element(m_values.begin(), m_values.end());
    float maxV = *std::max_element(m_values.begin(), m_values.end());
    float span = std::max(0.001f, maxV - minV);

    QPainterPath path;
    int n = static_cast<int>(m_values.size());
    for (int i = 0; i < n; ++i) {
        float x = width() * static_cast<float>(i) / (n - 1);
        float t = (m_values[i] - minV) / span;
        float y = height() - t * (height() - 2) - 1;
        if (i == 0) path.moveTo(x, y);
        else path.lineTo(x, y);
    }
    p.setPen(QPen(QColor("#0075c5"), 1.5));
    p.drawPath(path);
}

// ============================================================================
// StatItemWidget
// ============================================================================
StatItemWidget::StatItemWidget(const QString &label, const QString &unit,
                                bool withSparkline, QWidget *parent)
    : QWidget(parent)
    , m_unit(unit)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(owdp(1));

    QLabel *labelWidget = new QLabel(label, this);
    labelWidget->setStyleSheet(QString("background: transparent; color: #8a8a86; font-size: %1px; font-weight: 700; letter-spacing: 0.5px;").arg(owsp(9)));
    labelWidget->setMinimumHeight(owdp(12));
    labelWidget->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->addWidget(labelWidget);

    m_valueLabel = new QLabel("--", this);
    m_valueLabel->setStyleSheet(QString("background: transparent; color: #e8e8e8; font-size: %1px; font-weight: 700;").arg(owsp(15)));
    m_valueLabel->setMinimumHeight(owdp(24));
    m_valueLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    layout->addWidget(m_valueLabel);

    if (withSparkline) {
        m_sparkline = new SparklineWidget(this);
        layout->addWidget(m_sparkline);
        setMinimumHeight(owdp(52));
    } else {
        setMinimumHeight(owdp(34));
    }
}

void StatItemWidget::setValue(double value, int decimals)
{
    m_valueLabel->setText(QString("%1 %2").arg(value, 0, 'f', decimals).arg(m_unit));
    if (m_sparkline) m_sparkline->pushValue(static_cast<float>(value));
}

void StatItemWidget::setText(const QString &text)
{
    m_valueLabel->setText(text);
}

void StatItemWidget::setValueColor(const QColor &color)
{
    m_valueLabel->setStyleSheet(QString("background: transparent; color: %1; font-size: %2px; font-weight: 700;")
                                     .arg(color.name()).arg(owsp(15)));
}

// ============================================================================
// CaptureModeSwitch
// ============================================================================
CaptureModeSwitch::CaptureModeSwitch(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(owdp(72), owdp(32));
    setCursor(Qt::PointingHandCursor);
    setToolTip("Switch between video and photo");
}

void CaptureModeSwitch::setVideoMode(bool video)
{
    if (m_videoMode == video) return;
    m_videoMode = video;
    animateTo(m_videoMode ? 0.0 : 1.0);
}

void CaptureModeSwitch::animateTo(qreal target)
{
    if (!m_anim) {
        m_anim = new QVariantAnimation(this);
        m_anim->setDuration(180);
        m_anim->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            m_slide = v.toReal();
            update();
        });
    }
    m_anim->stop();
    m_anim->setStartValue(m_slide);
    m_anim->setEndValue(target);
    m_anim->start();
}

void CaptureModeSwitch::mouseReleaseEvent(QMouseEvent *event)
{
    if (isEnabled() && event->button() == Qt::LeftButton && rect().contains(event->pos())) {
        m_videoMode = !m_videoMode;
        animateTo(m_videoMode ? 0.0 : 1.0);
        emit toggled(m_videoMode);
    }
    QWidget::mouseReleaseEvent(event);
}

void CaptureModeSwitch::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF full(0, 0, width(), height());
    const qreal radius = height() / 2.0;

    // Track
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(20, 20, 15, 210));
    p.drawRoundedRect(full, radius, radius);

    // Sliding knob — green on both sides; only its position indicates
    // which mode is active.
    const qreal halfW = width() / 2.0;
    QRectF knob(halfW * m_slide, 0, halfW, height());
    knob.adjust(owdp(2), owdp(2), -owdp(2), -owdp(2));
    p.setBrush(QColor("#2ecc40"));
    p.drawRoundedRect(knob, radius - owdp(2), radius - owdp(2));

    // Icons — white on whichever side the knob currently covers (for
    // contrast against the green knob), gray otherwise.
    const int iconSize = owdp(15);
    QRectF videoRect(0, 0, halfW, height());
    QRectF photoRect(halfW, 0, halfW, height());

    QColor videoIconColor = m_slide < 0.5 ? Qt::white : QColor("#8a8a86");
    QIcon videoIcon = IconRenderer::icon(IconRenderer::Id::Record, videoIconColor, iconSize);
    videoIcon.paint(&p, videoRect.toRect(), Qt::AlignCenter);

    QColor photoIconColor = m_slide >= 0.5 ? Qt::white : QColor("#8a8a86");
    QIcon photoIcon = IconRenderer::icon(IconRenderer::Id::Camera, photoIconColor, iconSize);
    photoIcon.paint(&p, photoRect.toRect(), Qt::AlignCenter);
}

// ============================================================================
// ShutterButton
// ============================================================================
ShutterButton::ShutterButton(QWidget *parent)
    : QWidget(parent)
{
    setFixedSize(owdp(64), owdp(64));
    setCursor(Qt::PointingHandCursor);
}

void ShutterButton::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressed = true;
        update();
        event->accept();
    }
}

void ShutterButton::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    const bool wasPressed = m_pressed;
    m_pressed = false;
    update();
    if (wasPressed && rect().contains(event->pos())) {
        emit clicked();
    }
    event->accept();
}

void ShutterButton::setRecording(bool recording)
{
    if (m_recording == recording) return;
    m_recording = recording;
    update();
}

void ShutterButton::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // Same red in both modes — the mode switch above already communicates
    // video vs. photo, so the shutter itself doesn't need to change color.
    QColor bg = QColor("#e53935");
    if (m_pressed) bg = bg.darker(115);

    const qreal borderW = owdp(3);
    QRectF circleRect = QRectF(rect()).adjusted(borderW / 2.0, borderW / 2.0, -borderW / 2.0, -borderW / 2.0);

    p.setPen(QPen(QColor(255, 255, 255, 200), borderW));

    if (m_recording) {
        // Recording: the ring stays a circle, but the filled shape inside
        // it becomes a rounded square instead of a plain disc — the
        // universal "tap to stop" shape change every phone camera uses.
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(circleRect);

        const qreal side = circleRect.width() * 0.46;
        QRectF squareRect(0, 0, side, side);
        squareRect.moveCenter(circleRect.center());
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawRoundedRect(squareRect, side * 0.22, side * 0.22);
    } else {
        p.setBrush(bg);
        p.drawEllipse(circleRect);
    }
}

// ============================================================================
// CaptureControlWidget
// ============================================================================
CaptureControlWidget::CaptureControlWidget(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(owdp(8));
    outer->setAlignment(Qt::AlignHCenter);

    m_modeSwitch = new CaptureModeSwitch(this);
    connect(m_modeSwitch, &CaptureModeSwitch::toggled, this, [this](bool videoMode) {
        m_videoMode = videoMode;
    });
    outer->addWidget(m_modeSwitch, 0, Qt::AlignHCenter);

    m_shutterBtn = new ShutterButton(this);
    connect(m_shutterBtn, &ShutterButton::clicked, this, [this]() {
        emit shutterPressed(m_videoMode);
    });
    outer->addWidget(m_shutterBtn, 0, Qt::AlignHCenter);

    // No external layout manages this widget (it's a floating overlay
    // positioned via move()), so size it to its own layout's hint now —
    // otherwise it would sit at the default 100x30 QWidget size until
    // something else happened to trigger a relayout.
    adjustSize();
}

void CaptureControlWidget::setRecording(bool recording)
{
    m_recording = recording;
    m_modeSwitch->setEnabled(!m_recording); // can't change mode mid-recording
    if (m_shutterBtn) m_shutterBtn->setRecording(m_recording);
}
