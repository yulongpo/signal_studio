#include "ui/project_tree_presentation.h"

#include <QImage>
#include <QScrollBar>
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace signalstudio;
using namespace signalstudio::project_tree_presentation;

namespace {

void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

QTreeWidgetItem* add(QTreeWidgetItem* parent, const QString& kind,
                     const QString& text, const QString& id = {}) {
  auto* item = new QTreeWidgetItem(parent, {text});
  item->setData(0, Qt::UserRole, kind);
  item->setData(0, Qt::UserRole + 1, id);
  return item;
}

struct Fixture {
  Project project;
  QTreeWidget tree;
  QTreeWidgetItem *root, *file, *metadata, *marks, *source, *mark, *otherMark,
      *channels, *channel, *runs, *run, *more, *actions, *otherFile;

  Fixture() {
    tree.setFont(QFont("Microsoft YaHei UI", 9));
    tree.setHeaderHidden(true);
    tree.setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree.setUniformRowHeights(false);
    tree.setItemDelegate(new ProjectTreePresentationDelegate(&tree));
    tree.setStyleSheet(
        "QTreeWidget { background:#101d2f; color:#b2cbe4; }"
        "QTreeWidget::item:selected { background:#644f23; color:#fff0c7; }"
        "QTreeWidget::item:hover { background:#285473; color:#f0fcff; }");
    tree.resize(360, 240);
    project.name = "接收观测工程";
    project.activeFileId = "file-tech-01";
    project.activeChannelId = "channel-tech-01";
    project.files.resize(2);
    auto& f = project.files.front();
    f.metadata.id = project.activeFileId;
    f.metadata.name = "file-tech-01: 城市广播观测_完整文件名.raw";
    f.metadata.path = "D:/samples/城市广播观测_完整文件名.raw";
    f.metadata.centerFrequencyHz = 2.45e9;
    f.metadata.sampleRateHz = 80e6;
    f.metadata.sampleCount = 10000;
    f.metadata.availability.status = LoadStatus::Partial;
    f.metadata.availability.availableSamples = 6000;
    f.activeMarkId = "mark-tech-01";
    f.selectedMarkIds = {f.activeMarkId};
    f.hoveredMarkId = "not-the-tree-hover";
    f.channels.resize(1);
    auto& c = f.channels.front();
    c.id = project.activeChannelId;
    c.name = "[channel-tech-01] 城市调频广播";
    c.centerFrequencyHz = 100.125e6;
    c.bandwidthHz = 25e3;
    c.sourceTime = {100, 7000};
    c.processingState = ChannelProcessingState::Ready;
    c.sourceRelation = ChannelSourceRelation::Changed;
    f.detectionRuns.resize(101);  // The tree loads one run, but reports all 101.
    auto& second = project.files.back();
    second.metadata.id = "file-other";
    second.metadata.name = "第二个文件.raw";
    second.metadata.sampleRateHz = 1e6;
    root = new QTreeWidgetItem(&tree, {"▾ ◧ 接收观测工程"});
    root->setData(0, Qt::UserRole, "project");
    file = add(root, "file", "◈ file-tech-01: 城市广播观测_完整文件名.raw", "file-tech-01");
    file->setFlags(Qt::ItemIsEnabled);
    metadata = add(file, "metadata", "80 MS/s · 2450 MHz · 0.000125 s");
    marks = add(file, "marks", "信号标记");
    actions = add(marks, "treeActions", "");
    actions->setSizeHint(0, QSize(0, 61));
    source = add(marks, "annotationSource", "原始标签 (2/100000)", "0");
    mark = add(source, "mark", "▣ mark-tech-01: 调频广播 · 第 1/2 段", "mark-tech-01");
    mark->setData(0, Qt::UserRole + 3, 7);
    mark->setForeground(0, QColor("#65d2bd"));
    mark->setToolTip(0, "单击选择 · Ctrl 多选\nmark-tech-01\n第 1/2 段");
    otherMark = add(source, "mark", "▱ 保留用户前缀 CH-01 · 时间未定位", "mark-other");
    otherMark->setForeground(0, QColor("#edc56d"));
    more = add(source, "annotationMore", "加载后 200 条 · 已显示 2/100000", "0");
    channels = add(file, "channels", "窄带通道 (1)");
    channel = add(channels, "channel", "◇ old text · 100.125 MHz · 来源未读入", "channel-tech-01");
    channel->setBackground(0, QColor("#3d442f"));  // Old active-channel styling.
    channel->setForeground(0, QColor("#fff0bf"));
    runs = add(file, "detectionRuns", "检测运行 (101)");
    run = add(runs, "detectionRun", "SCN · 3/4 窗口", "run-technical-id");
    run->setToolTip(0, "run-technical-id\n原始错误诊断");
    otherFile = add(root, "file", "◈ 第二个文件.raw", "file-other");
    for (auto* item : {root, file, marks, source, channels, runs}) item->setExpanded(true);
  }

