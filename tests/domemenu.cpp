// SPDX-License-Identifier: LGPL-2.1-or-later
#include "domemenu.h"
#include "filesystem.h"
#include <QAction>
#include <QTemporaryDir>
#include <QFile>
#include <QWidget>
#include <QtTest>

class DomeMenuTest : public QObject {
    Q_OBJECT
private slots:
    void liveActionsAndDispatch() {
        QTemporaryDir temp;
        QFile file(temp.filePath("fixture.txt"));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("menu fixture"); file.close();
        FileSystem files;
        QWidget owner;
        DomeMenu menus(&files, &owner);
        QSignalSpy commands(&menus, &DomeMenu::commandRequested);
        auto action = [&menus](const char *id) { return menus.findChild<QAction *>(id); };
        files.navigate(temp.path()); QTRY_VERIFY(!files.busy());
        QVERIFY(!action("rename")->isEnabled());
        files.setSelected(0);
        QVERIFY(action("rename")->isEnabled());
        QVERIFY(action("open")->isEnabled());
        QVERIFY(!action("restore")->isEnabled());
        action("rename")->trigger();
        QCOMPARE(commands.takeFirst().first().toString(), QString("rename"));
        action("tree")->trigger();
        QCOMPARE(files.layout(), QString("tree"));
        QVERIFY(action("tree")->isChecked());
        QVERIFY(!action("map")->isChecked());
        QVERIFY(!action("closeTab")->isEnabled());
        action("newTab")->trigger(); QTRY_VERIFY(!files.busy());
        QCOMPARE(files.tabs().size(), 2);
        QVERIFY(action("closeTab")->isEnabled());
        action("closeTab")->trigger(); QTRY_VERIFY(!files.busy());
        QCOMPARE(files.tabs().size(), 1);
        files.setSelected(-1);
        QVERIFY(!action("rename")->isEnabled());
        // Clipboard commands still reach focused text fields without a file.
        QVERIFY(!action("copy")->isEnabled());
        menus.setTextEditing(true);
        QVERIFY(action("copy")->isEnabled());
        action("copy")->trigger();
        QCOMPARE(commands.takeFirst().first().toString(), QString("copy"));
        menus.setTextEditing(false);
        files.navigate("trash:/"); QTRY_VERIFY(!files.busy());
        QVERIFY(!action("mkdir")->isEnabled());
        QVERIFY(!action("terminal")->isEnabled());
        QVERIFY(action("emptyTrash")->isEnabled());
    }
};
QTEST_MAIN(DomeMenuTest)
#include "domemenu.moc"
