#include "daemonclient.h"

#include <QTimer>

DaemonClient::DaemonClient(QObject *parent)
	: QObject(parent)
	, socket(new QLocalSocket(this))
	, retryTimer(new QTimer(this))
{
	const QByteArray env = qgetenv("DEVMOND_SOCKET");
	path = env.isEmpty() ? QString("/tmp/devmond.sock") : QString::fromLocal8Bit(env);

	retryTimer->setInterval(1000);
	retryTimer->setSingleShot(true);

	connect(socket, &QLocalSocket::connected, this, &DaemonClient::onConnected);
	connect(socket, &QLocalSocket::disconnected, this, &DaemonClient::onDisconnected);
	connect(socket, &QLocalSocket::readyRead, this, &DaemonClient::onReadyRead);
	connect(socket, &QLocalSocket::stateChanged, this, &DaemonClient::onStateChanged);
	/* 板子是 Qt 5.12（只有 error 信号），主机预览可能是 5.15（改名为 errorOccurred） */
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
	connect(socket, &QLocalSocket::errorOccurred, this, &DaemonClient::onError);
#else
	connect(socket,
		static_cast<void (QLocalSocket::*)(QLocalSocket::LocalSocketError)>(&QLocalSocket::error),
		this, &DaemonClient::onError);
#endif
	connect(retryTimer, &QTimer::timeout, this, &DaemonClient::reconnect);
}

void DaemonClient::start()
{
	reconnect();
}

bool DaemonClient::isConnected() const
{
	return socket->state() == QLocalSocket::ConnectedState;
}

void DaemonClient::send(const QString &command)
{
	if (isConnected()) {
		socket->write(command.toUtf8() + '\n');
	}
}

void DaemonClient::reconnect()
{
	if (socket->state() == QLocalSocket::UnconnectedState) {
		socket->connectToServer(path);
	}
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

/*
 * 重连只看状态：Qt 5.12 的 QLocalSocket 连接失败时先发 error 信号、之后才把状态改成
 * UnconnectedState，在 error 槽里判断状态会漏掉重连（守护进程不在时界面一直连不回去）
 */
void DaemonClient::onStateChanged(QLocalSocket::LocalSocketState state)
{
	if (state == QLocalSocket::UnconnectedState) {
		retryTimer->start();
	}
}

void DaemonClient::onError(QLocalSocket::LocalSocketError error)
{
	Q_UNUSED(error);
	/* 连不上（守护进程没运行）时只报告一次原因，之后安静地重试 */
	const QString reason = QString("devmond not reachable at %1: %2").arg(path, socket->errorString());
	if (reason != lastReason) {
		lastReason = reason;
		emit connectionChanged(false, reason);
	}
}

void DaemonClient::onReadyRead()
{
	while (socket->canReadLine()) {
		const QString line = QString::fromUtf8(socket->readLine()).trimmed();
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
