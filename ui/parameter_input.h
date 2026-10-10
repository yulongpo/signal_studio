#pragma once

#include <QApplication>
#include <QAbstractScrollArea>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QWheelEvent>
#include <functional>

namespace signalstudio {
// Draft text must never become an analysis parameter through a refresh,
// window deactivation, or a wheel event. Return, Tab and outside clicks commit.
class ParameterDraft {
public:
    virtual ~ParameterDraft() = default;
    virtual void commit() = 0;
    virtual void discard() = 0;
};

template<class Base> class ParameterSpinBox : public Base, public ParameterDraft {
public:
    explicit ParameterSpinBox(QWidget* parent = nullptr) : Base(parent) {
        this->setKeyboardTracking(false);
        auto* editor = this->template findChild<QLineEdit*>();
        editor->installEventFilter(this);
        QObject::connect(editor, &QLineEdit::textEdited, this, [this] {
            this->setProperty("parameterDraft", true);
        });
    }
    void commit() override {
        if (!this->property("parameterDraft").toBool()) return;
        this->setProperty("parameterDraft", false);
        this->interpretText();
        // interpretText may reject an intermediate/invalid draft. Show the
        // last accepted value, without emitting a second parameter change.
        const QSignalBlocker block(this);
        this->setValue(this->value());
    }
    void discard() override {
        this->setProperty("parameterDraft", false);
        const QSignalBlocker block(this);
        this->setValue(this->value());
    }
protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent*>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter) {
                commit(); event->accept(); return true;
            }
        }
        return Base::eventFilter(watched, event);
    }
    void focusOutEvent(QFocusEvent* event) override {
        if (event->reason() == Qt::MouseFocusReason || event->reason() == Qt::TabFocusReason ||
            event->reason() == Qt::BacktabFocusReason) {
            commit(); Base::focusOutEvent(event); return;
        }
        auto* editor = this->template findChild<QLineEdit*>();
        const auto value = this->value(); const auto draft = editor->text();
        const bool pending = this->property("parameterDraft").toBool();
        const QSignalBlocker block(this), editBlock(editor);
        Base::focusOutEvent(event);
        this->setValue(value);
        if (pending) editor->setText(draft);
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            commit(); event->accept(); return;
        }
        if (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down ||
            event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown) {
            auto* editor = this->template findChild<QLineEdit*>();
            const auto value = this->value();
            const QSignalBlocker block(this), editBlock(editor);
            this->interpretText(); Base::keyPressEvent(event);
            const auto draft = editor->text();
            this->setValue(value); editor->setText(draft);
            this->setProperty("parameterDraft", true); return;
        }
        Base::keyPressEvent(event);
    }
    void wheelEvent(QWheelEvent* event) override { event->ignore(); }
};
using ParameterDoubleSpinBox = ParameterSpinBox<QDoubleSpinBox>;
using ParameterIntSpinBox = ParameterSpinBox<QSpinBox>;

class ParameterComboCommit final : public QObject, public ParameterDraft {
public:
    ParameterComboCommit(QComboBox* combo, std::function<void()> apply)
        : QObject(combo), combo_(combo), apply_(std::move(apply)) {
        combo_->lineEdit()->installEventFilter(this);
        connect(combo_->lineEdit(), &QLineEdit::textEdited, this, [this] {
            combo_->setProperty("parameterDraft", true);
        });
        connect(combo_, &QComboBox::currentIndexChanged, this, [this] {
            combo_->setProperty("parameterDraft", false);
            apply_();
        });
    }
    void commit() override {
        if (!combo_->property("parameterDraft").toBool()) return;
        combo_->setProperty("parameterDraft", false);
        combo_->setProperty("parameterCustomCommit", true);
        apply_();
        combo_->setProperty("parameterCustomCommit", false);
    }
    void discard() override {
        combo_->setProperty("parameterDraft", false);
        const QSignalBlocker block(combo_), editBlock(combo_->lineEdit());
        combo_->setEditText(combo_->property("parameterCommittedText").toString());
    }
protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent*>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter) {
                commit(); event->accept(); return true;
            }
        }
        if (event->type() == QEvent::FocusOut) {
            const auto reason = static_cast<QFocusEvent*>(event)->reason();
            if (reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason) commit();
        }
        return false;
    }
