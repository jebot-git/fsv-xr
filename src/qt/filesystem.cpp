// SPDX-License-Identifier: LGPL-2.1-or-later
#include "filesystem.h"
#include "archives.h"
#include "thumbnail.h"
#include "trash.h"
#include <QtConcurrent/QtConcurrentRun>
#include <QClipboard>
#include <QDrag>
#include <QWindow>
#include <QColor>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QGuiApplication>
#include <QMimeData>
#include <QMimeDatabase>
#include <QProcess>
#include <QTimer>
#include <QUrl>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QXmlStreamReader>
#include <QSet>
#include <QRectF>
#include <algorithm>
#include <limits>
#include <cmath>

namespace {
QString navigationTarget(const QString &path) {
    if (Archives::isLocation(path)) return path;
    const QFileInfo info(QUrl(path).isLocalFile() ? QUrl(path).toLocalFile() : path);
    if (info.isFile() && Archives::supported(info.filePath())) return Archives::location(info.absoluteFilePath());
    return path == "trash:/" ? path : info.absoluteFilePath();
}
bool readableLocation(const QString &path) {
    if (path == "trash:/") return true;
    if (Archives::isLocation(path)) return Archives::validLocation(path);
    const QFileInfo info(path); return info.isDir() && info.isReadable();
}
}

FileSystem::FileSystem(QObject *parent) : QAbstractListModel(parent)
{
    m_tabs.append(Tab{});
    reloadPlaces();
    connect(&m_placesWatcher, &QFileSystemWatcher::fileChanged, this, &FileSystem::reloadPlaces);
    connect(&m_placesWatcher, &QFileSystemWatcher::directoryChanged, this, &FileSystem::reloadPlaces);
    auto placesTimer = new QTimer(this);
    placesTimer->setInterval(5000);
    connect(placesTimer, &QTimer::timeout, this, [this] {
        reloadPlaces();
        if (inTrash() && !busy()) refresh();
    });
    placesTimer->start();
    connect(&m_scan, &QFutureWatcher<ScanResult>::finished, this, [this] {
        m_inFlight = false;
        if (m_scanGeneration == m_generation && !*m_cancel) {
            m_scanning = false;
            const auto result = m_scan.result();
            if (!result.rootRedirect.isEmpty()) {
                m_root = result.rootRedirect;
                m_tabs[m_currentTab].root = m_root;
                m_tabs[m_currentTab].reveal = result.revealPath;
                emit rootChanged(); emit tabsChanged();
            }
            applyResult(result, false, true);
            saveTab();
            auto &tab = m_tabs[m_currentTab];
            if (!tab.reveal.isEmpty()) {
                const QString requested = tab.reveal;
                const bool found = selectedPath() == requested;
                const bool show = tab.showProperties;
                tab.reveal.clear(); tab.showProperties = false;
                if (!found) emit error("The requested item is not in the displayed directory. The view may be limited; refresh or open its parent directory: " + requested);
                else if (show) emit propertiesRequested();
            }
        }
        if (m_pending) launchScan();
        else emit busyChanged();
    });
    connect(&m_operation, &QFutureWatcher<QString>::finished, this, [this] {
        m_operating = false;
        if (!m_operation.result().isEmpty()) emit error(m_operation.result());
        refresh();
        emit busyChanged();
    });
    connect(&m_archiveOpen, &QFutureWatcher<Archives::Materialized>::finished, this, [this] {
        const auto result = m_archiveOpen.result();
        m_operating = false;
        emit busyChanged();
        if (!result.error.isEmpty()) emit error(result.error);
        else if (!m_openProgram.isEmpty()) {
            if (!QProcess::startDetached(m_openProgram, {result.path})) emit error("Could not start " + m_openProgram);
        } else if (!QDesktopServices::openUrl(QUrl::fromLocalFile(result.path))) emit error("No application could open the archive file.");
    });
    auto debounce = new QTimer(this);
    debounce->setSingleShot(true);
    debounce->setInterval(400);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, debounce, [debounce] { debounce->start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, debounce, [debounce] { debounce->start(); });
    connect(debounce, &QTimer::timeout, this, [this, debounce] {
        if (busy()) debounce->start(); else refresh();
    });
}
FileSystem::~FileSystem()
{
    if (m_cancel) *m_cancel = true;
    m_scan.waitForFinished();
    m_operation.waitForFinished();
    if (m_openCancel) *m_openCancel = true;
    m_archiveOpen.waitForFinished();
}
int FileSystem::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_entries.size(); }
QHash<int, QByteArray> FileSystem::roleNames() const
{
    return {{Path, "filePath"}, {Name, "fileName"}, {Mime, "mimeType"}, {Bytes, "fileBytes"},
            {Directory, "isDirectory"}, {Center, "center"}, {Extent, "extent"},
            {Tint, "tint"}, {Thumbnail, "thumbnail"}, {Depth, "depth"}};
}
QVariant FileSystem::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size()) return {};
    const auto &e = m_entries[index.row()];
    switch (role) {
    case Path: return e.path;
    case Name: return e.name;
    case Mime: return e.mime;
    case Bytes: return e.bytes;
    case Directory: return e.directory;
    case Center: return QVariant::fromValue(e.center);
    case Extent: return QVariant::fromValue(e.extent);
    case Depth: return e.depth;
    case Tint:
        if (e.directory) return QColor("#dba43a");
        if (e.mime.startsWith("image/")) return QColor("#6bbf94");
        if (e.mime.startsWith("video/")) return QColor("#b580cd");
        if (e.mime.startsWith("audio/")) return QColor("#dba361");
        return QColor("#739fbe");
    case Thumbnail: return ThumbnailProvider::url(e.path, inArchive() ? QFileInfo(Archives::source(m_root)).lastModified().toMSecsSinceEpoch() : e.modifiedMs);
    default: return {};
    }
}

