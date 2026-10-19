// SPDX-License-Identifier: LGPL-2.1-or-later
#include "appearance.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QSaveFile>
#include <QtTest>

class AppearanceTest : public QObject {
    Q_OBJECT
private slots:
    void liveDomeTheme() {
        QTemporaryDir temp;
        const auto originalConfig = qgetenv("XDG_CONFIG_HOME");
        qputenv("XDG_CONFIG_HOME", temp.path().toUtf8());
        const auto originalPalette = QApplication::palette();
        struct Restore { QByteArray config; QPalette palette; ~Restore() {
            if (config.isNull()) qunsetenv("XDG_CONFIG_HOME"); else qputenv("XDG_CONFIG_HOME", config);
            QApplication::setPalette(palette);
        } } restore{originalConfig, originalPalette};
        Appearance theme(true);
        QCOMPARE(QApplication::palette().color(QPalette::Window), QColor(31, 37, 45));
        QCOMPARE(theme.panelColor().alpha(), 248);
        QDir(temp.path()).mkdir("dome");
        const QString path = temp.filePath("dome/appearance.json");
        auto save = [path](QByteArray data) {
            QSaveFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(data), data.size()); QVERIFY(file.commit());
        };
        save("{\"light\":true,\"accent\":\"#d33a79\",\"background\":[0.2,0.3,0.4],\"opacity\":200}");
        QTRY_COMPARE(QApplication::palette().color(QPalette::Window), QColor(229,233,239));
        QCOMPARE(QApplication::palette().color(QPalette::Accent), QColor("#d33a79"));
        QCOMPARE(theme.panelColor().alpha(), 200);
        QCOMPARE(theme.sceneBackground(), QColor::fromRgbF(.2,.3,.4));
        save("{\"light\":false,\"accent\":\"#53ad7f\",\"background\":[0.02,0.03,0.04],\"opacity\":240}");
        QTRY_COMPARE(QApplication::palette().color(QPalette::Window), QColor(31,37,45));
        QCOMPARE(QApplication::palette().color(QPalette::Accent), QColor("#53ad7f"));
        save("{\"light\":true,\"accent\":\"invalid\"}");
        QTest::qWait(100);
        QCOMPARE(QApplication::palette().color(QPalette::Window), QColor(31,37,45));
        QVERIFY(QFile::remove(path));
        QTRY_COMPARE(QApplication::palette().color(QPalette::Accent), QColor("#6babff"));
        Appearance desktop(false);
        QCOMPARE(desktop.panelColor(), QApplication::palette().color(QPalette::Window));
    }
};
QTEST_MAIN(AppearanceTest)
#include "appearance.moc"
