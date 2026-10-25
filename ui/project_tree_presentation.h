#pragma once

#include <QApplication>
#include <QPainter>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

#include "domain/project.h"

namespace signalstudio {

namespace project_tree_presentation {

// Presentation only. UserRole, +1 (identity) and +3 (mark state) stay owned by
// MainWindow. DisplayRole also stays intact for its existing sync functions.
enum Role {
  TitleRole = Qt::UserRole + 64,
  DetailRole,
  BadgeRole,
  CountRole,
  ActiveRole,
  SourceToolTipRole,
  PresentedToolTipRole
};

inline QString cleanName(QString name, const QString& id,
                         const QString& fallback) {
  name = name.trimmed();
  const QString decorations = QStringLiteral("▾▸▿▹◧◈◇▱▣");
  while (!name.isEmpty() && decorations.contains(name.front()))
    name = name.mid(1).trimmed();
  if (!id.isEmpty()) {
    for (const auto& pair : {QStringLiteral("[]"), QStringLiteral("()"),
                             QStringLiteral("【】")}) {
      const auto token = pair.front() + id + pair.back();
      if (name.startsWith(token)) name = name.mid(token.size()).trimmed();
      if (name.endsWith(token)) name.chop(token.size());
      name = name.trimmed();
    }
    if (name == id) name.clear();
    else if (name.startsWith(id) && name.size() > id.size() &&
             QStringLiteral(" :：·|—-_").contains(name.at(id.size()))) {
      name = name.mid(id.size()).trimmed();
      while (!name.isEmpty() && QStringLiteral(":：·|—-_").contains(name.front()))
        name = name.mid(1).trimmed();
    }
    for (const auto& separator : {QStringLiteral(" · "), QStringLiteral(" | "),
                                  QStringLiteral(" : ")}) {
      const auto suffix = separator + id;
      if (name.endsWith(suffix)) name.chop(suffix.size());
    }
  }
  while (!name.isEmpty() && QStringLiteral(" :：·|—").contains(name.front()))
    name = name.mid(1).trimmed();
  name = name.trimmed();
  return name.isEmpty() ? fallback : name;
}

inline QString shortRate(double value, bool samples = false) {
  if (!std::isfinite(value)) return QStringLiteral("—");
  const double magnitude = std::abs(value);
  const double scale = magnitude >= 1e9 ? 1e9 : magnitude >= 1e6 ? 1e6
                                      : magnitude >= 1e3 ? 1e3 : 1;
  const QString prefix = scale == 1e9 ? "G" : scale == 1e6 ? "M"
                                        : scale == 1e3 ? "k" : "";
  return QString::number(value / scale, 'g', 6) + " " + prefix +
         (samples ? "S/s" : "Hz");
}

inline QString exact(double value) {
  return std::isfinite(value)
             ? QString::number(value, 'g', std::numeric_limits<double>::max_digits10)
             : QStringLiteral("未知");
}

inline int rowHeight(const QFont& font, bool twoLines, bool group = false) {
  const auto height = QFontMetrics(font).height();
  return twoLines ? std::max(46, height * 2 + 9)
                 : std::max(group ? 25 : 28, height + (group ? 6 : 9));
}

inline void setData(QTreeWidgetItem* item, int role, const QVariant& value) {
  if (item->data(0, role) != value) item->setData(0, role, value);
}

inline QString sourceToolTip(QTreeWidgetItem* item) {
  if (item->toolTip(0) != item->data(0, PresentedToolTipRole).toString())
    setData(item, SourceToolTipRole, item->toolTip(0));
  return item->data(0, SourceToolTipRole).toString();
}

inline QString relationText(ChannelSourceRelation relation) {
  switch (relation) {
    case ChannelSourceRelation::Current: return QStringLiteral("来源当前");
    case ChannelSourceRelation::Changed: return QStringLiteral("来源已修改；保留创建快照");
    case ChannelSourceRelation::Missing: return QStringLiteral("来源已删除或缺失");
    case ChannelSourceRelation::Rejected: return QStringLiteral("来源质量：已忽略");
    case ChannelSourceRelation::Unconfirmed: return QStringLiteral("旧版段确认记录；保留创建快照");
    case ChannelSourceRelation::LegacyNeedsReview: return QStringLiteral("旧版来源待复核");
  }
  return {};
}

inline QStringList channelBadges(const FileState& file, const Channel& channel) {
  QStringList badges;
  if (channel.sourceTime.end > availableSamples(file.metadata)) badges << "未读";
  switch (channel.sourceRelation) {
    case ChannelSourceRelation::Current: break;
    case ChannelSourceRelation::Changed: badges << "差异"; break;
    case ChannelSourceRelation::Missing: badges << "缺失"; break;
    case ChannelSourceRelation::Rejected: badges << "已忽略"; break;
    case ChannelSourceRelation::Unconfirmed: badges << "复核"; break;
    case ChannelSourceRelation::LegacyNeedsReview: badges << "旧版"; break;
  }
  switch (channel.processingState) {
    case ChannelProcessingState::Ready: break;
    case ChannelProcessingState::SourceMissing: badges << "缺失"; break;
    case ChannelProcessingState::LegacyNeedsReview: badges << "待确认"; break;
    case ChannelProcessingState::Invalid: badges << "无效"; break;
  }
  badges.removeDuplicates();
  return badges;
}

}  // namespace project_tree_presentation

class ProjectTreePresentationDelegate final : public QStyledItemDelegate {
 public:
  using QStyledItemDelegate::QStyledItemDelegate;

