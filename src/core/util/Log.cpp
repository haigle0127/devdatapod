#include "core/util/Log.h"

#include "core/util/AppPaths.h"

#include <QDir>
#include <QString>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <memory>
#include <vector>

namespace devdatapod::log {

namespace {

constexpr const char* kLoggerName = "devdatapod";
constexpr std::size_t kMaxBytes = 2 * 1024 * 1024;
constexpr std::size_t kMaxFiles = 3;

std::shared_ptr<spdlog::logger> g_logger;

// The install dir may contain Chinese characters (AGENTS.md rule 7). With
// SPDLOG_WCHAR_FILENAMES on Windows, filename_t is std::wstring; elsewhere it is
// a narrow path and toStdString is fine.
spdlog::filename_t toNativePath(const QString& path) {
#ifdef SPDLOG_WCHAR_FILENAMES
    return path.toStdWString();
#else
    return path.toStdString();
#endif
}

}

bool init() {
    if (g_logger) return true;

    const QString file = QDir(paths::logsDir()).filePath(QStringLiteral("devdatapod.log"));
    try {
        std::vector<spdlog::sink_ptr> sinks;
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            toNativePath(file), kMaxBytes, kMaxFiles));
#ifdef DEVDATAPOD_DEBUG
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
#endif
        g_logger = std::make_shared<spdlog::logger>(kLoggerName, sinks.begin(), sinks.end());
        g_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
        g_logger->set_level(spdlog::level::debug);
        spdlog::register_logger(g_logger);
        spdlog::flush_on(spdlog::level::info);
    } catch (const spdlog::spdlog_ex& ex) {
        fprintf(stderr, "log init failed: %s\n", ex.what());
        g_logger.reset();
        return false;
    }
    return true;
}

void shutdown() {
    if (!g_logger) return;
    spdlog::drop(kLoggerName);
    g_logger.reset();
}

void installQtMessageHandler() {
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& ctx, const QString& message) {
        if (!g_logger) {
            fputs(qPrintable(message), stderr);
            return;
        }
        const QString where = ctx.file ? QStringLiteral(" (%1:%2)")
                                             .arg(QString::fromUtf8(ctx.file)).arg(ctx.line)
                                       : QString();
        const std::string text = (message + where).toStdString();
        switch (type) {
        case QtCriticalMsg: g_logger->error(text); break;
        case QtWarningMsg: g_logger->warn(text); break;
        case QtFatalMsg: g_logger->critical(text); g_logger->flush(); break;
        default: g_logger->debug(text); break;
        }
    });
}

static void write(spdlog::level::level_enum level, const QString& message) {
    if (!g_logger) return;
    g_logger->log(level, message.toStdString());
}

void info(const QString& message) { write(spdlog::level::info, message); }
void warn(const QString& message) { write(spdlog::level::warn, message); }
void error(const QString& message) { write(spdlog::level::err, message); }

}
