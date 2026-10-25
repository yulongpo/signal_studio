#pragma once

#include <QImage>
#include <QJsonObject>
#include <QLineF>
#include <QPainterPath>
#include <QTimer>
#include <QWidget>
#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

#include "application/session.h"
#include "ui/charts/axis_layout.h"
#include "ui/charts/axis_description.h"
#include "ui/charts/display_sampling.h"
#include "ui/charts/interaction_feedback.h"

class QMenu;
class QPointingDevice;

namespace signalstudio {
class AcceleratedSurface;
class HeatmapWorker;
class CurveWorker;
struct HeatmapPayload;
struct CurvePayload;

class PlotWidget : public QWidget {
  Q_OBJECT
  Q_PROPERTY(AxisTitleMode axisTitleMode READ axisTitleMode WRITE setAxisTitleMode)
 public:
  enum class Kind { Navigation, Auxiliary, Main };
  enum class AxisTitleMode { UnitAtTop, Header, Rotated, CompactHeader };
  Q_ENUM(AxisTitleMode)
  PlotWidget(Session& session, Kind kind, QWidget* parent = nullptr);
  ~PlotWidget() override;
  QRectF plotRect() const;
  AxisTitleMode axisTitleMode() const { return axisTitleMode_; }
  Q_INVOKABLE void setAxisTitleMode(AxisTitleMode mode);
  axis_layout::Layout axisLayout() const;
  AxisDescription axisDescription() const;
  Q_INVOKABLE QJsonObject axisLayoutStatistics() const;
  QPointF toPixel(SampleIndex sample, double frequencyHz) const;
  void setCreating(bool enabled);
  bool isCreating() const { return creating_; }
  void beginDetectionRoi();
  bool isSelectingDetectionRoi() const { return detectionRoi_.has_value(); }
  bool hasDetectionRoiPreview() const {
    return detectionRoi_ && detectionRoi_->preview.has_value();
  }
  std::optional<ViewRange> detectionRoiPreview() const {
    return detectionRoi_ ? detectionRoi_->preview : std::nullopt;
  }
  void confirmDetectionRoi();
  void cancelDetectionRoi();
  bool hasPendingInteraction() const {
    return gesture_.has_value() || wheelBase_.has_value() ||
           detectionRoi_.has_value();
  }
  void cancelGesture(bool exitCreating = false);
  void syncState();
  void setCursorCoordinates(SampleIndex sample, double frequencyHz);
  void setHoveredMark(const QString& id, const QString& segmentId = {});
  void refreshAnnotationOverlay();
  QString statusText() const;
  QString tipText() const;
  QMenu* createContextMenu(const QPoint& position);
  bool gpuReady() const;
  QString renderingBackend() const;
  quint64 textureUploadCount() const;
  quint64 completedFrameCount() const;
  quint64 powerGenerationCount() const { return powerGenerations_; }
  qsizetype drawnPointCount() const;
  qsizetype sourcePointCount() const;
  QString renderQuality() const;
  bool samplePointsVisible() const { return curveShowsSamplePoints_; }
  qsizetype samplePointCount() const {
    return curveShowsSamplePoints_ ? static_cast<qsizetype>(curveTrace_.size())
                                   : 0;
  }
  quint64 displayGeneration() const { return renderGeneration_; }
  QJsonObject renderStatistics() const;
  bool isDisplaySettled() const;
  std::shared_ptr<const SpectrogramData> currentSpectrogram() const;
  std::shared_ptr<const SpectralFrame> currentPowerFrame() const;
 signals:
  void axisDescriptionChanged();
  void stateChanged();
  void statusMessage(const QString& text);
  void markSelectionRequested(const QString& id,
                              Qt::KeyboardModifiers modifiers);
  void annotationHoverChanged();
  void markSegmentSelectionRequested(const QString& id,
                                     const QString& segmentId,
                                     Qt::KeyboardModifiers modifiers);
  void backendChanged(const QString& description);
  void cursorChanged(SampleIndex sample, double frequencyHz);
  void interactionHint(const QString& text);
  void markRenameRequested();
  void annotationChannelRequested();
  void widebandDetectionRequested();
  void detectionRoiRequested();
  void detectionRoiConfirmed(const ViewRange& range);
  void detectionRoiPreviewChanged();
  void markDeleteRequested();
  void autoPowerFitRequested();

 protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void mouseDoubleClickEvent(QMouseEvent*) override;
  void wheelEvent(QWheelEvent*) override;
  void contextMenuEvent(QContextMenuEvent*) override;
  void keyPressEvent(QKeyEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  bool event(QEvent*) override;

 private:
  struct AxisLayoutCache;
  const AxisLayoutCache& measuredAxes() const;
  void freezeAxisLayouts();
  void releaseAxisLayouts();
  void refreshAxisLayouts();
  mutable std::unique_ptr<AxisLayoutCache> axisLayoutCache_;
  std::optional<axis_layout::Budget> frozenAxisBudget_;
  AxisTitleMode axisTitleMode_ = AxisTitleMode::CompactHeader;
  using Zone = chart_feedback::Zone;
  enum class Tool {
    ZoomBox,
    CreateMark,
    DetectionRoi,
    MarkEdit,
    PanTime,
    PanFrequency,
    Navigate,
    AuxiliaryY,
    AuxiliaryZoom
  };
  enum class WheelAxis { Time, Frequency, AuxiliaryY };
  struct Gesture {
    Tool tool;
    ViewSnapshot before;
    QPointF start, current;
    const QPointingDevice* device = nullptr;
    std::string markId, clickMarkId, clickSegmentId;
    ViewRange mark, draft;
    AnnotationEditContext editContext;
    bool observationOnly = false;
    int edges = 0;
    bool changed = false;
    bool navigationInside = false;
    MainMode mainMode = MainMode::TimeFrequency;
    AuxiliaryMode auxiliaryMode = AuxiliaryMode::Waveform;
  };
  struct Hit {
    std::string id;
    int edges = 0;
    bool selected = false;
    std::string segmentId;
  };
  struct DetectionRoiDraft {
    std::string fileId;
    std::uint64_t projectGeneration = 0;
    std::uint64_t availabilityGeneration = 0;
    MainMode mainMode = MainMode::TimeFrequency;
    ViewRange view, bounds;
    bool previousCreating = false;
    std::optional<ViewRange> preview;
  };
  // An offset from the integer view origin keeps edits above 2^53 precise.
  struct Coordinate {
    long double sample;
    double frequency;
  };
  Coordinate fromPixel(QPointF position, const ViewRange& view) const;
  bool detectionRoiContextMatches() const;
  std::optional<ViewRange> detectionRoiRange(const Gesture& gesture) const;
  QRectF markRect(const ViewRange&) const;
  Hit hitMark(QPointF) const;
  std::vector<Hit> markChoicesAt(QPointF) const;
  std::vector<std::string> marksAt(QPointF) const;
  void addMarkChoices(QMenu*, const std::vector<Hit>&, Qt::KeyboardModifiers);
  void selectAt(QPointF, Qt::KeyboardModifiers);
  Zone zoneAt(QPointF) const;
  QRectF navigationWindow() const;
  void updateHover(QPointF);
  void finishWheel();
  void finishGesture(Qt::KeyboardModifiers modifiers);
  void updateHeatmap();
  void updateCurve();
  void beginPreview();
  void acceptHeatmap(std::shared_ptr<HeatmapPayload> result);
  void acceptCurve(std::shared_ptr<CurvePayload> result);
  void acceptNavigationCurve(std::shared_ptr<CurvePayload> result);
  std::pair<QRectF, QRectF> heatmapPlacement() const;
  void paintScene(QPainter& painter, bool accelerated);
  void repaintChart();
  void drawCursors(QPainter& painter, bool inverseMask = false);
  void pinAt(QPointF point);
  void expandAnalysisTime();
  Session& session_;
  Kind kind_;
  bool creating_ = false;
  bool releasing_ = false;
  std::optional<DetectionRoiDraft> detectionRoi_;
  std::optional<Gesture> gesture_;
  QTimer wheelTimer_;
  QTimer renderSettleTimer_;
  std::optional<ViewSnapshot> wheelBase_;
  WheelAxis wheelAxis_ = WheelAxis::Time;
  QString powerKey_, colorKey_;
  QString requestedPowerKey_;
  std::shared_ptr<HeatmapPayload> renderedPower_;
  std::deque<std::shared_ptr<HeatmapPayload>> powerCache_;
  std::unique_ptr<HeatmapWorker> worker_;
  std::unique_ptr<CurveWorker> curveWorker_;
  std::unique_ptr<CurveWorker> navigationWorker_;
  quint64 renderGeneration_ = 0, staleResults_ = 0, heatmapCacheHits_ = 0,
          matrixReductions_ = 0;
  quint64 heatmapRequestGeneration_ = 0;
  quint64 committedGeneration_ = 0;
  QSize requestedHeatSize_;
  bool acceptedPreview_ = false;
  bool preview_ = false;
  std::optional<ViewRange> displayedView_;
  QString curveSourceKey_, curveReductionKey_, curvePathKey_, navigationKey_;
  QString navigationCurveKey_, navigationCurveError_;
  std::vector<float> navigationCurve_;
  quint64 navigationRequestGeneration_ = 0;
  bool navigationCurvePending_ = false, navigationCurveCompleted_ = false;
  QString curveError_;
  quint64 curveRequestGeneration_ = 0;
  bool curvePending_ = false, curveCompleted_ = false;
  std::vector<float> curveSource_;
  std::shared_ptr<const SpectralFrame> averagePower_;
  std::vector<display::TracePoint> curveTrace_;
  bool curveShowsSamplePoints_ = false;
  QPainterPath curvePath_;
  std::vector<QLineF> curveSegments_;
  std::array<QPainterPath, 2> navigationPaths_;
  std::array<std::vector<QLineF>, 2> navigationSegments_;
  quint64 curveGenerations_ = 0, curveReductions_ = 0, curveCacheHits_ = 0;
  QImage heatmap_;
  QSize heatSize_;
  AcceleratedSurface* surface_ = nullptr;
  bool softwareFallback_ = false;
  quint64 powerGenerations_ = 0, colorTransformGenerations_ = 0;
  Zone hoverZone_ = Zone::None;
  SampleIndex cursorSample_ = 0;
  double cursorFrequency_ = 0;
  std::string displayedFile_;
  std::uint64_t displayedProjectGeneration_ = 0;
  MainMode displayedMainMode_ = MainMode::TimeFrequency;
  AuxiliaryMode displayedAuxiliaryMode_ = AuxiliaryMode::Waveform;
  int displayedFft_ = 2048;
  bool displayedColorScale_ = false;
  std::optional<QPointF> hoverPosition_;
};
}  // namespace signalstudio
