#pragma once
#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QResizeEvent>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <vector>
#include "ui/settings_popover.h"

namespace signalstudio {
// Each field has one editor. Only responsive transitions change its host;
// normal domain refreshes never recreate controls or their drafts.
class ChartControlStrip final : public QWidget {
 public:
  ChartControlStrip(SettingsPopoverHost* popovers, const QString& name,
                    QWidget* parent = nullptr) : QWidget(parent), popovers_(popovers) {
    setObjectName(name); setFixedHeight(28); setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    row_ = new QHBoxLayout(this); row_->setContentsMargins(0,1,0,1);
    row_->setSpacing(5); row_->setSizeConstraint(QLayout::SetNoConstraint);
    row_->addStretch();
    more_ = new QToolButton(this); more_->setObjectName(name+"Overflow");
    more_->setText(QStringLiteral("更多设置")); more_->setToolTip(more_->text());
    more_->setAccessibleName(more_->text()); more_->setFixedHeight(24);
    more_->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed);
    overflowContent_ = new QWidget;
    overflowLayout_ = new QVBoxLayout(overflowContent_);
    overflowLayout_->setContentsMargins(2,2,2,2); overflowLayout_->setSpacing(7);
    frame_ = popovers_->add(more_,overflowContent_,name+"OverflowPopover");
    row_->addWidget(more_); more_->hide();
    qApp->installEventFilter(this);
  }
  ~ChartControlStrip() override { if(qApp) qApp->removeEventFilter(this); }
  QSize minimumSizeHint() const override { return {0,28}; }
  QSize sizeHint() const override { return {480,28}; }

