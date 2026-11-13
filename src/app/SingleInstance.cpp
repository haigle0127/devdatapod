#include "app/SingleInstance.h"

#include "core/util/AppPaths.h"
#include "core/util/Log.h"

#include <QCryptographicHash>
#include <QDir>
#include <QLocalSocket>

namespace devdatapod {

namespace {
constexpr int kLockWaitMs = 100;
constexpr int kPeerTimeoutMs = 300;
}

SingleInstance::SingleInstance(QObject* parent)
    : QObject(parent),
      lock_(QDir(paths::dataDir()).filePath(QStringLiteral("devdatapod.lock"))),
      serverName_(QStringLiteral("devdatapod-") + QString::fromLatin1(
                      QCryptographicHash::hash(paths::installDir().toUtf8(),
                                               QCryptographicHash::Sha1)
                          .toHex()
                          .left(12))) {
    connect(&server_, &QLocalServer::newConnection, this, [this] {
        QLocalSocket* peer = server_.nextPendingConnection();
        if (!peer) return;
        peer->deleteLater();
        emit activateRequested();
    });
}

bool SingleInstance::acquire() {
    if (!lock_.tryLock(kLockWaitMs)) {
        QLocalSocket peer;
        peer.connectToServer(serverName_);
        if (peer.waitForConnected(kPeerTimeoutMs)) {
            peer.write("activate");
            peer.flush();
            peer.waitForBytesWritten(kPeerTimeoutMs);
            peer.disconnectFromServer();
        }
        return false;
    }
    acquired_ = true;
    QLocalServer::removeServer(serverName_);
    if (!server_.listen(serverName_)) {
        log::warn(QStringLiteral("activate channel unavailable: %1").arg(server_.errorString()));
    }
    return true;
}

SingleInstance::~SingleInstance() {
    if (acquired_) {
        server_.close();
        QLocalServer::removeServer(serverName_);
    }
    lock_.unlock();
}

}
