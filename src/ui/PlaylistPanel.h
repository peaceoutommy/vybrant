// Playlist: song arrangement timeline with pattern, audio and automation clips on independent tracks.
#pragma once
#include "ui/App.h"
#include <QWidget>
#include <QScrollBar>
#include <QComboBox>
#include <QToolButton>
#include <QPixmap>
#include <QPolygonF>
#include "ui/Anim.h"
#include <set>
#include <map>

namespace mt {

class PlaylistView : public QWidget {
    Q_OBJECT
public:
    enum Tool { Draw, Select, Erase, Slice, MuteTool };
    explicit PlaylistView(App& app, QWidget* parent = nullptr);
    void setTool(Tool t);
    static QCursor toolCursorFor(Tool t);
    Tool tool() const { return m_tool; }
    // tests: view-local point inside track idx at tick t
    QPoint debugPoint(int idx, Tick t) const { return QPoint(tickToX(t) + 2, trackTop(idx) + trackH(idx) / 2); }
    double debugPpt() const { return m_ppt; }
    void revealClips(const std::vector<Id>& ids);   // select these clips and scroll the earliest into view
    std::set<Id> debugSelection() const { return m_selected; }
    void setSnapDivision(int d) { m_snapDiv = d; }
    void refresh();
    void uiTick();
    void zoomToFit();
    Tick maxExtent() const;
    void deleteSelected();
    void selectAll();
    void duplicateSelected();
    void muteSelected(bool m);
    void crossfadeSelected();
    void copySelected();
    void cutSelected();
    void paste();
    void selectBySource();
    void sliceAtPlayhead();
    void deleteLoopSpace();
    void insertLoopSpace();
    std::vector<Clip> m_clipboard;
Q_SIGNALS:
    void openPattern(Id patternId);
    void openAudioClip(Id clipId);
    void toolChanged(int tool);
    void modeChanged(int tool);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    bool event(QEvent*) override;
    void zoomTimeAt(int x, double factor);
    void keyPressEvent(QKeyEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dropEvent(QDropEvent*) override;

private:
    enum Drag {
        NoDrag,
        Moving,
        ResizingR,
        ResizingL,
        Rubber,
        LoopRange,
        Seek,
        Muting,
        TrackHeight,
        Pan,
        AutoPoint,
        FadeIn,
        FadeOut,
        Gain,
        Erasing
    };
    // audio clip handles: 1 = fade-in, 2 = fade-out, 3 = gain (0 = none); positions in view coordinates
    int clipHandleAt(const Clip& c, const QPoint& pos, QPoint* where = nullptr) const;
    QPolygonF fadeCurve(const Clip& c, int which) const;
    int gainY(const Clip& c) const;
    QRect fadeIconRect(const Clip& c, int which) const;
    QRect gainIconRect(const Clip& c) const;
    struct Layout {
        int headerW = 170, rulerH = 24;
        QRect ruler, headers, area;
    };
    Layout layout() const;
    QColor clipColor(const Clip& c) const;
    Tick xToTick(int x) const;
    int tickToX(Tick t) const;
    int trackAtY(int y) const;
    int trackTop(int idx) const;
    int trackH(int idx) const;
    Tick snap(Tick t, bool force = false) const;
    Tick snapRound(Tick t) const;
    Tick snapNear(Tick t) const;
    Tick snapTicks() const;
    Clip* clipAt(const QPoint& p, int* edge = nullptr) const;
    QRect clipRect(const Clip& c) const;
    int autoPointAt(const Clip& c, const QPoint& p) const;
    float autoValueAtY(const Clip& c, int y) const;
    Tick autoTickAtX(const Clip& c, int x) const;
    void beginEdit();
    void commitEdit(const QString& desc);
    void invalidate() {
        m_cacheValid = false;
        update();
    }
    void renderCache();
    void updateScrollbars();
    void drawClip(QPainter& p, const Clip& c, const QRectF& rect, double alpha);
    anim::RectTracker<Id> m_clipAnim;
    std::map<Id, QColor> m_clipColor; /* last drawn colour per clip, so a deleted clip's ghost keeps it */
    anim::Ticker* m_ticker = nullptr;
    void showClipMenu(Clip* c, const QPoint& global);
    void showTrackMenu(int idx, const QPoint& global);
    void showAreaMenu(const QPoint& pos, const QPoint& globalPos);
    App& m_app;
    Tool m_tool = Draw;
    int m_snapDiv = -1;
    double m_ppt = 0.03;
    Tick m_scrollTick = 0;
    int m_scrollY = 0;
    QScrollBar *m_hbar, *m_vbar;
    bool m_updatingBars = false;
    std::set<Id> m_selected, m_selBase;
    Id m_toggleOnRelease = 0;
    float m_handleStartShape = 0, m_handleStartGain = 1;
    Tick m_handleStartFade = 0;
    int m_hoverHandle = 0;
    Id m_hoverHandleClip = 0;
    Drag m_drag = NoDrag;
    QPoint m_pressPos, m_lastPos;
    Tick m_pressTick = 0;
    int m_pressTrack = -1;
    Id m_dragClip = 0;
    std::map<Id, Clip> m_before;
    bool m_editing = false;
    bool m_duplicated = false;
    QRect m_rubber;
    int m_heightTrack = -1;
    int m_heightStart = 0;
    Id m_autoLane = 0;
    int m_autoPoint = -1;
    AutomationLane m_laneBefore;
    double m_playTick = -1;
    QPixmap m_cache;
    bool m_cacheValid = false;
    int m_themeRev = -1;
    bool m_altHeld = false;
    QPoint m_hoverPos;
    Tick m_loopDragStart = 0;
    bool m_erasedAny = false;
    bool m_placed = false;
    // FL: the last clicked (or resized) clip is the brush - the draw tool places a copy of it: the same pattern / sample /
    // automation, at its size and with its settings. Choosing another pattern elsewhere goes back to placing that pattern.
    Id m_brushClip = 0;
    Clip m_brush;   // kept in case that clip is deleted
    Id m_brushPatSel = 0;   // selected pattern when the brush was set
    bool m_hasBrush = false;
    void setBrush(const Clip& c);
    const Clip* brush() const;   // nullptr: place the selected pattern
    Tick brushLength() const;
    std::set<Id> m_touched;
    qint64 m_lastEraseTime = 0;
    bool m_lastEraseWasClick = false;
    double m_vzoom = 1.0;
};

class PlaylistPanel : public QWidget {
    Q_OBJECT
public:
    explicit PlaylistPanel(App& app, QWidget* parent = nullptr);
    PlaylistView* view() const { return m_view; }
    void refresh(uint32_t flags);
    void uiTick();
Q_SIGNALS:
    void openPattern(Id patternId);
    void openAudioClip(Id clipId);

private:
    App& m_app;
    PlaylistView* m_view;
    QComboBox* m_snap;
    std::vector<QToolButton*> m_tools;
};

}   // namespace mt
