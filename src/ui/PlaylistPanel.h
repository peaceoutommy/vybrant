// Playlist: song arrangement timeline with pattern, audio and automation clips on independent tracks.
#pragma once
#include "ui/TimelineView.h"
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

class PlaylistView : public TimelineView {
    Q_OBJECT
public:
    explicit PlaylistView(App& app, QWidget* parent = nullptr);
    // tests: view-local point inside track idx at tick t
    QPoint debugPoint(int idx, Tick t) const { return QPoint(tickToX(t) + 2, trackTop(idx) + trackH(idx) / 2); }
    double debugPpt() const { return m_ppt; }
    QPoint debugGainHandle(Id clipId) const {
        const Clip* c = m_app.project().clip(clipId);
        return c ? gainIconRect(*c).center() : QPoint(-1, -1);
    }
    void revealClips(const std::vector<Id>& ids);   // select these clips and scroll the earliest into view
    std::set<Id> debugSelection() const { return m_selected; }
    QString debugInfo() const { return QString("sel=%1 ppt=%2 tool=%3").arg(m_selected.size()).arg(m_ppt).arg(m_tool); }
    void refresh();
    void uiTick();
    void zoomToFit();
    Tick maxExtent() const;
    void deleteSelected() override;
    void selectAll() override;
    void duplicateSelected() override;
    void muteSelected(bool m) override;
    void invertSelection() override;
    void crossfadeSelected();
    void copySelected() override;
    void paste() override;
    void selectBySource();
    void sliceAtPlayhead();
    void useTake(Id clipId);
    void deleteSpace() override;   // the loop area's span
    void insertSpace() override;
    std::vector<Clip> m_clipboard;
Q_SIGNALS:
    void openPattern(Id patternId);
    void openAudioClip(Id clipId);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
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
        Erasing,
        TempoPointDrag
    };
    // audio clip handles: 1 = fade-in, 2 = fade-out, 3 = gain (0 = none); positions in view coordinates
    int clipHandleAt(const Clip& c, const QPoint& pos, QPoint* where = nullptr) const;
    QPolygonF fadeCurve(const Clip& c, int which) const;
    QRect fadeIconRect(const Clip& c, int which) const;
    QRect gainIconRect(const Clip& c) const;
    struct Layout {
        int headerW = 170, rulerH = 44;   // everything above the tracks: the ruler and the tempo lane under it
        QRect ruler, tempo, headers, area;
    };
    Layout layout() const;
    // tempo lane: the tempo curve with its points (double-click adds, drag moves / changes the BPM, right-click deletes / ramps)
    std::pair<double, double> tempoRange() const;   // BPM shown at the lane's bottom and top
    int tempoY(double bpm) const;
    int tempoPointAt(const QPoint& p) const;   // index into transport.tempoPoints, -1 = none
    void paintTempoLane(QPainter& p, const Layout& L);
    void showTempoMenu(int idx, const QPoint& global);
    void addTimeSigActions(QMenu& m, int x);   // the ruler menu's time-signature entries for a right-click at x
    QColor clipColor(const Clip& c) const;
    int trackAtY(int y) const;
    int trackTop(int idx) const;
    int trackH(int idx) const;
    Tick snapRound(Tick t) const;
    Clip* clipAt(const QPoint& p, int* edge = nullptr) const;
    QRect clipRect(const Clip& c) const;
    int autoPointAt(const Clip& c, const QPoint& p) const;
    float autoValueAtY(const Clip& c, int y) const;
    Tick autoTickAtX(const Clip& c, int x) const;
    void beginEdit();
    void commitEdit(const QString& desc);
    QRect timeArea() const override { return layout().area; }
    int rulerHeight() const override { return layout().rulerH; }
    std::pair<double, double> pptRange() const override { return {(double)layout().area.width() / (double)maxExtent(), 1.0}; }
    void onVScroll(int v) override { m_scrollY = v; }
    void selectInRect(const QRect& r) override;
    void renderCache() override;
    void updateScrollbars() override;
    void drawClip(QPainter& p, const Clip& c, const QRectF& rect, double alpha);
    anim::RectTracker<Id> m_clipAnim;
    std::map<Id, QColor> m_clipColor; /* last drawn colour per clip, so a deleted clip's ghost keeps it */
    void showClipMenu(Clip* c, const QPoint& global);
    void showTrackMenu(int idx, const QPoint& global);
    void showAreaMenu(const QPoint& pos, const QPoint& globalPos);
    int m_scrollY = 0;
    float m_handleStartShape = 0, m_handleStartGain = 1;
    Tick m_handleStartFade = 0;
    int m_hoverHandle = 0;
    Id m_hoverHandleClip = 0;
    Drag m_drag = NoDrag;
    Tick m_pressTick = 0;
    int m_pressTrack = -1;
    Id m_dragClip = 0;
    std::map<Id, Clip> m_before;
    int m_heightTrack = -1;
    int m_heightStart = 0;
    Id m_autoLane = 0;
    int m_autoPoint = -1;
    AutomationLane m_laneBefore;
    int m_tempoPoint = -1;
    std::vector<TempoPoint> m_tempoBefore;
    std::pair<double, double> m_tempoRange;   // frozen while a point is dragged
    Tick m_loopDragStart = 0;
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
