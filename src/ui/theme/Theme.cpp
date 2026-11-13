#include "ui/theme/Theme.h"

#include "core/util/Log.h"

#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QStringList>

namespace devdatapod {

namespace {

constexpr const char* kTokensRes = ":/devdatapod/ui/theme/generated/tokens.json";
constexpr const char* kQssRes = ":/devdatapod/ui/theme/generated/theme.qss";
constexpr const char* kQmlTokens = "qrc:/devdatapod/ui/theme/generated/Tokens.qml";

QVariantMap readJsonMap(const char* resourcePath) {
    QFile file(QString::fromLatin1(resourcePath));
    if (!file.open(QIODevice::ReadOnly)) {
        log::error(QStringLiteral("missing %1 - run `node scripts/gen-theme.mjs`").arg(resourcePath));
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object().toVariantMap();
}

QStringList splitFontStack(const QString& stack) {
    QStringList out;
    for (const QString& part : stack.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        out << part.trimmed().remove(QLatin1Char('"'));
    }
    return out;
}

}

Theme& Theme::instance() {
    static Theme only;
    if (!only.loaded_) only.load();
    return only;
}

bool Theme::load() {
    loadFonts();
    const QVariantMap tokens = readJsonMap(kTokensRes);
    colors_ = tokens.value(QStringLiteral("colors")).toMap();
    fontStacks_ = tokens.value(QStringLiteral("fontStacks")).toMap();
    loaded_ = !colors_.isEmpty() && !fontStacks_.isEmpty();
    if (!loaded_) {
        log::error(QStringLiteral("theme tokens unusable - %1").arg(kTokensRes));
        return false;
    }

    QFile qss(QString::fromLatin1(kQssRes));
    if (qss.open(QIODevice::ReadOnly)) stylesheet_ = QString::fromUtf8(qss.readAll());
    log::info(QStringLiteral("theme: %1 colors, %2 font stacks, %3 QSS bytes")
                  .arg(colors_.size())
                  .arg(fontStacks_.size())
                  .arg(stylesheet_.size()));
    return true;
}

QColor Theme::color(const QString& token) const {
    return QColor(colors_.value(token).toString());
}

QFont Theme::font(const QString& key, int pixelSize, bool bold) const {
    QFont f;
    f.setFamilies(splitFontStack(fontStacks_.value(key).toString()));
    f.setPixelSize(pixelSize);
    f.setBold(bold);
    return f;
}

void Theme::loadFonts() {
    // 内嵌的 MiSans 两个字重，注册进字体库，全程序默认字体用它（绿色便携，不依赖系统装没装）。
    const QStringList files{
        QStringLiteral(":/devdatapod/ui/theme/fonts/MiSans-Regular.ttf"),
        QStringLiteral(":/devdatapod/ui/theme/fonts/MiSans-Medium.ttf"),
    };
    int ok = 0;
    for (const QString& file : files) {
        if (QFontDatabase::addApplicationFont(file) >= 0) ++ok;
        else log::warn(QStringLiteral("字体加载失败：%1").arg(file));
    }
    log::info(QStringLiteral("字体：MiSans %1/%2 个字重已注册").arg(ok).arg(files.size()));
}

void Theme::applyToApplication() {
    // MiSans 优先，token 字体栈（Funnel Sans / Inter + CJK 回退）作为后备。
    QFont defaultFont = font(QStringLiteral("body"), 12);
    if (QFontDatabase::hasFamily(QStringLiteral("MiSans"))) {
        QStringList families{QStringLiteral("MiSans")};
        families.append(defaultFont.families());
        defaultFont.setFamilies(families);
    }
    qApp->setFont(defaultFont);
    qApp->setStyleSheet(stylesheet_);
}

QObject* Theme::createTokenObject(QQmlEngine* engine, QObject* parent) const {
    auto* component = new QQmlComponent(engine, QUrl(QString::fromLatin1(kQmlTokens)), parent);
    if (!component->isReady()) {
        log::error(QStringLiteral("Tokens.qml: %1").arg(component->errorString()));
        return nullptr;
    }
    QObject* object = component->create();
    if (!object) {
        log::error(QStringLiteral("Tokens.qml: %1").arg(component->errorString()));
        return nullptr;
    }
    object->setParent(parent);
    return object;
}

QIcon Theme::appIcon() const {
    // 托盘和窗口图标统一用 assets/logo.svg（圆角白底草莓）。
    return QIcon(QStringLiteral(":/devdatapod/assets/logo.svg"));
}

}
