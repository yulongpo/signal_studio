#pragma once

#include <QObject>
#include <QPointer>
#include <QVector>
#include <QAbstractButton>
#include <QFrame>
#include <QScrollArea>

namespace signalstudio {

enum class PopoverCloseReason {
  Outside, Escape, Deactivate, Context, AnchorHidden, Replaced
};

// Root-owned child widgets, never popup windows: editing follows the shared
// ParameterInputPolicy. Register content once; refresh it in place.
class SettingsPopoverHost : public QObject {
  Q_OBJECT
 public:
  explicit SettingsPopoverHost(QWidget* root);
  ~SettingsPopoverHost() override;
  QFrame* add(QAbstractButton* anchor, QWidget* content,
              const QString& objectName);
  void open(QAbstractButton* registeredAnchor, QWidget* anchorOverride = nullptr);
  void toggle(QAbstractButton* anchor, QWidget* anchorOverride = nullptr);
  void close(PopoverCloseReason reason = PopoverCloseReason::Outside);
  void closeForContextChange();
  void reposition();
  QFrame* activePopover() const;

 protected:
  bool eventFilter(QObject*, QEvent*) override;

 private:
  struct Entry {
    QPointer<QAbstractButton> anchor;
    QPointer<QFrame> frame;
    QPointer<QScrollArea> scroll;
    QPointer<QWidget> content;
  };
  static bool belongsTo(QObject* owner, QObject* object);
  bool hasChildPopup() const;
  void queueReposition();
  QPointer<QWidget> root_;
  QVector<Entry> entries_;
  QPointer<QAbstractButton> activeAnchor_;
  QPointer<QWidget> positioningAnchor_;
  QPointer<QFrame> active_;
  quint64 revision_ = 0;
  bool positioning_ = false;
  bool repositionPending_ = false;
};
}  // namespace signalstudio
