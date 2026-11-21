#ifndef DAEMONCLIENT_H
#define DAEMONCLIENT_H

#include <QHash>
#include <QObject>
#include <QString>

class QIODevice;
class QLocalSocket;
class QTcpSocket;
class QTimer;

/*
 * devmond 客户端，文本行协议见 daemon/protocol.md。
 *
 * 连接方式：
 *   默认            QLocalSocket（Unix 域套接字）/tmp/devmond.sock，或环境变量 DEVMOND_SOCKET
 *   DEVMOND_ADDR    host:port 时改用 QTcpSocket 远程连接（devmond -t 9100），同一界面可在 PC 上运行
 * 两种套接字都是 QIODevice，按行读取和解析共用。
 *
 * 每行解析为 类型 + 字段表：
 *   "AP ir=3 als=151 lux=54.36 ps=70 flags=7"  -> type "AP"，fields {ir:3, ...}
 *   "ERR CONFIG msg=..."                        -> type "ERR"，fields {cmd:CONFIG, msg:...}
 * msg= 总在行尾，值可以含空格。断开后每秒重连，守护进程重启后界面自动恢复。
 */
class DaemonClient : public QObject
{
	Q_OBJECT

public:
	explicit DaemonClient(QObject *parent = nullptr);

	void start();
	bool isConnected() const;
	void send(const QString &command);
	QString address() const { return addressText; }

signals:
	void connectionChanged(bool connected, const QString &reason);
	void message(const QString &type, const QHash<QString, QString> &fields);

private slots:
	void onConnected();
	void onDisconnected();
	void onError();
	void onReadyRead();
	void reconnect();

private:
	void onUnconnected();

	QLocalSocket *local;
	QTcpSocket *tcp;
	QIODevice *io;
	QTimer *retryTimer;
	QString path;		/* 本地套接字路径 */
	QString host;		/* TCP 模式 */
	quint16 port;
	QString addressText;
	QString lastReason;
};

#endif /* DAEMONCLIENT_H */
