#ifndef RTSPVIDEOCLIENT_H
#define RTSPVIDEOCLIENT_H

#include <QObject>
#include <QImage>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <QMutex>

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
 * Pipeline: rtspsrc (tunable latency/drop-on-latency, UDP preferred with
 * TCP-interleaved fallback — matches the server, which is UDP-only, so this
 * only kicks in against dev servers/NATs that never deliver UDP) ->
 * rtph265depay -> h265parse -> decodebin
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

    // Called from the GStreamer streaming thread via the buffer probe
    // installed in connectToServer() (by a plain C-style callback, which
    // can't reach a private member) — must stay lock-free/thread-safe. Not
    // meant to be called from application code.
    void addReceivedBytes(qint64 bytes);

    // Called from the GStreamer streaming thread (depay src-pad probe): the
    // sender-side capture time of the frame with this PTS, as Unix seconds on
    // the Jetson's clock (from RTCP sender reports). Thread-safe.
    void noteCaptureTime(quint64 pts, double captureUnixSec);

public slots:
    // Feed one telemetry packet: the Jetson's wall clock ("t" in the packet)
    // and this device's wall clock when it arrived. The smallest recent
    // (phone - jetson) difference estimates the clock offset (+ min one-way
    // delay), so measured video latency is a slight UNDERestimate by that delay.
    void addServerTimeSample(double jetsonUnixSec, double phoneUnixSec);

signals:
    void connected();
    void disconnected();
    void error(const QString &errorString);

    // Emitted from the appsink pull thread — Qt auto-marshals this to the
    // receiving slot's thread (normally the GUI thread), so connect it
    // directly to ImageViewerWidget::setImage() with no extra plumbing.
    void frameReady(const QImage &image);

    // Measured over the previous ~1s window; kbps (kilobits/sec). Sampled
    // from the RTP payload bytes arriving at the depayloader (see the
    // buffer probe set up in connectToServer()), so it's actual received
    // network bytes, not decoded-frame size.
    void downlinkBitrateChanged(double kbps);

    // Mean capture-to-decoded-frame latency over the last ~1 s, in ms (Jetson
    // camera frame timestamp -> frame ready on this device, before painting).
    // -1 = unknown (no RTCP time yet, no telemetry clock sample, or no frames).
    void videoLatencyMeasured(double ms);

private slots:
    void pollBus();
    void onStatsTimer();

private:
    void teardownPipeline();
    void reportLatency(quint64 pts);

    // The actual pipeline build+start, split out of connectToServer() so a
    // reconnect can defer it until any in-flight cleanup thread (see
    // teardownPipeline()) has actually released the old decoder — see
    // connectToServer()'s comment for the freeze this avoids.
    void startPipeline(const QString &rtspUrl);

    // Runs on m_pullThread until told to stop. Takes its appsink handle and
    // stop/alive flag as parameters (captured by value when the thread is
    // launched) rather than reading m_appsink/m_alive through `this` on
    // every iteration — see teardownPipeline()'s comment for why: once a
    // pipeline is handed off for async cleanup, `this` may be destroyed
    // while this loop is still blocked in native code, and a member access
    // through a dangling `this` would be undefined behavior. The passed-in
    // shared_ptr keeps the flag itself alive independently of `this`.
    void pullLoop(GstElement *appsink, std::shared_ptr<std::atomic<bool>> alive);

    static const quint16 kRtspPort = 8554;
    static constexpr const char *kRtspPath = "/stream";

    // Milliseconds of jitterbuffer slack given to rtspsrc. Trades latency
    // for resilience to network jitter/reordering — VLC needed ~2000ms to
    // fully hide the black-screen issue, QGC gets away with far less
    // because it's also using drop-on-latency to shed stale frames rather
    // than stalling. Start here and tune against the real link.
    static const int kRtspLatencyMs = 150;

    // Video latency measurement (see noteCaptureTime / addServerTimeSample).
    QMutex m_latMutex;
    std::map<quint64, double> m_captureByPts;  // pts -> capture Unix seconds (bounded)
    std::deque<double> m_clockDeltas;          // recent (phone - jetson) seconds
    double m_clockDelta = std::nan("");        // min of m_clockDeltas
    double m_latSumMs = 0.0;
    int m_latCount = 0;

    GstElement *m_pipeline = nullptr;
    GstElement *m_appsink = nullptr;
    GstBus *m_bus = nullptr;

    QThread *m_pullThread = nullptr;

    // The disposable cleanup thread teardownPipeline() hands the old
    // pipeline off to, kept around (only) so a subsequent connectToServer()
    // can tell whether it's still releasing the old hardware decoder.
    QThread *m_cleanupThread = nullptr;
    QMetaObject::Connection m_pendingConnect;

    // One fresh flag per connection (see connectToServer()), shared with
    // pullLoop's thread-launch capture — doubles as both "stop the pull
    // loop" (teardownPipeline flips it false) and "is it still safe to
    // touch `this`" (see pullLoop's doc comment above and
    // teardownPipeline()'s comment below).
    std::shared_ptr<std::atomic<bool>> m_alive;

    QTimer *m_busPollTimer = nullptr;
    QTimer *m_statsTimer = nullptr;
    std::atomic<qint64> m_bytesReceived{0};
    bool m_wasConnected = false;
};

#endif // RTSPVIDEOCLIENT_H
