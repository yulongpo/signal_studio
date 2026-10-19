#pragma once
#include <QIcon>
#include <QLabel>
#include <QPixmap>
class QDialog;
namespace signalstudio {
class BrandAssets {
public:
    static QIcon windowIcon();
    static QPixmap pixmap(const QString& kind, const QSize& size, qreal dpr,
                          const QString& variant = "dark", bool raster = false);
    static QLabel* label(const QString& kind, const QSize& size, QWidget* parent,
                         const char* name, const QString& variant = "dark");
    static QString buildInformation();
    static QDialog* aboutDialog(QWidget* parent);
};
}
