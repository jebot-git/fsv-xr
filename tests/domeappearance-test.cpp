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
    void liveBitmapAndQtFallback() {
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
        save(R"({"font":"Cozette / Medium","font_size":13,"light":false})");
        {
            Appearance appearance(true);
            QQmlEngine engine;
            engine.rootContext()->setContextProperty("appearance", &appearance);
            QQmlComponent component(&engine);
            component.setData(R"(import QtQuick; import QtQuick.Controls; import "qrc:/qml" as Fsv; Label { font: Fsv.Theme.font; text: "Live bitmap" })", QUrl());
            auto qml = std::unique_ptr<QObject>(component.create());
            QVERIFY2(qml != nullptr, qPrintable(component.errorString()));
            auto quickFont = [&] { return qml->property("font").value<QFont>(); };
            QTRY_COMPARE(QFontInfo(quickFont()).family(), QString("Cozette"));
            QCOMPARE(quickFont().styleName(), QString("Medium"));
            QLabel label("English 日本語 한글");
            label.show();
            QTRY_COMPARE(QFontInfo(label.font()).family(), QString("Cozette"));
            QCOMPARE(QFontInfo(label.font()).pixelSize(), 13);
            QCOMPARE(label.font().styleName(), QString("Medium"));
            QVERIFY(!QFontDatabase::isSmoothlyScalable("Cozette","Medium"));
            // A forged outline choice must fall back to Qt, not be accepted as a bitmap.
            save(R"({"font":"Noto Sans / Regular","font_size":13,"light":true})");
            QTRY_COMPARE(appearance.font(), original);
            QTRY_COMPARE(quickFont(), original);
            QTRY_COMPARE(QApplication::palette().color(QPalette::Window), QColor(229,233,239));
            save(R"({"font":"Cozette / Medium","font_size":13,"light":false})");
            QTRY_COMPARE(QFontInfo(appearance.font()).family(), QString("Cozette"));
            // An unavailable fixed strike must not be scaled to a requested size.
            save(R"({"font":"Cozette / Medium","font_size":14})");
            QTRY_COMPARE(appearance.font(), original);
            save(R"({"font":"Cozette / Medium","font_size":13})");
            QTRY_COMPARE(QFontInfo(appearance.font()).family(), QString("Cozette"));
            save("{"); QTest::qWait(200);
            QCOMPARE(QFontInfo(appearance.font()).family(), QString("Cozette"));
        }
        QApplication::setFont(original); QApplication::setPalette(palette);
        {
            Appearance native(false);
            save(R"({"font":"Cozette / Medium","font_size":13,"light":true})");
            QTest::qWait(200);
            QCOMPARE(QApplication::font(), original);
            QCOMPARE(QApplication::palette(), palette);
            QFont changed("sans-serif");changed.setPixelSize(19);
            QApplication::setFont(changed);
            QCOMPARE(native.font(), changed);
        }
    }
};
QTEST_MAIN(AppearanceTest)
#include "domeappearance-test.moc"
