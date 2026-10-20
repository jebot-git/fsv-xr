// SPDX-License-Identifier: LGPL-2.1-or-later
#include "filesystem.h"
#include "archives.h"
#include <archive.h>
#include <archive_entry.h>
#include "trash.h"
#include "launchcontext.h"
#include "gestures.h"
#include "thumbnail.h"
#include <QDesktopServices>
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QSignalSpy>
#include <QGuiApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QRectF>
#include <QStandardPaths>
#include <QScopeGuard>

class ArchiveOpenReceiver : public QObject {
    Q_OBJECT
public:
    QUrl url;
public slots:
    void receive(const QUrl &value) { url = value; }
};

class CoreTest : public QObject {
    Q_OBJECT
    static void put(const QString &path, QByteArray content = "sample") {
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(content), content.size());
    }
    struct ArchiveItem { QString name; QByteArray data; int type = AE_IFREG; QString link; };
    static void pack(const QString &path, const QList<ArchiveItem> &items) {
        auto *writer = archive_write_new();
        if (path.endsWith(".zip")) QCOMPARE(archive_write_set_format_zip(writer), ARCHIVE_OK);
        else {
            QCOMPARE(archive_write_set_format_pax_restricted(writer), ARCHIVE_OK);
            if (path.endsWith(".gz")) QCOMPARE(archive_write_add_filter_gzip(writer), ARCHIVE_OK);
        }
        QCOMPARE(archive_write_open_filename(writer, QFile::encodeName(path).constData()), ARCHIVE_OK);
        for (const auto &item : items) {
            auto *entry = archive_entry_new();
            archive_entry_set_pathname_utf8(entry, item.name.toUtf8().constData());
            archive_entry_set_filetype(entry, item.type); archive_entry_set_perm(entry, 0644);
            archive_entry_set_size(entry, item.data.size()); archive_entry_set_mtime(entry, 1700000000, 0);
            if (!item.link.isEmpty()) archive_entry_set_symlink(entry, item.link.toUtf8().constData());
            QCOMPARE(archive_write_header(writer, entry), ARCHIVE_OK);
            if (!item.data.isEmpty()) QCOMPARE(archive_write_data(writer, item.data.constData(), item.data.size()), item.data.size());
            archive_entry_free(entry);
        }
        QCOMPARE(archive_write_close(writer), ARCHIVE_OK); archive_write_free(writer);
    }
    static int namedRow(FileSystem &files, const QString &name) {
        for (int i = 0; i < files.rowCount(); ++i)
            if (files.data(files.index(i), FileSystem::Name).toString() == name) return i;
        return -1;
    }
    static QList<QVector3D> hand() {
        QList<QVector3D> joints(26, QVector3D(0, 12, 0));
        joints[1] = {0, 0, 0}; joints[6] = {-4, 0, 0}; joints[21] = {4, 0, 0};
        joints[5] = {0, 5, 0};
        joints[10] = {-7, 12, 0}; joints[15] = {0, 14, 0}; joints[20] = {7, 12, 0};
        return joints;
    }
