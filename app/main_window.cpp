#include "app/main_window.h"
#include "infrastructure/project_store.h"
#include "ui/charts/plot_widget.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <array>

namespace signalstudio {
namespace {
QString q(const std::string& text) { return QString::fromStdString(text); }
QPushButton* button(const QString& text, QLayout* layout) {
    auto* b=new QPushButton(text);layout->addWidget(b);return b;
}
QLabel* valueLabel() { auto* l=new QLabel; l->setTextFormat(Qt::PlainText);l->setWordWrap(true);l->setTextInteractionFlags(Qt::TextSelectableByMouse);return l; }
QWidget* chartPanel(const QString& title, PlotWidget* plot, QComboBox* mode=nullptr, QComboBox* palette=nullptr) {
    auto* panel=new QWidget; auto* layout=new QVBoxLayout(panel);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
    auto* header=new QWidget;header->setObjectName("chartHeader");header->setFixedHeight(31);
    auto* row=new QHBoxLayout(header);row->setContentsMargins(10,1,9,1);
    row->addWidget(new QLabel(title));if(mode) row->addWidget(mode);row->addStretch();if(palette)row->addWidget(palette);
    layout->addWidget(header);layout->addWidget(plot,1);return panel;
}
QFormLayout* section(QVBoxLayout* target,const QString& name,bool open=true) {
    auto* toggle=new QToolButton;toggle->setText(name);toggle->setCheckable(true);toggle->setChecked(open);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);toggle->setArrowType(open?Qt::DownArrow:Qt::RightArrow);toggle->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
    target->addWidget(toggle);auto* content=new QWidget;auto* form=new QFormLayout(content);form->setContentsMargins(10,9,10,9);form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    target->addWidget(content);content->setVisible(open);
    QObject::connect(toggle,&QToolButton::toggled,content,[content,toggle](bool state){content->setVisible(state);toggle->setArrowType(state?Qt::DownArrow:Qt::RightArrow);});
    return form;
}
}

