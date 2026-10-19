#pragma once

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QWidget>
#include <QtMath>
#include <functional>

namespace signalstudio {

// Reports cumulative desktop deltas in Qt logical pixels. The owner snapshots
// panel sizes in beginResize, clamps resizeDelta and saves only in endResize.
class PanelResizeHandle final : public QWidget {
 public:
  explicit PanelResizeHandle(Qt::Orientation orientation,
                             QWidget* parent = nullptr)
      : QWidget(parent), orientation_(orientation) {
    setFocusPolicy(Qt::StrongFocus);
    if (orientation_ == Qt::Horizontal) {
      setCursor(Qt::SplitHCursor);
      setFixedWidth(5);
      setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    } else {
      setCursor(Qt::SplitVCursor);
      setFixedHeight(5);
      setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
  }

  ~PanelResizeHandle() override {
    // The owning window may already be destructing; do not call its callbacks.
    clearTracking();
  }

  std::function<void()> beginResize;
  std::function<void(int)> resizeDelta;
  std::function<void()> endResize;
  std::function<void()> cancelResize;

  bool isDragging() const { return dragging_; }

  void cancel() {
    if (!dragging_) return;
    clearTracking();
    if (cancelResize) cancelResize();
  }

 protected:
  void mousePressEvent(QMouseEvent* event) override {
    if (event->button() != Qt::LeftButton) {
      QWidget::mousePressEvent(event);
      return;
    }
    if (dragging_) {
      event->accept();
      return;
    }
    setFocus(Qt::MouseFocusReason);
    origin_ = event->globalPosition();
    lastDelta_ = 0;
    dragWindow_ = window();
    dragging_ = true;
    if (qApp) qApp->installEventFilter(this);
    const QPointer<PanelResizeHandle> alive(this);
    if (beginResize) beginResize();
    if (alive && dragging_) grabMouse();
    event->accept();
  }

  void mouseMoveEvent(QMouseEvent* event) override {
    if (!dragging_) {
      QWidget::mouseMoveEvent(event);
      return;
    }
    if (!(event->buttons() & Qt::LeftButton)) {
      cancel();
      event->accept();
      return;
    }
    reportDelta(event->globalPosition());
    event->accept();
  }

  void mouseReleaseEvent(QMouseEvent* event) override {
    if (!dragging_ || event->button() != Qt::LeftButton) {
      QWidget::mouseReleaseEvent(event);
      return;
    }
    const QPointer<PanelResizeHandle> alive(this);
    reportDelta(event->globalPosition());
    if (alive && dragging_) {
      clearTracking();
      if (endResize) endResize();
    }
    event->accept();
  }

  bool event(QEvent* event) override {
    if (event->type() == QEvent::UngrabMouse ||
        event->type() == QEvent::WindowDeactivate ||
        event->type() == QEvent::FocusOut || event->type() == QEvent::Hide) {
      cancel();
    }
    return QWidget::event(event);
  }

  bool eventFilter(QObject* watched, QEvent* event) override {
    if (!dragging_) return false;
    if (event->type() == QEvent::ApplicationDeactivate ||
        (watched == dragWindow_ && event->type() == QEvent::WindowDeactivate)) {
      cancel();
      return false;
    }
    const auto* widget = qobject_cast<QWidget*>(watched);
    if (event->type() == QEvent::Hide && widget &&
        (widget == this || widget->isAncestorOf(this))) {
      cancel();
      return false;
    }
    if (event->type() == QEvent::KeyPress && widget &&
        widget->window() == dragWindow_ &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
      cancel();
      event->accept();
      return true;
    }
    return false;
  }

 private:
  void reportDelta(QPointF position) {
    const qreal delta = orientation_ == Qt::Horizontal
                            ? position.x() - origin_.x()
                            : position.y() - origin_.y();
    const int rounded = qRound(delta);
    if (rounded == lastDelta_) return;
    lastDelta_ = rounded;
    if (resizeDelta) resizeDelta(rounded);
  }

  void clearTracking() {
    dragging_ = false;
    if (qApp) qApp->removeEventFilter(this);
    dragWindow_.clear();
    // releaseMouse emits UngrabMouse; dragging_ is already false, so cleanup
    // cannot invoke cancellation after a successful release or invoke it twice.
    if (mouseGrabber() == this) releaseMouse();
  }

  Qt::Orientation orientation_;
  QPointF origin_;
  QPointer<QWidget> dragWindow_;
  int lastDelta_ = 0;
  bool dragging_ = false;
};

}  // namespace signalstudio
