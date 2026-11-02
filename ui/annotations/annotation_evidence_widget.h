#pragma once

#include <QByteArray>
#include <QWidget>
#include <array>
#include <cstddef>
#include <memory>

class QGridLayout;
class QLabel;
class QPlainTextEdit;
class QToolButton;
class QVBoxLayout;
class QDialog;
class QLineEdit;

namespace signalstudio {

struct Annotation;
struct AnnotationSourceSet;
struct FileState;
struct OriginEvidence;

// Displays saved evidence only. Selecting a result never starts analysis.
class AnnotationEvidenceWidget final : public QWidget {
  Q_OBJECT

 public:
  // Both entry points share a short summary and a non-modal full-evidence view.
  enum class Mode { Inspector, Auxiliary };

  explicit AnnotationEvidenceWidget(QWidget* parent = nullptr);
  explicit AnnotationEvidenceWidget(Mode mode, QWidget* parent = nullptr);
  Mode mode() const { return mode_; }
  void setAnnotation(const Annotation*, const FileState*);

 private:
  enum class Fact {
    Method,
    TimeBasis,
    Uncertainty,
    FrequencyBasis,
    CurrentRange,
    OriginalRange,
    Observation,
    Mean,
    Maximum,
    Windows,
    WindowIds,
    Noise,
    Reason,
    Class,
    Format,
    Origin,
    Coordinates,
    Count
  };

  void buildFacts(QVBoxLayout*);
  void addFact(QGridLayout*, Fact, const QString& caption,
               const QString& objectName);
  void setFact(Fact, const QString& value);
  void clearEvidence();
  void showTimeEvidence(const Annotation&, double currentSampleRate,
                        double originalSampleRate);
  void showOriginEvidence(const Annotation&, const AnnotationSourceSet*);
  void showScores(const Annotation&, bool fixture);
  void openDetails();
  void setFullText(QString);
  void showRawPage(qsizetype);
  void findRawText();
  void copyFullText(quint64 request, const QString& text, int attempt);

  Mode mode_ = Mode::Inspector;
  QLabel* title_ = nullptr;
  QLabel* context_ = nullptr;
  QLabel* notice_ = nullptr;
  QLabel* scoreDefinition_ = nullptr;
  QLabel* summary_ = nullptr;
  QDialog* details_ = nullptr;
  QWidget* rawNavigation_ = nullptr;
  QLabel* rawPageLabel_ = nullptr;
  QLineEdit* rawSearch_ = nullptr;
  QString fullText_;
  qsizetype rawPage_ = 0;
  qsizetype rawPageStart_ = 0;
  qsizetype rawPageEnd_ = 0;
  qsizetype rawNextSearch_ = 0;
  QString rawSearchNeedle_;
  quint64 copyRequest_ = 0;
  QToolButton* rawToggle_ = nullptr;
  QPlainTextEdit* rawFields_ = nullptr;
  QWidget* measuredTrace_ = nullptr;
  std::array<QLabel*, static_cast<std::size_t>(Fact::Count)> values_{};
  QByteArray displayKey_;
  std::shared_ptr<const OriginEvidence> displayedOrigin_;
  bool empty_ = false;
};

}  // namespace signalstudio
