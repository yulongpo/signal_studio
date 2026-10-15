#pragma once

#include <QMainWindow>
#include <QPointer>
#include <QStringList>
#include <array>
#include <map>
#include <unordered_set>

#include "application/session.h"

class QTreeWidget;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSplitter;
class QToolButton;
class QPlainTextEdit;
class QAction;
class QCheckBox;
class QPushButton;
class QScrollArea;
class QVBoxLayout;
class QStackedWidget;
class QMenu;
class QTimer;

namespace signalstudio {
class AnnotationPanel;
class AnnotationEvidenceWidget;
class DetectionPanel;
class DetectionCard;
class PlotWidget;
class NarrowbandWorkspace;
class SourceLoader;
class SpectralSettingsWidget;
class ParameterInputPolicy;
class PanelResizeHandle;
class MainWindow : public QMainWindow {
  Q_OBJECT
 public:
  explicit MainWindow(QWidget* parent = nullptr);
  ~MainWindow() override;
  Session& session() { return session_; }
  void refresh();
  // Repaints only annotation projections, without requesting DSP or changing
  // the selected set, active object/segment, page, or visible range.
  void refreshAnnotationHover();
  bool importLabels(const QString& path, QString* error = nullptr,
                    bool preview = false);
  bool createProject(const QString& parentDirectory, const QString& projectName,
                     QString* error = nullptr);
  bool openProject(const QString& path);
  bool addIqFile(const QString& path, QString* error = nullptr);
  bool addImportedSource(FileState file, QString& error);
  bool saveProject(const QString& path);
  void openNarrowbandDemoProject();
  void cancelInteractions(bool exitCreating = true);
  bool clearActiveLinkedCursor();

 protected:
  void resizeEvent(QResizeEvent*) override;
  void moveEvent(QMoveEvent*) override;
  void changeEvent(QEvent*) override;
  void keyPressEvent(QKeyEvent*) override;
  bool eventFilter(QObject*, QEvent*) override;

