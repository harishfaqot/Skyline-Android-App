#ifndef RTSPVIDEOCLIENT_H
#define RTSPVIDEOCLIENT_H

#include <QObject>
#include <QImage>
#include <QMediaPlayer>
#include <QVideoSink>
#include <QVideoFrame>

/**
 * @brief Live video client — RTSP/H.265, replaces the old per-frame
 * JSON+base64+JPEG-over-WebSocket image feed (formerly RosImageClient).
 *
 * The drone side now runs a real encoder (Jetson hardware H.265 via
 * GStreamer, see skyline_ws/src/skyline/src/video_rtsp_server.py) and
 * serves it as an RTSP stream on port 8554. This class points Qt
 * Multimedia's QMediaPlayer at that stream and republishes decoded
 * frames as QImage through the same imageReceived() signal the old
 * RosImageClient used, so ImageViewerWidget/MainWindow need no changes
 * beyond the type name.
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

    void connectToServer(const QString &url);
    void disconnect();
    bool isConnected() const;

signals:
    void imageReceived(const QImage &image);

    void connected();
    void disconnected();
    void error(const QString &errorString);

private slots:
    void onVideoFrameChanged(const QVideoFrame &frame);
    void onMediaStatusChanged(QMediaPlayer::MediaStatus status);
    void onPlayerError(QMediaPlayer::Error err, const QString &errorString);

private:
    static const quint16 kRtspPort = 8554;
    static constexpr const char *kRtspPath = "/stream";

    QMediaPlayer m_player;
    QVideoSink m_videoSink;
    bool m_wasConnected = false;
};

#endif // RTSPVIDEOCLIENT_H
