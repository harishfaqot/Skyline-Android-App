#include "ImageViewerWidget.h"
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QDebug>

ImageViewerWidget::ImageViewerWidget(QWidget *parent)
    : QWidget(parent)
    , m_fitToWindow(true)
    , m_zoomLevel(1.0)
    , m_isPanning(false)
    , m_panOffset(0, 0)
{
    // FIX: this used to be setMinimumSize(320, 240). The mini-player's
    // default content area is smaller than that (its outer size is
    // dp(260)x dp(160), and the title bar + margins eat further into that),
    // so the layout could never shrink this widget below 320x240 and it
    // overflowed the mini-player's content container. Since QWidget clips
    // children to their parent's rect, the overflowing part was silently
    // cut off — that's the "mini-player camera view is cropped" bug (full
    // view had enough room so it was never noticeable there). The letterbox
    // logic in updateDisplayPixmap()/paintEvent already keeps aspect ratio
    // at any size, so a large hard floor isn't needed — just enough to
    // avoid a zero/negative-size widget.
    setMinimumSize(64, 48);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setStyleSheet("background-color: #1e1e1e;");
}

ImageViewerWidget::~ImageViewerWidget()
{
}

void ImageViewerWidget::setImage(const QImage &image)
{
    if (image.isNull()) {
        qWarning() << "ImageViewerWidget: Received null image";
        return;
    }
    
    m_currentImage = image;
    updateDisplayPixmap();
    emit imageChanged();
    update();
}

void ImageViewerWidget::clearImage()
{
    m_currentImage = QImage();
    m_displayPixmap = QPixmap();
    update();
}

void ImageViewerWidget::setVideoActive(bool active)
{
    if (!active) {
        clearImage(); // repaints the "No camera feed" placeholder
    }
}

void ImageViewerWidget::setFitToWindow(bool fit)
{
    m_fitToWindow = fit;
    if (m_fitToWindow) {
        m_zoomLevel = 1.0;
        m_panOffset = QPoint(0, 0);
    }
    updateDisplayPixmap();
    update();
}

void ImageViewerWidget::setZoomLevel(double zoom)
{
    m_zoomLevel = qBound(MIN_ZOOM, zoom, MAX_ZOOM);
    m_fitToWindow = false;
    updateDisplayPixmap();
    emit zoomChanged(m_zoomLevel);
    update();
}

void ImageViewerWidget::zoomIn()
{
    setZoomLevel(m_zoomLevel * ZOOM_STEP);
}

void ImageViewerWidget::zoomOut()
{
    setZoomLevel(m_zoomLevel / ZOOM_STEP);
}

void ImageViewerWidget::resetZoom()
{
    setZoomLevel(1.0);
    m_panOffset = QPoint(0, 0);
}

void ImageViewerWidget::fitToWindow()
{
    setFitToWindow(true);
}

void ImageViewerWidget::updateDisplayPixmap()
{
    if (m_currentImage.isNull()) {
        m_displayPixmap = QPixmap();
        return;
    }
    
    QSize targetSize = calculateScaledSize();
    m_displayPixmap = QPixmap::fromImage(
        m_currentImage.scaled(targetSize, 
                             Qt::KeepAspectRatio, 
                             Qt::SmoothTransformation)
    );
}

QSize ImageViewerWidget::calculateScaledSize() const
{
    if (m_currentImage.isNull()) {
        return QSize();
    }
    
    if (m_fitToWindow) {
        return size();
    } else {
        return m_currentImage.size() * m_zoomLevel;
    }
}

void ImageViewerWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    
    // Draw background
    painter.fillRect(rect(), QColor(30, 30, 30));
    
    if (m_displayPixmap.isNull()) {
        // Draw "No Image" message
        painter.setPen(QColor(150, 150, 150));
        painter.setFont(QFont("Arial", 14));
        painter.drawText(rect(), Qt::AlignCenter, "No camera feed");
        return;
    }
    
    // Calculate centered position with pan offset
    QPoint imagePos;
    if (m_fitToWindow) {
        imagePos = QPoint(
            (width() - m_displayPixmap.width()) / 2,
            (height() - m_displayPixmap.height()) / 2
        );
    } else {
        imagePos = QPoint(
            (width() - m_displayPixmap.width()) / 2 + m_panOffset.x(),
            (height() - m_displayPixmap.height()) / 2 + m_panOffset.y()
        );
    }
    
    // Draw the image
    painter.drawPixmap(imagePos, m_displayPixmap);
}

void ImageViewerWidget::resizeEvent(QResizeEvent *event)
{
    Q_UNUSED(event);
    if (m_fitToWindow) {
        updateDisplayPixmap();
    }
}

void ImageViewerWidget::wheelEvent(QWheelEvent *event)
{
    if (m_currentImage.isNull()) {
        return;
    }
    
    // Zoom with mouse wheel
    int delta = event->angleDelta().y();
    if (delta > 0) {
        zoomIn();
    } else if (delta < 0) {
        zoomOut();
    }
    
    event->accept();
}

void ImageViewerWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && !m_fitToWindow) {
        m_isPanning = true;
        m_panStartPos = event->pos();
        setCursor(Qt::ClosedHandCursor);
    }
}

void ImageViewerWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_isPanning) {
        QPoint delta = event->pos() - m_panStartPos;
        m_panOffset += delta;
        m_panStartPos = event->pos();
        update();
    }
}

void ImageViewerWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_isPanning) {
        m_isPanning = false;
        setCursor(Qt::ArrowCursor);
    }
}
