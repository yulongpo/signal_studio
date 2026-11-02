#pragma once

#include <QFontMetricsF>
#include <QRectF>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace signalstudio::axis_layout {

enum class TitleMode { UnitAtTop, Header, Rotated, CompactHeader };

// All distances are logical pixels. QFontMetricsF uses the actual widget paint
// device; multiplying these distances by DPR would scale the margins twice.
struct Labels {
  QStringList xTicks, yTicks;
  QString xTitle, yTitle, yUnit;
};

struct Budget {
  double tickLabelWidth = 0;
  double tickHeight = 0;
  double titleHeight = 0;
  double headerHeight = 0;
  double rotatedTitleWidth = 0;
  double left = 0, top = 0, bottom = 0;
};

inline double xTickOffset(const Budget& budget) {
  // The bottom Y tick straddles plot.bottom(); start X ticks below its ink.
  return std::ceil(budget.tickHeight / 2) + 4;
}

inline double textWidth(const QFontMetricsF& metrics, const QString& text) {
  const auto ink = metrics.boundingRect(text);
  return std::ceil(std::max(metrics.horizontalAdvance(text), ink.right()) -
                   std::min(0.0, ink.left()));
}

inline Budget measure(const Labels& labels, const QFontMetricsF& metrics,
                      TitleMode mode, double widgetWidth,
                      double widgetHeight) {
  Budget result;
  result.tickHeight = std::ceil(metrics.height());
  result.titleHeight = mode == TitleMode::CompactHeader ? 0 : result.tickHeight;
  for (const auto& tick : labels.yTicks)
    result.tickLabelWidth =
        std::max(result.tickLabelWidth, textWidth(metrics, tick));
  result.bottom = xTickOffset(result) + result.tickHeight +
                  (result.titleHeight > 0 ? 3 + result.titleHeight : 0) + 4;
  result.top = std::ceil(std::max(12.0, 8 + result.tickHeight / 2));
  if (mode == TitleMode::Rotated) {
    const auto title = metrics.boundingRect(
        QRectF(0, 0, std::max(1.0, widgetHeight - result.bottom - result.top), 10000),
        Qt::TextWordWrap, labels.yTitle);
    result.rotatedTitleWidth = std::ceil(title.height()) + 6;
  } else if (mode != TitleMode::CompactHeader) {
    const auto title = mode == TitleMode::Header ? labels.yTitle : labels.yUnit;
    result.headerHeight = std::ceil(metrics.boundingRect(
        QRectF(0, 0, std::max(1.0, widgetWidth - 16), 10000),
        Qt::TextWordWrap, title).height());
  }
  // Reserve a usable Y-axis hit zone even for one-character tick labels.
  result.left = std::ceil(std::max(36.0, 8 + result.rotatedTitleWidth +
                                           result.tickLabelWidth + 7));
  result.top = std::ceil(std::max(12.0, result.headerHeight +
      result.tickHeight / 2 + (mode == TitleMode::CompactHeader ? 4 : 8)));
  return result;
}

inline Budget sharedBudget(Budget first, const Budget& second,
                           bool leftOnly = false) {
  first.left = std::max(first.left, second.left);
  if (!leftOnly) {
    first.top = std::max(first.top, second.top);
    first.bottom = std::max(first.bottom, second.bottom);
  }
  return first;
}

struct Layout {
  QRectF plot, header, xTicks, xTitle, rotatedTitle;
  Budget budget;
};

inline Layout arrange(QSizeF size, Budget budget, double right = 22) {
  Layout result;
  result.budget = budget;
  result.plot = QRectF(budget.left, budget.top,
                      std::max(1.0, size.width() - budget.left - right),
                      std::max(1.0, size.height() - budget.top - budget.bottom));
  result.header = QRectF(8, 4, std::max(1.0, size.width() - 16),
                         budget.headerHeight);
  result.xTicks = QRectF(result.plot.left(),
                         result.plot.bottom() + xTickOffset(budget),
                         result.plot.width(), budget.tickHeight);
  result.xTitle = QRectF(result.plot.left(), result.xTicks.bottom() + 3,
                         result.plot.width(), budget.titleHeight);
  result.rotatedTitle = QRectF(8, result.plot.top(),
                               std::max(0.0, budget.rotatedTitleWidth - 6),
                               result.plot.height());
  return result;
}

}  // namespace signalstudio::axis_layout
