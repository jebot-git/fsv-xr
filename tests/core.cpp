// SPDX-License-Identifier: LGPL-2.1-or-later
#include "filesystem.h"
#include "gestures.h"
#include "thumbnail.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QSignalSpy>
#include <QGuiApplication>
#include <QClipboard>
#include <QElapsedTimer>

class CoreTest : public QObject {
    Q_OBJECT
    static void put(const QString &path, QByteArray content = "sample") {
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(content), content.size());
    }
    static QList<QVector3D> hand() {
        QList<QVector3D> joints(26, QVector3D(0, 12, 0));
        joints[1] = {0, 0, 0}; joints[6] = {-4, 0, 0}; joints[21] = {4, 0, 0};
        joints[5] = {0, 5, 0};
        joints[10] = {-7, 12, 0}; joints[15] = {0, 14, 0}; joints[20] = {7, 12, 0};
        return joints;
    }
private slots:
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
        QCOMPARE(places.size(), 1);
        QCOMPARE(places[0].toMap()["path"].toString(), target);
        QCOMPARE(places[0].toMap()["name"].toString(), QString("Shared bookmark"));
        QCOMPARE(places[0].toMap()["icon"].toString(), QString("folder-pictures"));
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
    QTemporaryDir cache;
    qputenv("XDG_CACHE_HOME", cache.path().toUtf8());
    QGuiApplication app(argc, argv);
    CoreTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "core.moc"
