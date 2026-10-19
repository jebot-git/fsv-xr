// SPDX-License-Identifier: LGPL-2.1-or-later
#include "appearance.h"
#include <QApplication>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QTimer>
#include <cmath>

Appearance::Appearance(bool dome, QObject *parent) : QObject(parent), m_original(QApplication::palette())
{
    m_scene = m_original.color(QPalette::Base);
    m_panel = m_original.color(QPalette::Window);
    if (!dome) return;
    m_path = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/dome/appearance.json";
    auto *timer = new QTimer(this); timer->setSingleShot(true); timer->setInterval(30);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, timer, [timer] { timer->start(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, timer, [timer] { timer->start(); });
    connect(timer, &QTimer::timeout, this, [this] { reload(); watch(); });
    reload(); watch();
}
void Appearance::watch()
{
    if (!m_watcher.files().isEmpty()) m_watcher.removePaths(m_watcher.files());
    if (!m_watcher.directories().isEmpty()) m_watcher.removePaths(m_watcher.directories());
    QString directory = QFileInfo(m_path).absolutePath();
    while (!QFileInfo::exists(directory)) {
        const QString parent = QFileInfo(directory).dir().absolutePath();
        if (parent == directory) break;
        directory = parent;
    }
    m_watcher.addPath(directory);
    if (QFileInfo::exists(m_path)) m_watcher.addPath(m_path);
}
void Appearance::reload()
{
    QJsonObject config;
    QFile file(m_path);
    if (file.exists()) {
        if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) return;
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject()) return;
        config = document.object();
    }
    if (config.contains("light") && !config["light"].isBool()) return;
    if (config.contains("accent") && !config["accent"].isString()) return;
    if (config.contains("opacity") && !config["opacity"].isDouble()) return;
    if (config.contains("background") && !config["background"].isArray()) return;
    const bool light = config["light"].toBool(false);
    const QColor accent(config["accent"].toString("#6babff"));
    const QString accentString = config["accent"].toString("#6babff");
    if (!accent.isValid() || accentString.size() != 7 || !accentString.startsWith('#')) return;
    const double opacity = config["opacity"].toDouble(248);
    if (opacity < 160 || opacity > 255 || std::floor(opacity) != opacity) return;
    const auto background = config.value("background").toArray(QJsonArray{.025, .045, .06});
    if (background.size() != 3) return;
    for (const auto &channel : background)
        if (!channel.isDouble() || channel.toDouble() < 0 || channel.toDouble() > 1) return;
    const QColor surface = light ? QColor(229, 233, 239) : QColor(31, 37, 45);
    const QColor text = light ? QColor(30, 37, 46) : QColor(231, 235, 241);
    const QColor muted = light ? QColor(104, 116, 132) : QColor(146, 154, 165);
    const QColor selection = light ? QColor(199, 213, 232) : QColor(63, 77, 96);
    QPalette palette = m_original;
    for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        const QColor foreground = group == QPalette::Disabled ? muted : text;
        for (auto role : {QPalette::Window, QPalette::Button, QPalette::Base, QPalette::ToolTipBase}) palette.setColor(group, role, surface);
        for (auto role : {QPalette::WindowText, QPalette::ButtonText, QPalette::Text, QPalette::ToolTipText}) palette.setColor(group, role, foreground);
        palette.setColor(group, QPalette::Highlight, selection);
        palette.setColor(group, QPalette::HighlightedText, text);
        palette.setColor(group, QPalette::Accent, accent);
        palette.setColor(group, QPalette::Link, accent);
        palette.setColor(group, QPalette::Mid, muted);
        palette.setColor(group, QPalette::PlaceholderText, muted);
    }
    m_scene = QColor::fromRgbF(background[0].toDouble(), background[1].toDouble(), background[2].toDouble());
    m_panel = surface; m_panel.setAlpha(int(opacity));
    QApplication::setPalette(palette);
    emit changed();
}
