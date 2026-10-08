#pragma once

#include "application/session.h"
#include <QMainWindow>
#include <QStringList>

class QTreeWidget;
class QComboBox;
class QLabel;
class QSplitter;
class QToolButton;
class QPlainTextEdit;
class QAction;
class QDoubleSpinBox;
class QSpinBox;
class QCheckBox;

namespace signalstudio {
class PlotWidget;
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    Session& session() { return session_; }
    void refresh();
    bool openProject(const QString& path);
    bool saveProject(const QString& path);
    void cancelInteractions(bool exitCreating = true);
private:
    void buildMenus();
    void buildWorkspace();
    void rebuildTree();
    void updateTree();
    void syncTreeState();
    QStringList treeStructure() const;
    void selectMark(const QString& id, Qt::KeyboardModifiers modifiers);
    void deleteMarks();
    void renameMark();
    void log(const QString& message);
    Session session_;
    QString projectPath_;
    std::string selectionAnchor_;
    bool refreshing_ = false;
    bool treeRebuildPending_ = false;
    QStringList treeStructure_;
    QTreeWidget* tree_ = nullptr;
    PlotWidget *navigation_ = nullptr, *auxiliary_ = nullptr, *main_ = nullptr;
    QSplitter* graphs_ = nullptr;
    QComboBox *mainMode_ = nullptr, *auxMode_ = nullptr, *palette_ = nullptr, *stft_ = nullptr, *psd_ = nullptr;
    QDoubleSpinBox *dynamic_ = nullptr, *reference_ = nullptr;
    QCheckBox *grid_ = nullptr, *relative_ = nullptr, *colorScale_ = nullptr;
    QLabel *scope_ = nullptr, *fileDetails_ = nullptr, *markDetails_ = nullptr, *viewDetails_ = nullptr;
    QToolButton* resultToggle_ = nullptr;
    QPlainTextEdit* results_ = nullptr;
    QAction *removeAction_ = nullptr, *saveAction_ = nullptr, *deleteAction_ = nullptr, *backAction_ = nullptr, *forwardAction_ = nullptr;
};
} // namespace signalstudio
