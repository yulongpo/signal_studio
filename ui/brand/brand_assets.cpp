#include "ui/brand/brand_assets.h"
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QImageReader>
#include <QPixmapCache>
#include <QPushButton>
#include <QVBoxLayout>
#include <QResource>
#include <QPainter>

static void initializeBrandResources() { Q_INIT_RESOURCE(branding); }
namespace signalstudio {
QIcon BrandAssets::windowIcon() {
    initializeBrandResources();
    QIcon icon;
    for (int n : {16,24,32,48,64,128,256,512})
        icon.addFile(QString(":/branding/raster/signal_studio_icon_%1.png").arg(n), QSize(n,n));
    return icon;
}
QPixmap BrandAssets::pixmap(const QString& kind, const QSize& size, qreal dpr,
                          const QString& variant, bool raster) {
    initializeBrandResources();
    raster = raster || qEnvironmentVariableIsSet("SS_BRAND_FORCE_PNG");
    const auto key = QString("brand:%1:%2:%3:%4:%5:%6").arg(kind,variant).arg(size.width()).arg(size.height()).arg(dpr).arg(raster);
    QPixmap result;
    if (QPixmapCache::find(key, &result)) return result;
    const QSize pixels(qCeil(size.width()*dpr),qCeil(size.height()*dpr));
    QImageReader reader(QString(":/branding/%1-%2.svg").arg(kind,variant));
    reader.setScaledSize(pixels);
    QImage image;
    if (!raster) image = reader.read();
    if (image.isNull()) image.load(kind=="icon" ? ":/branding/raster/signal_studio_icon_512.png" :
        QString(":/branding/raster/signal_studio_%1_1024.png").arg(kind));
    if (image.isNull()) return {};
    result = QPixmap(pixels); result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const auto fitted = image.size().scaled(pixels,Qt::KeepAspectRatio);
    painter.drawImage(QRect(QPoint((pixels.width()-fitted.width())/2,(pixels.height()-fitted.height())/2),fitted),image);
    painter.end(); result.setDevicePixelRatio(dpr); QPixmapCache::insert(key,result); return result;
}
QLabel* BrandAssets::label(const QString& kind,const QSize& size,QWidget* parent,const char* name,const QString& variant) {
    auto* label = new QLabel(parent); label->setObjectName(name); label->setFixedSize(size);
    label->setPixmap(pixmap(kind,size,parent?parent->devicePixelRatioF():qApp->devicePixelRatio(),variant));
    label->setAccessibleName("Signal Studio"); label->setStyleSheet("background:transparent;border:0;padding:0;"); return label;
}
QString BrandAssets::buildInformation() {
    return QString("Signal Studio %1\nImport A1.2 / Brand v2.0\nBuild: %2\nQt: %3\nCompiler: %4\nBuilt: %5 %6")
        .arg(SS_PRODUCT_VERSION,SS_BUILD_REVISION,QT_VERSION_STR,
#ifdef _MSC_VER
        QString("MSVC %1 / C++20 / x64").arg(_MSC_VER),
#else
        QString("C++20"),
#endif
        __DATE__,__TIME__);
}
QDialog* BrandAssets::aboutDialog(QWidget* parent) {
    auto* dialog=new QDialog(parent);dialog->setObjectName("aboutSignalStudioDialog");
    dialog->setWindowTitle("关于 Signal Studio");dialog->setWindowIcon(windowIcon());dialog->setModal(true);dialog->setFixedWidth(540);
    dialog->setStyleSheet("QDialog{background:#101d2f;color:#dbe8f8;} QLabel{color:#bed5e6;background:transparent;} QPushButton{padding:8px 16px;background:#203e56;color:#dbe8f8;border:1px solid #43627c;border-radius:5px;}");
    auto* layout=new QVBoxLayout(dialog);layout->setContentsMargins(28,26,28,24);layout->setSpacing(18);
    layout->addWidget(label("lockup",QSize(360,90),dialog,"aboutBrand"),0,Qt::AlignCenter);
    auto* info=new QLabel(buildInformation());info->setObjectName("aboutBuildInformation");info->setTextInteractionFlags(Qt::TextSelectableByMouse);info->setStyleSheet("font-size:13px;");layout->addWidget(info);
    auto* license=new QLabel("许可与资源声明\nQt 采用动态链接；Qt 与插件的许可说明：qt.io/licensing/open-source-lgpl-obligations。\nBrand v2 为生成的 SVG 路径及 PNG/ICO，文字已轮廓化，不分发字体。\n真实 IQ / ADC 分析与标为 Demo 的功能分开显示。");license->setWordWrap(true);layout->addWidget(license);
    auto* buttons=new QDialogButtonBox;auto* copy=buttons->addButton("复制构建信息",QDialogButtonBox::ActionRole);copy->setObjectName("aboutCopyBuild");
    QObject::connect(copy,&QPushButton::clicked,dialog,[]{qApp->clipboard()->setText(buildInformation());});
    auto* close=buttons->addButton("关闭",QDialogButtonBox::RejectRole);QObject::connect(close,&QPushButton::clicked,dialog,&QDialog::reject);layout->addWidget(buttons);return dialog;
}
}
