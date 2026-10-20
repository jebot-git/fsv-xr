// SPDX-License-Identifier: LGPL-2.1-or-later
#include "domemenu.h"
#include "filesystem.h"
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QMenu>
#include <QMessageBox>

QAction *DomeMenu::item(QMenu *menu, const QString &id, const QString &label,
                       const QString &shortcut, const std::function<void()> &callback)
{
    auto *action = menu->addAction(label);
    action->setObjectName(id);
    action->setMenuRole(QAction::NoRole);
    if (!shortcut.isEmpty()) action->setShortcut(QKeySequence(shortcut));
    connect(action, &QAction::triggered, this, callback);
    m_actions.insert(id, action);
    return action;
}
DomeMenu::DomeMenu(FileSystem *files, QWidget *owner) : QMenuBar(owner), m_files(files)
{
    setNativeMenuBar(true);
    auto command = [this](const QString &id) { return [this, id] { emit commandRequested(id); }; };
    auto *file = addMenu(tr("&File"));
    item(file, "newTab", tr("New Tab"), "Ctrl+T", [files] { files->newTab(); });
    item(file, "open", tr("Open"), "", [files] { files->open(files->selected()); });
    item(file, "openTab", tr("Open in New Tab"), "", [files] { files->newTab(files->selection().value("path").toString()); });
    item(file, "openWith", tr("Open With…"), "", command("openWith"));
    file->addSeparator();
    item(file, "mkdir", tr("Create Folder…"), "Ctrl+Shift+N", command("mkdir"));
    item(file, "compress", tr("Compress…"), "", command("compress"));
    item(file, "extract", tr("Extract Archive"), "", [files] { files->extractArchive(); });
    item(file, "properties", tr("Properties"), "Alt+Return", command("properties"));
    file->addSeparator();
    item(file, "closeTab", tr("Close Tab"), "Ctrl+W", [files] { files->closeTab(files->currentTab()); });
    item(file, "closeWindow", tr("Close Window"), "Ctrl+Shift+W", [owner] { owner->close(); });
    item(file, "quit", tr("Quit"), "Ctrl+Q", [] { QApplication::closeAllWindows(); })->setMenuRole(QAction::QuitRole);
    auto *edit = addMenu(tr("&Edit"));
    // Forward clipboard commands to the Quick focus item when editing text.
    // These actions stay available there even without a filesystem selection.
    item(edit, "cut", tr("Cut"), "Ctrl+X", command("cut"));
    item(edit, "copy", tr("Copy"), "Ctrl+C", command("copy"));
    item(edit, "paste", tr("Paste"), "Ctrl+V", command("paste"));
    item(edit, "copyLocation", tr("Copy Location"), "", [files] { files->copyLocation(); });
    edit->addSeparator();
    item(edit, "rename", tr("Rename…"), "F2", command("rename"));
    item(edit, "trash", tr("Move to Trash"), "Delete", [this, files] {
        if (files->inTrash()) emit commandRequested("delete"); else files->remove(false);
    });
    item(edit, "delete", tr("Delete Permanently…"), "Shift+Delete", command("delete"));
    item(edit, "restore", tr("Restore"), "", [files] { files->restore(); });
    item(edit, "emptyTrash", tr("Empty Trash…"), "", command("emptyTrash"));
    auto *view = addMenu(tr("&View"));
    auto *layouts = view->addMenu(tr("Layout"));
    auto *group = new QActionGroup(this);
    for (const auto &layout : {QString("map"), QString("tree"), QString("disc")}) {
        auto *action = item(layouts, layout, layout.left(1).toUpper() + layout.mid(1), "", [files, layout] { files->setLayout(layout); });
        action->setCheckable(true); group->addAction(action);
    }
    item(view, "refresh", tr("Refresh"), "F5", [files] { files->refresh(); });
    item(view, "resetView", tr("Reset View"), "", command("resetView"));
    auto *go = addMenu(tr("&Go"));
    item(go, "up", tr("Up"), "Alt+Up", [files] { files->up(); });
    item(go, "home", tr("Home"), "Alt+Home", [files] { files->navigate(QDir::homePath()); });
    item(go, "location", tr("Enter Location…"), "Ctrl+L", command("location"));
    item(go, "showTrash", tr("Trash"), "", [files] { files->navigate("trash:/"); });
    item(go, "terminal", tr("Open Terminal Here"), "", [files] { files->terminal(); });
    auto *window = addMenu(tr("&Window"));
    item(window, "nextTab", tr("Next Tab"), "Ctrl+Tab", [files] { files->activateTab((files->currentTab() + 1) % files->tabs().size()); });
    item(window, "previousTab", tr("Previous Tab"), "Ctrl+Shift+Tab", [files] { files->activateTab((files->currentTab() + files->tabs().size() - 1) % files->tabs().size()); });
    auto *help = addMenu(tr("&Help"));
    item(help, "about", tr("About fsvr"), "", [owner] {
        QMessageBox::about(owner, tr("About fsvr"), tr("fsvr %1\nVulkan filesystem explorer\nhttps://github.com/jebot-git/fsv-xr").arg(QApplication::applicationVersion()));
    })->setMenuRole(QAction::AboutRole);
    connect(files, &FileSystem::selectedChanged, this, &DomeMenu::updateActions);
    connect(files, &FileSystem::rootChanged, this, &DomeMenu::updateActions);
    connect(files, &FileSystem::busyChanged, this, &DomeMenu::updateActions);
    connect(files, &FileSystem::layoutChanged, this, &DomeMenu::updateActions);
    connect(files, &FileSystem::tabsChanged, this, &DomeMenu::updateActions);
    connect(QApplication::clipboard(), &QClipboard::dataChanged, this, &DomeMenu::updateActions);
    updateActions();
}
void DomeMenu::setTextEditing(bool editing)
{
    m_textEditing = editing;
    updateActions();
}
void DomeMenu::updateActions()
{
    const bool idle = !m_files->busy(), local = !m_files->inTrash() && !m_files->inArchive();
    const bool selected = idle && m_files->selected() >= 0;
    const auto selection = m_files->selection();
    auto enable = [this](const QString &id, bool enabled) { m_actions[id]->setEnabled(enabled); };
    enable("open", selected && !m_files->inTrash() && selection.value("canOpen").toBool());
    enable("openTab", selected && !m_files->inTrash() && (selection.value("directory").toBool() || selection.value("archive").toBool()));
    enable("openWith", m_actions["open"]->isEnabled() && !selection.value("directory").toBool());
    for (const auto &id : {"rename", "compress", "copyLocation"}) enable(id, selected && (QString(id) == "copyLocation" || local));
    enable("cut", m_textEditing || (selected && local));
    enable("copy", m_textEditing || (selected && local));
    enable("paste", m_textEditing || (idle && local && m_files->canPaste()));
    enable("mkdir", idle && local);
    enable("extract", selected && local && selection.value("archive").toBool());
    enable("properties", selected);
    enable("trash", selected && local);
    m_actions["trash"]->setVisible(local);
    m_actions["delete"]->setShortcut(QKeySequence(m_files->inTrash() ? "Delete" : "Shift+Delete"));
    enable("delete", selected && !m_files->inArchive());
    enable("restore", selected && m_files->inTrash() && selection.value("canRestore").toBool());
    enable("emptyTrash", idle && m_files->inTrash());
    enable("terminal", idle && local);
    for (const auto &id : {"newTab", "refresh", "up", "home", "showTrash"}) enable(id, !m_files->operating());
    for (const auto &id : {"closeTab", "nextTab", "previousTab"}) enable(id, !m_files->operating() && m_files->tabs().size() > 1);
    for (const auto &layout : {"map", "tree", "disc"}) m_actions[layout]->setChecked(m_files->layout() == layout);
}
