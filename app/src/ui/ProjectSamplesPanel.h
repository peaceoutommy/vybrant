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
    std::function<bool(const QMimeData*)> onDrop;   // something dragged in from elsewhere (the browser); true = taken
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
    bool m_draggingOut = false;
    void startDrag(Qt::DropActions) override;
    // drops from the browser save samples to the project; our own rows dragged back onto the list are ignored
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
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
    void reveal(const QString& payload);   // switch to the row's tab and select it (App::revealInProject)
    QString debugState() const;   // tests: "tab=<n> current=<payload>"
    QSize sizeHint() const override { return {170, 600}; }
    void debugDoubleClickFirst();   // tests: double-click the first row of the current tab
    bool debugDropFile(const QString& path);   // tests: a browser drop of a file onto the list
    void debugShowFirst();   // tests: 'Show on playlist' on the first row
    void debugPick(int tab, int row) {   // tests: click a row of a tab
        setTab(tab);
        if (auto* it = m_list->item(row)) Q_EMIT m_list->itemClicked(it);
    }
    void debugRemoveFirst() {   // tests: 'Delete' on the first sample row
        if (m_list->count()) removeSamples({m_list->item(0)->data(ProjectItemList::PathRole).toString()});
    }
    void debugRemoveUnused() { removeUnusedSamples(); }
Q_SIGNALS:
    void openAudioClip(Id clipId);
    void openSample(Id sourceId);   // a project sample no clip uses: sample-only details
    void openEditor(Id channelId);   // a sample that only lives on a channel: that channel's editor
    void showOnPlaylist(const std::vector<Id>& clipIds);
    void picked(ClipType type, Id id, const QString& file);   // a row was chosen: the playlist's draw tool places it next (file: samples)

private:
    void rebuild();
    void onContext(const QPoint& pos);
    void openDetails(const QString& path);
    std::vector<Id> clipsOf(const QString& path) const;   // playlist clips playing this sample, earliest first
    std::pair<std::vector<Id>, QStringList> selection() const;   // ids and sample paths of the selected rows
    void removeSamples(const QStringList& paths);
    void removeUnusedSamples();
    bool takeDrop(const QMimeData* md);
    QListWidgetItem* addItem(const QString& text, const QColor& dot, const QString& tip);
    App& m_app;
    ProjectItemList* m_list;
    QToolButton* m_tabs[3];
    int m_tab = Samples;
};

}   // namespace mt