  void addControl(QWidget* control,const QString& title,int priority,
                  std::function<void()> = {}) {
    if(!control) return;
    for(const auto& item:items_) if(item.control==control) return;
    if(auto* old=dynamic_cast<ChartControlStrip*>(control->property("chartControlOwner").value<QObject*>()))
      if(old!=this) old->releaseControl(control);
    control->setParent(this); control->setFixedHeight(24);
    control->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed);
    if(control->toolTip().isEmpty()) control->setToolTip(title);
    if(control->accessibleName().isEmpty()) control->setAccessibleName(title);
    control->setProperty("chartControlOwner",QVariant::fromValue(static_cast<QObject*>(this)));
    auto* holder=new QWidget(overflowContent_);
    holder->setObjectName(control->objectName()+"OverflowRow");
    auto* layout=new QHBoxLayout(holder); layout->setContentsMargins(0,0,0,0);
    auto* caption=new QLabel(title,holder); caption->setMinimumWidth(85);
    layout->addWidget(caption); holder->hide(); overflowLayout_->addWidget(holder);
    row_->insertWidget(row_->count()-1,control);
    items_.push_back({control,holder,priority,true,false,false}); schedule();
  }
  void addPanel(QWidget* panel,const QString& title={}) {
    if(!panel) return;
    if(auto* old=dynamic_cast<ChartControlStrip*>(panel->property("chartPanelOwner").value<QObject*>())) {
      if(old==this) return;
      old->releasePanel(panel);
    }
    panel->setParent(overflowContent_);
    panel->setProperty("chartPanelOwner",QVariant::fromValue(static_cast<QObject*>(this)));
    QWidget* heading=nullptr;
    if(!title.isEmpty()) { auto* label=new QLabel(title,overflowContent_); label->setProperty("uiRole","hint"); heading=label; overflowLayout_->addWidget(label); }
    overflowLayout_->addWidget(panel); panel->show(); panels_.push_back({panel,heading}); schedule();
  }
  void requestVisible(QWidget* control,bool visible) {
    for(auto& item:items_) if(item.control==control&&item.requested!=visible) {item.requested=visible;schedule();}
  }
  QToolButton* overflowButton() const {return more_;}
  QFrame* popoverFrame() const {return frame_;}
  QWidget* overflowContent() const {return overflowContent_;}
  void setExternalHost(QWidget* control,QWidget* host) {
    for(auto& item:items_) if(item.control==control) {
      const bool external=host!=nullptr;
      if(item.external==external&&(!external||control->parentWidget()==host)) return;
      protect(control,[&]{row_->removeWidget(control);item.holder->layout()->removeWidget(control);control->setParent(host?host:this);if(host&&host->layout())host->layout()->addWidget(control);});
      item.external=external;item.overflow=false;schedule();return;
    }
  }
  void reflow() {
    pending_=false;
    if(!isVisible()||popovers_->activePopover()==frame_) return;
    std::vector<int> candidates; int required=0;bool advanced=false;
    for(const auto& panel:panels_){const bool shown=panel.widget&&!panel.widget->isHidden();advanced|=shown;if(panel.heading)panel.heading->setVisible(shown);}
    for(int i=0;i<static_cast<int>(items_.size());++i) {
      const auto& item=items_[i];
      if(!item.control||item.external||!wanted(item)) continue;
      required+=fieldWidth(item.control)+5;candidates.push_back(i);
    }
    std::stable_sort(candidates.begin(),candidates.end(),[this](int a,int b){return items_[a].priority<items_[b].priority;});
    const int moreWidth=std::max(72,more_->sizeHint().width())+5;
    const bool needMore=advanced||required>width();
    const int budget=std::max(0,width()-(needMore?moreWidth:0));
    std::vector<bool> overflow(items_.size(),false);
    for(int index:candidates) {if(required<=budget)break;overflow[index]=true;required-=fieldWidth(items_[index].control)+5;}
    auto* focused=QApplication::focusWidget();
    auto* popup=QApplication::activePopupWidget();
    for(int i=0;i<static_cast<int>(items_.size());++i) {
      auto& item=items_[i]; if(!item.control||item.external)continue;
      if(item.overflow!=overflow[i]&&((item.control->property("parameterDraft").toBool()&&focused&&(focused==item.control||item.control->isAncestorOf(focused))) || (popup&&item.control->isAncestorOf(popup))))return;
    }
    bool hasOverflow=false;
    for(int i=0;i<static_cast<int>(items_.size());++i) {
      auto& item=items_[i];if(!item.control)continue;
      if(item.external){item.holder->hide();continue;}
      const bool visible=wanted(item);
      if(item.overflow!=overflow[i]||(!overflow[i]&&row_->indexOf(item.control)<0)) {
        protect(item.control,[&]{
          row_->removeWidget(item.control); item.holder->layout()->removeWidget(item.control);
          item.control->setParent(overflow[i]?item.holder:this);
          if(overflow[i])item.holder->layout()->addWidget(item.control);
          else row_->insertWidget(inlinePosition(i),item.control);
        });
      }
      item.overflow=overflow[i]; item.holder->setVisible(visible&&item.overflow);
      item.control->setVisible(visible); hasOverflow|=visible&&item.overflow;
    }
    more_->setVisible(advanced||hasOverflow);
  }
 protected:
  void resizeEvent(QResizeEvent* event) override {QWidget::resizeEvent(event);schedule();}
  void showEvent(QShowEvent* event) override {QWidget::showEvent(event);schedule();}
  bool eventFilter(QObject* watched,QEvent* event) override {
    if(event->type()==QEvent::FocusOut||event->type()==QEvent::DynamicPropertyChange||
       (watched==frame_&&event->type()==QEvent::Hide)) {
      if(watched==frame_||std::any_of(items_.begin(),items_.end(),[watched](const Item& item){return item.control&&(watched==item.control||item.control->isAncestorOf(qobject_cast<QWidget*>(watched)));}))schedule();
    }
    return QWidget::eventFilter(watched,event);
  }
 private:
  struct Item {QPointer<QWidget> control;QWidget* holder;int priority;bool requested,overflow,external;};
  struct Panel {QPointer<QWidget> widget;QWidget* heading;};
  bool wanted(const Item& item) const {return item.requested&&(!item.control->property("chartSettingRelevant").isValid()||item.control->property("chartSettingRelevant").toBool());}
  static int fieldWidth(QWidget* field) {return std::max({field->minimumWidth(),field->minimumSizeHint().width(),field->sizeHint().width()});}
  int inlinePosition(int index) const {int position=1;for(int i=0;i<index;++i)if(items_[i].control&&!items_[i].external&&!items_[i].overflow&&row_->indexOf(items_[i].control)>=0)++position;return position;}
  template<class F> void protect(QWidget* control,F action) {setProperty("parameterLayerHiding",true);overflowContent_->setProperty("parameterLayerHiding",true);control->setProperty("parameterLayerHiding",true);action();control->setProperty("parameterLayerHiding",false);overflowContent_->setProperty("parameterLayerHiding",false);setProperty("parameterLayerHiding",false);}
  void releaseControl(QWidget* control) {
    for(auto it=items_.begin();it!=items_.end();++it)if(it->control==control){protect(control,[&]{row_->removeWidget(control);it->holder->layout()->removeWidget(control);control->setParent(window());control->hide();});delete it->holder;items_.erase(it);schedule();return;}
  }
  void releasePanel(QWidget* panel) {for(auto it=panels_.begin();it!=panels_.end();++it)if(it->widget==panel){overflowLayout_->removeWidget(panel);if(it->heading)delete it->heading;panel->setParent(window());panel->hide();panels_.erase(it);schedule();return;}}
  void schedule(){if(pending_)return;pending_=true;QTimer::singleShot(0,this,[this]{reflow();});}
  SettingsPopoverHost* popovers_;QHBoxLayout* row_;QToolButton* more_;QWidget* overflowContent_;QVBoxLayout* overflowLayout_;QFrame* frame_;
  std::vector<Item> items_;std::vector<Panel> panels_;bool pending_=false;
};
}
