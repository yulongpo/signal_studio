#pragma once
#include <QApplication>
#include <QAbstractButton>
#include <QAbstractItemView>
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
    frame_->installEventFilter(this);
  }
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
    if(auto* combo=qobject_cast<QComboBox*>(control)) {
      combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
      combo->view()->setTextElideMode(Qt::ElideNone);
    }
    watchControl(control,true);
    const int index=static_cast<int>(std::find_if(items_.begin(),items_.end(),
        [priority](const Item& item){return item.priority<priority;})-items_.begin());
    const int position=inlinePosition(index);
    auto* holder=new QWidget(overflowContent_);
    holder->setObjectName(control->objectName()+"OverflowRow");
    auto* layout=new QHBoxLayout(holder); layout->setContentsMargins(0,0,0,0);
    QLabel* caption=nullptr;
    const auto* button=qobject_cast<QAbstractButton*>(control);
    if(!button||button->text().trimmed().isEmpty()) {
      caption=new QLabel(title,this);caption->setObjectName(control->objectName()+"Label");
      caption->setProperty("uiRole","settingLabel");caption->setBuddy(control);
      caption->setFixedWidth(caption->fontMetrics().horizontalAdvance(title)+2);
      row_->insertWidget(position,caption);
    }
    holder->hide(); overflowLayout_->insertWidget(index,holder);
    row_->insertWidget(position+(caption?1:0),control);
    items_.insert(items_.begin()+index,{control,caption,holder,priority,true,false,false}); schedule();
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
  void setAuxiliaryStatus(QWidget* status) {auxiliaryStatus_=status;schedule();}
  void setExternalHost(QWidget* control,QWidget* host) {
    for(auto& item:items_) if(item.control==control) {
      const bool external=host!=nullptr;
      if(item.external==external&&(!external||control->parentWidget()==host)) return;
      protect(control,[&]{
        removeField(item);auto* parent=host?host:this;
        if(item.caption){item.caption->setParent(parent);if(host&&host->layout())host->layout()->addWidget(item.caption);}
        control->setParent(parent);if(host&&host->layout())host->layout()->addWidget(control);
      });
      item.external=external;item.overflow=false;schedule();return;
    }
  }
  void reflow() {
    pending_=false;
    if(!isVisible()) return;
    if(popovers_->activePopover()==frame_) {
      syncVisibility();
      return;
    }
    std::vector<int> candidates; int required=0;bool advanced=false;
    for(const auto& panel:panels_){const bool shown=panel.widget&&!panel.widget->isHidden();advanced|=shown;if(panel.heading)panel.heading->setVisible(shown);}
    for(int i=0;i<static_cast<int>(items_.size());++i) {
      const auto& item=items_[i];
      if(!item.control||item.external||!wanted(item)) continue;
      if(item.priority<0){advanced=true;continue;}
      if(item.caption&&!item.overflow)item.caption->setFixedWidth(captionWidth(item));
      fitEditor(item.control);
      required+=itemWidth(item)+5;candidates.push_back(i);
    }
    std::stable_sort(candidates.begin(),candidates.end(),[this](int a,int b){return items_[a].priority<items_[b].priority;});
    const int moreWidth=std::max(72,more_->sizeHint().width())+5;
    if(auxiliaryStatus_) {
      const int spacing=parentWidget()->layout()->spacing();
      const bool shown=!auxiliaryStatus_->isHidden();
      const int room=width()+(shown?auxiliaryStatus_->width()+spacing:0);
      const bool show=room>=required+(advanced?moreWidth:0)+auxiliaryStatus_->sizeHint().width()+spacing;
      if(shown!=show){auxiliaryStatus_->setVisible(show);parentWidget()->layout()->activate();}
    }
    const bool needMore=advanced||required>width();
    const int budget=std::max(0,width()-(needMore?moreWidth:0));
    std::vector<bool> overflow(items_.size(),false);
    for(int i=0;i<static_cast<int>(items_.size());++i)
      overflow[i]=items_[i].priority<0;
    for(int index:candidates) {if(required<=budget)break;overflow[index]=true;required-=itemWidth(items_[index])+5;}
    auto* focused=QApplication::focusWidget();
    auto* popup=QApplication::activePopupWidget();
    bool editing=false,rehosting=false;
    for(int i=0;i<static_cast<int>(items_.size());++i) {
      auto& item=items_[i]; if(!item.control||item.external)continue;
      rehosting|=item.overflow!=overflow[i];
      editing|=((focused&&(focused==item.control||item.control->isAncestorOf(focused))&&
          (qobject_cast<QComboBox*>(item.control.data())||item.control->property("parameterDraft").toBool())) ||
          (popup&&item.control->isAncestorOf(popup)));
    }
    if(editing&&rehosting){syncVisibility();return;}
    bool hasOverflow=false;
    for(int i=0;i<static_cast<int>(items_.size());++i) {
      auto& item=items_[i];if(!item.control)continue;
      if(item.external){item.holder->hide();continue;}
      const bool visible=wanted(item);
      if(item.overflow!=overflow[i]||(!overflow[i]&&row_->indexOf(item.control)<0)) {
        protect(item.control,[&]{
          removeField(item);
          if(item.caption){
            item.caption->setParent(overflow[i]?item.holder:this);
            item.caption->setFixedWidth(overflow[i]?std::max(85,captionWidth(item)):captionWidth(item));
          }
          item.control->setParent(overflow[i]?item.holder:this);
          if(overflow[i]){
            if(item.caption)item.holder->layout()->addWidget(item.caption);
            item.holder->layout()->addWidget(item.control);
          } else {
            const int position=inlinePosition(i);
            if(item.caption)row_->insertWidget(position,item.caption);
            row_->insertWidget(position+(item.caption?1:0),item.control);
          }
        });
      }
      item.overflow=overflow[i]; item.holder->setVisible(visible&&item.overflow);
      if(item.caption)item.caption->setVisible(visible);
      item.control->setVisible(visible); hasOverflow|=visible&&item.overflow;
    }
    more_->setVisible(advanced||hasOverflow);
  }
 protected:
  void resizeEvent(QResizeEvent* event) override {QWidget::resizeEvent(event);schedule();}
  void showEvent(QShowEvent* event) override {QWidget::showEvent(event);schedule();}
  bool eventFilter(QObject* watched,QEvent* event) override {
    if(event->type()==QEvent::FocusIn||event->type()==QEvent::FocusOut||
       event->type()==QEvent::DynamicPropertyChange||event->type()==QEvent::Hide) schedule();
    return QWidget::eventFilter(watched,event);
  }
 private:
  struct Item {QPointer<QWidget> control;QLabel* caption;QWidget* holder;int priority;bool requested,overflow,external;};
  struct Panel {QPointer<QWidget> widget;QWidget* heading;};
  void watchControl(QWidget* control,bool watch) {
    const auto apply=[this,watch](QObject* object){if(watch)object->installEventFilter(this);else object->removeEventFilter(this);};
    apply(control);for(auto* child:control->findChildren<QWidget*>())apply(child);
  }
  bool wanted(const Item& item) const {return item.requested&&(!item.control->property("chartSettingRelevant").isValid()||item.control->property("chartSettingRelevant").toBool());}
  void syncVisibility() {
    for(auto& item:items_)if(item.control&&!item.external){
      item.holder->setVisible(wanted(item)&&item.overflow);
      item.control->setVisible(wanted(item));
      if(item.caption)item.caption->setVisible(wanted(item));
    }
    for(const auto& panel:panels_)if(panel.heading)panel.heading->setVisible(panel.widget&&!panel.widget->isHidden());
  }
  static int fieldWidth(QWidget* field) {return std::max({field->minimumWidth(),field->minimumSizeHint().width(),field->sizeHint().width()});}
  static int captionWidth(const Item& item) {return item.caption?item.caption->fontMetrics().horizontalAdvance(item.caption->text())+2:0;}
  static int itemWidth(const Item& item) {return fieldWidth(item.control)+(item.caption?captionWidth(item)+5:0);}
  static void fitEditor(QWidget* control) {
    auto* combo=qobject_cast<QComboBox*>(control);if(!combo)return;
    if(combo->hasFocus()||combo->property("parameterDraft").toBool())return;
    const int width=std::max(combo->property("chartMinimumWidth").toInt(),combo->sizeHint().width());
    if(combo->width()!=width)combo->setFixedWidth(width);
  }
  void removeField(Item& item) {
    row_->removeWidget(item.control);item.holder->layout()->removeWidget(item.control);
    if(item.caption){row_->removeWidget(item.caption);item.holder->layout()->removeWidget(item.caption);}
  }
  int inlinePosition(int index) const {int position=1;for(int i=0;i<index;++i)if(items_[i].control&&!items_[i].external&&!items_[i].overflow&&row_->indexOf(items_[i].control)>=0)position+=items_[i].caption?2:1;return position;}
  template<class F> void protect(QWidget* control,F action) {setProperty("parameterLayerHiding",true);overflowContent_->setProperty("parameterLayerHiding",true);control->setProperty("parameterLayerHiding",true);action();control->setProperty("parameterLayerHiding",false);overflowContent_->setProperty("parameterLayerHiding",false);setProperty("parameterLayerHiding",false);}
  void releaseControl(QWidget* control) {
    for(auto it=items_.begin();it!=items_.end();++it)if(it->control==control){watchControl(control,false);protect(control,[&]{removeField(*it);control->setParent(window());control->hide();});delete it->caption;delete it->holder;items_.erase(it);schedule();return;}
  }
  void releasePanel(QWidget* panel) {for(auto it=panels_.begin();it!=panels_.end();++it)if(it->widget==panel){overflowLayout_->removeWidget(panel);if(it->heading)delete it->heading;panel->setParent(window());panel->hide();panels_.erase(it);schedule();return;}}
  void schedule(){if(pending_)return;pending_=true;QTimer::singleShot(0,this,[this]{reflow();});}
  SettingsPopoverHost* popovers_;QHBoxLayout* row_;QToolButton* more_;QWidget* overflowContent_;QVBoxLayout* overflowLayout_;QFrame* frame_;
  QPointer<QWidget> auxiliaryStatus_;
  std::vector<Item> items_;std::vector<Panel> panels_;bool pending_=false;
};
}
