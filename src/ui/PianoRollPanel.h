// Piano Roll: full MIDI note editor operating on Pattern channel data in ticks.
#pragma once
#include "ui/TimelineView.h"
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

class PianoRollView : public TimelineView {
    Q_OBJECT
public:
    enum KeyLabels { LabelsC = 0, LabelsAll = 1, LabelsNone = 2 };   // note names drawn on the key column
    explicit PianoRollView(App& app, QWidget* parent = nullptr);
    // tests: view-local point of a tick/pitch cell, and the vertical scroll state
    QPoint debugPoint(Tick t, int pitch) const { return QPoint(tickToX(t) + 3, pitchToY(pitch) + m_rowH / 2); }
    QString debugScroll() const {
        Layout L = layout();
        return QString("top=%1 min=%2 rowH=%3 gridH=%4 vbar=%5/%6 ppt=%7 sel=%8 rulerY=%9 velY=%10..%11 tool=%12")
            .arg(m_topPitch)
            .arg(minTopPitch())
            .arg(m_rowH)
            .arg(layout().grid.height())
            .arg(m_vbar->value())
            .arg(m_vbar->maximum())
            .arg(m_ppt)
            .arg(m_selected.size())
            .arg(L.ruler.center().y())
            .arg(L.vel.top())
            .arg(L.vel.bottom())
            .arg(m_tool);
    }
    void setChannel(Id id) {
        m_channel = id;
        m_selected.clear();
        invalidate();
    }
    Id channel() const { return m_channel; }
    void setGhost(bool g) {
        m_ghost = g;
        invalidate();
    }
    void setScale(int root, int scaleIdx) {
        m_scaleRoot = root;
        m_scale = scaleIdx;
        invalidate();
    }
    void setChord(int chordIdx) { m_chord = chordIdx; }
    void setNoteRepeat(bool r) { m_noteRepeat = r; }
    void setKeyLabels(int m) {
        m_keyLabels = m;
        invalidate();
    }
    int keyLabels() const { return m_keyLabels; }
    // note lanes: 0 velocity, 1 pan, 2 release velocity, 3 fine pitch; controller lanes: 4 pitch bend, 5 mod wheel, 6 expression,
    // 7 sustain, 8 channel pressure, 9 the CC number picked from the lane menu
    static constexpr int kLaneCount = 10;
    void setLane(int lane) {
        m_lane = lane;
        invalidate();
    }
    int laneController() const;   // the controller id of a controller lane, -1 for a note lane
    float laneValue(const Note& n) const;
    void setLaneValue(Note& n, float v) const;
    bool laneBipolar() const;
    QString laneName() const;
    int laneBarWidth() const { return std::max(6, std::min(48, (int)(snapTicks() * m_ppt * 0.6))); }
    void refresh();
    void uiTick();
    // tools
    void quantize(bool lengths);
    void arpeggiate();
    void strum();
    void legato();
    void randomize(float velAmt, float panAmt);
    void transpose(int semis);
    void selectAll() override;
    void deleteSelected() override;
    void copySelected() override;
    void paste() override;
    void duplicateSelected() override;
    void setSelectedLength(Tick len);
    void invertSelection() override;
    void selectByPitch();
    void muteSelected(bool mute) override;
    void glue();
    void deleteSpace() override;
    void insertSpace() override;
    QScrollBar* vbar() const { return m_vbar; }
    void zoomToFit();
    static QStringList scaleNames();
    static QStringList chordNames();
Q_SIGNALS:
    void channelRequested(Id channelId);

protected:
    bool event(QEvent* e) override;
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void leaveEvent(QEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dropEvent(QDropEvent*) override;

private:
    enum Drag {
        NoDrag,
        Moving,
        Resizing,
        Rubber,
        Velocity,
        Erasing,
        Pan,
        KeyPreview,
        Creating,
        TimeSelect,
        Muting,
        Seek,
        LaneResize,
        ControlDraw,
        ControlErase
    };
    struct Layout {
        int keyW = 60, rulerH = 22, velH = 70;
        QRect grid, ruler, keys, vel;
    };
    Layout layout() const;
    Pattern* pattern() const;
    PatternChannelData* data() const;
    static constexpr Id kVirtualBase = (Id)1 << 60;
    std::vector<Note> stepsAsNotes() const;
    std::vector<Note> ghostNotesOf(const PatternChannelData& oc) const;
    bool stepMode() const;
    const std::vector<Note>& notesView() const;
    mutable std::vector<Note> m_virtual;
    int yToPitch(int y) const;
    int pitchToY(int pitch) const;
    int minTopPitch() const;
    Note* noteAt(const QPoint& p, bool* nearEdge = nullptr) const;
    void beginEdit();
    void commitEdit(const QString& desc, int mergeId = 0);   // mergeId: consecutive edits with the same id are one undo step
    void seekToPatternTick(Tick t);
    QRect timeArea() const override { return layout().grid; }
    int rulerHeight() const override { return layout().rulerH; }
    std::pair<double, double> pptRange() const override { return {0.002, 2.0}; }
    void onVScroll(int v) override { m_topPitch = clamp(127 - v, minTopPitch(), 127); }
    void selectInRect(const QRect& r) override;
    bool hasSelection() const override { return !m_selected.empty() || m_timeSel; }
    void renderCache() override;
    void updateScrollbars() override;
    void previewNote(int pitch, bool on);
    bool previewAllowed() const;
    std::map<Id, Note> m_created;
    void showAreaMenu(const QPoint& pos, const QPoint& globalPos);
    bool inScale(int pitch) const;
    Id m_channel = 0;
    bool m_ghost = true;
    int m_scaleRoot = 0, m_scale = 0, m_chord = 0;
    bool m_noteRepeat = false;
    int m_rowH = 17;
    int m_topPitch = 84;
    int m_keyLabels = LabelsC;
    Tick m_lastLength = PPQ / 4;
    float m_lastVelocity = 0.8f;
    Drag m_drag = NoDrag;
    Tick m_pressTick = 0;
    int m_pressPitch = 0;
    Id m_dragNote = 0;
    PatternChannelData m_before;
    std::vector<Note> m_baseNotes;
    int m_previewPitch = -1;
    QTimer m_repeatTimer, m_autoScroll;
    QPoint m_lastMouse;
    float m_laneLock = -1;   // Shift in the lane: value captured at the first Shift position
    std::map<std::pair<Id, int>, float> m_laneOrig;   // (note, lane) -> value before the lane first edited it (Alt restores)
    int m_lane = 0;
    int m_otherCc = 7;
    int m_velH = 80;
    int m_laneResizeStart = 0;
    QRect m_laneLabelRect;
    Tick m_selStart = 0, m_selEnd = 0;
    bool m_timeSel = false;
    Tick m_selAnchor = 0;
    anim::RectTracker<Id> m_noteAnim;
    std::vector<Note> m_clipboard;
};

class PianoRollPanel : public QWidget {
    Q_OBJECT
public:
    explicit PianoRollPanel(App& app, QWidget* parent = nullptr);
    void refresh(uint32_t flags);
    void uiTick();
    void showChannel(Id id);
    PianoRollView* view() const { return m_view; }

private:
    App& m_app;
    PianoRollView* m_view;
    QComboBox *m_channel, *m_snap, *m_scale, *m_root, *m_chord, *m_labels;
    QToolButton *m_draw, *m_select, *m_erase, *m_mute, *m_ghost, *m_repeat;
    bool m_updating = false;
};

}   // namespace mt
