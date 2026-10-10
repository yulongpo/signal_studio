#pragma once
#include "infrastructure/import_controller.h"
#include "infrastructure/format_template_store.h"
#include <QSettings>
#include <QDialog>
#include <functional>

class QComboBox;class QLabel;class QPushButton;class QCheckBox;class QLineEdit;class QStackedWidget;class QTableWidget;class QTimer;class QProgressBar;class QVBoxLayout;
namespace signalstudio {
class AdaptiveValueEdit;class SourcePreviewWidget;class ParameterInputPolicy;
class SignalImportDialog final:public QDialog {
    Q_OBJECT
public:
    explicit SignalImportDialog(const QString& projectName,QWidget* parent=nullptr,QSettings* templateSettings=nullptr);
    ~SignalImportDialog() override;
    bool addPath(const QString&);
    ImportController& controller(){return controller_;}
    void setPage(int);
    bool saveTemplate(const QString&,QString&);
    bool importTemplates(const QString&,QString&);
    bool exportTemplates(const QString&,QString&) const;
    std::function<bool(FileState,QString&)> commitSource;
public slots:
    void startImport();
    void pollImport();
protected:
    void resizeEvent(QResizeEvent*) override;
    void reject() override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
private:
    void buildSingle(QWidget*);
    void buildBatch(QWidget*);
    void buildTemplates(QWidget*);
    void refreshTemplates();
    void chooseFiles();
    void loadCurrent();
    void commitConfiguration();
    void refreshSummary(bool preview=true);
    void refreshBatch();
    void selectRow(int);
    void updateProgress();
    void buildOverlay();
    void applyTemplate(int,bool batch=false);
    ImportController controller_;
    QSettings templateSettings_;
    FormatTemplateStore templates_{templateSettings_};
    QVBoxLayout* templateGridHost_=nullptr;QLabel* recentTemplates_=nullptr;
    QString batchKey_;
    std::optional<SampleIndex> previewSample_;
    std::size_t current_=0;bool updating_=false,previewAfterLoad_=false;
    ByteOrder previousOrder_=ByteOrder::Little;IQLayout previousLayout_=IQLayout::IQInterleaved;
    ParameterInputPolicy* inputPolicy_=nullptr;
    QStackedWidget* pages_=nullptr;
    QList<QPushButton*> tabs_;
    QLabel *filename_=nullptr,*details_=nullptr,*provenance_=nullptr,*summary_=nullptr,*stats_=nullptr,*validation_=nullptr,*footer_=nullptr,*fsLabel_=nullptr,*fcLabel_=nullptr;
    QLabel *formatCode_=nullptr,*formatSize_=nullptr,*countStat_=nullptr,*durationStat_=nullptr,*frequencyStat_=nullptr;
    QComboBox *structure_=nullptr,*encoding_=nullptr,*order_=nullptr,*layout_=nullptr,*channelLayout_=nullptr,*channel_=nullptr,*templateChoice_=nullptr;
    QWidget *orderField_=nullptr,*layoutField_=nullptr,*advanced_=nullptr;
    QLineEdit *header_=nullptr,*trailer_=nullptr,*channels_=nullptr;
    AdaptiveValueEdit *fs_=nullptr,*fc_=nullptr,*bw_=nullptr,*start_=nullptr;
    QCheckBox *normalize_=nullptr,*confirmed_=nullptr;
    SourcePreviewWidget* preview_=nullptr;
    QPushButton* import_=nullptr;
    QTableWidget* table_=nullptr;QLabel* batchSelected_=nullptr;
    QWidget* overlay_=nullptr;QLabel *progressTitle_=nullptr,*progressDescription_=nullptr,*progressMetrics_=nullptr;
    QList<QLabel*> stages_;QProgressBar* progress_=nullptr;
    QList<QLabel*> stageIndicators_;
    QPushButton *stop_=nullptr,*finish_=nullptr,*restart_=nullptr;
    QTimer* timer_=nullptr;
};
}