QVariantList FileSystem::readPlaces(const QString &bookmarkFile, bool includeDevices)
{
    QVariantList result;
    QSet<QString> seen;
    auto add = [&](const QString &name, const QString &path, const QString &icon, const QString &group) {
        const QFileInfo info(path);
        const QString clean = QDir::cleanPath(info.absoluteFilePath());
        if (path.isEmpty() || !info.isDir() || !info.isReadable() || seen.contains(clean)) return;
        seen.insert(clean);
        result.append(QVariantMap{{"name", name.isEmpty() ? info.fileName() : name},
            {"path", clean}, {"icon", icon.isEmpty() ? QStringLiteral("folder") : icon}, {"group", group}});
    };
    QFile file(bookmarkFile);
    bool loaded = false, trashAdded = false, trashHidden = false;
    if (file.open(QIODevice::ReadOnly) && file.size() <= 4 * 1024 * 1024) {
        QXmlStreamReader xml(&file);
        bool xbel = false;
        while (!xml.atEnd()) {
            xml.readNext();
            if (!xml.isStartElement()) continue;
            if (xml.name() == u"xbel") xbel = true;
            if (!xbel || xml.name() != u"bookmark") continue;
            const QUrl url(xml.attributes().value("href").toString());
            QString title, icon;
            bool hidden = false, privateEntry = false;
            int depth = 1;
            while (depth && !xml.atEnd()) {
                xml.readNext();
                if (xml.isEndElement()) { --depth; continue; }
                if (!xml.isStartElement()) continue;
                ++depth;
                if (xml.name() == u"icon") icon = xml.attributes().value("name").toString();
                if (xml.name() == u"title" || xml.name() == u"IsHidden" || xml.name() == u"OnlyInApp") {
                    const auto name = xml.name().toString();
                    const auto value = xml.readElementText(); --depth;
                    if (name == "title") title = value;
                    else if (name == "IsHidden") hidden = value.compare("true", Qt::CaseInsensitive) == 0;
                    else privateEntry = !value.isEmpty() && value != "fsvr";
                }
            }
            if (url.scheme() == "trash" && !privateEntry) {
                trashHidden = hidden;
                if (!hidden && !trashAdded) {
                    result.append(QVariantMap{{"name", title.isEmpty() ? tr("Trash") : title},
                        {"path", "trash:/"}, {"icon", "user-trash"}, {"group", tr("Places")}});
                    trashAdded = true;
                }
            }
            // Other virtual KIO locations are not local directories.
            if (url.isLocalFile() && !hidden && !privateEntry)
                add(title, url.toLocalFile(), icon, tr("Places"));
        }
        loaded = xbel && !xml.hasError();
        if (!loaded) { result.clear(); seen.clear(); trashAdded = false; trashHidden = false; }
    }
    if (!loaded) {
        add(tr("Home"), QDir::homePath(), "user-home", tr("Places"));
        const QList<std::pair<QStandardPaths::StandardLocation, QString>> defaults = {
            {QStandardPaths::DesktopLocation, "user-desktop"}, {QStandardPaths::DocumentsLocation, "folder-documents"},
            {QStandardPaths::DownloadLocation, "folder-downloads"}, {QStandardPaths::MusicLocation, "folder-music"},
            {QStandardPaths::PicturesLocation, "folder-pictures"}, {QStandardPaths::MoviesLocation, "folder-videos"}};
        for (const auto &place : defaults)
            add(QStandardPaths::displayName(place.first), QStandardPaths::writableLocation(place.first), place.second, tr("Places"));
    }
    if (!trashAdded && !trashHidden)
        result.append(QVariantMap{{"name", tr("Trash")}, {"path", "trash:/"}, {"icon", "user-trash"}, {"group", tr("Places")}});
    if (includeDevices) {
        add(tr("File System"), QDir::rootPath(), "drive-harddisk", tr("Devices"));
        for (const auto &volume : QStorageInfo::mountedVolumes()) {
            const QString path = volume.rootPath();
            // Only user-facing mounted volumes; omit OS bind/subvolume mounts.
            if (!volume.isValid() || !volume.isReady() || !(path.startsWith("/run/media/")
                || path.startsWith("/media/") || path.startsWith("/mnt/"))) continue;
            add(volume.displayName(), path, "drive-removable-media", tr("Devices"));
        }
    }
    return result;
}

