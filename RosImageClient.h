#ifndef ROSIMAGECLIENT_H
#define ROSIMAGECLIENT_H

#include <QObject>
#include <QWebSocket>
#include <QImage>
#include <QJsonObject>
#include <QJsonDocument>

/**
 * @brief ROS Image Client using rosbridge_server protocol
 *
 * Connects to rosbridge WebSocket (default port 9090) and subscribes to:
 *   - /image_raw   (sensor_msgs/Image)   → imageReceived
 *
 * rosbridge encodes the "data" field of sensor_msgs/Image as base64.
 * The image is reconstructed from the raw pixel buffer using the
 * encoding, width, height, and step fields in the message.
 *
 * Common encodings supported:
 *   "rgb8"  – 3 bytes/pixel, R G B
 *   "bgr8"  – 3 bytes/pixel, B G R
 *   "mono8" – 1 byte/pixel, greyscale
 *   "rgba8" – 4 bytes/pixel
 */

class RosImageClient : public QObject
{
    Q_OBJECT

public:
    explicit RosImageClient(QObject *parent = nullptr);
    ~RosImageClient();

    void connectToServer(const QString &url);
    void disconnect();
    bool isConnected() const;

signals:
    void imageReceived(const QImage &image);

    void connected();
    void disconnected();
    void error(const QString &errorString);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString &message);
    void onError(QAbstractSocket::SocketError error);

private:
    void sendSubscribe(const QString &topic, const QString &type);
    void processImage(const QJsonObject &msg);

    QWebSocket m_webSocket;
};

#endif // ROSIMAGECLIENT_H
