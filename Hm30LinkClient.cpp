#include "Hm30LinkClient.h"

#include <QDateTime>
#include <QTimer>

namespace {
constexpr quint8 kCmdSystem = 0x16;
constexpr quint8 kCmdVideoLink = 0x44;

qint32 le32(const QByteArray &d, int off)
{
    return static_cast<qint32>(static_cast<quint8>(d[off]) | (static_cast<quint8>(d[off + 1]) << 8)
                               | (static_cast<quint8>(d[off + 2]) << 16)
                               | (static_cast<quint32>(static_cast<quint8>(d[off + 3])) << 24));
}
} // namespace

Hm30LinkClient::Hm30LinkClient(QObject *parent)
    : QObject(parent)
{
    connect(&m_socket, &QUdpSocket::readyRead, this, &Hm30LinkClient::onReadyRead);

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(1000);
    connect(m_pollTimer, &QTimer::timeout, this, &Hm30LinkClient::poll);

    m_watchdog = new QTimer(this);
    m_watchdog->setInterval(1000);
    connect(m_watchdog, &QTimer::timeout, this, &Hm30LinkClient::onWatchdog);
}

void Hm30LinkClient::start(const QString &host)
{
    stop();
    m_host = QHostAddress(host);
    m_socket.bind(QHostAddress(QHostAddress::AnyIPv4), quint16(0));
    m_lastValidMs = QDateTime::currentMSecsSinceEpoch();
    m_unreachableSent = false;
    m_pollTimer->start();
    m_watchdog->start();
    poll();
}

void Hm30LinkClient::stop()
{
    m_pollTimer->stop();
    m_watchdog->stop();
    m_socket.close();
}

// CRC-16/XMODEM (poly 0x1021, init 0) - same as the SDK's CRC16_cal().
quint16 Hm30LinkClient::crc16(const QByteArray &data)
{
    quint16 crc = 0;
    for (char c : data) {
        crc ^= static_cast<quint16>(static_cast<quint8>(c)) << 8;
        for (int i = 0; i < 8; ++i) {
            crc = (crc & 0x8000) ? static_cast<quint16>((crc << 1) ^ 0x1021) : static_cast<quint16>(crc << 1);
        }
    }
    return crc;
}

QByteArray Hm30LinkClient::buildRequest(quint8 cmd, quint16 seq)
{
    QByteArray f;
    f.append(char(0x55)).append(char(0x66));
    f.append(char(0x01));                       // CTRL: need_ack
    f.append(char(0)).append(char(0));          // data length (little-endian)
    f.append(char(seq & 0xFF)).append(char(seq >> 8));
    f.append(char(cmd));
    const quint16 crc = crc16(f);
    f.append(char(crc & 0xFF)).append(char(crc >> 8));
    return f;
}

void Hm30LinkClient::poll()
{
    if (m_socket.state() != QAbstractSocket::BoundState) {
        return;
    }
    m_socket.writeDatagram(buildRequest(kCmdVideoLink, m_seq++), m_host, kPort);
    m_socket.writeDatagram(buildRequest(kCmdSystem, m_seq++), m_host, kPort);
}

void Hm30LinkClient::onReadyRead()
{
    while (m_socket.hasPendingDatagrams()) {
        QByteArray d;
        d.resize(static_cast<int>(m_socket.pendingDatagramSize()));
        m_socket.readDatagram(d.data(), d.size());
        parse(d);
    }
}

void Hm30LinkClient::parse(const QByteArray &d)
{
    // Frame: 55 66 | CTRL | len(2,LE) | seq(2) | cmd | data[len] | crc16(2,LE)
    for (int i = 0; i + 10 <= d.size(); ++i) {
        if (static_cast<quint8>(d[i]) != 0x55 || static_cast<quint8>(d[i + 1]) != 0x66) {
            continue;
        }
        const int len = static_cast<quint8>(d[i + 3]) | (static_cast<quint8>(d[i + 4]) << 8);
        const int total = 8 + len + 2;
        if (i + total > d.size()) {
            continue;
        }
        const quint16 crc = static_cast<quint8>(d[i + total - 2]) | (static_cast<quint8>(d[i + total - 1]) << 8);
        if (crc16(d.mid(i, total - 2)) != crc) {
            continue;
        }
        const quint8 cmd = static_cast<quint8>(d[i + 7]);
        const QByteArray data = d.mid(i + 8, len);
        m_lastValidMs = QDateTime::currentMSecsSinceEpoch();
        m_unreachableSent = false;

        if (cmd == kCmdVideoLink && len >= 36) {
            emit linkInfo(le32(data, 0), le32(data, 24), le32(data, 28), le32(data, 32),
                          le32(data, 16) / 1000.0, le32(data, 20) / 1000.0);
        } else if (cmd == kCmdSystem && len >= 4) {
            emit batteryVolts(static_cast<quint8>(data[3]) / 10.0);
        }
        i += total - 1;
    }
}

void Hm30LinkClient::onWatchdog()
{
    if (!m_unreachableSent && QDateTime::currentMSecsSinceEpoch() - m_lastValidMs > 5000) {
        m_unreachableSent = true;
        emit unreachable();
    }
}
