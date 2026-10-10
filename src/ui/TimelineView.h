// Shared base of the piano roll and the playlist: a scrolling, zoomable time grid with a ruler, a playhead, tools, snapping,
// a cached background, a rubber-band selection and the editing shortcuts both editors share. Subclasses draw their content
// and provide the editing operations on their own items (notes / clips).
#pragma once
#include "ui/App.h"
#include "ui/Anim.h"
#include <QWidget>
#include <QScrollBar>
#include <QPixmap>
#include <set>

namespace mt {

class TimelineView : public QWidget {
    Q_OBJECT
public:
    enum Tool { Draw, Select, Erase, Slice, MuteTool };   // Slice: playlist only
    void setTool(Tool t);
    static QCursor toolCursorFor(Tool t);
    Tool tool() const { return m_tool; }
    void setSnapDivision(int d) { m_snapDiv = d; }
    void zoomTimeAt(int x, double factor);   // keeps the tick under x in place
    // editing operations both editors offer (their shortcuts live in handleSharedKey)
    virtual void selectAll() = 0;
    virtual void deleteSelected() = 0;
    virtual void copySelected() = 0;
    void cutSelected() {
        copySelected();
        deleteSelected();
    }
    virtual void paste() = 0;
    virtual void duplicateSelected() = 0;
    virtual void muteSelected(bool mute) = 0;
    virtual void invertSelection() = 0;
    virtual void deleteSpace() = 0;   // Ctrl+Delete: remove the selected time span, shifting what follows
    virtual void insertSpace() = 0;   // Ctrl+Insert
Q_SIGNALS:
    void toolChanged(int tool);
    void modeChanged(int tool);   // tool currently shown in the toolbar (temporary modes included)

protected:
    TimelineView(App& app, Tick hbarStep, QWidget* parent);
    // layout hooks
    virtual QRect timeArea() const = 0;   // the grid where ticks map to x
    virtual int rulerHeight() const = 0;
    virtual std::pair<double, double> pptRange() const = 0;   // zoom limits (pixels per tick)
    virtual void updateScrollbars() = 0;
    virtual void onVScroll(int value) = 0;
    virtual void renderCache() = 0;   // paints the static background into m_cache
    virtual void selectInRect(const QRect& r) = 0;   // rubber band: add every item touching r to m_selected
    virtual bool hasSelection() const { return !m_selected.empty(); }   // what Escape would clear

    bool event(QEvent* e) override;   // trackpad pinch zoom
    void resizeEvent(QResizeEvent*) override;
    bool handleSharedKey(QKeyEvent* e);   // the shortcuts both editors share; true when handled
    bool handleTimeWheel(QWheelEvent* e);   // Ctrl = zoom, Shift / sideways = scroll in time; true when handled
    void pickTool(Tool t) {
        setTool(t);
        Q_EMIT toolChanged(t);
    }

    Tick xToTick(int x) const { return m_scrollTick + (Tick)((x - timeArea().left()) / m_ppt); }
    int tickToX(Tick t) const { return timeArea().left() + (int)((t - m_scrollTick) * m_ppt); }
    Tick snapTicks() const;
    Tick snap(Tick t, bool force = false) const;   // grid line at or before t (Alt = free unless forced)
    Tick snapNear(Tick t) const;   // nearest grid line

    void invalidate() {
        m_cacheValid = false;
        update();
    }
    void prepareCache();   // a fresh background pixmap at the screen's resolution
    void drawCache(QPainter& p);   // re-renders when stale (or the theme changed) and draws it
    void paintRuler(QPainter& p, const QRect& ruler, Tick barStep, Tick last);   // ticks + bar numbers; leaves the clip on the ruler
    void paintPlayhead(QPainter& p, int top, int bottom);
    void paintRubber(QPainter& p);
    void beginRubber(const QPoint& pos, Id toggleOnRelease = 0);
    void dragRubber(const QPoint& pos, Qt::KeyboardModifiers mods);
    void finishRubber(Qt::KeyboardModifiers mods);

    App& m_app;
    Tool m_tool = Draw;
    int m_snapDiv = 4;
    double m_ppt = 0.08;
    Tick m_scrollTick = 0;
    Tick m_hbarStep;   // ticks per horizontal scrollbar unit
    QScrollBar *m_hbar, *m_vbar;
    bool m_updatingBars = false;
    std::set<Id> m_selected, m_selBase;
    Id m_toggleOnRelease = 0;   // Ctrl+click on an item: toggles it unless the press turns into a rubber band
    QRect m_rubber;
    QPoint m_pressPos, m_lastPos, m_hoverPos;
    bool m_altHeld = false;
    bool m_editing = false, m_duplicated = false, m_erasedAny = false;
    std::set<Id> m_touched;
    qint64 m_lastEraseTime = 0;
    bool m_lastEraseWasClick = false;
    double m_playTick = -1;
    anim::Ticker* m_ticker = nullptr;
    QPixmap m_cache;
    bool m_cacheValid = false;
    int m_themeRev = -1;
};

}   // namespace mt
