#include "RtspVideoClient.h"
#include <QDebug>
#include <QUrl>
#include <QThread>
#include <QTimer>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

// Sums bytes of every buffer crossing rtph265depay's sink pad — i.e. the RTP
// payload bytes actually received off the network for this stream, before
// depayloading/decoding. GStreamer probe callbacks run on the pipeline's own
// streaming thread, not the GUI thread, so this must stay lock-free; the
// RtspVideoClient side accumulates into a std::atomic (see addReceivedBytes).
static GstPadProbeReturn rtspBufferProbeCallback(GstPad *pad, GstPadProbeInfo *info, gpointer userData)
{
    Q_UNUSED(pad);
    auto *self = static_cast<RtspVideoClient *>(userData);
    if (GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info)) {
        self->addReceivedBytes(static_cast<qint64>(gst_buffer_get_size(buffer)));
    }
    return GST_PAD_PROBE_OK;
}

RtspVideoClient::RtspVideoClient(QObject *parent)
    : QObject(parent)
{
    // gst_init() is cheap to call more than once per-process (it no-ops
    // after the first call) but isn't thread-safe against concurrent first
    // calls — fine here since this constructor only ever runs on the GUI
    // thread. On Android, gst_android_init() (see main.cpp) must run
    // before this, or GStreamer's plugin registry never finds the
    // statically-linked plugins bundled into libgstreamer_android.so.
    static bool s_gstInited = false;
    if (!s_gstInited) {
        gst_init(nullptr, nullptr);
        s_gstInited = true;
    }

    m_busPollTimer = new QTimer(this);
    m_busPollTimer->setInterval(200);
    connect(m_busPollTimer, &QTimer::timeout, this, &RtspVideoClient::pollBus);

    m_statsTimer = new QTimer(this);
    m_statsTimer->setInterval(1000);
    connect(m_statsTimer, &QTimer::timeout, this, &RtspVideoClient::onStatsTimer);
}

RtspVideoClient::~RtspVideoClient()
{
    teardownPipeline();
}

void RtspVideoClient::connectToServer(const QString &url)
{
    QString host = QUrl(url).host();
    if (host.isEmpty()) {
        qWarning() << "RtspVideoClient: no host in URL" << url;
        return;
    }

    // Reconnect: drop whatever pipeline is already running first, quietly
    // (no disconnected() signal — that's for an established stream that
    // drops, not for switching targets).
    teardownPipeline();

    const QString rtspUrl = QStringLiteral("rtsp://%1:%2%3")
                                 .arg(host)
                                 .arg(kRtspPort)
                                 .arg(QString::fromLatin1(kRtspPath));

    QObject::disconnect(m_pendingConnect);
    if (m_cleanupThread && m_cleanupThread->isRunning()) {
        // The previous pipeline's decoder may still be mid-release (see
        // teardownPipeline()'s comment) — starting a new hardware decoder
        // before it's done can block gst_element_set_state() on the GUI
        // thread for as long as the old one takes to let go, which is what
        // used to freeze the whole app on reconnect. Defer instead of
        // racing it.
        m_pendingConnect = connect(m_cleanupThread, &QThread::finished, this, [this, rtspUrl]() {
            startPipeline(rtspUrl);
        });
        qDebug() << "RtspVideoClient: deferring reconnect until previous pipeline finishes cleanup";
        return;
    }

    startPipeline(rtspUrl);
}

