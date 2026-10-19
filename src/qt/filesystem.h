// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <QAbstractListModel>
#include <QFutureWatcher>
#include <QFileSystemWatcher>
#include <QVariantMap>
#include <QVector3D>
#include <atomic>
#include <memory>
#include <functional>
#include <optional>

struct Entry {
    QString path, name, mime;
    QString trashDirectory, trashTop, originalPath, deletionDate;
    bool trashHome = false;
    qint64 bytes = 0;
    qint64 modifiedMs = 0;
    bool directory = false, symlink = false;
    int parent = -1, depth = 0;
    QVector3D center, extent;
};
struct ScanResult { QList<Entry> entries; QString warning; };

class FileIndex {
public:
    static QString directory();
    static std::optional<ScanResult> load(const QString &root, const QString &directory);
    static bool store(const QString &root, const ScanResult &result, const QString &directory);
};

class FileSystem : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString root READ root NOTIFY rootChanged)
    Q_PROPERTY(bool inTrash READ inTrash NOTIFY rootChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString warning READ warning NOTIFY warningChanged)
    Q_PROPERTY(int selected READ selected WRITE setSelected NOTIFY selectedChanged)
    Q_PROPERTY(QVariantMap selection READ selection NOTIFY selectedChanged)
    Q_PROPERTY(QString layout READ layout WRITE setLayout NOTIFY layoutChanged)
    Q_PROPERTY(QVariantList places READ places NOTIFY placesChanged)
    Q_PROPERTY(QVariantList tabs READ tabs NOTIFY tabsChanged)
    Q_PROPERTY(int currentTab READ currentTab NOTIFY currentTabChanged)
    Q_PROPERTY(QVariantMap viewState READ viewState WRITE setViewState NOTIFY viewStateChanged)
    Q_PROPERTY(bool operating READ operating NOTIFY busyChanged)
    Q_PROPERTY(bool cached READ cached NOTIFY busyChanged)
    Q_PROPERTY(QVariantMap treeRoot READ treeRoot NOTIFY treeGeometryChanged)
    Q_PROPERTY(QVariantList treeBranches READ treeBranches NOTIFY treeGeometryChanged)
public:
    enum Role { Path = Qt::UserRole + 1, Name, Mime, Bytes, Directory, Center, Extent, Tint, Thumbnail, Depth };
    explicit FileSystem(QObject *parent = nullptr);
    ~FileSystem() override;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    QString root() const { return m_root; }
    bool inTrash() const { return m_root == "trash:/"; }
    bool busy() const { return m_scanning || m_operating; }
    bool operating() const { return m_operating; }
    bool cached() const { return m_cached; }
    QString warning() const { return m_warning; }
    int selected() const { return m_selected; }
    void setSelected(int index);
    QVariantMap selection() const;
    QString layout() const { return m_layout; }
    void setLayout(const QString &value);
    QVariantList places() const { return m_places; }
    static QVariantList readPlaces(const QString &bookmarkFile, bool includeDevices = true);
    QVariantMap treeRoot() const { return m_treeRoot; }
    QVariantList treeBranches() const { return m_treeBranches; }
    Q_INVOKABLE void openPlace(const QString &path, bool inNewTab = false);
    QVariantList tabs() const;
    int currentTab() const { return m_currentTab; }
    QVariantMap viewState() const;
    void setViewState(const QVariantMap &state);
    Q_INVOKABLE void newTab(const QString &path = QString());
    Q_INVOKABLE void activateTab(int index);
    Q_INVOKABLE void closeTab(int index);
    Q_INVOKABLE void navigate(const QString &path);
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void up();
    Q_INVOKABLE void open(int index);
    Q_INVOKABLE void openWith(const QString &program);
    Q_INVOKABLE void clipboard(bool cut);
    Q_INVOKABLE bool canPaste() const;
    Q_INVOKABLE void paste();
    Q_INVOKABLE void rename(const QString &name);
    Q_INVOKABLE void newFolder(const QString &name);
    Q_INVOKABLE void remove(bool permanently = false);
    Q_INVOKABLE void restore();
    Q_INVOKABLE void emptyTrash();
    Q_INVOKABLE void copyLocation();
    Q_INVOKABLE void terminal();
    static ScanResult scan(const QString &root, const std::shared_ptr<std::atomic_bool> &cancel,
                           int maxDepth = 4, int maxEntries = 2500,
                           const std::function<void(const ScanResult &)> &progress = {});
    static QString copyEntry(const QString &source, const QString &destination);
signals:
    void rootChanged();
    void busyChanged();
    void warningChanged();
    void selectedChanged();
    void layoutChanged();
    void placesChanged();
    void tabsChanged();
    void currentTabChanged();
    void viewStateChanged();
    void indexRestored();
    void treeGeometryChanged();
    void error(const QString &message);
private:
    void arrange();
    void arrangeTree();
    void reloadPlaces();
    void requestScan(bool useIndex);
    void launchScan();
    void applyResult(const ScanResult &result, bool cached, bool watchChildren = false);
    void saveTab();
    QString selectedPath() const;
    QString targetDirectory() const;
    void operation(const std::function<QString()> &work);
    bool validName(const QString &name);
    QString m_root, m_warning, m_layout = "map";
    int m_selected = -1;
    QList<Entry> m_entries;
    QFutureWatcher<ScanResult> m_scan;
    QFutureWatcher<QString> m_operation;
    QFileSystemWatcher m_watcher;
    QFileSystemWatcher m_placesWatcher;
    QVariantList m_places;
    QVariantMap m_treeRoot;
    QVariantList m_treeBranches;
    struct Tab {
        QString root, layout = "map";
        ScanResult snapshot;
        QVariantMap view;
        QString selectedPath;
        bool loaded = false;
    };
    QList<Tab> m_tabs;
    int m_currentTab = 0;
    bool m_scanning = false, m_operating = false, m_cached = false;
    bool m_inFlight = false, m_pending = false, m_useIndex = false;
    quint64 m_generation = 0, m_scanGeneration = 0;
    std::shared_ptr<std::atomic_bool> m_cancel;
};
