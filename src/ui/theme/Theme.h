#pragma once

#include <QColor>
#include <QFont>
#include <QIcon>
#include <QVariantMap>

class QQmlEngine;
class QObject;

namespace devdatapod {

// Sole runtime reader of the generated design tokens. QSS and QML both come out of
// scripts/gen-theme.mjs - nothing else in the app may carry a literal color.
class Theme {
public:
    static Theme& instance();

    bool isLoaded() const { return loaded_; }

    QColor color(const QString& token) const;

    void applyToApplication();

    // Instantiates generated Tokens.qml inside `engine`; parent owns the result.
    QObject* createTokenObject(QQmlEngine* engine, QObject* parent) const;

    QIcon appIcon() const;

private:
    Theme() = default;
    bool load();
    void loadFonts();
    QFont font(const QString& key, int pixelSize, bool bold = false) const;

    bool loaded_ = false;
    QVariantMap colors_;
    QVariantMap fontStacks_;
    QString stylesheet_;
};

}