void RtspVideoClient::startPipeline(const QString &rtspUrl)
{
    // Prefer UDP to match the production server (see video_rtsp_server.py —
    // TCP-interleaved would reintroduce head-of-line blocking there), but
    // allow rtspsrc to fall back to TCP-interleaved if UDP RTP never shows
    // up. video_rtsp_server.py's server is UDP-only so this is a no-op
    // against the real drone link; it matters against dev servers (mediamtx)
    // reached from behind NAT that never lets unsolicited inbound UDP
    // through — notably the Android emulator's SLIRP virtual network, which
    // drops RTP data because rtspsrc never sends anything out on the RTP
    // receive port first. latency + drop-on-latency give rtspsrc's internal
    // jitterbuffer room to absorb reordered/late RTP packets instead of the
    // pipeline stalling or corrupting a frame the instant one arrives late —
    // this is the actual fix for the black-screen issue QMediaPlayer had,
    // which exposed no equivalent control. decodebin auto-selects a hardware
    // decoder if the platform has a GStreamer plugin for one (e.g. Android's
    // amcviddec-* via MediaCodec), falling back to software (avdec_h265)
    // otherwise — same pipeline description works unmodified on every
    // platform.
    const QString pipelineDesc =
        QStringLiteral("rtspsrc location=%1 latency=%2 drop-on-latency=true protocols=tcp+udp "
                        "! rtph265depay name=depay ! h265parse ! decodebin "
                        "! videoconvert ! video/x-raw,format=RGBA "
                        "! appsink name=sink emit-signals=false sync=false max-buffers=2 drop=true")
            .arg(rtspUrl)
            .arg(kRtspLatencyMs);

    GError *err = nullptr;
    m_pipeline = gst_parse_launch(pipelineDesc.toUtf8().constData(), &err);
    if (!m_pipeline || err) {
        QString errStr = err ? QString::fromUtf8(err->message)
                              : QStringLiteral("gst_parse_launch failed");
        qWarning() << "RtspVideoClient: failed to build pipeline:" << errStr;
        g_clear_error(&err);
        if (m_pipeline) {
            gst_object_unref(m_pipeline);
            m_pipeline = nullptr;
        }
        emit error(errStr);
        return;
    }

    m_appsink = gst_bin_get_by_name(GST_BIN(m_pipeline), "sink");
    m_bus = gst_pipeline_get_bus(GST_PIPELINE(m_pipeline));

    // Downlink bandwidth: probe rtph265depay's sink pad (a static/always
    // pad, unlike rtspsrc's dynamically-created src pad, so it's safe to
    // grab right away rather than waiting on a pad-added signal). The probe
    // and its callback outlive this local element handle — they're attached
    // to the pad itself, which the pipeline keeps alive — so unref'ing
    // `depay` here doesn't tear the probe down.
    if (GstElement *depay = gst_bin_get_by_name(GST_BIN(m_pipeline), "depay")) {
        if (GstPad *depaySink = gst_element_get_static_pad(depay, "sink")) {
            gst_pad_add_probe(depaySink, GST_PAD_PROBE_TYPE_BUFFER, rtspBufferProbeCallback, this, nullptr);
            gst_object_unref(depaySink);
        }
        gst_object_unref(depay);
    }
    m_bytesReceived = 0;

    m_alive = std::make_shared<std::atomic<bool>>(true);
    GstElement *appsinkForThread = m_appsink;
    std::shared_ptr<std::atomic<bool>> aliveForThread = m_alive;
    m_pullThread = QThread::create([this, appsinkForThread, aliveForThread]() {
        pullLoop(appsinkForThread, aliveForThread);
    });
    m_pullThread->start();

    m_busPollTimer->start();
    m_statsTimer->start();

    gst_element_set_state(m_pipeline, GST_STATE_PLAYING);

    qDebug() << "RtspVideoClient: connecting to" << rtspUrl;
}

void RtspVideoClient::disconnect()
{
    const bool notify = m_wasConnected;
    teardownPipeline();
    m_wasConnected = false;
    if (notify) {
        emit disconnected();
    }
}

bool RtspVideoClient::isConnected() const
{
    return m_wasConnected;
}

void RtspVideoClient::pullLoop(GstElement *appsink, std::shared_ptr<std::atomic<bool>> alive)
{
    GstAppSink *sink = GST_APP_SINK(appsink);
    if (!sink) {
        return;
    }

    // Blocks until a decoded sample is ready, or returns null once the
    // pipeline is flushed/torn down (teardownPipeline() sets state to
    // NULL, which is *supposed* to unblock this immediately — but see
    // teardownPipeline()'s comment for why that's no longer assumed to be
    // guaranteed, hence the `alive` flag instead of reading a member
    // through `this` here).
    while (alive->load(std::memory_order_acquire)) {
        GstSample *sample = gst_app_sink_pull_sample(sink);
        if (!sample) {
            break;
        }

        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstCaps *caps = gst_sample_get_caps(sample);
        GstVideoInfo vinfo;
        if (buffer && caps && gst_video_info_from_caps(&vinfo, caps)) {
            GstMapInfo map;
            if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                QImage frame(map.data, vinfo.width, vinfo.height,
                             vinfo.stride[0], QImage::Format_RGBA8888);
                // Re-check right before touching `this` via emit: if
                // teardownPipeline() abandoned this thread (see its
                // comment), `this` may be mid-destruction or already gone.
                // Deep copy before unmapping — the GstBuffer's memory is
                // only valid while mapped. Qt auto-marshals this signal to
                // the receiving slot's (GUI) thread since we're emitting
                // from m_pullThread.
                if (alive->load(std::memory_order_acquire)) {
                    emit frameReady(frame.copy());
                }
                gst_buffer_unmap(buffer, &map);
            }
        }

        gst_sample_unref(sample);
    }
}

