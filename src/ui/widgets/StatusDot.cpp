#include "ui/widgets/StatusDot.h"

#include <QLabel>
#include <QStyle>

namespace devdatapod {

QString runStateLabel(RunState state) {
    switch (state) {
    case RunState::Stopped: return QStringLiteral("已停止");
    case RunState::Starting: return QStringLiteral("启动中");
    case RunState::Running: return QStringLiteral("运行中");
    case RunState::Switching: return QStringLiteral("切换中");
    case RunState::Crashed: return QStringLiteral("异常退出");
    case RunState::Idle: return QStringLiteral("未就绪");
    }
    return {};
}

void setDotState(QLabel* dot, RunState state) {
    if (!dot) return;
    dot->setProperty("state", runStateKey(state));
    dot->style()->unpolish(dot);
    dot->style()->polish(dot);
    dot->update();
}

QLabel* makeDot(RunState state, QWidget* parent, int size) {
    auto* dot = new QLabel(parent);
    dot->setObjectName(QStringLiteral("StatusDot"));
    dot->setFixedSize(size, size);
    dot->setAccessibleName(runStateLabel(state));
    setDotState(dot, state);
    return dot;
}

}
