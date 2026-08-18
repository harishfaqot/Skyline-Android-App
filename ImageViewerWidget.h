#ifndef IMAGEVIEWERWIDGET_H
#define IMAGEVIEWERWIDGET_H

#include <QWidget>
#include <QPixmap>
#include <QImage>
#include <QLabel>
#include <QVBoxLayout>
#include <QPainter>

QT_BEGIN_NAMESPACE
class QVideoWidget;
QT_END_NAMESPACE

/**
 * @brief Widget for displaying drone camera feed
 *
 * Displays images received from the drone camera via WebSocket.
 * Supports zoom, pan, and maintains aspect ratio.
 */
class ImageViewerWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ImageViewerWidget(QWidget *parent = nullptr);
    ~ImageViewerWidget();

    // Image control
    void setImage(const QImage &image);
    void clearImage();
    QImage getCurrentImage() const { return m_currentImage; }

    // Live H.265/RTSP video (RtspVideoClient) renders through Qt's own GPU
    // path (QVideoWidget), not through setImage() above — hardware-decoded
    // frames on Android come back as GPU-only surfaces that QVideoFrame::
    // toImage() cannot convert to a CPU-side QImage, so pulling frames out
    // manually the way the old JPEG feed did silently produces nothing.
    // videoOutput() hands the render target to RtspVideoClient once at
    // startup; setVideoActive() shows/hides it over the existing
    // paintEvent-drawn placeholder without touching any of the pan/zoom
    // logic below (video has no use for that — QVideoWidget letterboxes
    // itself).
    QVideoWidget *videoOutput() const { return m_videoWidget; }
    void setVideoActive(bool active);

    // Display settings
    void setFitToWindow(bool fit);
    void setZoomLevel(double zoom);
    double getZoomLevel() const { return m_zoomLevel; }

public slots:
    void zoomIn();
    void zoomOut();
    void resetZoom();
    void fitToWindow();

signals:
    void imageChanged();
    void zoomChanged(double zoom);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void updateDisplayPixmap();
    QSize calculateScaledSize() const;
    
    QImage m_currentImage;
    QPixmap m_displayPixmap;
    QVideoWidget *m_videoWidget;

    // Display settings
    bool m_fitToWindow;
    double m_zoomLevel;
    
    // Pan support
    bool m_isPanning;
    QPoint m_panStartPos;
    QPoint m_panOffset;
    
    // Status overlay
    QString m_statusText;
    bool m_showStatus;
    
    // Constants
    static constexpr double MIN_ZOOM = 0.1;
    static constexpr double MAX_ZOOM = 10.0;
    static constexpr double ZOOM_STEP = 1.2;
};

#endif // IMAGEVIEWERWIDGET_H
