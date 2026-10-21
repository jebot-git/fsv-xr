// SPDX-License-Identifier: MIT
#pragma once
#include <QApplication>
#include <QPalette>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFontDatabase>
#include <QFontInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTimer>
#include <functional>
#include <cmath>

// Per-application adapter: no dependency on a sibling checkout or global Qt config.
class DomeAppearance : public QObject {
public:
    static bool inSession() {
        const auto names = qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':');
        return names.contains("Dome") || names.contains("DomeD") || names.contains("dome");
    }
    explicit DomeAppearance(bool enabled, QObject *parent = nullptr)
        : QObject(parent), originalFont(QApplication::font()), originalPalette(QApplication::palette()), active(enabled) {
        if (!active) {
            connect(qApp, &QGuiApplication::fontChanged, this, [this](const QFont &font) { currentFont = font; if (changed) changed(); });
            return;
        }
        qApp->setProperty("domeBitmapAppearance", true);
        path = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/dome/appearance.json";
        timer.setInterval(100); timer.setSingleShot(true);
        connect(&watcher, &QFileSystemWatcher::fileChanged, this, [this] { timer.start(); });
        connect(&watcher, &QFileSystemWatcher::directoryChanged, this, [this] { timer.start(); });
        connect(&timer, &QTimer::timeout, this, [this] { reload(); watch(); });
        reload(); watch();
    }
    std::function<void()> changed;
    QFont font() const { return currentFont; }
    bool enabled() const { return active; }
    void reload() {
        if (!active) return;
        QJsonObject config;
        QFile file(path);
        if (file.exists()) {
            if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) return;
            QJsonParseError error;
            const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
            if (error.error != QJsonParseError::NoError || !doc.isObject()) return;
            config = doc.object();
        }
        const auto id = config.value("font").toString("GNU Unifont / Sans-Serif");
        const auto parts = id.split(" / ");
        const double size = config.value("font_size").toDouble(16);
        const auto accent = QColor(config.value("accent").toString("#6babff"));
        if (id.size() > 256 || size < 8 || size > 24 || size != std::floor(size) || !accent.isValid()) return;
        QFont selected = originalFont;
        if (parts.size() == 2 && QFontDatabase::families().contains(parts[0]) &&
            QFontDatabase::styles(parts[0]).contains(parts[1]) &&
            !QFontDatabase::isSmoothlyScalable(parts[0], parts[1])) {
            QFont candidate(parts[0]); candidate.setStyleName(parts[1]); candidate.setPixelSize(int(size));
            candidate.setStyleStrategy(QFont::StyleStrategy(QFont::PreferBitmap | QFont::NoAntialias));
            QFontInfo resolved(candidate);
            if (resolved.family() == parts[0] && resolved.pixelSize() == int(size)) selected = candidate;
        }
        const bool light = config.value("light").toBool(false);
        const QColor surface = light ? QColor(229,233,239) : QColor(31,37,45);
        const QColor text = light ? QColor(30,37,46) : QColor(231,235,241);
        const QColor muted = light ? QColor(104,116,132) : QColor(146,154,165);
        QPalette palette = originalPalette;
        for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
            for (auto role : {QPalette::Window, QPalette::Button, QPalette::Base, QPalette::ToolTipBase}) palette.setColor(group, role, surface);
            for (auto role : {QPalette::WindowText, QPalette::ButtonText, QPalette::Text, QPalette::ToolTipText}) palette.setColor(group, role, group == QPalette::Disabled ? muted : text);
            palette.setColor(group,QPalette::Highlight,light?QColor(199,213,232):QColor(63,77,96));
            palette.setColor(group,QPalette::HighlightedText,text);
            palette.setColor(group,QPalette::Link,accent);
            palette.setColor(group,QPalette::PlaceholderText,muted);
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
            palette.setColor(group,QPalette::Accent,accent);
#endif
        }
        currentFont = selected;
        QApplication::setFont(selected);
        QApplication::setPalette(palette);
        if (changed) changed();
    }
private:
    void watch() {
        if (!watcher.files().isEmpty()) watcher.removePaths(watcher.files());
        if (!watcher.directories().isEmpty()) watcher.removePaths(watcher.directories());
        QString directory = QFileInfo(path).absolutePath();
        while (!QFileInfo::exists(directory)) {
            const auto parent = QFileInfo(directory).dir().absolutePath();
            if (parent == directory) break;
            directory = parent;
        }
        watcher.addPath(directory);
        if (QFileInfo::exists(path)) watcher.addPath(path);
    }
    QFont originalFont, currentFont = QApplication::font();
    QPalette originalPalette;
    bool active;
    QString path;
    QFileSystemWatcher watcher;
    QTimer timer;
};