private:
    QComboBox* combo_;
    std::function<void()> apply_;
};
inline void bindParameterCombo(QComboBox* combo, std::function<void()> apply) {
    combo->setProperty("parameterCommittedText", combo->currentText());
    new ParameterComboCommit(combo, std::move(apply));
}

template<class Spin, class Value> void displayParameterValue(Spin* spin, Value value) {
    if (spin->property("parameterDraft").toBool()) return;
    spin->setProperty("parameterDraft", false);
    spin->setValue(value);
}

class ParameterInputPolicy final : public QObject {
public:
    explicit ParameterInputPolicy(QWidget* root) : QObject(root), root_(root) {
        qApp->installEventFilter(this);
    }
    ~ParameterInputPolicy() override { qApp->removeEventFilter(this); }
    void setContext(const QString& context) {
        if (context_ == context) return;
        for (auto* widget : root_->findChildren<QWidget*>()) {
            if (auto* draft = dynamic_cast<ParameterDraft*>(widget)) draft->discard();
            for (auto* child : widget->children())
                if (auto* draft = dynamic_cast<ParameterComboCommit*>(child)) draft->discard();
        }
        context_ = context;
    }
protected:
    static bool contains(QObject* owner, QObject* child) {
        // QWidget::isAncestorOf stops at a separate window. Dialogs and combo
        // popups still belong to this application's QObject ownership tree.
        for (auto* current = child; current; current = current->parent())
            if (current == owner) return true;
        return false;
    }
    bool eventFilter(QObject* watched, QEvent* event) override {
        auto* target = qobject_cast<QWidget*>(watched);
        if (!target || !contains(root_, target)) return false;
        if (event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent*>(event)->key();
            if (key == Qt::Key_Tab || key == Qt::Key_Backtab) {
                for (auto* owner = target; owner && owner != root_; owner = owner->parentWidget()) {
                    if (auto* draft = dynamic_cast<ParameterDraft*>(owner)) { draft->commit(); break; }
                    if (qobject_cast<QComboBox*>(owner)) {
                        for (auto* child : owner->children())
                            if (auto* binding = dynamic_cast<ParameterComboCommit*>(child)) binding->commit();
                        break;
                    }
                }
            }
        }
        if (event->type() == QEvent::Wheel) {
            for (auto* owner = target; owner && owner != root_; owner = owner->parentWidget()) {
                if (qobject_cast<QComboBox*>(owner) || qobject_cast<QAbstractSpinBox*>(owner) || qobject_cast<QLineEdit*>(owner)) {
                    // Scroll the containing panel without sending the event
                    // through the parameter control. An open combo popup must
                    // neither change selection nor scroll the panel behind it.
                    if (target->window() == owner->window()) {
                        for (auto* parent = owner->parentWidget(); parent; parent = parent->parentWidget()) {
                            if (auto* area = qobject_cast<QAbstractScrollArea*>(parent)) {
                                auto* wheel = static_cast<QWheelEvent*>(event);
                                QWheelEvent scroll(area->viewport()->mapFromGlobal(wheel->globalPosition().toPoint()),
                                    wheel->globalPosition(), wheel->pixelDelta(), wheel->angleDelta(), wheel->buttons(),
                                    wheel->modifiers(), wheel->phase(), wheel->inverted(), wheel->source(), wheel->pointingDevice());
                                QCoreApplication::sendEvent(area->viewport(), &scroll); break;
                            }
                        }
                    }
                    event->accept(); return true;
                }
            }
        }
        if (event->type() == QEvent::MouseButtonPress) {
            // Include non-focusable labels and plots as outside-click targets.
            // Commit before the clicked control switches file/channel context.
            QList<QPointer<QWidget>> pending;
            for (auto* widget : root_->findChildren<QWidget*>())
                if (widget->property("parameterDraft").toBool() && !contains(widget, target)) pending.append(widget);
            for (const auto& widget : pending) if (widget) {
                if (auto* draft = dynamic_cast<ParameterDraft*>(widget.data())) draft->commit();
                else for (auto* child : widget->children())
                    if (auto* binding = dynamic_cast<ParameterComboCommit*>(child)) { binding->commit(); break; }
            }
        }
        return false;
    }
private:
    QWidget* root_;
    QString context_;
};
}
