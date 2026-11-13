#include "core/service/SiteStore.h"

#include "core/util/AppPaths.h"
#include "core/util/AtomicFile.h"
#include "core/util/Log.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>

namespace devdatapod::sites {

namespace {

constexpr int kSchema = 1;

QString normalize(const QString& raw) {
    return raw.trimmed().toLower();
}

// 主机名：localhost 这种单段名字允许，a-b.example.com 也允许；协议头、斜杠、端口一律不允许。
bool looksLikeHost(const QString& domain) {
    static const QRegularExpression pattern(
        QStringLiteral("^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?"
                       "(\\.[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)*$"));
    return !domain.isEmpty() && domain.size() <= 253 && pattern.match(domain).hasMatch();
}

// 站点建好时放的占位页。用户把自己的 index.html 丢进目录就会被顶掉（这里只在没有
// index.* 的时候写）。
QByteArray placeholderPage(const Site& site) {
    return QStringLiteral(
               "<!doctype html>\n"
               "<html lang=\"zh-CN\">\n"
               "<head>\n"
               "  <meta charset=\"utf-8\">\n"
               "  <title>%1</title>\n"
               "  <style>\n"
               "    body { margin: 0; min-height: 100vh; display: flex; align-items: center;\n"
               "           justify-content: center; background: #F7F4EC; color: #262040;\n"
               "           font-family: 'MiSans', 'Microsoft YaHei', sans-serif; }\n"
               "    .card { padding: 36px 44px; background: #FFFFFF; border-radius: 16px;\n"
               "            box-shadow: 0 12px 32px rgba(38, 32, 64, 0.08); text-align: center; }\n"
               "    h1 { margin: 0 0 8px; font-size: 20px; font-weight: 600; }\n"
               "    p { margin: 6px 0 0; font-size: 13px; color: #6F6985; }\n"
               "    code { color: #6C5CE7; }\n"
               "  </style>\n"
               "</head>\n"
               "<body>\n"
               "  <div class=\"card\">\n"
               "    <h1>%1</h1>\n"
               "    <p>站点已经部署到 <code>%2</code>。</p>\n"
               "    <p>把这段占位页换成你自己的 <code>index.html</code> 就行。</p>\n"
               "  </div>\n"
               "</body>\n"
               "</html>\n")
        .arg(site.domain.toHtmlEscaped(), site.rootDir.toHtmlEscaped())
        .toUtf8();
}

} // namespace

QString filePath() {
    return paths::dataDir() + QStringLiteral("/sites.json");
}

QVector<Site> load(QString* problem) {
    QVector<Site> out;
    const QString path = filePath();
    if (!QFileInfo::exists(path)) return out;

    QByteArray raw;
    QString readError;
    if (!file::readAll(path, &raw, &readError)) {
        if (problem) *problem = readError;
        return out;
    }
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        if (problem) {
            *problem = QStringLiteral("%1 不是合法 JSON（%2）").arg(path, parse.errorString());
        }
        return out;
    }

    const QJsonArray items = doc.object().value(QStringLiteral("items")).toArray();
    out.reserve(items.size());
    for (const QJsonValue& value : items) {
        const QJsonObject object = value.toObject();
        Site site;
        site.domain = normalize(object.value(QStringLiteral("domain")).toString());
        if (site.domain.isEmpty()) continue;
        for (const QJsonValue& alias : object.value(QStringLiteral("aliases")).toArray()) {
            const QString text = normalize(alias.toString());
            if (!text.isEmpty() && text != site.domain && !site.aliases.contains(text)) {
                site.aliases.append(text);
            }
        }
        site.port = object.value(QStringLiteral("port")).toInt();
        site.rootDir = object.value(QStringLiteral("rootDir")).toString();
        site.expireAt = object.value(QStringLiteral("expireAt")).toString();
        site.remark = object.value(QStringLiteral("remark")).toString();
        out.append(site);
    }
    return out;
}

