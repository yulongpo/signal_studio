#pragma once
#include "app/main_window.h"
#include "ui/chart_control_strip.h"

// Resolve the actual editor owner after responsive/workspace transitions.
// Old setting categories now share one flat More panel per plot.
inline signalstudio::ChartControlStrip* chartSettingsStrip(signalstudio::MainWindow& window, const QString& category) {
  auto* field=window.findChild<QWidget*>(category.startsWith("psd")?"psdSource":"stftMethod");
  return field?dynamic_cast<signalstudio::ChartControlStrip*>(field->property("chartControlOwner").value<QObject*>()):nullptr;
}
inline QFrame* chartSettingsFrame(signalstudio::MainWindow& window,const QString& category) {
  auto* owner=chartSettingsStrip(window,category);return owner?owner->popoverFrame():nullptr;
}
inline QAbstractButton* chartSettingsEntry(signalstudio::MainWindow& window,const QString& category) {
  auto* owner=chartSettingsStrip(window,category);return owner?owner->overflowButton():nullptr;
}
