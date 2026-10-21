// SPDX-License-Identifier: LGPL-2.1-or-later
#include "thumbnail.h"
#include "archives.h"
#include <QGuiApplication>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QDir>
#include <QImageReader>
#include <QMimeDatabase>
#include <QPainter>
#include <QProcess>
#include <QHash>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

QString ThumbnailProvider::url(const QString &path, qint64 modifiedMs, bool archivePreview)
{
    // Base64 keeps #, ?, %, and non-ASCII filenames intact through QML URL handling.
    return "image://thumbnail/" + QString::fromLatin1(path.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))
        + '/' + QString::number(modifiedMs < 0 ? QFileInfo(Archives::isLocation(path) ? Archives::source(path) : path).lastModified().toMSecsSinceEpoch() : modifiedMs)
        + (archivePreview ? "/preview" : "");
}
static QImage readImage(const QString &path, const QSize &size)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize original = reader.size();
    if (original.isValid()) reader.setScaledSize(original.scaled(size, Qt::KeepAspectRatio));
    return reader.read();
}
static QHash<QString, QString> thumbnailerEntry(const QString &path)
{
    // Desktop Entry values are not QSettings INI strings: quotes in Exec must
    // survive parsing, and semicolons delimit MIME lists rather than comments.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) return {};
    QHash<QString, QString> result;
    bool group = false;
    for (const auto &line : QString::fromUtf8(file.readAll()).split('\n')) {
        const QString text = line.trimmed();
        if (text.startsWith('[')) { group = text == "[Thumbnailer Entry]"; continue; }
        if (!group || text.startsWith('#')) continue;
        const int equals = text.indexOf('=');
        if (equals < 1) continue;
        QString value = text.mid(equals + 1), decoded;
        for (int i = 0; i < value.size(); ++i) {
            if (value[i] == '\\' && i + 1 < value.size()) {
                const QChar next = value[i + 1];
                if (next == '\\' || next == 's' || next == 'n' || next == 't' || next == 'r') {
                    decoded += next == 's' ? QChar(' ') : next == 'n' ? QChar('\n') : next == 't' ? QChar('\t') : next == 'r' ? QChar('\r') : QChar('\\');
                    ++i; continue;
                }
            }
            decoded += value[i];
        }
        result.insert(text.left(equals).trimmed(), decoded);
    }
    return result;
}
static QString expandArgument(const QString &arg, const QString &path, const QString &uri, const QString &output, int size)
{
    QString result;
    for (int i = 0; i < arg.size(); ++i) {
        if (arg[i] == '%' && i + 1 < arg.size()) {
            const QChar code = arg[++i];
            if (code == 'i') result += path;
            else if (code == 'u') result += uri;
            else if (code == 'o') result += output;
            else if (code == 's') result += QString::number(size);
            else if (code == '%') result += '%';
            else { result += '%'; result += code; }
        } else result += arg[i];
    }
    return result;
}
QImage ThumbnailProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    const QString original = QString::fromUtf8(QByteArray::fromBase64(id.section('/', 0, 0).toLatin1(), QByteArray::Base64UrlEncoding));
    const bool archived = Archives::isLocation(original);
    QString path = original;
    if (archived) path = id.section('/', 2, 2) == "preview" ? Archives::materialize(original).path : QString();
    const QFileInfo info(path);
    const QSize target = requestedSize.isValid() ? requestedSize.boundedTo(QSize(1024, 1024)) : QSize(256, 256);
    const QUrl fileUrl = QUrl::fromLocalFile(path);
    const QString uri = fileUrl.toString(QUrl::FullyEncoded);
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(uri.toUtf8(), QCryptographicHash::Md5).toHex());
    const QString displayName = archived ? Archives::title(original) : info.fileName();
    const QString mime = archived ? QMimeDatabase().mimeTypeForFile(displayName, QMimeDatabase::MatchExtension).name()
        : QMimeDatabase().mimeTypeForFile(info).name();
    QImage result;
    if (info.isFile()) {
        const QString cache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/thumbnails/";
        for (const auto &folder : {"xx-large", "x-large", "large", "normal"}) {
            const QString cachedPath = cache + folder + '/' + hash + ".png";
            QImage cached(cachedPath);
            if (!cached.isNull() && cached.text("Thumb::URI") == uri &&
                cached.text("Thumb::MTime").toLongLong() == info.lastModified().toSecsSinceEpoch()) {
                result = cached.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation); break;
            }
        }
        if (result.isNull() && info.size() <= 64 * 1024 * 1024) result = readImage(path, target);
        // Use registered system thumbnailers for PDF, video, office and other MIME types.
        // Parse argv and invoke directly; filenames never enter a shell command.
        if (result.isNull()) {
            const auto dirs = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, "thumbnailers", QStandardPaths::LocateDirectory);
            for (const auto &dir : dirs) {
                for (const auto &file : QDir(dir).entryList({"*.thumbnailer"}, QDir::Files)) {
                    const auto settings = thumbnailerEntry(dir + '/' + file);
                    QStringList types = settings.value("MimeType").split(';', Qt::SkipEmptyParts);
                    if (!types.contains(mime)) continue;
                    const QString tryExec = settings.value("TryExec");
                    if (!tryExec.isEmpty() && QStandardPaths::findExecutable(tryExec).isEmpty()) continue;
                    QStringList argv = QProcess::splitCommand(settings.value("Exec"));
                    if (argv.isEmpty()) continue;
                    QTemporaryDir temp;
                    if (!temp.isValid()) continue;
                    const QString output = temp.filePath("preview.png");
                    for (auto &arg : argv)
                        arg = expandArgument(arg, path, uri, output, qMax(target.width(), target.height()));
                    QProcess process;
                    process.setProgram(argv.takeFirst()); process.setArguments(argv);
                    process.setStandardOutputFile(QProcess::nullDevice());
                    process.setStandardErrorFile(QProcess::nullDevice());
                    process.start();
                    if (!process.waitForFinished(4000)) { process.kill(); process.waitForFinished(); }
                    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0) result = readImage(output, target);
                    if (!result.isNull()) break;
                }
                if (!result.isNull()) break;
            }
        }
    }
    if (result.isNull()) {
        result = QImage(target, QImage::Format_ARGB32_Premultiplied);
        result.fill(info.isDir() ? QColor("#355a78") : QColor("#223345"));
        QPainter painter(&result);
        painter.setPen(QColor("#e6edf3"));
        QFont font = QGuiApplication::font();
        if (!qGuiApp->property("domeBitmapAppearance").toBool()) font.setPixelSize(qMax(10, target.width() / 18));
        painter.setFont(font);
        QString text = info.isDir() ? "DIRECTORY" : mime + "\n\n" + displayName;
        if (!path.isEmpty() && (mime.startsWith("text/") || mime == "application/json" || mime.endsWith("+xml"))) {
            QFile file(path);
            if (file.open(QIODevice::ReadOnly)) text = QString::fromUtf8(file.read(4096));
        }
        painter.drawText(result.rect().adjusted(12, 12, -12, -12), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, text);
    }
    if (size) *size = result.size();
    return result;
}
