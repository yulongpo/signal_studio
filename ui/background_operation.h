#pragma once
#include <QApplication>
#include <QEventLoop>
#include <QPointer>
#include <QProgressDialog>
#include <QTimer>
#include <atomic>
#include <functional>
#include <future>

namespace signalstudio {
// Bounded IO/validation runs against caller-owned snapshots. Only the caller
// publishes to Session after the worker finishes and context is checked again.
inline bool runBackgroundOperation(QWidget* parent, const QString& title,
    const std::function<bool(std::function<bool()>)>& operation) {
  std::atomic_bool cancelled = false;
  QProgressDialog progress(title, "取消", 0, 0, parent);
  progress.setWindowModality(Qt::ApplicationModal);
  progress.setMinimumDuration(0);
  QObject::connect(&progress, &QProgressDialog::canceled, &progress, [&] { cancelled = true; });
  auto future = std::async(std::launch::async, [&] { return operation([&] { return cancelled.load(); }); });
  QEventLoop loop;
  const bool wasEnabled = parent->isEnabled();
  const auto wasHiding = parent->property("parameterLayerHiding");
  const QPointer<QWidget> previousFocus(QApplication::focusWidget());
  parent->setProperty("parameterLayerHiding", true);
  parent->setEnabled(false);
  progress.setEnabled(true);
  QTimer poll;
  poll.setInterval(25);
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
    if (future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) loop.quit();
  });
  poll.start(); progress.show(); loop.exec(); progress.close();
  parent->setEnabled(wasEnabled);
  if (wasEnabled && parent->isVisible()) {
    parent->window()->activateWindow();
    if (previousFocus && previousFocus->isVisible() && previousFocus->isEnabled())
      previousFocus->setFocus(Qt::OtherFocusReason);
  }
  // Finish queued activation/popover transitions before returning a stable UI
  // to the caller; no pending input is synthesized or committed here.
  QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  parent->setProperty("parameterLayerHiding", wasHiding);
  return future.get();
}
}
