#pragma once

#include "core/model/RunState.h"

#include <QString>

class QLabel;
class QWidget;

namespace devdatapod {

// Chinese caption for a state. The machine-readable spelling lives in core (RunState.h),
// this is the presentation half.
QString runStateLabel(RunState state);

// A #StatusDot label; QSS picks the token color from the "state" dynamic property.
QLabel* makeDot(RunState state, QWidget* parent = nullptr, int size = 9);
void setDotState(QLabel* dot, RunState state);

}
