#include "ui/ServiceOperator.h"

#include "core/service/Engine.h"

namespace devdatapod {

ServiceOperator::ServiceOperator(Engine* engine, QObject* parent)
    : QObject(parent), engine_(engine) {}

void ServiceOperator::start(const QString& id) {
    QString error;
    const bool ok = engine_->start(id, &error);
    emit finished(id, ok, error);
}

void ServiceOperator::stop(const QString& id) {
    QString error;
    const bool ok = engine_->stop(id, &error);
    emit finished(id, ok, error);
}

void ServiceOperator::restart(const QString& id) {
    QString error;
    const bool ok = engine_->restart(id, &error);
    emit finished(id, ok, error);
}

}
