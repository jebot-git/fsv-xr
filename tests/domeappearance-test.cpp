#include "appearance.h"
#include <QQmlEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QLabel>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QtTest>

class AppearanceTest : public QObject {
    Q_OBJECT
private slots:
    void liveBdfAndRejectedOutline() {
        QTemporaryDir config;
        QVERIFY(config.isValid());
        const auto oldConfig = qgetenv("XDG_CONFIG_HOME");
        const QFont original = QApplication::font();
        const QPalette palette = QApplication::palette();
        struct Restore { QByteArray config; QFont font; QPalette palette; ~Restore() {
            if(config.isNull()) qunsetenv("XDG_CONFIG_HOME"); else qputenv("XDG_CONFIG_HOME",config);
            QApplication::setFont(font); QApplication::setPalette(palette);
        }} restore{oldConfig,original,palette};
        qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
        const int loaded = QFontDatabase::addApplicationFont(QStringLiteral(DOME_FONT_FIXTURE));
        QVERIFY(loaded >= 0);
        QDir(config.path()).mkdir("dome");
        const QString path = config.filePath("dome/appearance.json");
        auto save = [&](const QByteArray &data) { QSaveFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(data); QVERIFY(file.commit()); };
        save(R"({"font":"Ttyp0 / Regular","font_size":13,"light":false})");
        {
            Appearance appearance(true);
            QQmlEngine engine;
            engine.rootContext()->setContextProperty("appearance", &appearance);
            QQmlComponent component(&engine);
            component.setData(R"(import QtQuick; import QtQuick.Controls; import "qrc:/qml" as Fsv; Label { font: Fsv.Theme.font; text: "Live bitmap" })", QUrl());
            auto qml = std::unique_ptr<QObject>(component.create());
            QVERIFY2(qml != nullptr, qPrintable(component.errorString()));
            auto quickFont = [&] { return qml->property("font").value<QFont>(); };
            QTRY_VERIFY(QFontInfo(quickFont()).family().startsWith("Dome Qt BDF "));
            QCOMPARE(quickFont().styleName(), QString("Regular"));
            QLabel label("English 日本語 한글");
            label.show();
            QTRY_VERIFY(QFontInfo(label.font()).family().startsWith("Dome Qt BDF "));
            QCOMPARE(QFontInfo(label.font()).pixelSize(), 13);
            QCOMPARE(label.font().styleName(), QString("Regular"));
            QVERIFY(!QFontDatabase::isSmoothlyScalable("Ttyp0","Regular"));
            // A forged outline choice must preserve the previous valid BDF font.
            save(R"({"font":"Noto Sans / Regular","font_size":13,"light":true})");
            QTRY_VERIFY(QFontInfo(appearance.font()).family().startsWith("Dome Qt BDF "));
            QTRY_VERIFY(QFontInfo(quickFont()).family().startsWith("Dome Qt BDF "));
            QCOMPARE(QApplication::palette().color(QPalette::Window), QColor(31,37,45));
            save(R"({"font":"Ttyp0 / Regular","font_size":13,"light":false})");
            QTRY_VERIFY(QFontInfo(appearance.font()).family().startsWith("Dome Qt BDF "));
            // A second real native strike resolves from its own BDF file.
            save(R"({"font":"Ttyp0 / Regular","font_size":14})");
            QTRY_COMPARE(QFontInfo(appearance.font()).pixelSize(), 14);
            save(R"({"font":"Ttyp0 / Regular","font_size":13})");
            QTRY_VERIFY(QFontInfo(appearance.font()).family().startsWith("Dome Qt BDF "));
            save("{"); QTest::qWait(200);
            QVERIFY(QFontInfo(appearance.font()).family().startsWith("Dome Qt BDF "));
        }
        QApplication::setFont(original); QApplication::setPalette(palette);
        {
            Appearance native(false);
            save(R"({"font":"Ttyp0 / Regular","font_size":13,"light":true})");
            QTest::qWait(200);
            QVERIFY(QFontInfo(QApplication::font()).family().startsWith("Dome Qt BDF "));
            QCOMPARE(QFontInfo(QApplication::font()).pixelSize(), 16);
            QCOMPARE(QApplication::palette(), palette);
            QFont changed("sans-serif");changed.setPixelSize(19);
            QApplication::setFont(changed);
            QCOMPARE(native.font(), changed);
        }
    }
};
QTEST_MAIN(AppearanceTest)
#include "domeappearance-test.moc"