 private:
  ParameterInputPolicy* parameterInputPolicy_ = nullptr;
  void startSourceLoad(FileState& file, SampleIndex target);
  void pollSourceLoads();
  struct LoadJob {
    std::shared_ptr<SourceLoader> loader;
    std::uint64_t project = 0;
    SampleIndex previous = 0;
  };
  std::map<std::string, LoadJob> sourceLoads_;
  QTimer* sourceLoadTimer_ = nullptr;
  QPushButton* stopSourceLoad_ = nullptr;
  SpectralSettingsWidget *psdParameters_ = nullptr, *stftParameters_ = nullptr;
  bool handleWindowChrome(QObject*, QEvent*);
  void toggleWindowState();
  void updateWindowControls();
  void openWidebandDetection(const DetectionRun* parentRun = nullptr);
  void beginDetectionRoi();
  void updateDetectionRoiConfirmation();
  void rememberNormalGeometry();
  Qt::Edges resizeEdges(QPoint) const;
  QRect normalWindowGeometry_;
  QPoint windowDragAnchor_, windowDragOrigin_;
  bool windowDragPending_ = false, windowDragFallback_ = false,
       restoringWindowGeometry_ = false;
  QToolButton* windowMaximize_ = nullptr;
  QPointer<QWidget> resizeCursorWidget_;
  void applyPowerInput(QComboBox* control);
  void normalizePowerInput(QComboBox* control);
  void updatePowerFit();
  void autoFitPower();
  void buildMenus();
  void buildWorkspace();
  void restoreUiState();
  void saveUiState();
  void restoreRightSidebarSettings(FileState& file);
  void applyGlobalRightSidebarSettings(const DisplaySettings& fallback);
  void propagateGlobalRightSidebarSettings();
  void scheduleRightSidebarSettingsSave();
  void saveRightSidebarSettings();
  void updateRecentProjectsMenu();
  void rememberProject(const QString& path);
  void rebuildTree();
  void updateTree();
  void syncTreeState();
  void syncTreeAnnotationStyles();
  void resizeTreeToContents();
  void revealTreeAnnotation();
  std::vector<std::string> treeSelectionOrder() const;
  QStringList treeStructure() const;
  void selectMark(const QString& id, Qt::KeyboardModifiers modifiers,
                  const QString& segmentId = {}, bool fromTree = false);
  void selectAllMarks();
  void locateMark();
  void deleteMarks();
  void renameMark();
  void showAddFileDialog();
  void showNewProjectDialog();
  void openDemoProject();
  void showChannelDialog(const QString& channelId = {});
  void locateActiveChannelSource();
  void activateTreeChannel(const QString& channelId);
  void toggleMaximized(int index);
  void enforceLayout(bool reset = false);
  void updatePropertyContext();
  void updateBottom();
  void updateCursor(SampleIndex sample, double frequencyHz);
  void log(const QString& message);
  QPushButton* autoPowerFit_ = nullptr;
  std::shared_ptr<const SpectrogramData> powerFitHeatmap_;
  std::shared_ptr<const SpectralFrame> powerFitPsd_;
  PowerFitResult cachedPowerFit_;
  bool powerFitReady_ = false;
  Session session_;
  DetectionPanel* detection_ = nullptr;
  DetectionCard* detectionCard_ = nullptr;
  QWidget* detectionSection_ = nullptr;
  QWidget* detectionRoiConfirmation_ = nullptr;
  QPushButton* detectionRoiTool_ = nullptr;
  QPushButton* manualTool_ = nullptr;
  PanelResizeHandle *leftResize_ = nullptr, *rightResize_ = nullptr,
                    *bottomResize_ = nullptr;
  int resourceWidth_ = 278, propertyWidth_ = 390, bottomHeight_ = 260;
  QPushButton* widebandDetect_ = nullptr;
  QAction* detectionAction_ = nullptr;
  AnnotationPanel* annotations_ = nullptr;
  QStackedWidget* auxiliaryDisplay_ = nullptr;
  AnnotationEvidenceWidget* auxiliaryEvidence_ = nullptr;
  QPushButton* evidenceMode_ = nullptr;
  bool auxiliaryEvidenceVisible_ = false;
  QString projectPath_;
  static constexpr std::size_t kTreeAnnotationBatchSize = 40;
  QString treeAnnotationContext_;
  std::array<std::size_t, 3> treeAnnotationLimits_{kTreeAnnotationBatchSize,
                                                   kTreeAnnotationBatchSize,
                                                   kTreeAnnotationBatchSize};
  std::unordered_set<std::string> treeRevealedAnnotationIds_;
  std::string treeActiveAnnotationId_;
  bool treeRevealPending_ = false;
  bool refreshing_ = false, treeRebuildPending_ = false, layoutBusy_ = false,
       destroying_ = false;
  bool resourcesCollapsed_ = false, propertiesCollapsed_ = false;
  int maximizedPanel_ = -1, bottomTab_ = 0;
  QString propertyContext_, cursorFileId_;
  SampleIndex cursorSample_ = 0;
  double cursorFrequency_ = 0;
  QStringList treeStructure_;
  QTreeWidget* tree_ = nullptr;
  PlotWidget *navigation_ = nullptr, *auxiliary_ = nullptr, *main_ = nullptr;
  QSplitter* graphs_ = nullptr;
  std::array<QWidget*, 3> panels_{};
  QList<int> panelSizes_;
  QWidget *resources_ = nullptr, *resourceBody_ = nullptr,
          *resourceTools_ = nullptr;
  QWidget *properties_ = nullptr, *propRail_ = nullptr, *bottom_ = nullptr,
          *empty_ = nullptr;
  QWidget *maxHost_ = nullptr, *maxVeil_ = nullptr;
  QScrollArea* propScroll_ = nullptr;
  QVBoxLayout* propertyLayout_ = nullptr;
  QWidget *fileSection_ = nullptr, *viewSection_ = nullptr,
          *markSection_ = nullptr, *psdSection_ = nullptr,
          *channelSection_ = nullptr;
  QComboBox *mainMode_ = nullptr, *auxMode_ = nullptr, *waveformMode_ = nullptr,
            *palette_ = nullptr, *propertyPalette_ = nullptr;
  QComboBox *stft_ = nullptr, *psd_ = nullptr, *psdScope_ = nullptr,
            *dynamic_ = nullptr, *reference_ = nullptr, *freqMode_ = nullptr;
  QDoubleSpinBox* effectiveBandwidth_ = nullptr;
  QCheckBox *grid_ = nullptr, *colorScale_ = nullptr;
  std::array<QPushButton*, 2> auxiliaryButtons_{}, mainButtons_{};
  std::array<QPushButton*, 3> bottomButtons_{};
  std::array<QLabel*, 4> fileValues_{};
  std::array<QLabel*, 2> viewValues_{};
  std::array<QLabel*, 3> markValues_{};
  std::array<QLabel*, 6> channelValues_{};
  QLabel *channelRevision_ = nullptr, *channelRelation_ = nullptr;
  QLabel *scope_ = nullptr, *projectLabel_ = nullptr, *navStatus_ = nullptr,
         *auxStatus_ = nullptr, *rangeTag_ = nullptr,
         *dataSourceStatus_ = nullptr;
  QLabel *specMode_ = nullptr, *specAxis_ = nullptr, *selectedCount_ = nullptr,
         *cursorData_ = nullptr, *viewData_ = nullptr,
         *selectionData_ = nullptr;
  QLabel *statusTime_ = nullptr, *statusFrequency_ = nullptr,
         *statusFile_ = nullptr;
  QLabel *resourceTitle_ = nullptr, *propertiesTitle_ = nullptr;
  QPushButton *extract_ = nullptr, *rename_ = nullptr, *locate_ = nullptr,
              *delete_ = nullptr;
  QToolButton* resultToggle_ = nullptr;
  QStackedWidget* bottomContent_ = nullptr;
  QStackedWidget* workspaceStack_ = nullptr;
  NarrowbandWorkspace* narrowband_ = nullptr;
  QPushButton* workspaceModeButton_ = nullptr;
  QLabel *resultSummary_ = nullptr, *taskSummary_ = nullptr;
  QPlainTextEdit* results_ = nullptr;
  QAction *removeAction_ = nullptr, *saveAction_ = nullptr,
          *addSignalAction_ = nullptr, *deleteAction_ = nullptr,
          *backAction_ = nullptr, *forwardAction_ = nullptr;
  QMenu* recentProjectsMenu_ = nullptr;
  QTimer* rightSidebarSaveTimer_ = nullptr;
  bool restoringUiState_ = false, splitterStateRestored_ = false;
  bool sharedPsdFromSelectionPreference_ = false;
};
}  // namespace signalstudio
