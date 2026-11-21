#include "daemonclient.h"

#include <QLocalSocket>
#include <QNetworkProxy>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>

DaemonClient::DaemonClient(QObject *parent)
	: QObject(parent)
	, local(nullptr)
	, tcp(nullptr)
	, io(nullptr)
	, retryTimer(new QTimer(this))
	, port(0)
{
	retryTimer->setInterval(1000);
	retryTimer->setSingleShot(true);
	connect(retryTimer, &QTimer::timeout, this, &DaemonClient::reconnect);

	/*
	 * 重连只看状态变成“未连接”：Qt 5.12 连接失败时先发 error 信号、之后才改状态，
	 * 在 error 槽里判断状态会漏掉重连（守护进程不在时界面一直连不回去）
	 */
	const QString remote = QString::fromLocal8Bit(qgetenv("DEVMOND_ADDR"));
	if (!remote.isEmpty()) {
		const int colon = remote.lastIndexOf(':');
		host = colon > 0 ? remote.left(colon) : remote;
		port = colon > 0 ? remote.mid(colon + 1).toUShort() : 9100;
		addressText = QString("%1:%2").arg(host).arg(port);

		tcp = new QTcpSocket(this);
		/*
		 * 直连局域网里的设备，不走代理：Qt 会读取 ALL_PROXY 等环境变量，
		 * PC 上开着代理时连接被转给本机代理端口，代理不转发到局域网地址，表现为 "Remote host closed"
		 */
		tcp->setProxy(QNetworkProxy::NoProxy);
		io = tcp;
		connect(tcp, &QTcpSocket::connected, this, &DaemonClient::onConnected);
		connect(tcp, &QTcpSocket::disconnected, this, &DaemonClient::onDisconnected);
		connect(tcp, &QTcpSocket::stateChanged, this, [this](QAbstractSocket::SocketState s) {
			if (s == QAbstractSocket::UnconnectedState) {
				onUnconnected();
			}
		});
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
		connect(tcp, &QTcpSocket::errorOccurred, this, &DaemonClient::onError);
#else
		connect(tcp, static_cast<void (QAbstractSocket::*)(QAbstractSocket::SocketError)>(
				     &QAbstractSocket::error),
			this, &DaemonClient::onError);
#endif
	} else {
		const QByteArray env = qgetenv("DEVMOND_SOCKET");
		path = env.isEmpty() ? QString("/tmp/devmond.sock") : QString::fromLocal8Bit(env);
		addressText = path;

		local = new QLocalSocket(this);
		io = local;
		connect(local, &QLocalSocket::connected, this, &DaemonClient::onConnected);
		connect(local, &QLocalSocket::disconnected, this, &DaemonClient::onDisconnected);
		connect(local, &QLocalSocket::stateChanged, this, [this](QLocalSocket::LocalSocketState s) {
			if (s == QLocalSocket::UnconnectedState) {
				onUnconnected();
			}
		});
		/* 板子是 Qt 5.12（只有 error 信号），主机可能是 5.15（改名为 errorOccurred） */
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
		connect(local, &QLocalSocket::errorOccurred, this, &DaemonClient::onError);
#else
		connect(local, static_cast<void (QLocalSocket::*)(QLocalSocket::LocalSocketError)>(
				       &QLocalSocket::error),
			this, &DaemonClient::onError);
#endif
	}
	connect(io, &QIODevice::readyRead, this, &DaemonClient::onReadyRead);
}

void DaemonClient::start()
{
	reconnect();
}

bool DaemonClient::isConnected() const
{
	return tcp ? tcp->state() == QAbstractSocket::ConnectedState
		   : local->state() == QLocalSocket::ConnectedState;
}

void DaemonClient::send(const QString &command)
{
	if (isConnected()) {
		io->write(command.toUtf8() + '\n');
	}
}

void DaemonClient::reconnect()
{
	if (tcp && tcp->state() == QAbstractSocket::UnconnectedState) {
		tcp->connectToHost(host, port);
	} else if (local && local->state() == QLocalSocket::UnconnectedState) {
		local->connectToServer(path);
	}
}

void DaemonClient::onUnconnected()
{
	retryTimer->start();
}

void DaemonClient::onConnected()
{
	lastReason.clear();
	emit connectionChanged(true, QString());
}

void DaemonClient::onDisconnected()
{
	emit connectionChanged(false, "devmond disconnected");
}

void DaemonClient::onError()
{
	/* 连不上时只报告一次原因，之后安静地重试 */
	const QString reason = QString("devmond not reachable at %1: %2")
		.arg(addressText, io->errorString());
	if (reason != lastReason) {
		lastReason = reason;
		emit connectionChanged(false, reason);
	}
}

void DaemonClient::onReadyRead()
{
	while (io->canReadLine()) {
		const QString line = QString::fromUtf8(io->readLine()).trimmed();
		if (line.isEmpty()) {
			continue;
		}

		/* msg= 之后的内容原样作为一个字段（可能含空格） */
		QString head = line;
		QHash<QString, QString> fields;
		const int msgPos = line.indexOf(" msg=");
		if (msgPos >= 0) {
			fields.insert("msg", line.mid(msgPos + 5));
			head = line.left(msgPos);
		}

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
		const QStringList tokens = head.split(' ', Qt::SkipEmptyParts);
#else
		const QStringList tokens = head.split(' ', QString::SkipEmptyParts);
#endif
		const QString type = tokens.value(0);
		for (int i = 1; i < tokens.size(); i++) {
			const int eq = tokens[i].indexOf('=');
			if (eq > 0) {
				fields.insert(tokens[i].left(eq), tokens[i].mid(eq + 1));
			} else if (i == 1) {
				fields.insert("cmd", tokens[i]); /* OK <CMD> / ERR <CMD> */
			}
		}
		emit message(type, fields);
	}
}
