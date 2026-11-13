#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace devdatapod::proc {

// AGENTS.md rule 5, the second half. Job objects (Supervisor) cover the children *this*
// panel started; they cannot cover the ones a previous panel left behind, because that job
// handle died with it. So a crashed or Task-Manager-killed panel can still have redis, nginx
// or php-fpm listening, and the next start has to find and take them out.
//
// The rule is by path, not by image name: anything whose executable lives inside the install
// root belongs to this install. Two side-by-side copies of the panel therefore only ever
// clean up their own, and a system nginx in C:\nginx is untouchable.
struct Orphan {
    qint64 pid = 0;
    QString executable;    // absolute, as the OS reports it
    QString name;
};

// Everything currently running from inside `root`, minus this process.
// Does not kill anything, so the caller can report what it found first.
QVector<Orphan> scanOrphans(const QString& root);

// Terminate them. Returns the pids that are actually gone; a pid still alive afterwards is
// left out so the caller can say "these survived" instead of claiming success.
QVector<qint64> killOrphans(const QVector<Orphan>& orphans);

// scan + kill in one call, for the startup path. `report` receives the human-readable lines.
int sweepInstallRoot(const QString& root, QStringList* report);

}
