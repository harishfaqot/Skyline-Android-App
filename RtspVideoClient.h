#ifndef RTSPVIDEOCLIENT_H
#define RTSPVIDEOCLIENT_H

#include <QObject>
#include <QImage>
#include <atomic>

QT_BEGIN_NAMESPACE
class QThread;
class QTimer;
QT_END_NAMESPACE

// Opaque forward declarations matching gst/gstelement.h / gst/gstbus.h —
// only pointers to these are stored here, so callers of this header don't
// need GStreamer's headers or include paths, only RtspVideoClient.cpp does.
typedef struct _GstElement GstElement;
typedef struct _GstBus GstBus;

/**
 * @brief Live video client — RTSP/H.265, receives via a raw GStreamer
 * pipeline instead of QMediaPlayer.
 *
 * QMediaPlayer's RTSP/RTP handling exposes no jitterbuffer control, so on
 * a real, jittery RF link it either blanked to black on any late/reordered
 * packet (short GOP) or stayed black for most of a GOP once one did (long
 * GOP) — VLC and QGroundControl don't have this problem because they both
 * run their own tunable jitterbuffer (`rtspsrc latency=...` under the
 * hood). This class does the same thing QGC and the drone-side encoder
 * (see skyline_ws/src/skyline/src/video_rtsp_server.py) already do: talk
 * to GStreamer directly.
 *
 * Pipeline: rtspsrc (tunable latency/drop-on-latency, UDP only — matches
 * the server, which is UDP-only) -> rtph265depay -> h265parse -> decodebin
 * (auto-picks a hardware decoder where the platform has one, falls back to
 * software otherwise — same code path on desktop and Android) ->
 * videoconvert -> appsink (RGBA).
 *
 * Decoded frames are pulled off the appsink on a dedicated thread (blocking
 * gst_app_sink_pull_sample loop) and delivered as QImage via frameReady() —
 * connect that straight to ImageViewerWidget::setImage(), the same sink the
 * old JPEG-over-WebSocket feed used. This avoids needing a native
 * video-overlay window handle (simpler, identical code path on every
 * platform) at the cost of one CPU-side frame copy per frame, which is
 * negligible next to the multi-second stalls this replaces.
 *
 * Requires GStreamer 1.0 (core + app + video + the RTP/RTSP + H.265 decode
 * plugins) linked in — see CMakeLists.txt for how that's wired per
 * platform. On Android this additionally requires libgstreamer_android.so's
 * JNI_OnLoad to have been called once at startup (see main.cpp) — see the
 * setup notes in CMakeLists.txt's GStreamer section for how that .so gets
 * built in the first place.
 *
 * connectToServer() takes the same "ws://<drone-ip>:9090" URL used for the
 * telemetry/pointcloud link — only the host is reused, the port is swapped
 * to the RTSP server's 8554 (same pattern UpdateClient/9091 already uses
 * for the firmware-update link).
 */
class RtspVideoClient : public QObject
{
    Q_OBJECT

public:
    explicit RtspVideoClient(QObject *parent = nullptr);
    ~RtspVideoClient() override;

    void connectToServer(const QString &url);
    void disconnect();
    bool isConnected() const;

signals:
    void connected();
    void disconnected();
    void error(const QString &errorString);

    // Emitted from the appsink pull thread — Qt auto-marshals this to the
    // receiving slot's thread (normally the GUI thread), so connect it
    // directly to ImageViewerWidget::setImage() with no extra plumbing.
    void frameReady(const QImage &image);

private slots:
    void pollBus();

private:
    void teardownPipeline();
    void pullLoop(); // runs on m_pullThread until the pipeline is torn down

    static const quint16 kRtspPort = 8554;
    static constexpr const char *kRtspPath = "/stream";

    // Milliseconds of jitterbuffer slack given to rtspsrc. Trades latency
    // for resilience to network jitter/reordering — VLC needed ~2000ms to
    // fully hide the black-screen issue, QGC gets away with far less
    // because it's also using drop-on-latency to shed stale frames rather
    // than stalling. Start here and tune against the real link.
    static const int kRtspLatencyMs = 150;

    GstElement *m_pipeline = nullptr;
    GstElement *m_appsink = nullptr;
    GstBus *m_bus = nullptr;

    QThread *m_pullThread = nullptr;
    std::atomic<bool> m_stopRequested{false};

    QTimer *m_busPollTimer = nullptr;
    bool m_wasConnected = false;
};

#endif // RTSPVIDEOCLIENT_H
