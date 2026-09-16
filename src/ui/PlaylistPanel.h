// Playlist: song arrangement timeline with pattern, audio and automation clips on independent tracks.
#pragma once
#include "ui/App.h"
#include <QWidget>
#include <QScrollBar>
#include <QComboBox>
#include <QToolButton>
#include <QPixmap>
#include <set>
#include <map>

namespace mt {

class PlaylistView : public QWidget {
    Q_OBJECT
public:
    enum Tool { Draw, Select, Erase, Slice, MuteTool };
    explicit PlaylistView(App& app, QWidget* parent = nullptr);
    void setTool(Tool t); Tool tool() const { return m_tool; }
    void setSnapDivision(int d) { m_snapDiv = d; }
    void refresh(); void uiTick(); void zoomToFit();
    void deleteSelected(); void selectAll(); void duplicateSelected(); void muteSelected(bool m); void crossfadeSelected();
Q_SIGNALS:
    void openPattern(Id patternId); void openAudioClip(Id clipId); void toolChanged(int tool);
protected:
    void paintEvent(QPaintEvent*) override; void mousePressEvent(QMouseEvent*) override; void mouseMoveEvent(QMouseEvent*) override; void mouseReleaseEvent(QMouseEvent*) override; void mouseDoubleClickEvent(QMouseEvent*) override; void wheelEvent(QWheelEvent*) override; void keyPressEvent(QKeyEvent*) override; void resizeEvent(QResizeEvent*) override; void contextMenuEvent(QContextMenuEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override; void dragMoveEvent(QDragMoveEvent*) override; void dropEvent(QDropEvent*) override;
private:
    enum Drag { NoDrag, Moving, ResizingR, ResizingL, Rubber, LoopRange, Seek, TrackHeight, Pan, AutoPoint, FadeIn, FadeOut, Erasing };
    struct Layout { int headerW = 170, rulerH = 24; QRect ruler, headers, area; };
    Layout layout() const;
    Tick xToTick(int x) const; int tickToX(Tick t) const; int trackAtY(int y) const; int trackTop(int idx) const; int trackH(int idx) const; Tick snap(Tick t, bool force = false) const; Tick snapRound(Tick t) const; Tick snapNear(Tick t) const; Tick snapTicks() const;
    Clip* clipAt(const QPoint& p, int* edge = nullptr) const; QRect clipRect(const Clip& c) const;
    int autoPointAt(const Clip& c, const QPoint& p) const; float autoValueAtY(const Clip& c, int y) const; Tick autoTickAtX(const Clip& c, int x) const;
    void beginEdit(); void commitEdit(const QString& desc);
    void invalidate() { m_cacheValid = false; update(); }
    void renderCache(); void updateScrollbars(); void drawClip(QPainter& p, const Clip& c);
    void showClipMenu(Clip* c, const QPoint& global); void showTrackMenu(int idx, const QPoint& global); void showAreaMenu(const QPoint& pos, const QPoint& globalPos);
    App& m_app; Tool m_tool = Draw; int m_snapDiv = -1; double m_ppt = 0.03; Tick m_scrollTick = 0; int m_scrollY = 0; QScrollBar *m_hbar, *m_vbar; bool m_updatingBars = false;
    std::set<Id> m_selected; Drag m_drag = NoDrag; QPoint m_pressPos, m_lastPos; Tick m_pressTick = 0; int m_pressTrack = -1; Id m_dragClip = 0; std::map<Id, Clip> m_before; bool m_editing = false; bool m_duplicated = false; QRect m_rubber; int m_heightTrack = -1; int m_heightStart = 0; Id m_autoLane = 0; int m_autoPoint = -1; AutomationLane m_laneBefore; double m_playTick = -1; QPixmap m_cache; bool m_cacheValid = false; bool m_altHeld = false; QPoint m_hoverPos; Tick m_loopDragStart = 0; bool m_erasedAny = false;
};

class PlaylistPanel : public QWidget {
    Q_OBJECT
public:
    explicit PlaylistPanel(App& app, QWidget* parent = nullptr);
    void refresh(uint32_t flags); void uiTick();
Q_SIGNALS:
    void openPattern(Id patternId); void openAudioClip(Id clipId);
private:
    App& m_app; PlaylistView* m_view; QComboBox* m_snap; std::vector<QToolButton*> m_tools;
};

} // namespace mt