void FileSystem::openPlace(const QString &path, bool inNewTab)
{
    if (inNewTab) newTab(path);
    else navigate(path);
}

void FileSystem::reloadPlaces()
{
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString path = directory + "/user-places.xbel";
    const auto next = readPlaces(path);
    if (next != m_places) { m_places = next; emit placesChanged(); }
    // Reattach after Dolphin's atomic file replacement; watch creation too.
    for (const auto &watch : {directory, path})
        if (QFileInfo::exists(watch) && !m_placesWatcher.files().contains(watch)
            && !m_placesWatcher.directories().contains(watch)) m_placesWatcher.addPath(watch);
}

ScanResult FileSystem::scan(const QString &root, const std::shared_ptr<std::atomic_bool> &cancel, int maxDepth, int maxEntries,
    const std::function<void(const ScanResult &)> &progress)
{
    ScanResult result;
    QMimeDatabase db;
    bool truncated = false, unreadable = false;
    std::function<void(const QString &, int, int)> visit = [&](const QString &path, int parent, int depth) {
        if (*cancel) return;
        if (!QFileInfo(path).isReadable()) { unreadable = true; return; }
        // Iterator avoids allocating an unbounded directory listing before enforcing the limit.
        QDirIterator it(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        QList<int> directories;
        while (it.hasNext() && !*cancel) {
            if (result.entries.size() >= maxEntries) { truncated = true; break; }
            it.next();
            const QFileInfo info = it.fileInfo();
            Entry e;
            e.path = info.absoluteFilePath(); e.name = info.fileName();
            e.directory = info.isDir(); e.symlink = info.isSymLink();
            e.parent = parent; e.depth = depth;
            e.bytes = e.directory ? 0 : qMax<qint64>(0, info.size());
            e.modifiedMs = info.lastModified().toMSecsSinceEpoch();
            e.mime = e.directory ? "inode/directory" : db.mimeTypeForFile(info, QMimeDatabase::MatchExtension).name();
            const int index = result.entries.size();
            result.entries << e;
            if (progress && parent == -1 && result.entries.size() == 128 && !*cancel) progress(result);
            if (e.directory && !e.symlink) directories << index;
        }
        if (progress && parent == -1 && result.entries.size() != 128 && !*cancel) progress(result);
        for (int index : directories) {
            if (depth < maxDepth) visit(result.entries[index].path, index, depth + 1);
            else truncated = true;
        }
    };
    visit(root, -1, 0);
    // Reverse order ensures child sizes are included before their parents.
    for (int i = result.entries.size() - 1; i >= 0; --i) {
        auto &e = result.entries[i];
        if (e.parent >= 0) result.entries[e.parent].bytes += e.bytes;
    }
    if (truncated) result.warning = "View limited to " + QString::number(maxEntries) + " entries / "
        + QString::number(maxDepth + 1) + " levels. Open a directory to explore further. Sizes cover scanned entries.";
    if (unreadable) result.warning += " Some directories could not be read.";
    return result;
}

void FileSystem::arrange()
{
    m_treeRoot.clear(); m_treeBranches.clear();
    QList<QList<int>> children(m_entries.size() + 1);
    for (int i = 0; i < m_entries.size(); ++i) children[m_entries[i].parent + 1] << i;
    if (m_layout == "tree") { arrangeTree(); emit treeGeometryChanged(); return; }
    std::function<void(int, float, float, float, float, float)> place =
        [&](int parent, float x, float z, float w, float d, float base) {
        const auto &list = children[parent + 1];
        if (list.isEmpty()) return;
        double largestWeight = 0;
        for (int i : list) largestWeight = qMax(largestWeight, std::sqrt(double(qMax<qint64>(4096, m_entries[i].bytes))));
        auto weight = [&](int i) {
            const double value = std::sqrt(double(qMax<qint64>(4096, m_entries[i].bytes)));
            return m_layout == "map" ? qMax(value, largestWeight / 4) : value;
        };
        double total = 0;
        for (int i : list) total += weight(i);
        QHash<int, QRectF> mapCells;
        if (m_layout == "map") {
            // Squarified treemap: choose rows by their worst aspect ratio,
            // rather than slicing every sibling into a long narrow strip.
            QList<int> pending = list;
            std::stable_sort(pending.begin(), pending.end(), [&](int a, int b) { return m_entries[a].bytes > m_entries[b].bytes; });
            auto area = [&](int i) { return double(w) * d * weight(i) / total; };
            QRectF remaining(x, z, w, d);
            QList<int> row;
            auto worst = [&](const QList<int> &items) {
                double sum = 0, smallest = std::numeric_limits<double>::max(), largest = 0;
                for (int i : items) { const double a = area(i); sum += a; smallest = qMin(smallest, a); largest = qMax(largest, a); }
                const double side = qMin(remaining.width(), remaining.height());
                return qMax(side * side * largest / (sum * sum), sum * sum / (side * side * smallest));
            };
            auto placeRow = [&] {
                double sum = 0; for (int i : row) sum += area(i);
                if (remaining.width() >= remaining.height()) {
                    const double width = sum / remaining.height();
                    double top = remaining.top();
                    for (int i : row) { const double height = area(i) / width; mapCells.insert(i, QRectF(remaining.left(), top, width, height)); top += height; }
                    remaining.setLeft(remaining.left() + width);
                } else {
                    const double height = sum / remaining.width();
                    double left = remaining.left();
                    for (int i : row) { const double width = area(i) / height; mapCells.insert(i, QRectF(left, remaining.top(), width, height)); left += width; }
                    remaining.setTop(remaining.top() + height);
                }
                row.clear();
            };
            for (int next : pending) {
                auto expanded = row; expanded << next;
                if (!row.isEmpty() && worst(expanded) > worst(row)) placeRow();
                row << next;
            }
            if (!row.isEmpty()) placeRow();
        }
        for (int j = 0; j < list.size(); ++j) {
            const int i = list[j]; auto &e = m_entries[i];
            float ew, ed, ex, ez;
            if (m_layout == "disc") {
                const float a = 6.2831853f * j / list.size();
                const float radius = qMin(w, d) * .34f;
                ew = ed = qMin(w, d) / (2.5f + std::sqrt(float(list.size())) * 1.5f);
                ex = x + w / 2 + std::cos(a) * radius - ew / 2;
                ez = z + d / 2 + std::sin(a) * radius - ed / 2;
            } else {
                const auto cell = mapCells.value(i);
                ew = cell.width(); ed = cell.height(); ex = cell.x(); ez = cell.y();
            }
            const float height = e.directory ? 1.8f : 2.0f + qMin(15.0f, float(std::log2(1.0 + double(e.bytes))) * .55f);
            e.extent = {qMax(.15f, ew * .94f), height, qMax(.15f, ed * .94f)};
            e.center = {ex + ew / 2, base + height / 2, ez + ed / 2};
            if (e.directory) place(i, ex + ew * .04f, ez + ed * .04f, ew * .92f, ed * .68f, base + height + .5f);
        }
    };
    place(-1, -80, -60, 160, 120, 0);
    emit treeGeometryChanged();
}
void FileSystem::setLayout(const QString &value)
{
    if (m_layout == value || (value != "map" && value != "tree" && value != "disc")) return;
    m_layout = value;
    beginResetModel(); arrange(); endResetModel();
    emit layoutChanged(); emit selectedChanged();
}
void FileSystem::navigate(const QString &path)
{
    const QString target = navigationTarget(path);
    if (!readableLocation(target)) { emit error("Cannot read location: " + path); return; }
    if (m_operating) { emit error("Wait for the current operation to finish."); return; }
    m_tabs[m_currentTab].reveal.clear(); m_tabs[m_currentTab].showProperties = false;
    if (m_root == target) { refresh(); return; }
    m_root = target;
    m_tabs[m_currentTab].root = m_root;
    m_tabs[m_currentTab].loaded = false;
    m_selected = -1;
    applyResult({}, false);
    emit rootChanged(); emit tabsChanged(); emit selectedChanged();
    requestScan(true);
}
void FileSystem::openLocation(const QString &location, bool selectItem, bool properties, bool inNewTab)
{
    if (Archives::isLocation(location)) {
        const bool reveal = (selectItem || properties) && !Archives::member(location).isEmpty();
        const QString target = reveal ? Archives::parent(location) : location;
        if (inNewTab) newTab(target); else navigate(target);
        if (reveal && m_root == target) {
            m_tabs[m_currentTab].reveal = location;
            m_tabs[m_currentTab].showProperties = properties;
        }
        return;
    }
    if (location == "trash:/" || location == "trash:///") {
        if (inNewTab) newTab("trash:/"); else navigate("trash:/");
        return;
    }
    const QUrl url(location);
    if (!url.scheme().isEmpty() && !url.isLocalFile()) { emit error("Only local file URLs and trash:/ are supported: " + location); return; }
    const QFileInfo info(url.isLocalFile() ? url.toLocalFile() : location);
    if (!info.exists() && !info.isSymLink()) { emit error("Location does not exist: " + location); return; }
    if (!selectItem && !properties && info.isFile() && Archives::supported(info.filePath())) {
        if (inNewTab) newTab(Archives::location(info.absoluteFilePath())); else navigate(Archives::location(info.absoluteFilePath()));
        return;
    }
    const bool reveal = selectItem || properties || !info.isDir();
    const QString directory = reveal ? info.absolutePath() : info.absoluteFilePath();
    if (inNewTab) newTab(directory); else navigate(directory);
    if (m_root != directory) return;
    if (reveal) {
        m_tabs[m_currentTab].reveal = info.absoluteFilePath();
        m_tabs[m_currentTab].showProperties = properties;
    }
}
void FileSystem::refresh()
{
    if (m_operating || m_root.isEmpty()) return;
    requestScan(false);
}
void FileSystem::requestScan(bool useIndex)
{
    ++m_generation;
    if (m_cancel) *m_cancel = true;
    m_pending = true; m_useIndex = useIndex; m_scanning = true;
    if (!m_inFlight) launchScan();
    emit busyChanged();
}
void FileSystem::launchScan()
{
    m_pending = false; m_inFlight = true;
    m_scanGeneration = m_generation;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_cancel;
    const auto generation = m_generation;
    const QString root = m_root, directory = FileIndex::directory();
    const bool useIndex = m_useIndex;
    const bool haveScene = !m_entries.isEmpty();
    m_scan.setFuture(QtConcurrent::run([this, root, directory, cancel, generation, useIndex, haveScene] {
        auto publish = [this, cancel, generation](const ScanResult &result, bool indexed) {
            if (*cancel) return;
            QMetaObject::invokeMethod(this, [this, generation, result, indexed] {
                if (generation != m_generation) return;
                applyResult(result, indexed);
                if (indexed) emit indexRestored();
            }, Qt::QueuedConnection);
        };
        if (Archives::isLocation(root)) return Archives::browse(root, cancel, !useIndex);
        if (root == "trash:/") return TrashStore::scan(TrashStore::locations(), cancel);
        bool restored = haveScene;
        if (useIndex && !haveScene && !*cancel) {
            if (auto snapshot = FileIndex::load(root, directory)) { publish(*snapshot, true); restored = true; }
        }
        const auto result = scan(root, cancel, 4, 2500,
            restored ? std::function<void(const ScanResult &)>{} : [publish](const ScanResult &partial) { publish(partial, false); });
        if (!*cancel) FileIndex::store(root, result, directory);
        return result;
    }));
}
void FileSystem::applyResult(const ScanResult &result, bool cached, bool watchChildren)
{
    const QString selection = m_tabs[m_currentTab].reveal.isEmpty() ? selectedPath() : m_tabs[m_currentTab].reveal;
    bool changed = result.entries.size() != m_entries.size()
        || (m_layout == "tree" && m_treeRoot.value("path").toString() != m_root);
    for (int i = 0; !changed && i < m_entries.size(); ++i) {
        const auto &a = result.entries[i], &b = m_entries[i];
        changed = a.path != b.path || a.mime != b.mime || a.bytes != b.bytes || a.modifiedMs != b.modifiedMs
            || a.originalPath != b.originalPath || a.deletionDate != b.deletionDate
            || a.parent != b.parent || a.depth != b.depth || a.directory != b.directory || a.symlink != b.symlink;
    }
    if (changed) {
        beginResetModel();
        m_entries = result.entries; m_selected = -1;
        for (int i = 0; i < m_entries.size(); ++i) if (m_entries[i].path == selection) m_selected = i;
        arrange(); endResetModel();
        emit selectedChanged();
    }
    if (!changed && !m_tabs[m_currentTab].reveal.isEmpty()) {
        for (int i = 0; i < m_entries.size(); ++i) if (m_entries[i].path == selection) setSelected(i);
    }
    if (m_warning != result.warning) { m_warning = result.warning; emit warningChanged(); }
    m_cached = cached;
    if (watchChildren || !m_watcher.directories().contains(m_root)) {
        if (!m_watcher.directories().isEmpty()) m_watcher.removePaths(m_watcher.directories());
        if (!m_watcher.files().isEmpty()) m_watcher.removePaths(m_watcher.files());
        QStringList paths;
        if (inTrash()) paths = TrashStore::watchPaths();
        else if (inArchive()) {
            paths << Archives::source(m_root) << QFileInfo(Archives::source(m_root)).absolutePath();
        } else if (!m_root.isEmpty()) paths << m_root;
        if (watchChildren && !inTrash() && !inArchive()) for (const auto &e : m_entries)
            if (e.directory && !e.symlink && paths.size() < 256) paths << e.path;
        if (!paths.isEmpty()) m_watcher.addPaths(paths);
    }
    emit busyChanged();
}

QVariantList FileSystem::tabs() const
{
    QVariantList result;
    for (const auto &tab : m_tabs) result.append(QVariantMap{{"path", tab.root},
        {"title", tab.root.isEmpty() ? tr("New Tab") : tab.root == "trash:/" ? tr("Trash") : Archives::isLocation(tab.root) ? Archives::title(tab.root) : tab.root == "/" ? QString("/") : QFileInfo(tab.root).fileName()}});
    return result;
}
QVariantMap FileSystem::viewState() const { return m_tabs[m_currentTab].view; }
void FileSystem::setViewState(const QVariantMap &state)
{
    if (m_tabs[m_currentTab].view == state) return;
    m_tabs[m_currentTab].view = state; emit viewStateChanged();
}
void FileSystem::saveTab()
{
    if (m_currentTab < 0 || m_currentTab >= m_tabs.size()) return;
    auto &tab = m_tabs[m_currentTab];
    tab.root = m_root; tab.layout = m_layout; tab.selectedPath = selectedPath();
    tab.snapshot = {m_entries, m_warning, {}, {}}; tab.loaded = !m_root.isEmpty();
}
void FileSystem::newTab(const QString &path)
{
    if (m_operating) { emit error("Wait for the current operation to finish."); return; }
    if (m_tabs.size() >= 32) { emit error("Up to 32 tabs can be open at once."); return; }
    const QString target = navigationTarget(path.isEmpty() ? (m_root.isEmpty() ? QDir::homePath() : m_root) : path);
    if (!readableLocation(target)) { emit error("Cannot read location: " + target); return; }
    Tab tab; tab.root = target;
    m_tabs.append(tab);
    activateTab(m_tabs.size() - 1);
}
void FileSystem::activateTab(int index)
{
    if (index < 0 || index >= m_tabs.size() || index == m_currentTab || m_operating) return;
    saveTab();
    m_currentTab = index;
    const auto tab = m_tabs[index];
    m_root = tab.root; m_layout = tab.layout;
    m_selected = -1;
    // Clear the old model so its selection and geometry cannot leak into a tab.
    beginResetModel(); m_entries.clear(); endResetModel();
    applyResult(tab.loaded ? tab.snapshot : ScanResult{}, tab.loaded);
    for (int i = 0; i < m_entries.size(); ++i) if (m_entries[i].path == tab.selectedPath) m_selected = i;
    emit rootChanged(); emit layoutChanged(); emit tabsChanged(); emit currentTabChanged();
    emit selectedChanged(); emit viewStateChanged();
    requestScan(!tab.loaded);
}
void FileSystem::closeTab(int index)
{
    if (m_tabs.size() <= 1 || index < 0 || index >= m_tabs.size() || m_operating) return;
    if (index == m_currentTab) {
        m_tabs.removeAt(index);
        m_currentTab = -1;
        activateTab(qMin(index, int(m_tabs.size()) - 1));
    } else {
        m_tabs.removeAt(index);
        if (index < m_currentTab) --m_currentTab;
        emit tabsChanged(); emit currentTabChanged();
    }
}
void FileSystem::up() { navigate(inArchive() ? Archives::parent(m_root) : inTrash() ? QDir::homePath() : QFileInfo(m_root).dir().absolutePath()); }
void FileSystem::setSelected(int index)
{
    if (index < -1 || index >= m_entries.size() || m_selected == index) return;
    m_selected = index; emit selectedChanged();
}
QString FileSystem::selectedPath() const { return m_selected < 0 ? QString() : m_entries[m_selected].path; }
QVariantMap FileSystem::selection() const
{
    if (m_selected < 0) return {};
    const auto &e = m_entries[m_selected]; const QFileInfo info(e.path);
    return {{"trashed", !e.trashDirectory.isEmpty()}, {"originalPath", e.originalPath},
            {"deleted", e.deletionDate}, {"canRestore", !e.originalPath.isEmpty()},
            {"path", e.path}, {"name", e.name}, {"mime", e.mime}, {"bytes", e.bytes},
            {"archive", !inArchive() && !e.directory && Archives::supported(e.path)}, {"canOpen", !inArchive() || !e.symlink}, {"directory", e.directory}, {"symlink", e.symlink}, {"modified", inArchive() ? (e.modifiedMs ? QDateTime::fromMSecsSinceEpoch(e.modifiedMs).toString(Qt::ISODate) : QString()) : info.lastModified().toString(Qt::ISODate)},
            {"owner", inArchive() ? QString() : info.owner()}, {"permissions", inArchive() ? QString() : QString::number(uint(info.permissions()), 16)},
            {"thumbnail", ThumbnailProvider::url(e.path, inArchive() ? QFileInfo(Archives::source(m_root)).lastModified().toMSecsSinceEpoch() : e.modifiedMs, inArchive())}, {"center", QVariant::fromValue(e.center)},
            {"extent", QVariant::fromValue(e.extent)}};
}
void FileSystem::open(int index)
{
    if (index < 0 || index >= m_entries.size()) return;
    const auto e = m_entries[index];
    if (inTrash()) { setSelected(index); restore(); return; }
    if (inArchive()) {
        if (e.symlink) { emit error("Links and special files cannot be opened from an archive."); return; }
        if (e.directory) navigate(e.path); else openArchiveMember(e.path);
    } else if (e.directory) navigate(e.path);
    else if (Archives::supported(e.path)) navigate(Archives::location(e.path));
    else if (!QDesktopServices::openUrl(QUrl::fromLocalFile(e.path))) emit error("No application could open " + e.name);
}
void FileSystem::browseArchive()
{
    if (inTrash() || inArchive() || selectedPath().isEmpty() || !selection().value("archive").toBool()) return;
    navigate(Archives::location(selectedPath()));
}
void FileSystem::openArchiveMember(const QString &path, const QString &program)
{
    if (busy()) { emit error("Wait for the current operation to finish."); return; }
    m_openProgram = program;
    m_openCancel = std::make_shared<std::atomic_bool>(false);
    const auto cancel = m_openCancel;
    m_operating = true;
    m_archiveOpen.setFuture(QtConcurrent::run([path, cancel] { return Archives::materialize(path, cancel); }));
    emit busyChanged();
}
void FileSystem::openWith(const QString &program)
{
    if (inTrash() || selectedPath().isEmpty()) return;
    if (inArchive()) { openArchiveMember(selectedPath(), program); return; }
    if (!QProcess::startDetached(program, {selectedPath()})) emit error("Could not start " + program);
}
QString FileSystem::targetDirectory() const
{
    if (m_selected >= 0 && m_entries[m_selected].directory) return selectedPath();
    return m_root;
}
void FileSystem::clipboard(bool cut)
{
    if (inArchive() || inTrash() || selectedPath().isEmpty()) return;
    auto *mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(selectedPath())});
    mime->setData("application/x-kde-cutselection", cut ? "1" : "0");
    mime->setData("x-special/gnome-copied-files", (cut ? QByteArray("cut\n") : QByteArray("copy\n")) + QUrl::fromLocalFile(selectedPath()).toEncoded());
    QGuiApplication::clipboard()->setMimeData(mime);
}
bool FileSystem::canPaste() const
{
    const auto *data = QGuiApplication::clipboard()->mimeData();
    return !inArchive() && !inTrash() && data && data->hasUrls();
}

