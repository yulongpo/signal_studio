#pragma once

#include <QLabel>
#include <QDynamicPropertyChangeEvent>
#include <QResizeEvent>

namespace signalstudio {

// The canvas and its existing title bar consume the same physical axis text.
// Short forms retain the units; origins and complete meanings remain available
// in the tooltip and the chart's settings panel.
struct AxisDescription {
  QString xTitle, yTitle, xShort, yShort, xOrigin, yOrigin;

  QString shortText() const {
    if (xShort.isEmpty() && yShort.isEmpty()) return {};
    return QStringLiteral("X %1 · Y %2").arg(xShort, yShort);
  }
  QString fullText() const {
    if (xTitle.isEmpty() && yTitle.isEmpty()) return {};
    QString result = QStringLiteral("X %1 · Y %2").arg(xTitle, yTitle);
    for (const auto& origin : {xOrigin, yOrigin})
      if (!origin.isEmpty()) result += QStringLiteral(" · ") + origin;
    return result;
  }
  bool operator==(const AxisDescription&) const = default;
};

class AxisHeaderLabel final : public QLabel {
 public:
  explicit AxisHeaderLabel(QWidget* parent = nullptr) : QLabel(parent) {
    setTextFormat(Qt::PlainText);
    setWordWrap(false);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setProperty("uiRole", "hint");
    setStyleSheet("color:#9db6ca;font-weight:400;");
  }
  void setDescription(const AxisDescription& description) {
    if (description_ == description) return;
    description_ = description;
    setToolTip(description_.fullText());
    setAccessibleName(description_.fullText());
    setProperty("axisFullText", description_.fullText());
    setProperty("axisShortText", description_.shortText());
    updateText();
    updateGeometry();
  }
  QSize minimumSizeHint() const override {
    return {fontMetrics().horizontalAdvance(description_.shortText()) + 4,
            fontMetrics().height()};
  }
  QSize sizeHint() const override {
    return {qMax(minimumSizeHint().width(),
                 qMin(480, fontMetrics().horizontalAdvance(description_.fullText()) + 4)),
            fontMetrics().height()};
  }
 protected:
  void changeEvent(QEvent* event) override {
    QLabel::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
      updateText();
      updateGeometry();
    }
  }
  void resizeEvent(QResizeEvent* event) override {
    QLabel::resizeEvent(event);
    updateText();
  }
 private:
  void updateText() {
    const auto full = description_.fullText();
    setText(fontMetrics().horizontalAdvance(full) <= contentsRect().width()
                ? full : description_.shortText());
  }
  AxisDescription description_;
};

class AxisDetailsLabel final : public QLabel {
 public:
  explicit AxisDetailsLabel(QObject* header, QWidget* parent = nullptr) : QLabel(parent) {
    setTextFormat(Qt::PlainText);
    setWordWrap(true);
    setText(header->property("axisFullText").toString());
    header->installEventFilter(this);
  }
 protected:
  bool eventFilter(QObject* source, QEvent* event) override {
    if (event->type() == QEvent::DynamicPropertyChange &&
        static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName() == "axisFullText")
      setText(source->property("axisFullText").toString());
    return QLabel::eventFilter(source, event);
  }
};

}  // namespace signalstudio
