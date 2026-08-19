#include "RtspVideoClient.h"
#include <QDebug>
#include <QUrl>
#include <QThread>
#include <QTimer>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

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
                        "! rtph265depay ! h265parse ! decodebin "
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

    m_stopRequested = false;
    m_pullThread = QThread::create([this]() { pullLoop(); });
    m_pullThread->start();

    m_busPollTimer->start();

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

void RtspVideoClient::pullLoop()
{
    GstAppSink *sink = GST_APP_SINK(m_appsink);
    if (!sink) {
        return;
    }

    // Blocks until a decoded sample is ready, or returns null once the
    // pipeline is flushed/torn down (teardownPipeline() sets state to
    // NULL, which unblocks this immediately instead of hanging forever) —
    // no polling, no manual stop-flag check needed on the happy path.
    while (!m_stopRequested.load()) {
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
                // Deep copy before unmapping — the GstBuffer's memory is
                // only valid while mapped. Qt auto-marshals this signal to
                // the receiving slot's (GUI) thread since we're emitting
                // from m_pullThread.
                emit frameReady(frame.copy());
                gst_buffer_unmap(buffer, &map);
            }
        }

        gst_sample_unref(sample);
    }
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
    m_stopRequested = true;

    if (m_pipeline) {
        // Flushes the appsink, which unblocks a pending
        // gst_app_sink_pull_sample() in the pull thread so pullLoop() can
        // exit on its own instead of the thread hanging forever.
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
    }

    if (m_pullThread) {
        m_pullThread->wait();
        delete m_pullThread;
        m_pullThread = nullptr;
    }

    if (m_bus) {
        gst_object_unref(m_bus);
        m_bus = nullptr;
    }
    if (m_appsink) {
        gst_object_unref(m_appsink);
        m_appsink = nullptr;
    }
    if (m_pipeline) {
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
}