void RtspVideoClient::addReceivedBytes(qint64 bytes)
{
    m_bytesReceived.fetch_add(bytes, std::memory_order_relaxed);
}

void RtspVideoClient::onStatsTimer()
{
    const qint64 bytes = m_bytesReceived.exchange(0, std::memory_order_relaxed);
    emit downlinkBitrateChanged((bytes * 8) / 1000.0); // kbps, ~1s window
}

void RtspVideoClient::pollBus()
{
    if (!m_bus) {
        return;
    }

    while (true) {
        GstMessage *msg = gst_bus_timed_pop_filtered(
            m_bus, 0,
            static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS
                                         | GST_MESSAGE_STATE_CHANGED));
        if (!msg) {
            break;
        }

        bool tornDown = false;
        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(msg, &err, &debug);
            QString errStr = err ? QString::fromUtf8(err->message)
                                  : QStringLiteral("unknown GStreamer error");
            qWarning() << "RtspVideoClient: pipeline error:" << errStr
                       << (debug ? debug : "");
            g_clear_error(&err);
            g_free(debug);
            emit error(errStr);
            disconnect();
            tornDown = true;
            break;
        }
        case GST_MESSAGE_EOS:
            qWarning() << "RtspVideoClient: end of stream";
            disconnect();
            tornDown = true;
            break;
        case GST_MESSAGE_STATE_CHANGED:
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(m_pipeline)) {
                GstState oldState, newState, pending;
                gst_message_parse_state_changed(msg, &oldState, &newState, &pending);
                if (newState == GST_STATE_PLAYING && !m_wasConnected) {
                    m_wasConnected = true;
                    emit connected();
                }
            }
            break;
        default:
            break;
        }

        gst_message_unref(msg);
        if (tornDown) {
            // m_bus was just unreffed/nulled by teardownPipeline() (called
            // via disconnect() above) — stop polling it.
            break;
        }
    }
}

void RtspVideoClient::teardownPipeline()
{
    m_busPollTimer->stop();
    m_statsTimer->stop();
    m_bytesReceived = 0;
    emit downlinkBitrateChanged(0.0);

    if (m_alive) {
        m_alive->store(false, std::memory_order_release); // stop pullLoop, forbid it from touching `this` again
    }

    if (!m_pipeline && !m_pullThread) {
        return; // nothing was running
    }

    // The actual teardown — gst_element_set_state(..., GST_STATE_NULL) is a
    // *synchronous* GStreamer call, and joining the pull thread waits on
    // whatever that call was meant to unblock. Both are normally fast, but
    // on Android, turning the screen off can revoke the hardware decoder's
    // MediaCodec Surface out from under a running pipeline; when that
    // happens the decoder element's state-change can wedge forever instead
    // of completing. This function used to run on the GUI thread and block
    // on exactly those two calls with no timeout — a wedged decoder froze
    // the *entire app* (every button, every tab) until force-killed, not
    // just the video tile. That was the actual bug behind "screen off then
    // on -> app hangs."
    //
    // Fix: detach the old pipeline/thread from this object immediately and
    // hand the blocking teardown to a disposable one-shot thread instead.
    // The GUI thread never blocks here, so connectToServer() can build a
    // fresh pipeline right away even if the old one never finishes dying.
    // Worst case a wedged decoder leaks one pipeline + one thread instead
    // of freezing the app — an acceptable trade, especially since the OS
    // reclaims both the moment the process actually exits.
    GstElement *pipeline = m_pipeline;
    GstElement *appsink = m_appsink;
    GstBus *bus = m_bus;
    QThread *pullThread = m_pullThread;
    m_pipeline = nullptr;
    m_appsink = nullptr;
    m_bus = nullptr;
    m_pullThread = nullptr;

    QThread *cleanupThread = QThread::create([pipeline, appsink, bus, pullThread]() {
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
        }
        if (pullThread) {
            pullThread->wait();
            delete pullThread;
        }
        if (bus) {
            gst_object_unref(bus);
        }
        if (appsink) {
            gst_object_unref(appsink);
        }
        if (pipeline) {
            gst_object_unref(pipeline);
        }
    });
    m_cleanupThread = cleanupThread;
    connect(cleanupThread, &QThread::finished, this, [this, cleanupThread]() {
        if (m_cleanupThread == cleanupThread) {
            m_cleanupThread = nullptr;
        }
    });
    QObject::connect(cleanupThread, &QThread::finished, cleanupThread, &QObject::deleteLater);
    cleanupThread->start();
}