MainWindow::MainWindow(QWidget* parent):QMainWindow(parent) {
    setObjectName("SignalStudioWindow");setWindowTitle("Signal Studio · A1.4.3 原生骨架");resize(1600,960);setMinimumSize(1050,650);
    setStyleSheet(R"(
        QWidget { background:#101d2f; color:#d5e5f4; font-family:'Microsoft YaHei UI'; font-size:12px; }
        QMainWindow,QMenuBar,QStatusBar { background:#0b1726; }
        QMenuBar::item { padding:5px 13px; } QMenuBar::item:selected { background:#235476; }
        QMenu { background:#1b2b41; border:1px solid #47627b; } QMenu::item { padding:8px 22px; } QMenu::item:selected { background:#275475; }
        QPushButton,QToolButton,QComboBox,QDoubleSpinBox { border:1px solid #324b65; border-radius:3px; padding:4px 7px; background:#14263b; }
        QPushButton:hover,QToolButton:hover { background:#264c68; } QPushButton:disabled,QComboBox:disabled { color:#577086; }
        QToolButton:checked { background:#1c4b6b; } QToolButton { text-align:left; }
        QComboBox QAbstractItemView { selection-background-color:#235476; }
        QTreeWidget { border:0; background:#101d2f; outline:0; } QTreeWidget::item { padding:6px 2px; }
        QTreeWidget::item:selected { background:#214c70; color:#ffe3a0; }
        QSplitter::handle { background:#22384e; } QSplitter::handle:hover { background:#40b5ef; }
        QScrollArea { border:0; } QPlainTextEdit { border:1px solid #30465d; background:#0d1828; font-family:Consolas; }
        #chartHeader { background:#182c42; border:1px solid #294058; }
        #scopeBar { background:#142438; border-bottom:1px solid #294058; padding:6px; }
        QLabel#panelTitle { background:#192c42; padding:9px; font-weight:600; }
        QCheckBox { padding:4px; } QStatusBar { border-top:1px solid #294058; color:#8fa7bf; }
    )");
    buildMenus();buildWorkspace();
    connect(qApp,&QApplication::applicationStateChanged,this,[this](Qt::ApplicationState state){if(state!=Qt::ApplicationActive)cancelInteractions();});
    refresh();log("独立原生骨架已启动；图谱为演示数据，未执行真实 IQ / DSP");
}

void MainWindow::buildMenus() {
    auto* file=menuBar()->addMenu("文件(&F)");
    auto* action=file->addAction("新建空工程",this,[this]{cancelInteractions();session_.newProject();projectPath_.clear();selectionAnchor_.clear();refresh();log("已新建空工程");});
    action->setObjectName("newProjectAction");action->setShortcut(QKeySequence::New);
    action=file->addAction("添加演示 IQ 文件",this,[this]{cancelInteractions();session_.addDemoFile();selectionAnchor_.clear();refresh();log("已添加演示文件");});action->setObjectName("addDemoAction");
    action=file->addAction("打开工程…",this,[this]{const auto path=QFileDialog::getOpenFileName(this,"打开原生工程",{},"Signal Studio 工程 (*.json)");if(!path.isEmpty())openProject(path);});action->setObjectName("openProjectAction");action->setShortcut(QKeySequence::Open);
    saveAction_=file->addAction("保存工程…",this,[this]{auto path=QFileDialog::getSaveFileName(this,"保存原生工程",projectPath_.isEmpty()?"signal-studio-project.json":projectPath_,"Signal Studio 工程 (*.json)");if(!path.isEmpty())saveProject(path);});saveAction_->setObjectName("saveProjectAction");saveAction_->setShortcut(QKeySequence::Save);
    removeAction_=file->addAction("移除当前文件",this,[this]{cancelInteractions();session_.removeActiveFile();selectionAnchor_.clear();refresh();log("已从工程移除当前文件");});removeAction_->setObjectName("removeFileAction");
    file->addSeparator();file->addAction("退出",this,&QWidget::close);
    auto* edit=menuBar()->addMenu("编辑(&E)");
    deleteAction_=edit->addAction("删除所选标记",this,&MainWindow::deleteMarks);deleteAction_->setObjectName("deleteMarksAction");deleteAction_->setShortcut(QKeySequence::Delete);
    edit->addAction("重命名活动标记…",this,&MainWindow::renameMark);
    auto* view=menuBar()->addMenu("视图(&V)");
    backAction_=view->addAction("视图后退",this,[this]{cancelInteractions();session_.back();refresh();});backAction_->setShortcut(QKeySequence(Qt::ALT|Qt::Key_Left));backAction_->setObjectName("backAction");
    forwardAction_=view->addAction("视图前进",this,[this]{cancelInteractions();session_.forward();refresh();});forwardAction_->setShortcut(QKeySequence(Qt::ALT|Qt::Key_Right));forwardAction_->setObjectName("forwardAction");
    view->addAction("完整文件",this,[this]{cancelInteractions();if(auto* f=session_.activeFile())session_.setView(fullRange(f->metadata));refresh();});
    view->addAction("恢复默认视图",this,[this]{cancelInteractions();session_.resetView();refresh();});
    auto* help=menuBar()->addMenu("帮助(&H)");help->addAction("关于当前骨架",this,[this]{QMessageBox::information(this,"Signal Studio","A1.4.3 原生基础骨架\nC++20 / Qt Widgets\n当前使用演示数据；真实 IQ、FFT/STFT、DDC 尚待接入。\n工程格式 signal-studio-native-project / v1");});
}

void MainWindow::buildWorkspace() {
    auto* horizontal=new QSplitter(Qt::Horizontal);horizontal->setChildrenCollapsible(false);horizontal->setHandleWidth(4);setCentralWidget(horizontal);
    auto* resources=new QWidget;resources->setMinimumWidth(205);auto* left=new QVBoxLayout(resources);left->setContentsMargins(5,0,5,6);left->setSpacing(6);
    auto* title=new QLabel("工程管理");title->setObjectName("panelTitle");left->addWidget(title);
    auto* add=button("＋ 添加演示 IQ 文件",left);connect(add,&QPushButton::clicked,this,[this]{findChild<QAction*>("addDemoAction")->trigger();});
    tree_=new QTreeWidget;tree_->setObjectName("projectTree");tree_->setHeaderHidden(true);tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);left->addWidget(tree_,1);
    auto* all=button("全选当前文件标记",left);connect(all,&QPushButton::clicked,this,[this]{cancelInteractions(false);if(auto* f=session_.activeFile()){std::vector<std::string> ids;for(const auto& m:f->marks)ids.push_back(m.id);session_.selectMarks(std::move(ids));refresh();}});
    auto* del=button("删除所选",left);connect(del,&QPushButton::clicked,this,&MainWindow::deleteMarks);
    auto* guide=new QLabel("单击选择 · Ctrl 多选 · Shift 连选\n双击标记定位\n主图右键开启持续选择\n选中框可移动和调整边角");guide->setWordWrap(true);left->addWidget(guide);
    horizontal->addWidget(resources);
    auto* workspace=new QWidget;workspace->setMinimumWidth(490);auto* center=new QVBoxLayout(workspace);center->setContentsMargins(3,0,3,2);center->setSpacing(4);
    scope_=new QLabel;scope_->setObjectName("scopeBar");scope_->setTextFormat(Qt::PlainText);center->addWidget(scope_);
    graphs_=new QSplitter(Qt::Vertical);graphs_->setObjectName("graphSplitter");graphs_->setChildrenCollapsible(false);graphs_->setHandleWidth(5);
    navigation_=new PlotWidget(session_,PlotWidget::Kind::Navigation);navigation_->setObjectName("navigationPlot");
    auxiliary_=new PlotWidget(session_,PlotWidget::Kind::Auxiliary);auxiliary_->setObjectName("auxPlot");
    main_=new PlotWidget(session_,PlotWidget::Kind::Main);main_->setObjectName("mainPlot");
    auxMode_=new QComboBox;auxMode_->setObjectName("modeAux");auxMode_->addItems({"时域波形","功率谱 PSD"});
    mainMode_=new QComboBox;mainMode_->setObjectName("modeMain");mainMode_->addItems({"时频图","瀑布图"});
    palette_=new QComboBox;palette_->setObjectName("palette");palette_->addItems({"Turbo","Viridis","Gray"});
    graphs_->addWidget(chartPanel("全局时间导航",navigation_));graphs_->addWidget(chartPanel("辅助分析",auxiliary_,auxMode_));graphs_->addWidget(chartPanel("宽带图谱",main_,mainMode_,palette_));
    graphs_->setStretchFactor(0,0);graphs_->setStretchFactor(1,0);graphs_->setStretchFactor(2,1);graphs_->setSizes({82,205,520});center->addWidget(graphs_,1);
    auto* actions=new QHBoxLayout;auto* locate=button("定位活动标记",actions);connect(locate,&QPushButton::clicked,this,[this]{cancelInteractions();if(auto* f=session_.activeFile())session_.focusMark(f->activeMarkId);refresh();});
    auto* channel=button("基于标记创建演示通道 →",actions);connect(channel,&QPushButton::clicked,this,[this]{cancelInteractions();if(session_.createChannelFromActiveMark()){refresh();log("演示通道配置已创建，未执行 DDC；后续编辑标记不更新通道配置");}});actions->addStretch();center->addLayout(actions);
    resultToggle_=new QToolButton;resultToggle_->setObjectName("resultsToggle");resultToggle_->setText("当前文件结果 / 日志  ▸");resultToggle_->setCheckable(true);center->addWidget(resultToggle_);
    results_=new QPlainTextEdit;results_->setObjectName("resultsPanel");results_->setReadOnly(true);results_->setMaximumBlockCount(300);results_->setFixedHeight(115);results_->hide();center->addWidget(results_);
    connect(resultToggle_,&QToolButton::toggled,results_,&QWidget::setVisible);horizontal->addWidget(workspace);
    auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);scroll->setMinimumWidth(230);auto* props=new QWidget;auto* right=new QVBoxLayout(props);right->setContentsMargins(0,0,0,0);right->setSpacing(0);
    title=new QLabel("当前文件 / 分析参数");title->setObjectName("panelTitle");right->addWidget(title);
    auto* form=section(right,"当前信号标记");markDetails_=valueLabel();form->addRow(markDetails_);
    auto* rename=new QPushButton("重命名…");form->addRow(rename);connect(rename,&QPushButton::clicked,this,&MainWindow::renameMark);
    form=section(right,"活动文件");fileDetails_=valueLabel();form->addRow(fileDetails_);
    form=section(right,"当前视图");viewDetails_=valueLabel();form->addRow(viewDetails_);
    form=section(right,"显示设置",false);
    dynamic_=new QDoubleSpinBox;dynamic_->setRange(10,160);dynamic_->setSuffix(" dB");form->addRow("动态范围",dynamic_);
    reference_=new QDoubleSpinBox;reference_->setRange(-150,50);reference_->setSuffix(" dB");form->addRow("参考电平",reference_);
    relative_=new QCheckBox("使用相对频率");grid_=new QCheckBox("显示网格");colorScale_=new QCheckBox("显示色阶条");form->addRow(relative_);form->addRow(grid_);form->addRow(colorScale_);
    form=section(right,"PSD / 时频参数",false);
    psd_=new QComboBox;psd_->addItems({"1024","2048","4096","8192","16384"});stft_=new QComboBox;stft_->addItems({"256","512","1024","2048","4096","8192"});
    form->addRow("PSD FFT",psd_);form->addRow("STFT FFT",stft_);
    auto* note=new QLabel("参数保存在当前文件中。当前图谱为确定性演示数据，FFT 参数仅影响缩放下限；尚未执行分析。");note->setWordWrap(true);form->addRow(note);right->addStretch();scroll->setWidget(props);horizontal->addWidget(scroll);
    horizontal->setStretchFactor(0,0);horizontal->setStretchFactor(1,1);horizontal->setStretchFactor(2,0);horizontal->setSizes({245,1065,280});
    connect(tree_,&QTreeWidget::itemClicked,this,[this](QTreeWidgetItem* item,int){
        if(refreshing_)return;
        if(item->data(0,Qt::UserRole).toString()=="file") {
            // Copy identity now; item belongs to the mouse event currently being dispatched.
            const auto id=item->data(0,Qt::UserRole+1).toString().toStdString();
            QTimer::singleShot(0,this,[this,id]{cancelInteractions();if(session_.activateFile(id)){selectionAnchor_.clear();refresh();}});
        }
    });
    connect(tree_,&QTreeWidget::itemSelectionChanged,this,[this]{
        if(refreshing_)return;auto* f=session_.activeFile();if(!f)return;
        std::vector<std::string> ids;for(auto* item:tree_->selectedItems())if(item->data(0,Qt::UserRole).toString()=="mark")ids.push_back(item->data(0,Qt::UserRole+1).toString().toStdString());
        if(auto* current=tree_->currentItem();current&&current->data(0,Qt::UserRole).toString()=="mark")session_.selectMarks(std::move(ids),current->data(0,Qt::UserRole+1).toString().toStdString());
        else return;
        refresh();
    });
    connect(tree_,&QTreeWidget::itemDoubleClicked,this,[this](QTreeWidgetItem* item,int){if(item->data(0,Qt::UserRole).toString()=="mark"){cancelInteractions();session_.focusMark(item->data(0,Qt::UserRole+1).toString().toStdString());refresh();}});
    for(auto* plot:{navigation_,auxiliary_,main_}) {connect(plot,&PlotWidget::stateChanged,this,&MainWindow::refresh);connect(plot,&PlotWidget::statusMessage,statusBar(),[this](const QString& message){statusBar()->showMessage(message);});connect(plot,&PlotWidget::markSelectionRequested,this,&MainWindow::selectMark);}
    auto change=[this]{
        if(refreshing_)return;
        auto* f=session_.activeFile();if(!f)return;
        const auto fileId=f->metadata.id;
        auto display=f->display;
        // Cancellation can commit a wheel group and synchronously refresh the controls.
        display.mainMode=static_cast<MainMode>(mainMode_->currentIndex());display.auxiliaryMode=static_cast<AuxiliaryMode>(auxMode_->currentIndex());display.palette=static_cast<Palette>(palette_->currentIndex());
        display.dynamicRangeDb=dynamic_->value();display.referenceLevelDb=reference_->value();display.absoluteFrequency=!relative_->isChecked();display.grid=grid_->isChecked();display.colorScale=colorScale_->isChecked();
        display.psdSize=psd_->currentText().toInt();display.stftSize=stft_->currentText().toInt();
        cancelInteractions();
        f=session_.activeFile();if(!f||f->metadata.id!=fileId)return;
        f->display=display;session_.setView(f->view,false);refresh();
    };
    for(auto* combo:{mainMode_,auxMode_,palette_,psd_,stft_})connect(combo,&QComboBox::currentIndexChanged,this,change);
    for(auto* spin:{dynamic_,reference_})connect(spin,&QDoubleSpinBox::valueChanged,this,change);
    for(auto* check:{relative_,grid_,colorScale_})connect(check,&QCheckBox::toggled,this,change);
    auto* badge=new QLabel("演示数据 · 未运行 DSP");statusBar()->addPermanentWidget(badge);
}

void MainWindow::rebuildTree() {
    QSignalBlocker block(tree_);tree_->clear();auto* root=new QTreeWidgetItem(tree_,{q(session_.project().name)});root->setExpanded(true);root->setFlags(root->flags()&~Qt::ItemIsSelectable);
    for(const auto& f:session_.project().files) {
        auto* file=new QTreeWidgetItem(root,{q(f.metadata.name)});file->setData(0,Qt::UserRole,"file");file->setData(0,Qt::UserRole+1,q(f.metadata.id));file->setToolTip(0,QString("%1 MS/s · %2 MHz · %3 s · 演示文件").arg(f.metadata.sampleRateHz/1e6).arg(f.metadata.centerFrequencyHz/1e6).arg(static_cast<double>(f.metadata.sampleCount)/f.metadata.sampleRateHz));
        const bool active=f.metadata.id==session_.project().activeFileId;file->setExpanded(active);
        if(active) {
            for(const auto& m:f.marks) {auto* item=new QTreeWidgetItem(file,{"▱ "+q(m.name)});item->setData(0,Qt::UserRole,"mark");item->setData(0,Qt::UserRole+1,q(m.id));if(std::find(f.selectedMarkIds.begin(),f.selectedMarkIds.end(),m.id)!=f.selectedMarkIds.end())item->setSelected(true);if(m.id==f.activeMarkId)tree_->setCurrentItem(item,0,QItemSelectionModel::NoUpdate);}
            for(const auto& c:f.channels) {auto* item=new QTreeWidgetItem(file,{"◇ "+q(c.name)});item->setFlags(item->flags()&~Qt::ItemIsSelectable);item->setToolTip(0,QString("%1 MHz / %2 kHz · 演示通道").arg(c.centerFrequencyHz/1e6).arg(c.bandwidthHz/1e3));}
            if(f.activeMarkId.empty())tree_->setCurrentItem(file,0,QItemSelectionModel::NoUpdate);
            file->setForeground(0,QColor("#84d4f5"));
        }
    }
    treeStructure_=treeStructure();
}

QStringList MainWindow::treeStructure() const {
    QStringList structure{q(session_.project().activeFileId)};
    for(const auto& file:session_.project().files) {
        structure.append("file:"+q(file.metadata.id));
        if(file.metadata.id==session_.project().activeFileId) {
            for(const auto& mark:file.marks)structure.append("mark:"+q(mark.id));
            for(const auto& channel:file.channels)structure.append("channel:"+q(channel.id));
        }
    }
    return structure;
}

void MainWindow::updateTree() {
    if(tree_->topLevelItemCount()==1&&treeStructure_==treeStructure()) {
        syncTreeState();
        return;
    }
    if(treeRebuildPending_)return;
    treeRebuildPending_=true;
    // Structural mutations wait until tree mouse/keyboard signals have finished.
    QTimer::singleShot(0,this,[this]{treeRebuildPending_=false;rebuildTree();});
}

void MainWindow::syncTreeState() {
    QSignalBlocker block(tree_);
    auto* root=tree_->topLevelItem(0);root->setText(0,q(session_.project().name));
    for(std::size_t index=0;index<session_.project().files.size();++index) {
        const auto& file=session_.project().files[index];auto* node=root->child(static_cast<int>(index));
        node->setText(0,q(file.metadata.name));
        if(file.metadata.id!=session_.project().activeFileId)continue;
        for(std::size_t markIndex=0;markIndex<file.marks.size();++markIndex) {
            const auto& mark=file.marks[markIndex];auto* item=node->child(static_cast<int>(markIndex));
            item->setText(0,"▱ "+q(mark.name));
            const bool selected=std::find(file.selectedMarkIds.begin(),file.selectedMarkIds.end(),mark.id)!=file.selectedMarkIds.end();
            if(item->isSelected()!=selected)item->setSelected(selected);
        }
        for(std::size_t channelIndex=0;channelIndex<file.channels.size();++channelIndex)
            node->child(static_cast<int>(file.marks.size()+channelIndex))->setText(0,"◇ "+q(file.channels[channelIndex].name));
        // Do not reset Qt's current index or Shift anchor during ordinary state refreshes.
    }
}

void MainWindow::refresh() {
    if(refreshing_)return;refreshing_=true;
    const auto* f=session_.activeFile();updateTree();
    scope_->setText(f?QString("宽带时频研判  ·  %1  |  Fs %2 MS/s  |  fc %3 MHz").arg(q(f->metadata.name)).arg(f->metadata.sampleRateHz/1e6).arg(f->metadata.centerFrequencyHz/1e6):"宽带时频研判 · 空工程");
    for(auto* w:std::array<QWidget*,10>{mainMode_,auxMode_,palette_,dynamic_,reference_,relative_,grid_,colorScale_,psd_,stft_})w->setEnabled(f!=nullptr);
    removeAction_->setEnabled(f!=nullptr);deleteAction_->setEnabled(f&&!f->selectedMarkIds.empty());backAction_->setEnabled(session_.canBack());forwardAction_->setEnabled(session_.canForward());
    if(f) {
        mainMode_->setCurrentIndex(static_cast<int>(f->display.mainMode));auxMode_->setCurrentIndex(static_cast<int>(f->display.auxiliaryMode));palette_->setCurrentIndex(static_cast<int>(f->display.palette));
        dynamic_->setValue(f->display.dynamicRangeDb);reference_->setValue(f->display.referenceLevelDb);relative_->setChecked(!f->display.absoluteFrequency);grid_->setChecked(f->display.grid);colorScale_->setChecked(f->display.colorScale);
        psd_->setCurrentText(QString::number(f->display.psdSize));stft_->setCurrentText(QString::number(f->display.stftSize));
        fileDetails_->setText(QString("%1\n采样率  %2 MS/s\n中心频率  %3 MHz\n总时长  %4 s\n类型  演示元数据").arg(q(f->metadata.name)).arg(f->metadata.sampleRateHz/1e6).arg(f->metadata.centerFrequencyHz/1e6).arg(static_cast<double>(f->metadata.sampleCount)/f->metadata.sampleRateHz));
        viewDetails_->setText(QString("时间  %1–%2 s\n频带  %3–%4 MHz\n最小时间窗  %5 us\n最小频带  %6 kHz").arg(static_cast<double>(f->view.time.begin)/f->metadata.sampleRateHz,0,'g',10).arg(static_cast<double>(f->view.time.end)/f->metadata.sampleRateHz,0,'g',10).arg(f->view.frequency.lowerHz/1e6,0,'g',10).arg(f->view.frequency.upperHz/1e6,0,'g',10).arg(f->display.stftSize/f->metadata.sampleRateHz*1e6).arg(f->metadata.sampleRateHz/f->display.stftSize/1e3));
        const auto* mark=findMark(*f,f->activeMarkId);
        markDetails_->setText(mark?QString("%1\n时间  %2–%3 s\n频带  %4–%5 MHz\n已选 %6 个标记").arg(q(mark->name)).arg(static_cast<double>(mark->range.time.begin)/f->metadata.sampleRateHz,0,'g',10).arg(static_cast<double>(mark->range.time.end)/f->metadata.sampleRateHz,0,'g',10).arg(mark->range.frequency.lowerHz/1e6,0,'g',10).arg(mark->range.frequency.upperHz/1e6,0,'g',10).arg(f->selectedMarkIds.size()):"尚未选择标记\n主图右键 → 选择信号（持续模式）");
    } else {fileDetails_->setText("无活动文件");markDetails_->setText("无标记");viewDetails_->setText("添加演示文件后开始研判");}
    for(auto* plot:{navigation_,auxiliary_,main_})plot->syncState();refreshing_=false;
}

void MainWindow::selectMark(const QString& id,Qt::KeyboardModifiers modifiers) {
    auto* f=session_.activeFile();if(!f)return;const auto key=id.toStdString();auto ids=f->selectedMarkIds;
    if(modifiers&Qt::ShiftModifier) {
        auto first=std::find_if(f->marks.begin(),f->marks.end(),[this](const Mark& m){return m.id==selectionAnchor_;});auto last=std::find_if(f->marks.begin(),f->marks.end(),[key](const Mark& m){return m.id==key;});
        if(first==f->marks.end())first=last;if(first!=f->marks.end()&&last!=f->marks.end()){if(!(modifiers&Qt::ControlModifier))ids.clear();if(first>last)std::swap(first,last);for(auto it=first;it<=last;++it)ids.push_back(it->id);}
    } else if(modifiers&Qt::ControlModifier) {auto it=std::find(ids.begin(),ids.end(),key);if(it==ids.end())ids.push_back(key);else ids.erase(it);selectionAnchor_=key;}
    else {if(std::find(ids.begin(),ids.end(),key)==ids.end())ids={key};selectionAnchor_=key;}
    session_.selectMarks(std::move(ids),key);refresh();
}
void MainWindow::cancelInteractions(bool exitCreating) {if(main_)main_->cancelGesture(exitCreating);if(auxiliary_)auxiliary_->cancelGesture();if(navigation_)navigation_->cancelGesture();}
void MainWindow::deleteMarks() {cancelInteractions();const auto count=session_.deleteSelectedMarks();refresh();if(count.marks)log(QString("已删除 %1 个标记，连带移除 %2 个演示通道").arg(count.marks).arg(count.channels));}
void MainWindow::renameMark() {
    cancelInteractions();auto* f=session_.activeFile();if(!f)return;auto* mark=findMark(*f,f->activeMarkId);if(!mark)return;
    bool ok=false;const auto name=QInputDialog::getText(this,"重命名标记","名称（最多 80 个字符）",QLineEdit::Normal,q(mark->name),&ok).trimmed().left(80);
    if(ok&&!name.isEmpty()){mark->name=name.toStdString();refresh();}
}
bool MainWindow::openProject(const QString& path) {
    cancelInteractions();Project candidate;QString error;
    if(!ProjectStore::load(path,candidate,error)){QMessageBox::critical(this,"打开失败",error);return false;}
    session_.replaceProject(std::move(candidate));projectPath_=path;selectionAnchor_.clear();refresh();log("已打开原生工程："+path);return true;
}
bool MainWindow::saveProject(const QString& path) {
    cancelInteractions();QString error;if(!ProjectStore::save(path,session_.project(),error)){QMessageBox::critical(this,"保存失败",error);return false;}
    projectPath_=path;log("工程已保存："+path);return true;
}
void MainWindow::log(const QString& message) {results_->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss")+"  "+message);statusBar()->showMessage(message,6000);}
} // namespace signalstudio
