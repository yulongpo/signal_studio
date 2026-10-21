#pragma once
#include <QLayout>
#include <QStackedWidget>

namespace signalstudio {
// The visible inspector page determines scrolling. An expanded editor on a
// different page must not leave a large blank scroll region on this page.
class ContextStack final : public QStackedWidget {
 public:
  explicit ContextStack(QWidget* parent = nullptr) : QStackedWidget(parent) {
    layout()->setSizeConstraint(QLayout::SetNoConstraint);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    connect(this, &QStackedWidget::currentChanged, this,
            [this] { updateGeometry(); });
  }
  QSize sizeHint() const override {
    return currentWidget() ? currentWidget()->sizeHint() : QSize(0, 0);
  }
  QSize minimumSizeHint() const override { return {0, 0}; }
  bool hasHeightForWidth() const override {
    return currentWidget() && currentWidget()->hasHeightForWidth();
  }
  int heightForWidth(int width) const override {
    return currentWidget() && currentWidget()->hasHeightForWidth()
        ? currentWidget()->heightForWidth(width) : sizeHint().height();
  }
};
}