  QSize sizeHint(const QStyleOptionViewItem& option,
                 const QModelIndex& index) const override {
    using namespace project_tree_presentation;
    if (!index.data(TitleRole).isValid())
      return QStyledItemDelegate::sizeHint(option, index);
    auto style = option;
    initStyleOption(&style, index);
    const auto kind = index.data(Qt::UserRole).toString();
    return {0, rowHeight(style.font, kind == "file" || kind == "channel",
                         isGroup(kind))};
  }

  void paint(QPainter* painter, const QStyleOptionViewItem& option,
             const QModelIndex& index) const override {
    using namespace project_tree_presentation;
    if (!index.data(TitleRole).isValid()) {
      QStyledItemDelegate::paint(painter, option, index);
      return;
    }
    auto style = option;
    initStyleOption(&style, index);
    const auto kind = index.data(Qt::UserRole).toString();
    const bool mark = kind == "mark";
    const bool active = index.data(ActiveRole).toBool();
    const int markState = mark ? index.data(Qt::UserRole + 3).toInt() : 0;
    const bool selected = mark ? (markState & 1) != 0
                               : (option.state & QStyle::State_Selected) != 0;
    const bool current = mark && (markState & 2);
    const bool hovered = mark ? (markState & 4) != 0
                              : (option.state & QStyle::State_MouseOver) != 0;
    const bool focused = (option.state & QStyle::State_HasFocus) != 0;
    const auto rect = option.rect.adjusted(0, 1, -1, -1);
    painter->save();
    painter->setClipRect(option.rect, Qt::IntersectClip);
    painter->setRenderHint(QPainter::Antialiasing);
    if (active) painter->fillRect(rect, QColor("#214c70"));
    else if (selected || current) painter->fillRect(rect, QColor("#1a3f56"));
    else if (hovered) painter->fillRect(rect, QColor("#22425a"));
    else if (kind == "project") painter->fillRect(rect, QColor("#1b2f45"));
    if (active || current)
      painter->fillRect(QRect(rect.left(), rect.top() + 2, 3, rect.height() - 4),
                        QColor("#53bcf0"));
    if (hovered) {
      painter->setPen(QPen(QColor("#75c7e5"), 1, Qt::DotLine));
      painter->setBrush(Qt::NoBrush);
      painter->drawRect(rect.adjusted(1, 1, -1, -1));
    }
    if (style.state.testFlag(QStyle::State_HasFocus)) {
      painter->setPen(QPen(QColor("#d0efff"), 1, Qt::DashLine));
      painter->setBrush(Qt::NoBrush);
      painter->drawRect(rect.adjusted(4, 3, -3, -3));
    }

    QRect textRect = rect.adjusted(mark ? 20 : 9, 0, -7, 0);
    if (mark) {
      const QRect box(rect.left() + 7, rect.center().y() - 4, 8, 8);
      painter->setPen(QPen(QColor(selected ? "#96d6ef" : "#506d85"), 1));
      painter->setBrush(selected ? QColor("#347c9e") : Qt::transparent);
      painter->drawRoundedRect(box, 1, 1);
      if (selected) {
        painter->setPen(QPen(QColor("#e0f6ff"), 1.3));
        painter->drawLine(box.left() + 2, box.top() + 4, box.left() + 4, box.top() + 6);
        painter->drawLine(box.left() + 4, box.top() + 6, box.left() + 7, box.top() + 2);
      }
    }

    auto font = style.font;
    font.setBold(active || current || kind == "project");
    painter->setFont(font);
    QColor textColor = active ? QColor("#e0f5ff") : QColor("#b2cbe4");
    if (mark || isGroup(kind)) {
      const auto brush = index.data(Qt::ForegroundRole).value<QBrush>();
      if (brush.style() != Qt::NoBrush) textColor = brush.color();
    }
    const auto title = index.data(TitleRole).toString();
    const auto detail = index.data(DetailRole).toString();
    const bool twoLines = kind == "file" || kind == "channel";
    if (twoLines) {
      const int lineHeight = QFontMetrics(font).height();
      const int top = rect.top() + std::max(0, (rect.height() - lineHeight * 2 - 1) / 2);
      drawText(painter, QRect(textRect.left(), top, textRect.width(), lineHeight),
               title, textColor);
      font.setBold(false);
      painter->setFont(font);
      QRect detailRect(textRect.left(), top + lineHeight + 1,
                       textRect.width(), lineHeight);
      const auto badge = index.data(BadgeRole).toStringList().join(" · ");
      if (!badge.isEmpty() && detailRect.width() > 24) {
        const int width = std::min(QFontMetrics(font).horizontalAdvance(badge) + 10,
                                    detailRect.width() / 2);
        const QRect badgeRect(detailRect.right() - width + 1, detailRect.top(),
                               width, detailRect.height());
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor("#493b25"));
        painter->drawRoundedRect(badgeRect, 3, 3);
        drawText(painter, badgeRect.adjusted(4, 0, -4, 0), badge, QColor("#efc776"));
        detailRect.setRight(badgeRect.left() - 5);
      }
      drawText(painter, detailRect, detail, active ? QColor("#b4d9f0")
                                                   : QColor("#8da8c2"));
    } else {
      const auto count = index.data(CountRole).toString();
      if (!count.isEmpty() && textRect.width() > 24) {
        const int width = std::min(QFontMetrics(font).horizontalAdvance(count) + 12,
                                    textRect.width() / 3);
        const QRect countRect(textRect.right() - width + 1, textRect.top(),
                               width, textRect.height());
        drawText(painter, countRect, count, QColor("#8da8c2"), Qt::AlignRight);
        textRect.setRight(countRect.left() - 5);
      }
      drawText(painter, textRect, title, textColor);
    }
    // Selection, current item, pointer hover and keyboard focus have separate
    // cues; none changes a mark's source text color, even under item QSS.
    painter->setRenderHint(QPainter::Antialiasing, false);
    painter->setBrush(Qt::NoBrush);
    if (hovered) {
      painter->setPen(QPen(QColor("#99d8f1"), 1));
      painter->drawRect(rect.adjusted(1, 1, -1, -1));
    }
    if (focused) {
      painter->setPen(QPen(QColor("#dbf1ff"), 1, Qt::DotLine));
      painter->drawRect(rect.adjusted(4, 3, -3, -3));
    }
    painter->restore();
  }

