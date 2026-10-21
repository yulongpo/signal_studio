#pragma once

#include <QAbstractItemModel>
#include <QAction>
#include <QComboBox>
#include <QHBoxLayout>
#include <QMenu>
#include <QPointer>
#include <QResizeEvent>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <vector>

#include "ui/settings_popover.h"

namespace signalstudio {
// Widgets keep a stable owner. Overflow exposes commands/presets, never a
// second editable copy, and refresh only changes requested visibility.
class ChartControlStrip final : public QWidget {
 public:
  ChartControlStrip(SettingsPopoverHost* popovers, const QString& name,
                    QWidget* parent = nullptr)
      : QWidget(parent), popovers_(popovers) {
    setObjectName(name);
    setFixedHeight(28);
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    row_ = new QHBoxLayout(this);
    row_->setContentsMargins(0, 1, 0, 1);
    row_->setSpacing(5);
    row_->setSizeConstraint(QLayout::SetNoConstraint);
    more_ = new QToolButton(this);
    more_->setObjectName(name + "Overflow");
    more_->setText(QStringLiteral("⋯"));
    more_->setToolTip(QStringLiteral("更多图谱操作"));
    more_->setAccessibleName(more_->toolTip());
    more_->setFixedSize(28, 24);
    more_->hide();
    overflowContent_ = new QWidget;
    overflowLayout_ = new QVBoxLayout(overflowContent_);
    overflowLayout_->setContentsMargins(0, 0, 0, 0);
    overflowLayout_->setSpacing(5);
    popovers_->add(more_, overflowContent_, name + "OverflowPopover");
    row_->addWidget(more_);
    row_->addStretch();
  }

  QSize minimumSizeHint() const override { return {0, 28}; }
  QSize sizeHint() const override { return {480, 28}; }

  void addControl(QWidget* control, const QString& title, int priority,
                  std::function<void()> command = {}) {
    control->setParent(this);
    control->setFixedHeight(24);
    control->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    row_->insertWidget(static_cast<int>(items_.size()), control);
    auto* alternate = new QToolButton(overflowContent_);
    alternate->setObjectName(control->objectName() + "OverflowAction");
    alternate->setText(title);
    alternate->setToolButtonStyle(Qt::ToolButtonTextOnly);
    alternate->setAccessibleName(title);
    alternate->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    if (auto* combo = qobject_cast<QComboBox*>(control)) {
      auto* menu = new QMenu(alternate);
      alternate->setMenu(menu);
      alternate->setPopupMode(QToolButton::InstantPopup);
      connect(menu, &QMenu::aboutToShow, menu, [menu, combo] {
        menu->clear();
        for (int index = 0; index < combo->count(); ++index) {
          auto* action = menu->addAction(combo->itemText(index));
          action->setCheckable(true);
          action->setChecked(combo->currentIndex() == index);
          action->setEnabled(combo->isEnabled() &&
                             combo->model()->flags(combo->model()->index(index, 0))
                                 .testFlag(Qt::ItemIsEnabled));
          connect(action, &QAction::triggered, combo,
                  [combo, index] { combo->setCurrentIndex(index); });
        }
      });
    } else {
      connect(alternate, &QToolButton::clicked, this,
              [this, control, command = std::move(command)] {
                popovers_->close(PopoverCloseReason::Replaced);
                if (command) command();
                else if (auto* button = qobject_cast<QAbstractButton*>(control))
                  button->click();
              });
    }
    alternate->hide();
    overflowLayout_->addWidget(alternate);
    items_.push_back({control, alternate, priority, true, false});
    schedule();
  }

  void requestVisible(QWidget* control, bool visible) {
    for (auto& item : items_)
      if (item.control == control && item.requested != visible) {
        item.requested = visible;
        schedule();
      }
  }

  QToolButton* overflowButton() const { return more_; }

  void reflow() {
    pending_ = false;
    std::vector<int> priority;
    int required = 0;
    for (int index = 0; index < static_cast<int>(items_.size()); ++index) {
      auto& item = items_[index];
      item.overflow = false;
      if (!item.requested) continue;
      required += std::max(item.control->minimumWidth(), item.control->sizeHint().width()) + 5;
      priority.push_back(index);
    }
    std::stable_sort(priority.begin(), priority.end(), [this](int a, int b) {
      return items_[a].priority < items_[b].priority;
    });
    const bool needOverflow = required > width();
    const int budget = std::max(0, width() - (needOverflow ? 33 : 0));
    for (const auto index : priority) {
      if (required <= budget) break;
      auto& item = items_[index];
      required -= std::max(item.control->minimumWidth(), item.control->sizeHint().width()) + 5;
      item.overflow = true;
    }
    bool overflow = false;
    for (auto& item : items_) {
      item.control->setVisible(item.requested && !item.overflow);
      item.alternate->setVisible(item.requested && item.overflow);
      item.alternate->setEnabled(item.control->isEnabled());
      overflow |= item.requested && item.overflow;
    }
    more_->setVisible(overflow);
  }

 protected:
  void resizeEvent(QResizeEvent* event) override {
    QWidget::resizeEvent(event);
    schedule();
  }

 private:
  void schedule() {
    if (pending_) return;
    pending_ = true;
    QTimer::singleShot(0, this, [this] { reflow(); });
  }
  struct Item {
    QWidget* control;
    QToolButton* alternate;
    int priority;
    bool requested, overflow;
  };
  SettingsPopoverHost* popovers_;
  QHBoxLayout* row_;
  QToolButton* more_;
  QWidget* overflowContent_;
  QVBoxLayout* overflowLayout_;
  std::vector<Item> items_;
  bool pending_ = false;
};
}  // namespace signalstudio