QString FileSystem::copyEntry(const QString &source, const QString &destination)
{
    const QFileInfo src(source), dst(destination);
    const QString canonicalSource = src.canonicalFilePath();
    const QString canonicalParent = dst.dir().canonicalPath();
    if (src.isDir() && !src.isSymLink() && (canonicalParent == canonicalSource || canonicalParent.startsWith(canonicalSource + '/')))
        return "Cannot copy a directory into itself.";
    if (dst.exists() || dst.isSymLink()) return "Destination already exists: " + destination;
    // Symlinks remain links; never recurse through them.
    if (src.isSymLink()) {
        if (!QFile::link(src.symLinkTarget(), destination)) return "Cannot copy symlink: " + source;
    } else if (src.isDir()) {
        if (!QDir().mkdir(destination)) return "Cannot create directory: " + destination;
        QDirIterator it(source, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        while (it.hasNext()) {
            it.next(); const auto error = copyEntry(it.filePath(), destination + '/' + it.fileName());
            if (!error.isEmpty()) return error; // Keep successful copies; never erase source on a partial failure.
        }
    } else if (!src.isFile() || !QFile::copy(source, destination)) {
        return "Cannot copy file: " + source;
    }
    return {};
}
void FileSystem::operation(const std::function<QString()> &work)
{
    if (busy()) { emit error(m_operating ? "Wait for the current operation to finish." : "Wait for the directory refresh to finish."); return; }
    m_operating = true;
    m_operation.setFuture(QtConcurrent::run(work)); emit busyChanged();
}
void FileSystem::extractArchive()
{
    if (inArchive() || inTrash() || selectedPath().isEmpty()) return;
    const QString path = selectedPath();
    operation([path] { return Archives::extract(path); });
}
void FileSystem::compress(const QString &name)
{
    if (inArchive() || inTrash() || selectedPath().isEmpty() || !validName(name)) return;
    const QString source = selectedPath();
    const QString destination = QFileInfo(source).dir().filePath(name);
    operation([source, destination] { return Archives::create(source, destination); });
}
void FileSystem::paste()
{
    if (inArchive() || inTrash()) return;
    const auto *mime = QGuiApplication::clipboard()->mimeData();
    const auto urls = mime->urls();
    const bool cut = mime->data("application/x-kde-cutselection") == "1" || mime->data("x-special/gnome-copied-files").startsWith("cut\n");
    const QString directory = targetDirectory();
    operation([urls, cut, directory] { return transferFiles(urls, directory, cut); });
}
QString FileSystem::dropDirectory(int index) const
{
    if (inArchive()) return {};
    return index >= 0 && index < m_entries.size() && m_entries[index].directory ? m_entries[index].path : m_root;
}
void FileSystem::startDrag()
{
    if (busy() || inArchive() || inTrash() || selectedPath().isEmpty()) return;
    auto *window = QGuiApplication::focusWindow();
    if (!window) return;
    auto *drag = new QDrag(window);
    auto *mime = new QMimeData;
    mime->setUrls({QUrl::fromLocalFile(selectedPath())});
    drag->setMimeData(mime);
    // File managers handle filesystem moves at the destination. Never delete
    // the source in response to the returned action (a target may have moved it).
    drag->exec(Qt::CopyAction | Qt::MoveAction, Qt::CopyAction);
    drag->deleteLater();
    refresh();
}
bool FileSystem::canDrop(const QList<QUrl> &urls, const QString &directory) const
{
    if (busy() || inArchive() || inTrash() || Archives::isLocation(directory) || urls.isEmpty()) return false;
    const QFileInfo target(directory);
    const QString parent = target.canonicalFilePath();
    if (!target.isDir() || !target.isWritable() || parent.isEmpty()) return false;
    QSet<QString> destinations;
    for (const auto &url : urls) {
        if (!url.isLocalFile()) return false;
        const QFileInfo source(url.toLocalFile());
        if ((!source.exists() && !source.isSymLink()) || source.fileName().isEmpty()) return false;
        const QString destination = QDir(directory).filePath(source.fileName());
        const QFileInfo dst(destination);
        if (dst.exists() || dst.isSymLink() || destinations.contains(destination)) return false;
        destinations.insert(destination);
        const QString canonical = source.canonicalFilePath();
        if (source.isDir() && !source.isSymLink()
            && (parent == canonical || parent.startsWith(canonical + '/'))) return false;
    }
    return true;
}
bool FileSystem::dropFiles(const QList<QUrl> &urls, const QString &directory, int action)
{
    if ((action != Qt::CopyAction && action != Qt::MoveAction) || !canDrop(urls, directory)) return false;
    operation([urls, directory, action] { return transferFiles(urls, directory, action == Qt::MoveAction); });
    return true;
}
QString FileSystem::transferFiles(const QList<QUrl> &urls, const QString &directory, bool move)
{
    if (Archives::isLocation(directory) || !QFileInfo(directory).isDir()) return "Destination is not a directory: " + directory;
    // Validate the entire batch before changing any files.
    QSet<QString> destinations;
    for (const auto &url : urls) {
        if (!url.isLocalFile()) return "Only local file URLs can be transferred.";
        const QFileInfo src(url.toLocalFile());
        const QString dst = QDir(directory).filePath(src.fileName());
        if ((!src.exists() && !src.isSymLink()) || src.fileName().isEmpty()) return "Source does not exist: " + src.filePath();
        if (QFileInfo::exists(dst) || QFileInfo(dst).isSymLink() || destinations.contains(dst)) return "Destination already exists: " + dst;
        destinations.insert(dst);
        const QString parent = QFileInfo(directory).canonicalFilePath(), canonical = src.canonicalFilePath();
        if (src.isDir() && !src.isSymLink() && (parent == canonical || parent.startsWith(canonical + '/')))
            return "Cannot transfer a directory into itself.";
    }
    for (const auto &url : urls) {
        const QString src = url.toLocalFile(), dst = QDir(directory).filePath(QFileInfo(src).fileName());
        if (move) {
            // Cross-device moves fail without risking source loss.
            if (!QDir().rename(src, dst)) return "Cannot move (permissions or different filesystem): " + src;
        } else {
            const auto error = copyEntry(src, dst);
            if (!error.isEmpty()) return error;
        }
    }
    return {};
}

bool FileSystem::validName(const QString &name)
{
    if (name.isEmpty() || name == "." || name == ".." || name.contains('/') || name.contains(QChar(0))) {
        emit error("Enter a single, non-empty filename."); return false;
    }
    return true;
}
void FileSystem::rename(const QString &name)
{
    if (inArchive() || inTrash()) return;
    const QString source = selectedPath();
    if (source.isEmpty() || !validName(name)) return;
    const QString destination = QFileInfo(source).dir().filePath(name);
    operation([source, destination] {
        if (QFileInfo::exists(destination) || QFileInfo(destination).isSymLink() || !QDir().rename(source, destination))
            return QString("Cannot rename to ") + destination;
        return QString();
    });
}
void FileSystem::newFolder(const QString &name)
{
    if (inArchive() || inTrash()) return;
    if (!validName(name)) return;
    const QString directory = targetDirectory();
    operation([directory, name] { return QDir(directory).mkdir(name) ? QString() : "Cannot create folder: " + name; });
}
void FileSystem::remove(bool permanently)
{
    if (inArchive()) return;
    const QString path = selectedPath(); if (path.isEmpty()) return;
    if (inTrash()) {
        if (permanently) { const auto entry = m_entries[m_selected]; operation([entry] { return TrashStore::erase(entry); }); }
        return;
    }
    operation([path, permanently] {
        const QFileInfo info(path);
        bool ok;
        if (!permanently) ok = QFile::moveToTrash(path);
        else if (info.isDir() && !info.isSymLink()) ok = QDir(path).removeRecursively();
        else ok = QFile::remove(path);
        return ok ? QString() : "Cannot remove: " + path;
    });
}
void FileSystem::restore()
{
    if (!inTrash() || m_selected < 0) return;
    const auto entry = m_entries[m_selected];
    operation([entry] { return TrashStore::restore(entry); });
}
void FileSystem::emptyTrash()
{
    if (!inTrash()) return;
    operation([] { return TrashStore::empty(TrashStore::locations()); });
}
void FileSystem::copyLocation() { if (!selectedPath().isEmpty()) QGuiApplication::clipboard()->setText(selectedPath()); }
void FileSystem::terminal()
{
    if (inArchive() || inTrash()) return;
    if (!QProcess::startDetached("konsole", {"--workdir", targetDirectory()})) emit error("Could not start Konsole.");
}