 private:
  static bool isGroup(const QString& kind) {
    return kind == "project" || kind == "marks" || kind == "annotationSource" ||
           kind == "channels" || kind == "detectionRuns";
  }
  static void drawText(QPainter* painter, const QRect& rect, const QString& text,
                       const QColor& color, Qt::Alignment alignment = Qt::AlignLeft) {
    if (rect.width() <= 0 || rect.height() <= 0) return;
    painter->setPen(color);
    painter->drawText(rect, static_cast<int>(alignment | Qt::AlignVCenter) | Qt::TextSingleLine,
                      QFontMetrics(painter->font()).elidedText(
                          QString(text).replace('\n', ' ').replace('\r', ' '),
                          Qt::ElideRight, rect.width()));
  }
};

// Call after rebuild/sync has supplied mark text, source ForegroundRole and +3
// state. Only existing items are visited; marks are never enumerated or loaded.
// No selection, current item, expansion, scroll position or domain data is set.
inline void refreshProjectTreePresentation(QTreeWidget* tree,
                                           const Project& project) {
  using namespace project_tree_presentation;
  if (!tree) return;
  const QSignalBlocker blocker(tree);
  struct FileEntry {
    const FileState* file = nullptr;
    std::unordered_map<std::string, const Channel*> channels;
  };
  std::unordered_map<std::string, FileEntry> files;
  for (const auto& file : project.files) {
    auto& entry = files[file.metadata.id];
    entry.file = &file;
    for (const auto& channel : file.channels) entry.channels.emplace(channel.id, &channel);
  }
  static const QRegularExpression countSuffix(QStringLiteral("\\s*\\((\\d+(?:/\\d+)?)\\)\\s*$"));
  for (QTreeWidgetItemIterator it(tree); *it; ++it) {
    auto* item = *it;
    const auto kind = item->data(0, Qt::UserRole).toString();
    if (kind == "metadata") {
      if (!item->isHidden()) item->setHidden(true);
      continue;
    }
    if (kind == "treeActions" || kind.isEmpty()) continue;
    const auto id = item->data(0, Qt::UserRole + 1).toString();
    const FileEntry* owner = nullptr;
    for (auto* ancestor = item; ancestor; ancestor = ancestor->parent()) {
      if (ancestor->data(0, Qt::UserRole).toString() != "file") continue;
      const auto found = files.find(ancestor->data(0, Qt::UserRole + 1).toString().toStdString());
      if (found != files.end()) owner = &found->second;
      break;
    }
    QString title = cleanName(item->text(0), id, QStringLiteral("未命名"));
    QString detail, count;
    QStringList badges;
    bool active = false;
    const auto originalToolTip = sourceToolTip(item);
    QString tooltip = title + (originalToolTip.isEmpty() ? QString{} : "\n" + originalToolTip);
    if (kind == "project") {
      title = cleanName(QString::fromStdString(project.name), {}, "未命名工程");
      count = QString::number(project.files.size());
      tooltip = QString::fromStdString(project.name) + "\n文件 " + count;
    } else if (kind == "file" && owner) {
      const auto& m = owner->file->metadata;
      title = cleanName(QString::fromStdString(m.name), id, "未命名文件");
      detail = "Fc " + shortRate(m.centerFrequencyHz) + " · Fs " + shortRate(m.sampleRateHz, true);
      active = m.id == project.activeFileId;
      if (m.availability.status == LoadStatus::Loading) badges << "读取中";
      if (m.availability.status == LoadStatus::Partial) badges << "未完整";
      if (m.availability.status == LoadStatus::Failed) badges << "失败";
      tooltip = QString::fromStdString(m.name) + "\n" + QString::fromStdString(m.path) +
                "\nFc " + exact(m.centerFrequencyHz) + " Hz\nFs " + exact(m.sampleRateHz) +
                " S/s\n有效带宽 " + (m.effectiveBandwidthHz > 0 ? exact(m.effectiveBandwidthHz) + " Hz" : "完整采样频带") +
                "\n声明带宽 " + exact(m.declaredBandwidthHz) + " Hz\n可用采样 " +
                QString::number(availableSamples(m)) + "/" + QString::number(m.sampleCount);
      if (std::isfinite(m.sampleRateHz) && m.sampleRateHz > 0)
        tooltip += "\n时长 " + exact(static_cast<double>(m.sampleCount) / m.sampleRateHz) + " s";
      if (!badges.isEmpty()) tooltip += "\n" + badges.join(" · ");
      if (!m.availability.error.empty()) tooltip += "\n" + QString::fromStdString(m.availability.error);
    } else if (kind == "channel" && owner && owner->channels.contains(id.toStdString())) {
      const auto& c = *owner->channels.at(id.toStdString());
      title = cleanName(QString::fromStdString(c.name), id, "未命名通道");
      detail = "Fc " + shortRate(c.centerFrequencyHz) + " · BW " + shortRate(c.bandwidthHz);
      active = owner->file->metadata.id == project.activeFileId && c.id == project.activeChannelId;
      badges = channelBadges(*owner->file, c);
      tooltip = QString::fromStdString(c.name) + "\nFc " + exact(c.centerFrequencyHz) +
                " Hz\nBW " + exact(c.bandwidthHz) + " Hz\n输出 Fs " + exact(c.outputSampleRateHz) +
                " S/s\n来源采样 [" + QString::number(c.sourceTime.begin) + ", " +
                QString::number(c.sourceTime.end) + ")\n" + relationText(c.sourceRelation);
      tooltip += c.sourceSnapshot ? "\n创建修订 " + QString::fromStdString(c.sourceSnapshot->revisionId)
                                  : "\n没有来源修订快照";
      if (c.sourceTime.end > availableSamples(owner->file->metadata)) tooltip += "\n来源未读入";
      switch (c.processingState) {
        case ChannelProcessingState::Ready: tooltip += "\n打开真实窄带分析工作区"; break;
        case ChannelProcessingState::LegacyNeedsReview: tooltip += "\n通道配置待确认"; break;
        case ChannelProcessingState::SourceMissing: tooltip += "\n通道来源缺失"; break;
        case ChannelProcessingState::Invalid: tooltip += "\n通道配置无效"; break;
      }
    } else if (kind == "marks") {
      // Reuse the three source-group totals already projected by sync. Counting
      // loaded mark children would misrepresent a paged/filtered collection.
      qulonglong matching = 0, total = 0;
      bool haveCounts = false;
      for (int child = 0; child < item->childCount(); ++child) {
        const auto* group = item->child(child);
        if (group->data(0, Qt::UserRole).toString() != "annotationSource") continue;
        const auto match = countSuffix.match(group->text(0));
        if (!match.hasMatch()) continue;
        const auto values = match.captured(1).split('/');
        matching += values.front().toULongLong();
        total += values.back().toULongLong();
        haveCounts = true;
      }
      if (haveCounts) count = QString::number(matching) + "/" + QString::number(total);
      tooltip = title + (count.isEmpty() ? QString{} : "\n匹配/总数 " + count);
    } else if (kind == "channels" || kind == "detectionRuns" || kind == "annotationSource") {
      const auto match = countSuffix.match(title);
      if (match.hasMatch()) {
        count = match.captured(1);
        title = title.left(match.capturedStart()).trimmed();
      }
      if (owner && kind == "channels") count = QString::number(owner->file->channels.size());
      if (owner && kind == "detectionRuns") count = QString::number(owner->file->detectionRuns.size());
      tooltip = title + (count.isEmpty() ? QString{} : "\n" + count) +
                (originalToolTip.isEmpty() ? QString{} : "\n" + originalToolTip);
    }
    setData(item, TitleRole, title);
    setData(item, DetailRole, detail);
    setData(item, CountRole, count);
    setData(item, BadgeRole, badges);
    setData(item, ActiveRole, active);
    setData(item, PresentedToolTipRole, tooltip);
    if (item->toolTip(0) != tooltip) item->setToolTip(0, tooltip);
    setData(item, Qt::AccessibleTextRole, title + (detail.isEmpty() ? QString{} : "\n" + detail) +
                                        (count.isEmpty() ? QString{} : " " + count) +
                                        (badges.isEmpty() ? QString{} : "\n" + badges.join(" · ")));
    const bool group = kind == "project" || kind == "marks" || kind == "annotationSource" ||
                       kind == "channels" || kind == "detectionRuns";
    const QSize size(0, rowHeight(item->font(0).resolve(tree->font()),
                                  kind == "file" || kind == "channel", group));
    if (item->sizeHint(0) != size) item->setSizeHint(0, size);
  }
}

}  // namespace signalstudio