bool save(const QVector<Site>& items, QString* error) {
    QJsonArray array;
    for (const Site& site : items) {
        QJsonObject object;
        object.insert(QStringLiteral("domain"), site.domain);
        QJsonArray aliases;
        for (const QString& alias : site.aliases) aliases.append(alias);
        object.insert(QStringLiteral("aliases"), aliases);
        object.insert(QStringLiteral("port"), site.port);
        object.insert(QStringLiteral("rootDir"), site.rootDir);
        object.insert(QStringLiteral("expireAt"), site.expireAt);
        object.insert(QStringLiteral("remark"), site.remark);
        array.append(object);
    }

    QJsonObject root;
    root.insert(QStringLiteral("schema"), kSchema);
    root.insert(QStringLiteral("items"), array);

    const QString path = filePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) *error = QStringLiteral("无法创建 %1").arg(QFileInfo(path).absolutePath());
        return false;
    }
    if (!file::writeAtomically(path, QJsonDocument(root).toJson(QJsonDocument::Indented))) {
        if (error) *error = QStringLiteral("无法写入 %1").arg(path);
        return false;
    }
    return true;
}

bool validate(const Site& site, const Site* self, const QVector<Site>& existing, QString* error) {
    const QString domain = normalize(site.domain);
    if (!looksLikeHost(domain)) {
        if (error) {
            *error = QStringLiteral("域名只填主机名，例如 www.example.com（不要带 http:// 或端口）");
        }
        return false;
    }
    if (site.port < 1 || site.port > 65535) {
        if (error) *error = QStringLiteral("端口必须在 1-65535 之间");
        return false;
    }
    if (site.rootDir.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("根目录不能为空");
        return false;
    }

    QStringList aliases;
    for (const QString& raw : site.aliases) {
        const QString alias = normalize(raw);
        if (alias.isEmpty() || alias == domain) continue;
        if (!looksLikeHost(alias)) {
            if (error) *error = QStringLiteral("第二域名格式不对：%1").arg(raw);
            return false;
        }
        if (aliases.contains(alias)) {
            if (error) *error = QStringLiteral("第二域名重复：%1").arg(alias);
            return false;
        }
        aliases.append(alias);
    }

    const QString selfDomain = self ? normalize(self->domain) : QString();
    for (const Site& other : existing) {
        if (!selfDomain.isEmpty() && normalize(other.domain) == selfDomain) continue;
        if (normalize(other.domain) == domain || other.aliases.contains(domain)) {
            if (error) *error = QStringLiteral("域名 %1 已经绑在别的站点上").arg(domain);
            return false;
        }
        if (aliases.contains(normalize(other.domain))) {
            if (error) {
                *error = QStringLiteral("第二域名 %1 已经是另一个站点的主域名")
                             .arg(normalize(other.domain));
            }
            return false;
        }
        for (const QString& alias : aliases) {
            if (other.aliases.contains(alias)) {
                if (error) *error = QStringLiteral("第二域名 %1 已经被别的站点占用").arg(alias);
                return false;
            }
        }
        if (other.port == site.port) {
            if (error) {
                *error = QStringLiteral("端口 %1 已经被站点 %2 占用").arg(site.port).arg(other.domain);
            }
            return false;
        }
    }
    return true;
}

QString defaultRootDir(const QString& domain) {
    return QDir::cleanPath(paths::wwwDir() + QLatin1Char('/') + normalize(domain));
}

bool prepareRoot(const Site& site, QString* error) {
    const QString root = QDir::cleanPath(site.rootDir);
    if (root.isEmpty()) {
        if (error) *error = QStringLiteral("根目录不能为空");
        return false;
    }
    if (!QDir().mkpath(root)) {
        if (error) *error = QStringLiteral("无法创建目录 %1").arg(root);
        return false;
    }
    // 已有 index.* 就一个字都不动；用户放的页面永远优先。
    for (const QString& index : {QStringLiteral("index.html"), QStringLiteral("index.htm")}) {
        if (QFileInfo::exists(root + QLatin1Char('/') + index)) return true;
    }
    const QString page = root + QStringLiteral("/index.html");
    if (!file::writeAtomically(page, placeholderPage(site))) {
        log::warn(QStringLiteral("sites: 无法写入占位页 %1").arg(page));
        return true;    // 目录在就够了，nginx 会回 403/404，这不影响部署
    }
    return true;
}

const Site* find(const QVector<Site>& items, const QString& domain) {
    const QString key = normalize(domain);
    for (const Site& site : items) {
        if (normalize(site.domain) == key) return &site;
    }
    return nullptr;
}

} // namespace devdatapod::sites
