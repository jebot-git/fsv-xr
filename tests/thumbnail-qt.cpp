#include "thumbnail.h"
#include <QGuiApplication>
#include <QTemporaryDir>
#include <QImage>
#include <QFile>
#include <QDebug>
#include <QCryptographicHash>
#include <QUrl>
#include <QFileInfo>
#include <QDateTime>
#include <QDir>
int main(int argc,char**argv){QGuiApplication app(argc,argv);QTemporaryDir dir;qputenv("XDG_CACHE_HOME",dir.path().toUtf8());QString file=dir.filePath("large ü.png");QImage image(6000,4000,QImage::Format_RGB888);image.fill(Qt::red);if(!image.save(file))return 1;image={};ThumbnailProvider provider;QSize size;auto url=ThumbnailProvider::url(file);QImage preview=provider.requestImage(url.mid(QString("image://thumbnail/").size()),&size,QSize(512,512));if(preview.isNull() || preview.width()>512 || preview.height()>512 || preview.pixelColor(0,0)!=QColor(Qt::red))return 2;QString uri=QUrl::fromLocalFile(file).toString(QUrl::FullyEncoded);QString hash=QString::fromLatin1(QCryptographicHash::hash(uri.toUtf8(),QCryptographicHash::Md5).toHex());QString cache=dir.path()+"/thumbnails/large";QDir().mkpath(cache);QImage cached(256,128,QImage::Format_RGB32);cached.fill(Qt::blue);cached.setText("Thumb::URI",uri);cached.setText("Thumb::MTime",QString::number(QFileInfo(file).lastModified().toSecsSinceEpoch()));if(!cached.save(cache+"/"+hash+".png"))return 3;preview=provider.requestImage(url.mid(QString("image://thumbnail/").size()),&size,QSize(512,512));if(preview.isNull() || preview.pixelColor(0,0)!=QColor(Qt::blue))return 4;qInfo()<<"PASS optional Qt inspector/image-provider and bounded thumbnail-cache validation";return 0;}
