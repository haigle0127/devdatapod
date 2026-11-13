#include "ui/widgets/ServiceRow.h"

#include "ui/widgets/RoleIndicator.h"
#include "ui/widgets/ToggleSwitch.h"
#include "ui/widgets/UiFactory.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>

namespace devdatapod {

namespace {
constexpr int kDotSize = 9;
constexpr int kPortTagWidth = 86;    // 端口小块统一宽度，多行才能对齐
constexpr int kNameWidth = 150;      // 服务名固定宽度，端口列才能对齐成一列

// QSS matches on dynamic properties, so every property write here has to force a re-polish
// or the badge keeps the color of the role it was created with.
void repolish(QWidget* widget) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}
}

ServiceRow::ServiceRow(const ServiceRowInfo& info, QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("ServiceRow"));
    setAttribute(Qt::WA_StyledBackground, true);
    id_ = info.id;

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(12);

    dot_ = makeDot(info.state, this, kDotSize);
    auto* name = ui::makeLabel(info.name, QStringLiteral("SvcName"), this);
    name->setFixedWidth(kNameWidth);    // 固定宽度，端口列对齐

    // 端口小块固定宽度，多行的端口列才能对齐。
    portLabel_ = ui::makeLabel(QString(), QStringLiteral("PortTag"), this);
    portLabel_->setFixedWidth(kPortTagWidth);
    portLabel_->setAlignment(Qt::AlignCenter);

    layout->addWidget(dot_);
    layout->addWidget(name);
    layout->addWidget(portLabel_);
    layout->addStretch();

    state_ = info.state;
    // 「是否为默认」的图标：default 显示实心星（不可点），备用显示空心星，点了设为默认。
    roleIndicator_ = new RoleIndicator(this);
    // 「启动/停止」的滑动开关：开 = 运行（对勾），关 = 停止（叉）。
    runSwitch_ = new ToggleSwitch(this);
    runSwitch_->setIcon(ToggleSwitch::CheckCross);
    restartButton_ = new QPushButton(QStringLiteral("重启"), this);
    restartButton_->setObjectName(QStringLiteral("SoftBtn"));
    auto* configure = new QPushButton(QStringLiteral("配置"), this);
    configure->setObjectName(QStringLiteral("SoftBtn"));

    connect(restartButton_, &QPushButton::clicked, this, &ServiceRow::restartRequested);
    connect(configure, &QPushButton::clicked, this, &ServiceRow::configureRequested);
    // 点备用实例的空心星 = 把它设为默认。
    connect(roleIndicator_, &RoleIndicator::clicked, this, &ServiceRow::promoteRequested);
    // 启停开关：滑到开 = 启动，滑到关 = 停止，统一走 primaryRequested 由上层判断。
    connect(runSwitch_, &ToggleSwitch::toggled, this, [this](bool) {
        emit primaryRequested();
    });

    layout->addWidget(roleIndicator_);
    layout->addWidget(runSwitch_);
    layout->addWidget(restartButton_);
    layout->addWidget(configure);

    setInfo(info);
    setState(info.state);
}

void ServiceRow::setInfo(const ServiceRowInfo& info) {
    id_ = info.id;
    portLabel_->setText(info.port.isEmpty() ? QString()
                                            : QStringLiteral("端口 ") + info.port);
    portLabel_->setVisible(!info.port.isEmpty());

    // 图标跟着角色走：default 实心星不可点；备用空心星，可点时才能切默认。
    const bool isDefault = info.role == QLatin1String("default");
    roleIndicator_->setRole(isDefault, !isDefault && info.promotable);
}

void ServiceRow::setState(RunState state) {
    state_ = state;
    setDotState(dot_, state);
    // 启停开关跟着运行状态走：运行中/启动中 = 开。启动中保持开，别在就绪前弹回关。
    runSwitch_->setOn(state == RunState::Running || state == RunState::Starting, false);

    const bool busy = state == RunState::Starting || state == RunState::Switching;
    // 过渡态（启动中/切换中）开关禁用，防止连点造成状态错乱。
    runSwitch_->setEnabled(!busy);
    restartButton_->setEnabled(!busy);
}

}
