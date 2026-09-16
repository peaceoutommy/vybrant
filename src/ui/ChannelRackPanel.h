// Channel Rack: channel list with step sequencer grid and per-step graph editor (velocity/pan/pitch/probability/offset).
#pragma once
#include "ui/App.h"
#include <QWidget>
#include <QScrollArea>
#include <QComboBox>
#include <QToolButton>
#include <QLabel>
#include <set>
#include <QTimer>
#include "ui/widgets/Controls.h"

namespace mt {

class RackView : public QWidget {
    Q_OBJECT
public:
    explicit RackView(App& app, QWidget* parent = nullptr);
    void refresh(); void uiTick();
    void setGraphMode(int mode) { m_graphMode = mode; update(); }   // -1 off, 0 vel, 1 pan, 2 pitch, 3 prob, 4 offset
    int graphMode() const { return m_graphMode; }
    QSize sizeHint() const override;
    int stepsThatFit(int availableWidth) const { int n = (availableWidth - m_leftW - 12 - 44) / (4 * stepW() + 4) * 4; return std::max(4, n); }
    int widthForSteps(int n) const { return m_leftW + 12 + 44 + (n / 4) * (4 * stepW() + 4) + 8; }
Q_SIGNALS:
    void openEditor(Id channelId); void openPianoRoll(Id channelId);
protected:
    void paintEvent(QPaintEvent*) override; void keyPressEvent(QKeyEvent*) override; void mousePressEvent(QMouseEvent*) override; void mouseMoveEvent(QMouseEvent*) override; void mouseReleaseEvent(QMouseEvent*) override; void mouseDoubleClickEvent(QMouseEvent*) override; void wheelEvent(QWheelEvent*) override; void contextMenuEvent(QContextMenuEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override; void dragMoveEvent(QDragMoveEvent*) override; void dropEvent(QDropEvent*) override;
private:
    enum Hit { None, Grip, Enable, Name, Mute, Solo, Pan, Vol, Mixer, StepCell, Graph };
    struct HitInfo { Hit hit = None; int row = -1; int step = -1; };
    HitInfo hitTest(const QPoint& p) const;
    QRect rowRect(int row) const; int stepX(int step) const; int stepW() const;
    Pattern* pattern() const; const Step* stepAt(Id ch, int step) const;
    void showChannelMenu(int row, const QPoint& global);
    void applyGraphAt(const QPoint& p, bool commit);
    App& m_app; int m_rowH = 24, m_leftW = 238, m_graphH = 90; int m_graphMode = -1;
    // drag state
    std::set<Id> m_multi; Id m_anchor = 0;   // multi-selection (Ctrl / Shift click); the project's selectedChannelId is the primary
    std::vector<Id> selectedIds() const;
    void deleteSelected();
    HitInfo m_press; bool m_paintValue = false; bool m_erasing = false; int m_dragRow = -1; QPoint m_lastPos; float m_knobStart = 0; int m_reorderTarget = -1; int m_playStep = -1; bool m_dragging = false;
};

class ChannelRackPanel : public QWidget {
    Q_OBJECT
public:
    explicit ChannelRackPanel(App& app, QWidget* parent = nullptr);
    void refresh(uint32_t flags); void uiTick();
    QSize sizeHint() const override { return {widthForSteps(16), 320}; }
    int widthForSteps(int n) const { return m_view->widthForSteps(n) + 16; }
    int heightForContent() const;   // toolbar + all channel rows (+ graph editor when open)
protected:
    bool eventFilter(QObject* o, QEvent* e) override;
Q_SIGNALS:
    void openEditor(Id channelId); void openPianoRoll(Id channelId);
private:
    void fitStepsToWidth();
    App& m_app; RackView* m_view; QScrollArea* m_scroll; QTimer* m_fitTimer = nullptr; QLabel* m_patName; Knob* m_swing; DragValue* m_steps; QComboBox* m_resolution; QComboBox* m_graph; bool m_updating = false;
};

} // namespace mt
