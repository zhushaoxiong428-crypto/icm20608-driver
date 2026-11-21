#ifndef DAEMONCLIENT_H
#define DAEMONCLIENT_H

#include <QHash>
#include <QLocalSocket>
#include <QObject>
#include <QString>

class QTimer;

/*
 * devmond 客户端：QLocalSocket（Linux 上即 Unix 域套接字）+ 文本行协议（daemon/protocol.md）
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

signals:
	void connectionChanged(bool connected, const QString &reason);
	void message(const QString &type, const QHash<QString, QString> &fields);

private slots:
	void onConnected();
	void onDisconnected();
	void onError(QLocalSocket::LocalSocketError error);
	void onStateChanged(QLocalSocket::LocalSocketState state);
	void onReadyRead();
	void reconnect();

private:
	QLocalSocket *socket;
	QTimer *retryTimer;
	QString path;
	QString lastReason;
};

#endif /* DAEMONCLIENT_H */
