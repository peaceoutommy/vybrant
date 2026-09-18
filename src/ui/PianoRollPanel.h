// Piano Roll: full MIDI note editor operating on Pattern channel data in ticks.
#pragma once
#include "ui/App.h"
#include <QWidget>
#include <QScrollBar>
#include <QComboBox>
#include <QToolButton>
#include <QPixmap>
#include <QTimer>
#include "ui/Anim.h"
#include <set>
#include <map>

namespace mt {

class PianoRollView : public QWidget {
    Q_OBJECT
public:
    enum Tool { Draw, Select, Erase, MuteTool };
    enum KeyLabels { LabelsC = 0, LabelsAll = 1, LabelsNone = 2 };   // note names drawn on the key column
    explicit PianoRollView(App& app, QWidget* parent = nullptr);
    void setTool(Tool t);
    static QCursor toolCursorFor(Tool t);
    Tool tool() const { return m_tool; }
    void setChannel(Id id) { m_channel = id; m_selected.clear(); invalidate(); }
    Id channel() const { return m_channel; }
    void setGhost(bool g) { m_ghost = g; invalidate(); }
    void setScale(int root, int scaleIdx) { m_scaleRoot = root; m_scale = scaleIdx; invalidate(); }
    void setChord(int chordIdx) { m_chord = chordIdx; }
    void setNoteRepeat(bool r) { m_noteRepeat = r; }
    void setSnapDivision(int div) { m_snapDiv = div; }
    void setKeyLabels(int m) { m_keyLabels = m; invalidate(); }
    int keyLabels() const { return m_keyLabels; }
    void setLane(int lane) { m_lane = lane; invalidate(); }   // 0 velocity, 1 pan, 2 release velocity, 3 fine pitch
    float laneValue(const Note& n) const; void setLaneValue(Note& n, float v) const; bool laneBipolar() const; QString laneName() const; int laneBarWidth() const { return std::max(6, std::min(48, (int)(snapTicks() * m_ppt * 0.6))); }
    void refresh(); void uiTick();
    // tools
    void quantize(bool lengths); void arpeggiate(); void strum(); void legato(); void randomize(float velAmt, float panAmt); void transpose(int semis); void selectAll(); void deleteSelected(); void copySelected(); void cutSelected(); void paste(); void duplicateSelected(); void setSelectedLength(Tick len); void invertSelection(); void selectByPitch(); void muteSelected(bool mute); void glue(); void deleteTimeSpace(); void insertTimeSpace();
    QScrollBar* hbar() const { return m_hbar; } QScrollBar* vbar() const { return m_vbar; }
    void zoomToFit();
    static QStringList scaleNames(); static QStringList chordNames();
Q_SIGNALS:
    void toolChanged(int tool); void channelRequested(Id channelId); void modeChanged(int tool);   // tool currently shown in the toolbar (temporary modes included)
protected:
    bool event(QEvent* e) override;
    void paintEvent(QPaintEvent*) override; void mousePressEvent(QMouseEvent*) override; void mouseMoveEvent(QMouseEvent*) override; void mouseReleaseEvent(QMouseEvent*) override; void mouseDoubleClickEvent(QMouseEvent*) override; void wheelEvent(QWheelEvent*) override; void keyPressEvent(QKeyEvent*) override; void keyReleaseEvent(QKeyEvent*) override; void resizeEvent(QResizeEvent*) override; void contextMenuEvent(QContextMenuEvent*) override; void leaveEvent(QEvent*) override;
private:
    enum Drag { NoDrag, Moving, Resizing, Rubber, Velocity, Erasing, Pan, KeyPreview, Creating, TimeSelect, Muting, Seek, LaneResize };
    struct Layout { int keyW = 60, rulerH = 22, velH = 70; QRect grid, ruler, keys, vel; };
    Layout layout() const;
    Pattern* pattern() const; PatternChannelData* data() const;
    static constexpr Id kVirtualBase = (Id)1 << 60; std::vector<Note> stepsAsNotes() const; std::vector<Note> ghostNotesOf(const PatternChannelData& oc) const; bool stepMode() const; const std::vector<Note>& notesView() const; mutable std::vector<Note> m_virtual; std::set<Id> m_selBase;
    Tick xToTick(int x) const; int tickToX(Tick t) const; int yToPitch(int y) const; int pitchToY(int pitch) const; Tick snap(Tick t, bool force = false) const; Tick snapNear(Tick t) const; Tick snapTicks() const;
    Note* noteAt(const QPoint& p, bool* nearEdge = nullptr) const;
    void beginEdit(); void commitEdit(const QString& desc); void cancelEdit(); void seekToPatternTick(Tick t);
    void invalidate() { m_cacheValid = false; update(); }
    void renderCache(); void updateScrollbars();
    void previewNote(int pitch, bool on); bool previewAllowed() const; std::map<Id, Note> m_created; void showAreaMenu(const QPoint& pos, const QPoint& globalPos);
    bool inScale(int pitch) const;
    App& m_app; Tool m_tool = Draw; Id m_channel = 0; bool m_ghost = true; int m_scaleRoot = 0, m_scale = 0, m_chord = 0; bool m_noteRepeat = false; int m_snapDiv = 4;
    double m_ppt = 0.08; int m_rowH = 17; Tick m_scrollTick = 0; int m_topPitch = 84; int m_keyLabels = LabelsC; QScrollBar *m_hbar, *m_vbar; bool m_updatingBars = false;
    std::set<Id> m_selected; Tick m_lastLength = PPQ / 4; float m_lastVelocity = 0.8f;
    Drag m_drag = NoDrag; QPoint m_pressPos, m_lastPos; Tick m_pressTick = 0; int m_pressPitch = 0; Id m_dragNote = 0; PatternChannelData m_before; bool m_editing = false; bool m_duplicated = false; QRect m_rubber; int m_previewPitch = -1; QTimer m_repeatTimer, m_autoScroll; QPoint m_lastMouse; bool m_altHeld = false; bool m_erasedAny = false;
    int m_lane = 0; int m_velH = 80; int m_laneResizeStart = 0; QRect m_laneLabelRect; Tick m_selStart = 0, m_selEnd = 0; bool m_timeSel = false; Tick m_selAnchor = 0; std::set<Id> m_touched; qint64 m_lastEraseTime = 0; bool m_lastEraseWasClick = false;   // time selection (Ctrl+drag on the ruler)
    anim::RectTracker<Id> m_noteAnim; anim::Ticker* m_ticker = nullptr;
    std::vector<Note> m_clipboard; QPixmap m_cache; bool m_cacheValid = false; int m_themeRev = -1; double m_playTick = -1; QPoint m_hoverPos;
};

class PianoRollPanel : public QWidget {
    Q_OBJECT
public:
    explicit PianoRollPanel(App& app, QWidget* parent = nullptr);
    void refresh(uint32_t flags); void uiTick();
    void showChannel(Id id); PianoRollView* view() const { return m_view; }
private:
    App& m_app; PianoRollView* m_view; QComboBox *m_channel, *m_snap, *m_scale, *m_root, *m_chord, *m_labels; QToolButton *m_draw, *m_select, *m_erase, *m_mute, *m_ghost, *m_repeat; bool m_updating = false;
};

} // namespace mt