private slots:
    void archiveBrowsing_data() {
        QTest::addColumn<QString>("suffix");
        QTest::newRow("zip") << QString(".zip");
        QTest::newRow("tar-gzip") << QString(".tar.gz");
    }
    void archiveBrowsing() {
        QFETCH(QString, suffix);
        QTemporaryDir temp;
        const QString path = temp.filePath("packed # % ? 漢字" + suffix);
        const QString filename = "nested/deep/hello # % ? 漢字.txt";
        pack(path, {{filename, "payload"}, {"empty", {}, AE_IFDIR}, {"link", {}, AE_IFLNK, "../../outside"}});
        const QString root = Archives::location(path);
        QVERIFY(Archives::validLocation(root)); QCOMPARE(Archives::source(root), path);
        const QString member = Archives::location(path, filename);
        QCOMPARE(Archives::member(member), filename);
        QVERIFY(!Archives::validLocation(Archives::location(path, "../escape")));
        auto cancel = std::make_shared<std::atomic_bool>(false);
        const auto listing = Archives::browse(root, cancel);
        QCOMPARE(listing.entries.size(), 5);
        QVERIFY(listing.warning.startsWith("Read-only archive."));
        qint64 rootBytes = 0;
        for (int i = 0; i < listing.entries.size(); ++i) {
            const auto &entry = listing.entries[i];
            QVERIFY(entry.parent < i);
            if (entry.name == "nested") { QVERIFY(entry.directory); QCOMPARE(entry.bytes, 7); }
            if (entry.name == "link") QVERIFY(entry.symlink);
            if (entry.name == "hello # % ? 漢字.txt") { QCOMPARE(entry.depth, 2); QCOMPARE(entry.mime, QString("text/plain")); }
            if (entry.parent < 0) rootBytes += entry.bytes;
        }
        QCOMPARE(rootBytes, 7);
        const auto copy = Archives::materialize(member); QVERIFY2(copy.error.isEmpty(), qPrintable(copy.error));
        QFile content(copy.path); QVERIFY(content.open(QIODevice::ReadOnly)); QCOMPARE(content.readAll(), QByteArray("payload"));
        QCOMPARE(QFileInfo(copy.path).fileName(), QString("hello # % ? 漢字.txt"));
        QCOMPARE(Archives::materialize(member).path, copy.path);
        QVERIFY(!Archives::materialize(Archives::location(path, "link")).error.isEmpty());
        QVERIFY(!QFileInfo::exists(temp.filePath("nested")));
        FileSystem files;
        files.navigate(temp.path()); QTRY_VERIFY(!files.busy());
        const int archiveRow = namedRow(files, QFileInfo(path).fileName()); QVERIFY(archiveRow >= 0);
        files.open(archiveRow); QTRY_VERIFY(!files.busy());
        QVERIFY(files.inArchive()); QCOMPARE(files.root(), root); QCOMPARE(files.rowCount(), 5);
        for (const auto &layout : {"map", "tree", "disc"}) {
            files.setLayout(layout);
            for (int i = 0; i < files.rowCount(); ++i) QVERIFY(files.data(files.index(i), FileSystem::Extent).value<QVector3D>().x() > 0);
        }
        files.setLayout("tree"); QCOMPARE(files.treeRoot().value("name").toString(), QFileInfo(path).fileName());
        QVERIFY(files.tabs()[0].toMap().value("title").toString() == QFileInfo(path).fileName());
        files.setSelected(namedRow(files, "link")); QVERIFY(!files.selection().value("canOpen").toBool());
        QSignalSpy errors(&files, &FileSystem::error); files.open(files.selected()); QCOMPARE(errors.size(), 1);
        files.setSelected(namedRow(files, "hello # % ? 漢字.txt"));
        QVERIFY(!files.selection().value("archive").toBool()); QVERIFY(files.selection().value("canOpen").toBool());
        ArchiveOpenReceiver receiver;
        QDesktopServices::setUrlHandler("file", &receiver, "receive");
        auto resetHandler = qScopeGuard([] { QDesktopServices::unsetUrlHandler("file"); });
        files.open(files.selected()); QTRY_VERIFY(!files.busy());
        QVERIFY(receiver.url.isLocalFile()); QCOMPARE(receiver.url.toLocalFile(), copy.path);
        files.rename("changed"); files.remove(true); files.remove(false); files.newFolder("new"); files.compress("new.zip");
        files.extractArchive(); files.paste(); files.startDrag(); files.clipboard(true);
        QVERIFY(!files.operating()); QVERIFY(!files.canPaste()); QVERIFY(files.dropDirectory(files.selected()).isEmpty());
        QVERIFY(!files.canDrop({QUrl::fromLocalFile(path)}, temp.path()));
        QCOMPARE(errors.size(), 1); QVERIFY(QFileInfo::exists(path));
        files.newTab(Archives::location(path, "nested/deep")); QTRY_VERIFY(!files.busy());
        QCOMPARE(files.rowCount(), 1); QCOMPARE(files.currentTab(), 1);
        files.up(); QTRY_VERIFY(!files.busy()); QCOMPARE(files.root(), Archives::location(path, "nested"));
        files.up(); QTRY_VERIFY(!files.busy()); QCOMPARE(files.root(), root);
        files.up(); QTRY_VERIFY(!files.busy()); QCOMPARE(files.root(), temp.path()); QVERIFY(!files.inArchive());
        files.activateTab(0); QTRY_VERIFY(!files.busy()); QCOMPARE(files.root(), root);
        files.openLocation(member); QTRY_VERIFY(!files.busy());
        QCOMPARE(files.root(), Archives::location(path, "nested/deep")); QCOMPARE(files.selection().value("path").toString(), member);
        QSignalSpy properties(&files, &FileSystem::propertiesRequested);
        files.openLocation(member, true, true); QTRY_VERIFY(!files.busy());
        QCOMPARE(properties.size(), 1); QCOMPARE(files.selection().value("path").toString(), member);
        *cancel = true; QVERIFY(Archives::browse(root, cancel).entries.isEmpty());
    }
    void archiveBrowseInvalidationAndLimits() {
        QTemporaryDir temp;
        const QString path = temp.filePath("update.tar.gz");
        auto cancel = std::make_shared<std::atomic_bool>(false);
        pack(path, {{"old.txt", "old"}});
        QCOMPARE(Archives::browse(Archives::location(path), cancel).entries[0].name, QString("old.txt"));
        const auto first = Archives::materialize(Archives::location(path, "old.txt")); QVERIFY(first.error.isEmpty());
        QVERIFY(QFile::remove(path)); pack(path, {{"new.txt", "new contents"}});
        const auto updated = Archives::browse(Archives::location(path), cancel);
        QCOMPARE(updated.entries.size(), 1); QCOMPARE(updated.entries[0].name, QString("new.txt"));
        QVERIFY(!Archives::materialize(Archives::location(path, "old.txt")).error.isEmpty());
        pack(temp.filePath("duplicate.tar"), {{"x", "one"}, {"./x", "two"}});
        const auto duplicate = Archives::browse(Archives::location(temp.filePath("duplicate.tar")), cancel);
        QVERIFY(duplicate.entries.isEmpty()); QVERIFY(duplicate.warning.contains("Duplicate"));
        pack(temp.filePath("conflict.tar"), {{"parent", "file"}, {"parent/child", "data"}});
        QVERIFY(Archives::browse(Archives::location(temp.filePath("conflict.tar")), cancel).entries.isEmpty());
        pack(temp.filePath("unsafe.tar"), {{"../escape", "data"}});
        const auto unsafe = Archives::browse(Archives::location(temp.filePath("unsafe.tar")), cancel);
        QVERIFY(unsafe.entries.isEmpty()); QVERIFY(unsafe.warning.contains("Unsafe"));
        put(temp.filePath("broken.zip"), "not an archive");
        QVERIFY(Archives::browse(Archives::location(temp.filePath("broken.zip")), cancel).entries.isEmpty());
        QVERIFY(!Archives::materialize(Archives::location(temp.filePath("broken.zip"), "x")).error.isEmpty());
        const QString deep = QString("folder/").repeated(7) + "file.txt";
        pack(temp.filePath("deep.tar"), {{deep, "deep"}});
        const auto limited = Archives::browse(Archives::location(temp.filePath("deep.tar")), cancel);
        QCOMPARE(limited.entries.size(), 5); QVERIFY(limited.warning.contains("View limited"));
        const auto deeper = Archives::browse(Archives::location(temp.filePath("deep.tar"), QString("folder/").repeated(6).chopped(1)), cancel);
        QCOMPARE(deeper.entries.size(), 2);
        pack(temp.filePath("large.tar.gz"), {{"large.txt", QByteArray(64 * 1024 * 1024 + 1, 'x')}});
        const auto large = Archives::materialize(Archives::location(temp.filePath("large.tar.gz"), "large.txt"));
        QVERIFY(large.path.isEmpty()); QVERIFY(large.error.contains("64 MiB"));
    }
    void archivePreview() {
        QTemporaryDir temp;
        QImage image(16, 16, QImage::Format_RGB32); image.fill(Qt::red);
        QByteArray data; QBuffer buffer(&data); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(image.save(&buffer, "PNG"));
        const QString path = temp.filePath("images.zip"); pack(path, {{"folder/red.png", data}});
        const QString member = Archives::location(path, "folder/red.png");
        ThumbnailProvider provider; QSize size;
        QString url = ThumbnailProvider::url(member, -1, true);
        auto preview = provider.requestImage(url.mid(QString("image://thumbnail/").size()), &size, QSize(50, 50));
        QVERIFY(!preview.isNull()); QCOMPARE(preview.pixelColor(0, 0), QColor(Qt::red));
        url = ThumbnailProvider::url(member);
        preview = provider.requestImage(url.mid(QString("image://thumbnail/").size()), &size, QSize(50, 50));
        QVERIFY(!preview.isNull()); QVERIFY(preview.pixelColor(0, 0) != QColor(Qt::red));
    }
    void archiveRoundTrips_data() {
        QTest::addColumn<QString>("suffix");
        QTest::newRow("zip") << QString(".zip");
        QTest::newRow("tar-gzip") << QString(".tar.gz");
    }
    void archiveRoundTrips() {
        QFETCH(QString, suffix);
        QTemporaryDir temp;
        const QString source = temp.filePath("source # 漢字");
        QVERIFY(QDir().mkpath(source + "/empty"));
        put(source + "/.hidden", "hidden");
        put(source + "/hello 漢字.txt", QByteArray(150000, 'x'));
        const QString path = temp.filePath("packed" + suffix);
        QVERIFY2(Archives::create(source, path).isEmpty(), qPrintable(path));
        QVERIFY(!Archives::create(source, path).isEmpty());
        QVERIFY(Archives::supported(path.toUpper()));
        QVERIFY(Archives::extract(path).isEmpty());
        const QString extracted = temp.filePath("packed/source # 漢字");
        QFile content(extracted + "/hello 漢字.txt"); QVERIFY(content.open(QIODevice::ReadOnly));
        QCOMPARE(content.readAll(), QByteArray(150000, 'x'));
        QVERIFY(QFileInfo(extracted + "/empty").isDir());
        QVERIFY(QFileInfo::exists(extracted + "/.hidden"));
        QVERIFY(!Archives::extract(path).isEmpty());
        QVERIFY(!Archives::create(source, source + "/inside.zip").isEmpty());
        QVERIFY(QFile::link(source + "/.hidden", source + "/link"));
        QVERIFY(!Archives::create(source, temp.filePath("links" + suffix)).isEmpty());
        QVERIFY(!QFileInfo::exists(temp.filePath("links" + suffix)));
        QCOMPARE(QDir(temp.path()).entryList({".fsvr-*"}, QDir::AllEntries | QDir::Hidden).size(), 0);
    }
    void unsafeArchives_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("type");
        QTest::newRow("parent") << QString("../escape") << int(AE_IFREG);
        QTest::newRow("nested-parent") << QString("child/../../escape") << int(AE_IFREG);
        QTest::newRow("absolute") << QString("/tmp/fsvr-escape") << int(AE_IFREG);
        QTest::newRow("backslash") << QString("child\\escape") << int(AE_IFREG);
        QTest::newRow("symlink") << QString("link") << int(AE_IFLNK);
        QTest::newRow("fifo") << QString("fifo") << int(AE_IFIFO);
    }
    void unsafeArchives() {
        QFETCH(QString, name); QFETCH(int, type);
        QTemporaryDir temp;
        const QString path = temp.filePath("unsafe.tar");
        auto *writer = archive_write_new();
        QCOMPARE(archive_write_set_format_pax_restricted(writer), ARCHIVE_OK);
        QCOMPARE(archive_write_open_filename(writer, QFile::encodeName(path).constData()), ARCHIVE_OK);
        auto *entry = archive_entry_new();
        archive_entry_set_pathname(entry, name.toUtf8().constData());
        archive_entry_set_filetype(entry, type);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_size(entry, 0);
        if (type == AE_IFLNK) archive_entry_set_symlink(entry, "../escape");
        QCOMPARE(archive_write_header(writer, entry), ARCHIVE_OK);
        archive_entry_free(entry);
        QCOMPARE(archive_write_close(writer), ARCHIVE_OK); archive_write_free(writer);
        QVERIFY(!Archives::extract(path).isEmpty());
        QVERIFY(!QFileInfo::exists(temp.filePath("unsafe")));
        QCOMPARE(QDir(temp.path()).entryList({".fsvr-*"}, QDir::AllEntries | QDir::Hidden).size(), 0);
    }
    void archiveModelOperations() {
        QTemporaryDir temp;
        put(temp.filePath("sample.txt"), "payload");
        FileSystem files; QSignalSpy errors(&files, &FileSystem::error);
        files.navigate(temp.path()); QTRY_VERIFY(!files.busy());
        files.setSelected(0); files.compress("sample.zip"); QTRY_VERIFY(!files.busy());
        QVERIFY(QFileInfo::exists(temp.filePath("sample.zip")));
        for (int i = 0; i < files.rowCount(); ++i)
            if (files.data(files.index(i), FileSystem::Name).toString() == "sample.zip") files.setSelected(i);
        QVERIFY(files.selection().value("archive").toBool());
        files.extractArchive(); QTRY_VERIFY(!files.busy());
        QVERIFY(QFileInfo::exists(temp.filePath("sample/sample.txt")));
        QCOMPARE(errors.size(), 0);
        put(temp.filePath("broken.zip"), "invalid archive");
        QVERIFY(!Archives::extract(temp.filePath("broken.zip")).isEmpty());
        QVERIFY(!QFileInfo::exists(temp.filePath("broken")));
    }
    void fileDrops() {
        QTemporaryDir source, destination;
        const QString name = "space # % 漢字.txt";
        const QString path = source.filePath(name);
        put(path, "drag contents");
        FileSystem files;
        files.navigate(destination.path()); QTRY_VERIFY(!files.busy());
        const QList<QUrl> urls{QUrl::fromLocalFile(path)};
        QVERIFY(files.canDrop(urls, destination.path()));
        QVERIFY(!files.canDrop({QUrl("https://example.com/file")}, destination.path()));
        QVERIFY(!files.canDrop(urls, source.path())); // Same path / collision.
        QVERIFY(!files.dropFiles(urls, destination.path(), Qt::LinkAction));
        QVERIFY(files.dropFiles(urls, destination.path(), Qt::CopyAction));
        QTRY_VERIFY(!files.busy());
        QVERIFY(QFileInfo::exists(path));
        QFile copy(destination.filePath(name)); QVERIFY(copy.open(QIODevice::ReadOnly));
        QCOMPARE(copy.readAll(), QByteArray("drag contents")); copy.close();
        QVERIFY(!files.canDrop(urls, destination.path()));
        QVERIFY(!files.dropFiles(urls, destination.path(), Qt::MoveAction));
        QVERIFY(QFile::remove(destination.filePath(name)));
        QTRY_VERIFY(!files.busy());
        QVERIFY(files.dropFiles(urls, destination.path(), Qt::MoveAction));
        QTRY_VERIFY(!files.busy());
        QVERIFY(!QFileInfo::exists(path)); QVERIFY(QFileInfo::exists(destination.filePath(name)));
        QVERIFY(QDir(source.path()).mkpath("folder/child"));
        const QList<QUrl> folder{QUrl::fromLocalFile(source.filePath("folder"))};
        QVERIFY(!files.canDrop(folder, source.filePath("folder/child")));
        QVERIFY(!FileSystem::transferFiles(folder, source.filePath("folder/child"), true).isEmpty());
        put(source.filePath("keep.txt"));
        const QList<QUrl> mixed{QUrl::fromLocalFile(source.filePath("keep.txt")), QUrl("https://example.com/file")};
        QVERIFY(!FileSystem::transferFiles(mixed, destination.path(), false).isEmpty());
        QVERIFY(!QFileInfo::exists(destination.filePath("keep.txt")));
    }
    void launchRealmMetadata() {
        QTemporaryDir temp;
        const QString socket = temp.filePath("wayland-test");
        const QString metadata = socket + ".dome-session.json";
        auto context = [&](int version, qint64 pid, const QString &realm) {
            put(metadata, QString("{\"version\":%1,\"compositor_pid\":%2,\"realm\":\"%3\"}").arg(version).arg(pid).arg(realm).toUtf8());
        };
        const auto pid = QCoreApplication::applicationPid();
        QVERIFY(!LaunchContext::spatialForSocket(socket, pid));
        context(1, pid, "xr"); QVERIFY(LaunchContext::spatialForSocket(socket, pid));
        QVERIFY(!LaunchContext::spatialForSocket(socket, pid + 1));
        context(1, pid, "desktop"); QVERIFY(!LaunchContext::spatialForSocket(socket, pid));
        context(2, pid, "xr"); QVERIFY(!LaunchContext::spatialForSocket(socket, pid));
        context(1, pid, "unknown"); QVERIFY(!LaunchContext::spatialForSocket(socket, pid));
        context(1, pid, "xr");
        QVERIFY(QFile::setPermissions(metadata, QFile::ReadOwner | QFile::WriteOwner | QFile::WriteOther));
        QVERIFY(!LaunchContext::spatialForSocket(socket, pid));
        QVERIFY(QFile::remove(metadata));
        const QString target = temp.filePath("linked-context");
        put(target, QString("{\"version\":1,\"compositor_pid\":%1,\"realm\":\"xr\"}").arg(pid).toUtf8());
        QVERIFY(QFile::link(target, metadata)); QVERIFY(!LaunchContext::spatialForSocket(socket, pid));
    }
    void fileManagerLocations() {
        QTemporaryDir temp;
        const QString file = temp.filePath("space # 漢字.txt"); put(file);
        QVERIFY(QDir(temp.path()).mkdir("folder"));
        FileSystem files;
        QSignalSpy properties(&files, &FileSystem::propertiesRequested);
        QSignalSpy errors(&files, &FileSystem::error);
        files.openLocation(QUrl::fromLocalFile(temp.path()).toString(QUrl::FullyEncoded));
        QTRY_VERIFY(!files.busy()); QCOMPARE(files.root(), temp.path());
        files.openLocation(QUrl::fromLocalFile(file).toString(QUrl::FullyEncoded));
        QTRY_VERIFY(!files.busy()); QCOMPARE(files.selection()["path"].toString(), file);
        files.openLocation(QUrl::fromLocalFile(temp.filePath("folder")).toString(QUrl::FullyEncoded), true, true);
        QTRY_VERIFY(!files.busy());
        QCOMPARE(files.root(), temp.path()); QCOMPARE(files.selection()["name"].toString(), QString("folder"));
        QCOMPARE(properties.size(), 1);
        files.openLocation(file, false, false, true);
        QTRY_VERIFY(!files.busy()); QCOMPARE(files.tabs().size(), 2);
        QCOMPARE(files.selection()["path"].toString(), file);
        files.openLocation("https://example.com/path"); QCOMPARE(errors.size(), 1);
        QCOMPARE(files.root(), temp.path());
        files.openLocation("trash:///"); QTRY_VERIFY(!files.busy()); QVERIFY(files.inTrash());
    }
    void treePlatformsAndConnections() {
        QTemporaryDir temp;
        QDir root(temp.path());
        QVERIFY(root.mkpath("A/Child")); QVERIFY(root.mkdir("B"));
        for (const auto &path : {"root.txt", "A/a.txt", "A/Child/deep.txt", "B/b.txt"}) put(root.filePath(path));
        FileSystem model;
        model.navigate(temp.path()); QTRY_VERIFY(!model.busy());
        model.setLayout("tree");
        QCOMPARE(model.treeRoot()["path"].toString(), temp.path());
        QVERIFY(!model.treeBranches().isEmpty());
        QHash<QString, int> byPath;
        for (int i = 0; i < model.rowCount(); ++i) byPath.insert(model.data(model.index(i), FileSystem::Path).toString(), i);
        auto center = [&](int i) { return i < 0 ? model.treeRoot()["center"].value<QVector3D>() : model.data(model.index(i), FileSystem::Center).value<QVector3D>(); };
        auto extent = [&](int i) { return i < 0 ? model.treeRoot()["extent"].value<QVector3D>() : model.data(model.index(i), FileSystem::Extent).value<QVector3D>(); };
        QList<QRectF> platforms;
        const auto rc = center(-1), re = extent(-1);
        platforms.append(QRectF(rc.x() - re.x()/2, rc.z() - re.z()/2, re.x(), re.z()));
        for (int i = 0; i < model.rowCount(); ++i) {
            const auto path = model.data(model.index(i), FileSystem::Path).toString();
            const int parent = byPath.value(QFileInfo(path).path(), -1);
            const auto c = center(i), e = extent(i), pc = center(parent), pe = extent(parent);
            if (model.data(model.index(i), FileSystem::Directory).toBool()) {
                QVERIFY(c.z() + e.z()/2 < pc.z() - pe.z()/2);
                bool connected = false;
                for (const auto &value : model.treeBranches()) {
                    const auto segment = value.toMap();
                    if (segment["child"].toInt() != i) continue;
                    QCOMPARE(segment["parent"].toInt(), parent);
                    QVERIFY((segment["to"].value<QVector3D>() - QVector3D(c.x(), c.y(), c.z() + e.z()/2)).length() < .001);
                    connected = true;
                }
                QVERIFY(connected);
                const QRectF rect(c.x() - e.x()/2, c.z() - e.z()/2, e.x(), e.z());
                for (const auto &other : platforms) QVERIFY(!rect.intersects(other));
                platforms << rect;
            } else {
                QVERIFY(c.x() - e.x()/2 >= pc.x() - pe.x()/2);
                QVERIFY(c.x() + e.x()/2 <= pc.x() + pe.x()/2);
                QVERIFY(c.z() - e.z()/2 >= pc.z() - pe.z()/2);
                QVERIFY(c.z() + e.z()/2 <= pc.z() + pe.z()/2);
                QVERIFY(c.y() - e.y()/2 >= pc.y() + pe.y()/2 - .001);
                QCOMPARE(e.x(), e.z());
            }
        }
        model.setLayout("map"); QVERIFY(model.treeBranches().isEmpty()); QVERIFY(model.treeRoot().isEmpty());
        QTemporaryDir empty;
        model.setLayout("tree"); model.navigate(empty.path()); QTRY_VERIFY(!model.busy());
        QCOMPARE(model.treeRoot()["path"].toString(), empty.path());
        QCOMPARE(model.rowCount(), 0); QVERIFY(model.treeBranches().isEmpty());
    }
    void directoryIndexAndLiveValidation() {
        QTemporaryDir temp, cache;
        put(temp.filePath("stale.txt")); put(temp.filePath("keep.txt"));
        QElapsedTimer timer; timer.start();
        {
            FileSystem cold;
            cold.navigate(temp.path()); QTRY_VERIFY(!cold.busy());
            QCOMPARE(cold.rowCount(), 2);
        }
        const auto coldMs = timer.elapsed();
        auto saved = FileIndex::load(temp.path(), FileIndex::directory()); QVERIFY(saved);
        QCOMPARE(saved->entries.size(), 2);
        QVERIFY(FileIndex::store(temp.path(), *saved, cache.path()));
        auto copy = FileIndex::load(temp.path(), cache.path()); QVERIFY(copy);
        QCOMPARE(copy->entries[0].modifiedMs, saved->entries[0].modifiedMs);
        QVERIFY(!FileIndex::load(temp.filePath("other"), cache.path()));
        QVERIFY(QFile::remove(temp.filePath("stale.txt"))); put(temp.filePath("new.txt"));
        FileSystem warm;
        QSignalSpy restored(&warm, &FileSystem::indexRestored);
        QSignalSpy errors(&warm, &FileSystem::error);
        bool sawStaleSnapshot = false;
        qint64 firstIndexedMs = -1;
        connect(&warm, &FileSystem::indexRestored, this, [&] {
            firstIndexedMs = timer.elapsed();
            QVERIFY(warm.busy()); QVERIFY(warm.cached());
            for (int i = 0; i < warm.rowCount(); ++i) {
                sawStaleSnapshot |= warm.data(warm.index(i), FileSystem::Name).toString() == "stale.txt";
                if (warm.data(warm.index(i), FileSystem::Name).toString() == "keep.txt") warm.setSelected(i);
            }
            warm.rename("unexpected.txt");
            QCOMPARE(errors.size(), 1);
            QVERIFY(QFileInfo::exists(temp.filePath("keep.txt")));
        });
        timer.restart(); warm.navigate(temp.path());
        QTRY_COMPARE(restored.size(), 1);
        QTRY_VERIFY(!warm.busy());
        QVERIFY(sawStaleSnapshot); QVERIFY(!warm.cached());
        QSet<QString> names;
        for (int i = 0; i < warm.rowCount(); ++i) names.insert(warm.data(warm.index(i), FileSystem::Name).toString());
        QCOMPARE(names, QSet<QString>({"keep.txt", "new.txt"}));
        qInfo() << "Cold scan (ms):" << coldMs << "indexed first model (ms):" << firstIndexedMs << "live validation (ms):" << timer.elapsed();
        const auto file = QDir(cache.path()).entryList({"*.json"}, QDir::Files).first();
        put(cache.filePath(file), "{broken");
        QVERIFY(!FileIndex::load(temp.path(), cache.path()));
    }
    void independentTabsAndCanceledScans() {
        QTemporaryDir first, second;
        put(first.filePath("first.txt")); put(second.filePath("second.txt"));
        FileSystem model;
        model.navigate(first.path()); QTRY_VERIFY(!model.busy());
        model.setLayout("tree"); model.setSelected(0);
        model.setViewState({{"zoom", 2.}, {"yaw", 25.}});
        model.newTab(second.path());
        QCOMPARE(model.currentTab(), 1); QCOMPARE(model.tabs().size(), 2);
        QTRY_VERIFY(!model.busy());
        model.setLayout("disc"); model.setViewState({{"zoom", .5}, {"pitch", -15.}});
        model.activateTab(0);
        QCOMPARE(model.root(), first.path()); QCOMPARE(model.layout(), QString("tree"));
        QCOMPARE(model.viewState()["zoom"].toDouble(), 2.);
        QCOMPARE(model.selection()["name"].toString(), QString("first.txt"));
        QCOMPARE(model.rowCount(), 1); // Warm tab restores before the worker finishes.
        model.activateTab(1); model.activateTab(0); model.activateTab(1);
        QTRY_VERIFY(!model.busy());
        QCOMPARE(model.root(), second.path()); QCOMPARE(model.layout(), QString("disc"));
        QCOMPARE(model.data(model.index(0), FileSystem::Name).toString(), QString("second.txt"));
        QCOMPARE(model.viewState()["pitch"].toDouble(), -15.);
        model.closeTab(0); QCOMPARE(model.currentTab(), 0); QCOMPARE(model.tabs().size(), 1);
        model.newTab(first.path()); model.closeTab(1);
        QTRY_VERIFY(!model.busy());
        QCOMPARE(model.root(), second.path()); QCOMPARE(model.tabs().size(), 1);
        model.closeTab(0); QCOMPARE(model.tabs().size(), 1);
    }
    void trashManagement() {
        QTemporaryDir temp;
        QDir dir(temp.path());
        QVERIFY(dir.mkpath("Trash/files")); QVERIFY(dir.mkpath("Trash/info"));
        QVERIFY(dir.mkdir("original"));
        TrashLocation location{dir.filePath("Trash"), temp.path(), true};
        auto record = [&](const QString &id, const QString &original) {
            put(dir.filePath("Trash/info/" + id + ".trashinfo"), "[Trash Info]\nPath="
                + QUrl::toPercentEncoding(original, "/") + "\nDeletionDate=2026-10-19T12:34:56\n");
        };
        const auto cancel = std::make_shared<std::atomic_bool>(false);
        const QString original = dir.filePath("original/space #漢字.txt");
        put(dir.filePath("Trash/files/collision.1")); record("collision.1", original);
        auto scan = TrashStore::scan({location}, cancel);
        QCOMPARE(scan.entries.size(), 1);
        const auto entry = scan.entries.first();
        QCOMPARE(entry.name, QString("space #漢字.txt"));
        QCOMPARE(entry.originalPath, original);
        QCOMPARE(entry.deletionDate, QString("2026-10-19T12:34:56"));
        put(original, "keep me");
        QVERIFY(!TrashStore::restore(entry).isEmpty()); // Never overwrite, including dangling symlinks.
        QFile preserved(original); QVERIFY(preserved.open(QIODevice::ReadOnly)); QCOMPARE(preserved.readAll(), QByteArray("keep me")); preserved.close();
        QVERIFY(QFile::remove(original));
        QVERIFY(QFile::link(dir.filePath("nonexistent"), original));
        QVERIFY(!TrashStore::restore(entry).isEmpty()); QVERIFY(QFileInfo(original).isSymLink());
        QVERIFY(QFile::remove(original));
        QVERIFY2(TrashStore::restore(entry).isEmpty(), "Restore failed");
        QVERIFY(QFileInfo::exists(original)); QVERIFY(!QFileInfo::exists(entry.path));
        QVERIFY(!QFileInfo::exists(dir.filePath("Trash/info/collision.1.trashinfo")));
        QVERIFY(!TrashStore::restore(entry).isEmpty()); // Stale selection.
        QVERIFY(dir.mkpath("Trash/files/folder/nested")); put(dir.filePath("Trash/files/folder/nested/a"));
        record("folder", dir.filePath("original/folder"));
        scan = TrashStore::scan({location}, cancel);
        QCOMPARE(scan.entries.size(), 1); QVERIFY(scan.entries.first().directory); // One restorable item per tree.
        QVERIFY(TrashStore::restore(scan.entries.first()).isEmpty());
        QVERIFY(QFileInfo::exists(dir.filePath("original/folder/nested/a")));
        put(dir.filePath("Trash/files/missing-parent")); record("missing-parent", dir.filePath("gone/file"));
        scan = TrashStore::scan({location}, cancel);
        QVERIFY(!TrashStore::restore(scan.entries.first()).isEmpty());
        QVERIFY(QFileInfo::exists(scan.entries.first().path));
        // Mounted-volume paths are relative to the mount root, with no traversal allowed.
        location.home = false;
        put(dir.filePath("Trash/files/relative")); record("relative", "original/relative.txt");
        put(dir.filePath("Trash/files/unsafe")); record("unsafe", "../outside");
        put(dir.filePath("Trash/files/orphan"));
        put(dir.filePath("Trash/files/symlink-info"));
        QVERIFY(QFile::link(original, dir.filePath("Trash/info/symlink-info.trashinfo")));
        scan = TrashStore::scan({location}, cancel);
        QVERIFY(!scan.warning.isEmpty());
        int restorable = 0;
        for (const auto &e : scan.entries) {
            if (!e.originalPath.isEmpty()) { ++restorable; QCOMPARE(e.originalPath, dir.filePath("original/relative.txt")); }
            if (e.name == "unsafe" || e.name == "orphan" || e.name == "symlink-info") QVERIFY(e.originalPath.isEmpty());
        }
        QCOMPARE(restorable, 1);
        // Empty does not follow links to surviving files or directories.
        QVERIFY(QFile::link(dir.filePath("original"), dir.filePath("Trash/files/directory-link")));
        const auto limited = TrashStore::scan({location}, cancel, 1);
        QCOMPARE(limited.entries.size(), 1); QVERIFY(!limited.warning.isEmpty());
        QVERIFY(TrashStore::empty({location}).isEmpty());
        QCOMPARE(TrashStore::scan({location}, cancel).entries.size(), 0);
        QVERIFY(QFileInfo::exists(original)); QVERIFY(QFileInfo::exists(dir.filePath("original/folder/nested/a")));
        // Neither scanning nor erasing accepts a symlink substituted for the store.
        QVERIFY(QDir(dir.filePath("Trash")).removeRecursively());
        QVERIFY(QFile::link(dir.filePath("original"), dir.filePath("Trash")));
        QCOMPARE(TrashStore::scan({location}, cancel).entries.size(), 0);
    }
    void internalTrashModel() {
        const QString trash = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Trash";
        QVERIFY(QDir().mkpath(trash + "/files")); QVERIFY(QDir().mkpath(trash + "/info"));
        QTemporaryDir destination;
        const auto original = destination.filePath("restored.txt");
        put(trash + "/files/model-test", "preview content");
        put(trash + "/info/model-test.trashinfo", "[Trash Info]\nPath=" + QUrl::toPercentEncoding(original, "/") + "\nDeletionDate=2026-10-19T12:34:56\n");
        FileSystem model;
        model.navigate("trash:/"); QTRY_VERIFY(!model.busy());
        int index = -1;
        for (int i = 0; i < model.rowCount(); ++i) if (model.data(model.index(i), FileSystem::Path).toString() == trash + "/files/model-test") index = i;
        QVERIFY(index >= 0); model.setSelected(index);
        QCOMPARE(model.selection()["name"].toString(), QString("restored.txt"));
        QCOMPARE(model.selection()["originalPath"].toString(), original);
        QVERIFY(model.selection()["trashed"].toBool()); QVERIFY(model.selection()["canRestore"].toBool());
        QVERIFY(!model.canPaste());
        model.rename("wrong"); model.newFolder("wrong"); model.remove(false);
        QVERIFY(QFileInfo::exists(trash + "/files/model-test"));
        model.restore(); QTRY_VERIFY(!model.busy());
        QVERIFY(QFileInfo::exists(original)); QVERIFY(!QFileInfo::exists(trash + "/files/model-test"));
    }
    void sharedLocalPlaces() {
        QTemporaryDir temp;
        QDir root(temp.path());
        QVERIFY(root.mkdir("bookmark #漢字")); QVERIFY(root.mkdir("hidden"));
        const QString target = root.filePath("bookmark #漢字");
        const QString xbel = root.filePath("places.xbel");
        put(xbel, ("<xbel xmlns:bookmark=\"http://freedesktop.org\"><bookmark href=\""
            + QUrl::fromLocalFile(target).toString(QUrl::FullyEncoded)
            + "\"><title>Shared bookmark</title><info><bookmark:icon name=\"folder-pictures\"/></info></bookmark>"
            + "<bookmark href=\"" + QUrl::fromLocalFile(target).toString(QUrl::FullyEncoded) + "\"><title>Duplicate</title></bookmark>"
            + "<bookmark href=\"" + QUrl::fromLocalFile(root.filePath("hidden")).toString(QUrl::FullyEncoded) + "\"><info><IsHidden>true</IsHidden></info></bookmark>"
            + "<bookmark href=\"trash:/\"><title>Trash</title></bookmark>"
            + "<bookmark href=\"sftp://host/data\"><title>Remote</title></bookmark></xbel>").toUtf8());
        const auto places = FileSystem::readPlaces(xbel, false);
        QCOMPARE(places.size(), 2);
        QCOMPARE(places[0].toMap()["path"].toString(), target);
        QCOMPARE(places[0].toMap()["name"].toString(), QString("Shared bookmark"));
        QCOMPARE(places[0].toMap()["icon"].toString(), QString("folder-pictures"));
        QCOMPARE(places[1].toMap()["path"].toString(), QString("trash:/"));
        FileSystem model;
        model.openPlace("trash:/");
        QTRY_VERIFY(!model.busy());
        QVERIFY(model.inTrash());
        QCOMPARE(model.root(), QString("trash:/"));
        model.openPlace("trash:/", true);
        QTRY_VERIFY(!model.busy());
        QCOMPARE(model.tabs().size(), 2);
        QCOMPARE(model.tabs()[1].toMap()["title"].toString(), QString("Trash"));
        model.navigate(target);
        QTRY_VERIFY(!model.busy());
        QVERIFY(!model.inTrash());
        put(xbel, "<xbel>");
        QVERIFY(!FileSystem::readPlaces(xbel, false).isEmpty());
    }
    void mapAvoidsThinStrips() {
        QTemporaryDir temp;
        for (int i = 0; i < 24; ++i) {
            QFile file(temp.filePath(QString::number(i) + ".bin"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            QVERIFY(file.resize(i == 0 ? 64 * 1024 * 1024 : 16));
        }
        FileSystem model;
        model.navigate(temp.path()); QTRY_COMPARE(model.rowCount(), 24);
        for (int i = 0; i < model.rowCount(); ++i) {
            const auto size = model.data(model.index(i), FileSystem::Extent).value<QVector3D>();
            QVERIFY2(qMax(size.x(), size.z()) / qMin(size.x(), size.z()) < 5,
                qPrintable(QString("Thin Map tile %1 x %2").arg(size.x()).arg(size.z())));
        }
    }
    void scanBoundsAndSymlinks() {
        QTemporaryDir temp;
        QDir root(temp.path());
        QVERIFY(root.mkdir("child")); put(root.filePath("child/data.txt"), "12345");
        QVERIFY(QFile::link(temp.path(), root.filePath("cycle")));
        const auto cancel = std::make_shared<std::atomic_bool>(false);
        auto scan = FileSystem::scan(temp.path(), cancel);
        QCOMPARE(scan.entries.size(), 3);
        qint64 total = 0;
        for (const auto &e : scan.entries) {
            if (e.name == "child") { QCOMPARE(e.bytes, 5); QVERIFY(e.directory); }
            if (e.name == "cycle") { QVERIFY(e.symlink); QVERIFY(e.directory); }
            if (e.name == "data.txt") { QCOMPARE(e.mime, QString("text/plain")); QCOMPARE(e.depth, 1); }
            total += e.bytes;
        }
        QCOMPARE(total, 10);
        auto limited = FileSystem::scan(temp.path(), cancel, 4, 1);
        QCOMPARE(limited.entries.size(), 1); QVERIFY(!limited.warning.isEmpty());
        *cancel = true;
        QVERIFY(FileSystem::scan(temp.path(), cancel).entries.isEmpty());
    }
    void copyProtectsSourcesAndLinks() {
        QTemporaryDir temp; QDir root(temp.path());
        QVERIFY(root.mkdir("source")); put(root.filePath("source/file"));
        QVERIFY(QFile::link(root.filePath("source"), root.filePath("source/link")));
        QVERIFY(!FileSystem::copyEntry(root.filePath("source"), root.filePath("source/nested")).isEmpty());
        QVERIFY(!QFileInfo::exists(root.filePath("source/nested")));
        QVERIFY(FileSystem::copyEntry(root.filePath("source"), root.filePath("copy")).isEmpty());
        QVERIFY(QFileInfo(root.filePath("copy/link")).isSymLink());
        QVERIFY(QFileInfo::exists(root.filePath("copy/file")));
        QVERIFY(!FileSystem::copyEntry(root.filePath("source/file"), root.filePath("copy/file")).isEmpty());
        QVERIFY(QFileInfo::exists(root.filePath("source/file")));
    }
    void modelLayoutsAndOperations() {
        QTemporaryDir temp; QDir root(temp.path()); put(root.filePath("a.txt")); put(root.filePath("b.txt"));
        FileSystem model;
        model.navigate(temp.path()); QTRY_VERIFY_WITH_TIMEOUT(!model.busy(), 5000);
        QCOMPARE(model.rowCount(), 2);
        for (const auto &layout : {"map", "tree", "disc"}) {
            model.setLayout(layout);
            for (int i = 0; i < model.rowCount(); ++i) {
                const auto extent = model.data(model.index(i), FileSystem::Extent).value<QVector3D>();
                QVERIFY(extent.x() > 0); QVERIFY(extent.y() > 0); QVERIFY(extent.z() > 0);
            }
        }
        model.setSelected(0);
        QSignalSpy errors(&model, &FileSystem::error);
        model.rename("../escape"); QCOMPARE(errors.size(), 1);
        const QString source = model.selection()["path"].toString();
        model.rename("renamed.txt"); QTRY_VERIFY_WITH_TIMEOUT(!model.busy(), 5000);
        QVERIFY(!QFileInfo::exists(source)); QVERIFY(QFileInfo::exists(root.filePath("renamed.txt")));
        model.setSelected(-1); model.newFolder("new"); QTRY_VERIFY_WITH_TIMEOUT(!model.busy(), 5000);
        QVERIFY(QFileInfo(root.filePath("new")).isDir());
        for (int i = 0; i < model.rowCount(); ++i)
            if (model.data(model.index(i), FileSystem::Name).toString() == "renamed.txt") model.setSelected(i);
        model.clipboard(false); QVERIFY(model.canPaste());
        for (int i = 0; i < model.rowCount(); ++i)
            if (model.data(model.index(i), FileSystem::Name).toString() == "new") model.setSelected(i);
        model.paste(); QTRY_VERIFY_WITH_TIMEOUT(!model.busy(), 5000);
        QVERIFY(QFileInfo::exists(root.filePath("new/renamed.txt")));
        QVERIFY(QFileInfo::exists(root.filePath("renamed.txt")));
        QGuiApplication::clipboard()->clear();
    }
    void pinchDebounceHysteresisAndTrackingLoss() {
        HandGesture gesture; auto joints = hand();
        // Held gesture at startup must not click.
        joints[10] = joints[5]; gesture.sample(joints, true, 0); gesture.sample(joints, true, 70);
        QCOMPARE(gesture.button(), 0);
        joints = hand(); gesture.sample(joints, true, 80);
        joints[10] = joints[5]; gesture.sample(joints, true, 90); gesture.sample(joints, true, 130);
        QCOMPARE(gesture.button(), 0);
        gesture.sample(joints, true, 150); QCOMPARE(gesture.button(), 1);
        joints[10] = joints[5] + QVector3D(3.8, 0, 0); gesture.sample(joints, true, 160);
        QCOMPARE(gesture.button(), 1);
        joints[10] = joints[5] + QVector3D(6, 0, 0); gesture.sample(joints, true, 170);
        QCOMPARE(gesture.button(), 0);
        joints[15] = joints[5]; gesture.sample(joints, true, 180); gesture.sample(joints, true, 240);
        QCOMPARE(gesture.button(), 2); QVERIFY(gesture.dragging());
        gesture.sample(joints, false, 250); QCOMPARE(gesture.button(), 0);
        gesture.sample(joints, true, 260); gesture.sample(joints, true, 330); QCOMPARE(gesture.button(), 0);
        joints = hand(); gesture.sample(joints, true, 340);
        joints[20] = joints[5]; gesture.sample(joints, true, 350); gesture.sample(joints, true, 410);
        QCOMPARE(gesture.button(), 3);
        gesture.sample(joints, true, 800); QCOMPARE(gesture.button(), 0); // stale frame gap
    }
    void ambiguousAndInvalidHands() {
        HandGesture gesture; auto joints = hand(); gesture.sample(joints, true, 0);
        joints[10] = joints[15] = joints[5];
        gesture.sample(joints, true, 20); gesture.sample(joints, true, 90); QCOMPARE(gesture.button(), 0);
        joints = hand(); joints[1].setX(qQNaN()); gesture.sample(joints, true, 100); QCOMPARE(gesture.button(), 0);
    }
    void thumbnailFilenameRoundTrip() {
        QTemporaryDir temp;
        const QString path = temp.filePath("hash#%? 漢字.png");
        QImage image(32, 20, QImage::Format_RGB32); image.fill(Qt::red); QVERIFY(image.save(path));
        const QString url = ThumbnailProvider::url(path);
        ThumbnailProvider provider; QSize size;
        const auto preview = provider.requestImage(url.mid(QString("image://thumbnail/").size()), &size, QSize(100, 100));
        QVERIFY(!preview.isNull()); QCOMPARE(preview.pixelColor(0, 0), QColor(Qt::red));
    }
    void registeredMimeThumbnailer() {
        QTemporaryDir temp;
        QDir root(temp.path()); QVERIFY(root.mkdir("thumbnailers"));
        const QByteArray previous = qgetenv("XDG_DATA_HOME");
        qputenv("XDG_DATA_HOME", temp.path().toUtf8());
        struct Restore { QByteArray value; ~Restore() { if (value.isNull()) qunsetenv("XDG_DATA_HOME"); else qputenv("XDG_DATA_HOME", value); } } restore{previous};
        QImage seed(40, 40, QImage::Format_RGB32); seed.fill(Qt::blue);
        QVERIFY(seed.save(root.filePath("seed.png")));
        const QString helper = root.filePath("thumb");
        put(helper, "#!/bin/sh\ncp -- \"$3\" \"$2\"\n");
        QVERIFY(QFile::setPermissions(helper, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        put(root.filePath("thumbnailers/test.thumbnailer"),
            ("[Thumbnailer Entry]\nMimeType=application/pdf;\nExec=\"" + helper + "\" %i %o \"" + root.filePath("seed.png") + "\"\n").toUtf8());
        const QString input = root.filePath("a # %o quoted ' document.pdf");
        put(input, "%PDF-1.7\nnot a decodable image\n");
        ThumbnailProvider provider; QSize size;
        const auto image = provider.requestImage(ThumbnailProvider::url(input).mid(QString("image://thumbnail/").size()), &size, QSize(64, 64));
        QVERIFY(!image.isNull()); QCOMPARE(image.pixelColor(0, 0), QColor(Qt::blue));
    }
};
int main(int argc, char **argv) {
    QTemporaryDir cache, data;
    qputenv("XDG_CACHE_HOME", cache.path().toUtf8());
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    QGuiApplication app(argc, argv);
    CoreTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "core.moc"
