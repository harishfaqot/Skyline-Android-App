#ifndef RTSPVIDEOCLIENT_H
#define RTSPVIDEOCLIENT_H

#include <QObject>
#include <QMediaPlayer>

QT_BEGIN_NAMESPACE
class QVideoWidget;
QT_END_NAMESPACE

/**
 * @brief Live video client — RTSP/H.265, replaces the old per-frame
 * JSON+base64+JPEG-over-WebSocket image feed (formerly RosImageClient).
 *
 * The drone side now runs a real encoder (Jetson hardware H.265 via
 * GStreamer, see skyline_ws/src/skyline/src/video_rtsp_server.py) and
 * serves it as an RTSP stream on port 8554. This class points Qt
 * Multimedia's QMediaPlayer at that stream.
 *
 * Rendering goes straight through QMediaPlayer::setVideoOutput() to a
 * QVideoWidget (see attachVideoOutput()/ImageViewerWidget::videoOutput()),
 * NOT through manually pulling QVideoFrames and converting to QImage —
 * hardware-decoded frames on Android come back as GPU-only surfaces that
 * QVideoFrame::toImage() cannot read back to the CPU, so that approach
 * silently produced nothing. Letting QVideoWidget render them via Qt's
 * own RHI path handles GPU-surface frames correctly.
 *
 * connectToServer() takes the same "ws://<drone-ip>:9090" URL used for
 * the telemetry/pointcloud link — only the host is reused, the port is
 * swapped to the RTSP server's 8554 (same pattern UpdateClient/9091
 * already uses for the firmware-update link).
 */
class RtspVideoClient : public QObject
{
    Q_OBJECT

public:
    explicit RtspVideoClient(QObject *parent = nullptr);
    ~RtspVideoClient();

    // Must be called once before/after connecting (either order is fine,
    // QMediaPlayer tolerates both) — typically right after the target
    // ImageViewerWidget is constructed. See MainWindow.cpp.
    void attachVideoOutput(QVideoWidget *widget);

    void connectToServer(const QString &url);
    void disconnect();
    bool isConnected() const;

signals:
    void connected();
    void disconnected();
    void error(const QString &errorString);

private slots:
    void onMediaStatusChanged(QMediaPlayer::MediaStatus status);
    void onPlayerError(QMediaPlayer::Error err, const QString &errorString);

private:
    static const quint16 kRtspPort = 8554;
    static constexpr const char *kRtspPath = "/stream";

    QMediaPlayer m_player;
    bool m_wasConnected = false;
};

#endif // RTSPVIDEOCLIENT_H
