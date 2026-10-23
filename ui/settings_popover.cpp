#include "ui/settings_popover.h"

#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>
#include <QLayout>
#include <QPalette>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>

#include "ui/parameter_input.h"

namespace signalstudio {

SettingsPopoverHost::SettingsPopoverHost(QWidget* root)
    : QObject(root), root_(root) {
  if (qApp) qApp->installEventFilter(this);
}

SettingsPopoverHost::~SettingsPopoverHost() {
  if (qApp) qApp->removeEventFilter(this);
  for (const auto& entry : entries_)
    if (entry.frame) delete entry.frame.data();
}

bool SettingsPopoverHost::belongsTo(QObject* owner, QObject* object) {
  if (!owner || !object) return false;
  for (auto* current = object; current; current = current->parent()) {
    if (current == owner) return true;
    // Native window notifications can target a QWindow instead of a widget.
    if (auto* window = qobject_cast<QWindow*>(current)) {
      if (auto* widget = qobject_cast<QWidget*>(owner)) {
        if (widget->windowHandle() == window) return true;
        for (auto* child : widget->findChildren<QWidget*>())
          if (child->windowHandle() == window) return true;
      }
    }
  }
  return false;
}

QFrame* SettingsPopoverHost::add(QAbstractButton* anchor, QWidget* content,
                                const QString& objectName) {
  if (!root_ || !anchor || !content) return nullptr;
  for (const auto& entry : entries_)
    if (entry.anchor == anchor) return entry.frame;
  auto* frame = new QFrame(root_);
  frame->setObjectName(objectName);
  frame->setFrameShape(QFrame::StyledPanel);
  frame->setAutoFillBackground(true);
  frame->setBackgroundRole(QPalette::Window);
  // An explicit opaque background also covers applications with transparent
  // QFrame stylesheet rules. Parameter children keep the application style.
  frame->setStyleSheet(QStringLiteral(
      "QFrame#%1 { background-color: #1b2230; border: 1px solid #394354; "
      "border-radius: 6px; }").arg(objectName));
  auto* layout = new QVBoxLayout(frame);
  // The visible client rectangle takes precedence over content minimums;
  // QScrollArea supplies access when the available space is smaller.
  layout->setSizeConstraint(QLayout::SetNoConstraint);
  layout->setContentsMargins(6, 6, 6, 6);
  auto* scroll = new QScrollArea(frame);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setWidgetResizable(true);
  scroll->setWidget(content);
  layout->addWidget(scroll);
  frame->hide();
  entries_.append({anchor, frame, scroll, content});
  connect(anchor, &QAbstractButton::clicked, this,
          [this, guard = QPointer<QAbstractButton>(anchor)] {
            if (guard) toggle(guard);
          });
  connect(anchor, &QObject::destroyed, this, [this] {
    if (active_ && !activeAnchor_) close(PopoverCloseReason::AnchorHidden);
  });
  connect(content, &QObject::destroyed, this, [this] {
    queueReposition();
  });
  return frame;
}

void SettingsPopoverHost::open(QAbstractButton* registeredAnchor,
                               QWidget* anchorOverride) {
  if (!root_ || !registeredAnchor) return;
  QWidget* placement = anchorOverride ? anchorOverride : registeredAnchor;
  if (!placement->isVisible() || !root_->isVisible()) return;
  for (const auto& entry : entries_) {
    if (entry.anchor != registeredAnchor || !entry.frame || !entry.content)
      continue;
    if (active_ == entry.frame && positioningAnchor_ == placement) {
      reposition();
      return;
    }
    close(PopoverCloseReason::Replaced);
    activeAnchor_ = registeredAnchor;
    positioningAnchor_ = placement;
    active_ = entry.frame;
    ++revision_;
    reposition();
    if (active_) {
      active_->show();
      active_->raise();
    }
    return;
  }
}

void SettingsPopoverHost::toggle(QAbstractButton* anchor, QWidget* anchorOverride) {
  auto* placement = anchorOverride ? anchorOverride : anchor;
  if (active_ && activeAnchor_ == anchor && positioningAnchor_ == placement) close();
  else open(anchor, anchorOverride);
}

void SettingsPopoverHost::close(PopoverCloseReason reason) {
  QPointer<QFrame> frame = active_;
  active_.clear();
  activeAnchor_.clear();
  positioningAnchor_.clear();
  ++revision_;
  if (!frame) return;
  if (reason == PopoverCloseReason::Escape ||
      reason == PopoverCloseReason::Context) {
    QList<QPointer<QWidget>> widgets;
    widgets.append(frame.data());
    for (auto* child : frame->findChildren<QWidget*>()) widgets.append(child);
    for (const auto& widget : widgets) {
      if (!widget) continue;
      if (auto* draft = dynamic_cast<ParameterDraft*>(widget.data()))
        draft->discard();
      if (!widget) continue;
      const auto children = widget->children();
      for (auto* child : children)
        if (auto* draft = dynamic_cast<ParameterComboCommit*>(child))
          draft->discard();
    }
  }
  if (!frame) return;
  // Hiding a focused QWidget can synthesize TabFocusReason while Qt chooses
  // the next visible widget. That transition is not the user's Tab command.
  frame->setProperty("parameterLayerHiding", true);
  for (auto* combo : frame->findChildren<QComboBox*>()) combo->hidePopup();
  // Other owned popup windows (including menus) must not outlive this layer.
  for (auto* child : frame->findChildren<QWidget*>())
    if (child->isWindow() && child->isVisible()) child->hide();
  frame->hide();
  frame->setProperty("parameterLayerHiding", false);
}

void SettingsPopoverHost::closeForContextChange() {
  close(PopoverCloseReason::Context);
}

QFrame* SettingsPopoverHost::activePopover() const { return active_; }

void SettingsPopoverHost::queueReposition() {
  if (!active_ || repositionPending_) return;
  repositionPending_ = true;
  QTimer::singleShot(0, this, [this] {
    repositionPending_ = false;
    reposition();
  });
}

void SettingsPopoverHost::reposition() {
  if (!active_ || positioning_) return;
  if (!root_ || !activeAnchor_ || !positioningAnchor_ ||
      !root_->isVisible() || !positioningAnchor_->isVisible()) {
    close(PopoverCloseReason::AnchorHidden);
    return;
  }
  const Entry* entry = nullptr;
  for (const auto& candidate : entries_)
    if (candidate.frame == active_) { entry = &candidate; break; }
  if (!entry || !entry->content) {
    close(PopoverCloseReason::AnchorHidden);
    return;
  }
  positioning_ = true;
  // Intersect ancestor client rectangles when root itself is clipped.
  QRect available = root_->rect();
  for (auto* parent = root_->parentWidget(); parent; parent = parent->parentWidget())
    available &= QRect(root_->mapFromGlobal(parent->mapToGlobal(QPoint())),
                       parent->size());
  // A chart setting overlays its own plot. Keep the other chart headers
  // reachable; long forms scroll within the originating chart's budget.
  for (auto* owner = positioningAnchor_->parentWidget(); owner && owner != root_;
       owner = owner->parentWidget()) {
    if ((owner->property("uiRole").toString() != "chartHead" &&
         !owner->property("chartIndex").isValid()) || !owner->parentWidget()) continue;
    auto* panel = owner->parentWidget();
    const int top = root_->mapFromGlobal(owner->mapToGlobal(QPoint(0, owner->height()))).y() + 4;
    const int bottom = root_->mapFromGlobal(panel->mapToGlobal(QPoint(0, panel->height()))).y() - 4;
    if (bottom - top >= 40) {
      available.setTop(std::max(available.top(), top));
      available.setBottom(std::min(available.bottom(), bottom));
    }
    break;
  }
  if (available.isEmpty()) {
    positioning_ = false;
    close(PopoverCloseReason::AnchorHidden);
    return;
  }
  const auto hint = entry->content->sizeHint();
  const int preferredWidth = std::max({320, hint.width() + 32,
                                       entry->content->minimumSizeHint().width() + 32});
  const int width = std::min(preferredWidth, available.width());
  const int contentWidth = std::max(1, width - 32);
  const int contentHeight = entry->content->hasHeightForWidth()
      ? entry->content->heightForWidth(contentWidth) : hint.height();
  const int height = std::min(available.height(),
      std::max({40, contentHeight + 16, entry->content->minimumHeight() + 16}));
  const QPoint anchor = root_->mapFromGlobal(positioningAnchor_->mapToGlobal(QPoint()));
  int y = anchor.y() + positioningAnchor_->height() + 4;
  if (y + height > available.bottom() + 1) y = anchor.y() - height - 4;
  const int x = std::clamp(anchor.x() + positioningAnchor_->width() - width,
                          available.left(), available.right() + 1 - width);
  y = std::clamp(y, available.top(), available.bottom() + 1 - height);
  active_->setGeometry(x, y, width, height);
  positioning_ = false;
}

bool SettingsPopoverHost::hasChildPopup() const {
  return active_ && belongsTo(active_, QApplication::activePopupWidget());
}

bool SettingsPopoverHost::eventFilter(QObject* watched, QEvent* event) {
  if (!active_) return false;
  const auto type = event->type();
  if (type == QEvent::KeyPress || type == QEvent::ShortcutOverride) {
    const auto* key = static_cast<QKeyEvent*>(event);
    if (key->key() == Qt::Key_Escape &&
        (belongsTo(root_, watched) || belongsTo(active_, watched))) {
      if (hasChildPopup()) return false;
      if (type == QEvent::KeyPress) close(PopoverCloseReason::Escape);
      event->accept();
      return true;
    }
  }
  // Native QWindow events precede QWidget routing. Inspect the routed widget;
  // treating the root QWindow as an outside target closes internal clicks.
  if (type == QEvent::MouseButtonPress && qobject_cast<QWidget*>(watched) &&
      !belongsTo(active_, watched) &&
      !belongsTo(positioningAnchor_, watched) &&
      !belongsTo(activeAnchor_, watched)) {
    // Application event filters execute newest first. Queue hiding so the
    // global parameter policy always sees this press before focus is removed.
    const auto revision = revision_;
    QTimer::singleShot(0, this, [this, revision] {
      if (revision_ == revision) close(PopoverCloseReason::Outside);
    });
  }
  if (type == QEvent::ApplicationDeactivate) {
    close(PopoverCloseReason::Deactivate);
  } else if (type == QEvent::WindowDeactivate &&
             belongsTo(root_, watched)) {
    const auto revision = revision_;
    QTimer::singleShot(0, this, [this, revision] {
      if (!active_ || revision_ != revision || hasChildPopup()) return;
      auto* window = QApplication::activeWindow();
      if (window && (belongsTo(root_, window) || belongsTo(active_, window))) return;
      close(PopoverCloseReason::Deactivate);
    });
  } else if (type == QEvent::Hide && positioningAnchor_ &&
             belongsTo(watched, positioningAnchor_)) {
    close(PopoverCloseReason::AnchorHidden);
  } else if (type == QEvent::Resize || type == QEvent::Move ||
             type == QEvent::LayoutRequest || type == QEvent::Show ||
             type == QEvent::Hide || type == QEvent::Destroy) {
    if (belongsTo(root_, watched) || belongsTo(watched, root_)) queueReposition();
  }
  return false;
}
}  // namespace signalstudio
