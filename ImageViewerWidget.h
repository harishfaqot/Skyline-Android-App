#ifndef IMAGEVIEWERWIDGET_H
#define IMAGEVIEWERWIDGET_H

#include <QWidget>
#include <QPixmap>
#include <QImage>
#include <QLabel>
#include <QVBoxLayout>
#include <QPainter>

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
