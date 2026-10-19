#pragma once

#include <QGuiApplication>
#include <QScreen>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <QWindow>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <QtGui/qscreen_platform.h>
#include <Windows.h>
#endif

namespace signalstudio {
// Preserve both the connected-screen ordinal and the native Windows identity.
// GDI device suffixes include historical devices and are not connection
// ordinals.
inline QString displayDeviceName(const QScreen* screen) {
  if (!screen) return {};
#ifdef Q_OS_WIN
  if (auto* native =
          screen->nativeInterface<QNativeInterface::QWindowsScreen>()) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(native->handle(), &info))
      return QString::fromWCharArray(info.szDevice);
  }
#endif
  return screen->name();
}
inline bool isDisplayNumber(const QScreen* screen, int number) {
  return screen && number > 0 &&
         QGuiApplication::screens().indexOf(const_cast<QScreen*>(screen)) + 1 ==
             number;
}
inline QSize displayPixelSize(const QScreen* screen) {
  if (!screen) return {};
#ifdef Q_OS_WIN
  if (auto* native =
          screen->nativeInterface<QNativeInterface::QWindowsScreen>()) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(native->handle(), &info))
      return {info.rcMonitor.right - info.rcMonitor.left,
              info.rcMonitor.bottom - info.rcMonitor.top};
  }
#endif
  return {qRound(screen->geometry().width() * screen->devicePixelRatio()),
          qRound(screen->geometry().height() * screen->devicePixelRatio())};
}
// The external acceptance monitor may also be the Windows primary monitor.
// Select by actual native dimensions and system DPI, never by a former ordinal
// or a simulated large window. Ambiguous setups require an explicit choice.
inline QScreen* nativeAcceptanceScreen() {
  QScreen* selected = nullptr;
  for (auto* screen : QGuiApplication::screens()) {
    if (displayPixelSize(screen) != QSize(3840, 2160) ||
        screen->geometry().size() != QSize(2560, 1440) ||
        qAbs(screen->devicePixelRatio() - 1.5) > .001)
      continue;
    if (selected) return nullptr;
    selected = screen;
  }
  return selected;
}
inline void showFullScreenOnScreen(QWidget& window, QScreen* screen) {
  window.setScreen(screen);
  window.move(screen->geometry().topLeft());
  window.show();
  // A QRhiWidget can recreate the top-level surface during the first show.
  // Reapply the target after native creation, then enter full screen there.
  QTimer::singleShot(100, &window, [&window, screen] {
    window.setScreen(screen);
    window.windowHandle()->setScreen(screen);
    // QRhi surface recreation needs the target geometry. Do not let this
    // transitional resize replace the main window's ordinary geometry.
    window.setProperty("enteringTargetFullscreen", true);
    window.setGeometry(screen->geometry());
    window.showFullScreen();
    window.setProperty("enteringTargetFullscreen", false);
  });
}
}  // namespace signalstudio