  void refresh() { refreshProjectTreePresentation(&tree, project); }
};

QImage paint(Fixture& fixture, QTreeWidgetItem* item, int state = 0,
              QStyle::State optionState = QStyle::State_Enabled,
              int width = 340, int height = 52) {
  item->setData(0, Qt::UserRole + 3, state);
  QImage image(width + 20, height + 20, QImage::Format_ARGB32_Premultiplied);
  image.fill(QColor("#101d2f"));
  QPainter painter(&image);
  QStyleOptionViewItem option;
  option.initFrom(&fixture.tree);
  option.widget = &fixture.tree;
  option.font = fixture.tree.font();
  option.fontMetrics = QFontMetrics(option.font);
  option.state = optionState;
  option.rect = QRect(10, 10, width, height);
  fixture.tree.itemDelegate()->paint(&painter, option, fixture.tree.indexFromItem(item));
  painter.end();
  return image;
}

int pixels(const QImage& image, const QColor& color, QRect rect = {}) {
  if (rect.isNull()) rect = image.rect();
  int count = 0;
  for (int y = rect.top(); y <= rect.bottom(); ++y)
    for (int x = rect.left(); x <= rect.right(); ++x)
      if (image.pixelColor(x, y) == color) ++count;
  return count;
}

void dataPresentation() {
  Fixture f;
  const auto raw = f.file->text(0);
  f.refresh();
  check(f.file->data(0, TitleRole).toString() == "城市广播观测_完整文件名.raw", "File title must omit its technical ID and decoration");
  check(f.file->text(0) == raw, "Refresh must retain DisplayRole owned by MainWindow");
  check(f.file->data(0, DetailRole).toString() == "Fc 2.45 GHz · Fs 80 MS/s", "File subtitle must use compact Fc/Fs");
  check(f.channel->data(0, TitleRole).toString() == "城市调频广播", "Channel title must omit bracketed ID");
  check(f.channel->data(0, DetailRole).toString() == "Fc 100.125 MHz · BW 25 kHz", "Channel subtitle must contain Fc/BW");
  check(f.channel->data(0, BadgeRole).toStringList() == QStringList({"未读", "差异"}), "Channel must retain independent concise anomaly badges");
  check(f.metadata->isHidden() && f.file->child(0) == f.metadata, "Metadata row must be hidden in place");
  check(f.file->sizeHint(0).height() >= 46 && f.channel->sizeHint(0).height() >= 46, "Two-line rows must reserve both lines");
  check(f.file->toolTip(0).contains(QString::fromStdString(f.project.files.front().metadata.name)) &&
            f.file->toolTip(0).contains("D:/samples/") && f.file->toolTip(0).contains("2450000000 Hz") &&
            f.file->toolTip(0).contains("80000000 S/s") && f.file->toolTip(0).contains("6000/10000"), "File tooltip must retain full name, path and exact metadata");
  check(f.channel->toolTip(0).contains("100125000 Hz") && f.channel->toolTip(0).contains("25000 Hz") &&
            f.channel->toolTip(0).contains("来源已修改；保留创建快照") && f.channel->toolTip(0).contains("来源未读入"), "Channel tooltip must explain badges and exact geometry");
  check(f.channel->data(0, Qt::AccessibleTextRole).toString().contains("BW 25 kHz"), "Accessible text must include the second line");
}

void activeItems() {
  Fixture f;
  f.tree.setCurrentItem(f.otherMark);
  f.refresh();
  check(f.file->data(0, ActiveRole).toBool() && f.channel->data(0, ActiveRole).toBool(), "Project IDs must activate file and channel independently of keyboard focus");
  const auto activeFile = paint(f, f.file);
  const auto activeChannel = paint(f, f.channel);
  check(activeFile.pixelColor(11, 20) == QColor("#53bcf0") && activeChannel.pixelColor(11, 20) == QColor("#53bcf0"), "Active file/channel must use the same blue bar");
  check(activeFile.pixelColor(340, 12) == QColor("#214c70") && activeChannel.pixelColor(340, 12) == QColor("#214c70"), "Active file/channel must share a blue background");
  f.project.activeFileId = "file-other";
  f.refresh();
  check(!f.file->data(0, ActiveRole).toBool() && !f.channel->data(0, ActiveRole).toBool() &&
            f.otherFile->data(0, ActiveRole).toBool(), "Switching files must clear stale active presentation");
  check(pixels(paint(f, f.channel), QColor("#53bcf0")) == 0, "Old channel background must not keep an active bar");
  check(f.tree.currentItem() == f.otherMark, "Presentation must not move keyboard current item");
}

void markDataAndLazyCounts() {
  Fixture f;
  const auto brush = f.mark->foreground(0);
  const auto flags = f.mark->flags();
  f.refresh();
  check(f.mark->data(0, TitleRole).toString() == "调频广播 · 第 1/2 段", "Mark title must retain segment information without ID prefix");
  check(f.otherMark->data(0, TitleRole).toString().startsWith("保留用户前缀 CH-01"), "User names must not lose unrelated prefixes");
  check(f.mark->foreground(0) == brush && f.mark->data(0, Qt::UserRole + 3).toInt() == 7 && f.mark->flags() == flags, "Refresh must preserve mark source, bits and flags");
  check(f.source->data(0, TitleRole).toString() == "原始标签" && f.source->data(0, CountRole).toString() == "2/100000", "Source count must retain full filtered total");
  check(f.marks->data(0, CountRole).toString() == "2/100000", "Mark group must summarize existing source totals without enumeration");
  check(f.runs->data(0, CountRole).toString() == "101" && f.runs->childCount() == 1, "Run count must not be limited to loaded child rows");
  check(f.source->childCount() == 3 && f.more->text(0).contains("已显示 2/100000"), "Refresh must retain the lazy-load placeholder and loaded subset");
  check(f.project.files.front().marks.empty(), "Projection must work from loaded mark rows without consulting the mark collection");
  check(f.run->toolTip(0).contains("SCN · 3/4 窗口") && f.run->toolTip(0).contains("原始错误诊断"), "Run tooltip must include all visible text and existing diagnostics");
  check(f.actions->sizeHint(0).height() == 61 && !f.actions->data(0, TitleRole).isValid(), "Embedded action row must remain owned by MainWindow");
}

void markPaintingStates() {
  Fixture f;
  f.refresh();
  const QColor source("#65d2bd");
  const auto neutral = paint(f, f.mark, 0);
  const auto selected = paint(f, f.mark, 1);
  const auto current = paint(f, f.mark, 2);
  const auto hovered = paint(f, f.mark, 4);
  const auto all = paint(f, f.mark, 7, QStyle::State_Enabled | QStyle::State_HasFocus | QStyle::State_Selected | QStyle::State_MouseOver);
  for (const auto& image : {neutral, selected, current, hovered, all})
    check(pixels(image, source, QRect(31, 12, 300, 48)) > 0, "Every mark state must retain source text color under item QSS");
  check(neutral != selected && neutral != current && neutral != hovered && selected != current, "Selected/current/hover states must remain visually distinct");
  std::cout << "Combined mark cue pixels: current=" << pixels(all, QColor("#53bcf0"))
            << ", hover=" << pixels(all, QColor("#99d8f1"))
            << ", focus=" << pixels(all, QColor("#dbf1ff"))
            << ", selection=" << pixels(all, QColor("#347c9e")) << '\n';
  const auto diagnostics = qEnvironmentVariable("SS_PROJECT_TREE_DIAGNOSTICS");
  if (!diagnostics.isEmpty()) all.save(diagnostics + "/combined-mark.png");
  check(pixels(all, QColor("#53bcf0")) > 0 && pixels(all, QColor("#99d8f1")) > 0 &&
            pixels(all, QColor("#dbf1ff")) > 0 && pixels(all, QColor("#347c9e")) > 0, "Combined state must show current bar, hover outline, focus and selection check together");
  check(pixels(paint(f, f.mark, 0, QStyle::State_Enabled | QStyle::State_Selected), QColor("#347c9e")) == 0, "Native selection must not replace authoritative mark bit state");
}

void refreshPreservesViewState() {
  Fixture f;
  for (int i = 0; i < 80; ++i) add(f.file, "detectionRun", QString("Other run %1").arg(i), QString::number(i));
  f.refresh();
  f.tree.show();
  QApplication::processEvents();
  f.channels->setExpanded(false);
  f.runs->setExpanded(false);
  f.mark->setSelected(true);
  f.otherMark->setSelected(true);
  f.tree.setCurrentItem(f.otherMark, 0, QItemSelectionModel::NoUpdate);
  f.tree.verticalScrollBar()->setValue(70);
  QApplication::processEvents();
  const auto selected = f.tree.selectedItems();
  const auto scroll = f.tree.verticalScrollBar()->value();
  const auto horizontal = f.tree.horizontalScrollBar()->value();
  const auto rows = f.file->childCount();
  const auto oldDomainSelection = f.project.files.front().selectedMarkIds;
  const auto oldDomainCurrent = f.project.files.front().activeMarkId;
  int selectionSignals = 0, expansionSignals = 0, currentSignals = 0;
  QObject::connect(f.tree.selectionModel(), &QItemSelectionModel::selectionChanged, &f.tree,
                   [&] { ++selectionSignals; });
  QObject::connect(&f.tree, &QTreeWidget::itemExpanded, &f.tree, [&] { ++expansionSignals; });
  QObject::connect(f.tree.selectionModel(), &QItemSelectionModel::currentChanged, &f.tree,
                   [&] { ++currentSignals; });
  for (int i = 0; i < 3; ++i) f.refresh();
  QApplication::processEvents();
  check(!f.channels->isExpanded() && !f.runs->isExpanded() && f.source->isExpanded(), "Refresh must preserve expanded and collapsed items");
  check(f.tree.selectedItems() == selected && f.tree.currentItem() == f.otherMark, "Refresh must preserve native selection and keyboard current");
  check(f.tree.verticalScrollBar()->value() == scroll && f.tree.horizontalScrollBar()->value() == horizontal, "Refresh must not scroll");
  check(f.file->childCount() == rows && f.file->child(0) == f.metadata && f.source->child(0) == f.mark, "Refresh must not rebuild, replace or append nodes");
  check(selectionSignals == 0 && expansionSignals == 0 && currentSignals == 0, "Refresh must not signal selection, focus or expansion changes");
  check(f.project.files.front().selectedMarkIds == oldDomainSelection && f.project.files.front().activeMarkId == oldDomainCurrent &&
            f.project.files.front().hoveredMarkId == "not-the-tree-hover", "Refresh must not write domain selection/current/hover");
}

void repeatedRefreshAndOldSync() {
  Fixture f;
  f.refresh();
  const auto tooltip = f.mark->toolTip(0);
  const auto runTooltip = f.run->toolTip(0);
  f.refresh();
  f.refresh();
  check(f.mark->toolTip(0) == tooltip && f.run->toolTip(0) == runTooltip, "Repeated refresh must not duplicate tooltip text");
  f.mark->setText(0, "▱ 改名后的标记 · 时间未定位");
  f.mark->setToolTip(0, "旧 sync 新证据\nmark-tech-01");
  f.mark->setForeground(0, QColor("#51c4e9"));
  f.mark->setData(0, Qt::UserRole + 3, 4);
  f.source->setText(0, "原始标签 (1/100000)");
  f.refresh();
  check(f.mark->data(0, TitleRole).toString() == "改名后的标记 · 时间未定位" &&
            f.mark->toolTip(0).contains("旧 sync 新证据") && !f.mark->toolTip(0).contains("第 1/2 段"), "Refresh must accept fresh text and evidence from old sync");
  check(f.mark->foreground(0).color() == QColor("#51c4e9") && f.mark->data(0, Qt::UserRole + 3).toInt() == 4,
        "Old sync must remain the source-color and state owner");
  check(f.source->data(0, CountRole).toString() == "1/100000", "Source counts must follow old sync without loading marks");
}

void anomaliesAndBoundaries() {
  Fixture f;
  auto& c = f.project.files.front().channels.front();
  c.sourceRelation = ChannelSourceRelation::Missing;
  c.processingState = ChannelProcessingState::SourceMissing;
  f.refresh();
  check(f.channel->data(0, BadgeRole).toStringList().count("缺失") == 1, "Duplicate relation and processing warnings must share one badge");
  c.sourceRelation = ChannelSourceRelation::Current;
  c.processingState = ChannelProcessingState::Invalid;
  f.refresh();
  check(f.channel->data(0, BadgeRole).toStringList().contains("无效") && f.channel->toolTip(0).contains("通道配置无效"), "Invalid configuration must have a concise badge with explanation");
  check(cleanName("[channel-tech-01]", "channel-tech-01", "未命名通道") == "未命名通道", "Technical-ID-only names need a human fallback");
  check(cleanName("file-tech-012 观测.raw", "file-tech-01", "未命名文件") == "file-tech-012 观测.raw", "Partial ID matches must not remove real name text");
  check(cleanName("channel-tech-01 - 自定义", "channel-tech-01", "未命名通道") == "自定义", "Removing an exact ID prefix must also remove its separator");
  check(shortRate(0) == "0 Hz" && shortRate(-500) == "-500 Hz" &&
            shortRate(std::numeric_limits<double>::quiet_NaN()) == "—", "Frequency formatting must handle zero, signed offsets and unknown values");
  refreshProjectTreePresentation(nullptr, f.project);
  QTreeWidget empty;
  refreshProjectTreePresentation(&empty, Project{});
  check(empty.topLevelItemCount() == 0, "Empty refresh must not create nodes");
}

void clippedPaintingAndReadableRows() {
  Fixture f;
  f.project.files.front().metadata.name = std::string(500, 'W') + "完整结尾.raw";
  f.project.files.front().channels.front().name = std::string(500, 'M') + "完整通道结尾";
  f.refresh();
  for (auto* item : {f.root, f.file, f.source, f.mark, f.channel, f.runs, f.run, f.more}) {
    const auto image = paint(f, item, item == f.mark ? 7 : 0, QStyle::State_Enabled | QStyle::State_HasFocus, 92);
    check(pixels(image, QColor("#101d2f"), QRect(0, 0, 10, image.height())) == 10 * image.height(), "Narrow text must not paint outside the row's left edge");
    check(pixels(image, QColor("#101d2f"), QRect(102, 0, 10, image.height())) == 10 * image.height(), "Long text and badges must not spill beyond the row");
  }
  check(f.file->toolTip(0).contains("完整结尾.raw") && f.channel->toolTip(0).contains("完整通道结尾"), "Elided names must remain complete in tooltips");
  auto largeFont = f.tree.font();
  largeFont.setPointSize(24);
  f.file->setFont(0, largeFont);
  f.refresh();
  check(f.file->sizeHint(0).height() >= QFontMetrics(largeFont).height() * 2 + 9, "Row height must support two lines at larger fonts");
}

}  // namespace

// Independent executable: compile this file and domain/project.cpp, linking
// Qt6::Widgets (C++20). No moc, MainWindow or changes to project CMake needed.
int main(int argc, char** argv) {
  QApplication application(argc, argv);
  const std::pair<const char*, std::function<void()>> tests[] = {
      {"data presentation", dataPresentation},
      {"active file and channel", activeItems},
      {"mark data and lazy group counts", markDataAndLazyCounts},
      {"independent mark painting states", markPaintingStates},
      {"refresh preserves view state", refreshPreservesViewState},
      {"repeat refresh and old sync", repeatedRefreshAndOldSync},
      {"anomalies and boundary data", anomaliesAndBoundaries},
      {"clipped painting and readable rows", clippedPaintingAndReadableRows}};
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
    }
  }
  std::cout << "Totals: " << std::size(tests) - failures << " passed, " << failures << " failed\n";
  return failures == 0 ? 0 : 1;
}
