#ifndef HM30LINKCLIENT_H
#define HM30LINKCLIENT_H

#include <QByteArray>
#include <QHostAddress>
#include <QObject>
#include <QUdpSocket>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

/**
 * @brief Reads link status from the SIYI HM30 ground unit (not the drone).
 *
 * Uses SIYI's data-link SDK protocol (HM30 manual, "Telemetry SDK communication
 * protocol"): the ground unit answers framed requests on UDP 192.168.144.12:19856
 * when its data-link mode is set to UDP (needs ground firmware >= 0.2.6).
 *   0x44  video link: signal %, RSSI dBm, frequency, channel, up/down bandwidth
 *   0x16  system settings: ground unit battery voltage (rc_bat, volts * 10)
 * The same port also carries MAVLink bytes; anything that is not a valid
 * SDK frame (bad header or CRC) is skipped.
 *
 * Only reachable when the phone/PC is on the HM30 LAN (192.168.144.x); over the
 * drone's Wi-Fi there is no reply and unreachable() fires after 5 s.
 */
class Hm30LinkClient : public QObject
{
    Q_OBJECT

public:
    explicit Hm30LinkClient(QObject *parent = nullptr);

    void start(const QString &host = QStringLiteral("192.168.144.12"));
    void stop();

    // Public for tests.
    static quint16 crc16(const QByteArray &data);
    static QByteArray buildRequest(quint8 cmd, quint16 seq = 0);
    void parse(const QByteArray &datagram);

signals:
    void linkInfo(int signalPercent, int rssiDbm, int freqMHz, int channel, double txMbps, double rxMbps);
    void batteryVolts(double volts);
    void unreachable();

private slots:
    void poll();
    void onReadyRead();
    void onWatchdog();

private:
    static constexpr quint16 kPort = 19856;

    QUdpSocket m_socket;
    QHostAddress m_host;
    QTimer *m_pollTimer = nullptr;
    QTimer *m_watchdog = nullptr;
    quint16 m_seq = 0;
    qint64 m_lastValidMs = 0;
    bool m_unreachableSent = false;
};

#endif // HM30LINKCLIENT_H
