#include "RtspVideoClient.h"
#include <QDebug>
#include <QUrl>

RtspVideoClient::RtspVideoClient(QObject *parent)
    : QObject(parent)
{
    m_player.setVideoSink(&m_videoSink);

    connect(&m_videoSink, &QVideoSink::videoFrameChanged,
            this, &RtspVideoClient::onVideoFrameChanged);
    connect(&m_player, &QMediaPlayer::mediaStatusChanged,
            this, &RtspVideoClient::onMediaStatusChanged);
    connect(&m_player, &QMediaPlayer::errorOccurred,
            this, &RtspVideoClient::onPlayerError);
}

RtspVideoClient::~RtspVideoClient()
{
    disconnect();
}

void RtspVideoClient::connectToServer(const QString &url)
{
    // Reuses the same "ws://<drone-ip>:9090" URL as the telemetry link —
    // only the host matters here, the video pipeline listens on its own
    // RTSP port (see video_rtsp_server.py), same pattern UpdateClient
    // already uses for the firmware-update link on port 9091.
    QString host = QUrl(url).host();
    if (host.isEmpty()) {
        qWarning() << "RtspVideoClient: no host in URL" << url;
        return;
    }

    QUrl rtspUrl;
    rtspUrl.setScheme("rtsp");
    rtspUrl.setHost(host);
    rtspUrl.setPort(kRtspPort);
    rtspUrl.setPath(QString::fromLatin1(kRtspPath));

    qDebug() << "RtspVideoClient: connecting to" << rtspUrl.toString();
    m_player.setSource(rtspUrl);
    m_player.play();
}

void RtspVideoClient::disconnect()
{
    m_player.stop();
    m_player.setSource(QUrl());
    if (m_wasConnected) {
        m_wasConnected = false;
        emit disconnected();
    }
}

bool RtspVideoClient::isConnected() const
{
    return m_wasConnected;
}

void RtspVideoClient::onVideoFrameChanged(const QVideoFrame &frame)
{
    if (!frame.isValid())
        return;
    QImage img = frame.toImage();
    if (!img.isNull())
        emit imageReceived(img);
}

void RtspVideoClient::onMediaStatusChanged(QMediaPlayer::MediaStatus status)
{
    switch (status) {
    case QMediaPlayer::BufferingMedia:
    case QMediaPlayer::BufferedMedia:
        if (!m_wasConnected) {
            m_wasConnected = true;
            emit connected();
        }
        break;
    case QMediaPlayer::NoMedia:
    case QMediaPlayer::InvalidMedia:
    case QMediaPlayer::EndOfMedia:
        if (m_wasConnected) {
            m_wasConnected = false;
            emit disconnected();
        }
        break;
    default:
        break;
    }
}

void RtspVideoClient::onPlayerError(QMediaPlayer::Error err, const QString &errorString)
{
    Q_UNUSED(err);
    qWarning() << "RtspVideoClient: player error:" << errorString;
    emit error(errorString);
    if (m_wasConnected) {
        m_wasConnected = false;
        emit disconnected();
    }
}
