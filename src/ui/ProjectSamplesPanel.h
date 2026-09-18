// Narrow project strip next to the browser (always visible): three tabs listing what this project contains -
// Samples (every referenced audio file), Patterns and Automation. Click previews / selects, drag places it in the
// playlist or rack, right-click for more.
#pragma once
#include "ui/App.h"
#include <QWidget>
#include <QListWidget>
#include <QLabel>
#include <QToolButton>
#include <QKeyEvent>
#include <functional>

namespace mt {

// QListWidget that starts our in-app drag (see InAppDrag.h) with the item's payload
class ProjectItemList : public QListWidget {
public:
    using QListWidget::QListWidget;
    std::function<void()> onDelete;   // Delete key: remove the selection (the panel decides what that means per tab)
    enum { PayloadRole = Qt::UserRole, PathRole = Qt::UserRole + 1, IdRole = Qt::UserRole + 2 };

protected:
    // no rubber-band selection: a press on empty space just clears the selection (Ctrl / Shift + click do the multi-selecting)
    void mousePressEvent(QMouseEvent* e) override {
        QModelIndex idx = indexAt(e->pos());
        if (!idx.isValid()) {
            if (!(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier))) clearSelection();
            m_emptyPress = true;
            return;
        }
        m_emptyPress = false;
        // Qt only starts a drag from an item that was selected *before* the press, otherwise a drag extends the selection.
        // A plain press therefore selects the item first, so the following drag moves it (Ctrl / Shift keep their meaning).
        if (e->button() == Qt::LeftButton && !(e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) &&
            !selectionModel()->isSelected(idx)) {
            clearSelection();
            setCurrentIndex(idx);
            selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect);
        }
        QListWidget::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (m_emptyPress) return;
        QListWidget::mouseMoveEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (m_emptyPress) {
            m_emptyPress = false;
            return;
        }
        QListWidget::mouseReleaseEvent(e);
    }
    bool m_emptyPress = false;
    void startDrag(Qt::DropActions) override;
    void keyPressEvent(QKeyEvent* e) override {
        if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && onDelete) {
            onDelete();
            return;
        }
        QListWidget::keyPressEvent(e);
    }
};

class ProjectSamplesPanel : public QWidget {
    Q_OBJECT
public:
    enum Tab { Samples = 0, Patterns = 1, Automation = 2 };
    explicit ProjectSamplesPanel(App& app, QWidget* parent = nullptr);
    void refresh(uint32_t flags);
    void setTab(int t);
    QSize sizeHint() const override { return {170, 600}; }

private:
    void rebuild();
    void onContext(const QPoint& pos);
    QListWidgetItem* addItem(const QString& text, const QColor& dot, const QString& tip);
    App& m_app;
    ProjectItemList* m_list;
    QToolButton* m_tabs[3];
    int m_tab = Samples;
};

}   // namespace mt
