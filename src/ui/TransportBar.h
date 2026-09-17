// Main toolbar: transport controls, tempo/time signature, pattern selector, snap, mode, stats.
#pragma once
#include "ui/App.h"
#include "ui/widgets/Controls.h"
#include <QWidget>
#include <QToolButton>
#include <QComboBox>
#include <QLabel>
#include <QHBoxLayout>

namespace mt {

// FL-style stacked PAT / SONG switch
class ModeSwitch : public QWidget {
    Q_OBJECT
public:
    explicit ModeSwitch(QWidget* parent = nullptr) : QWidget(parent) { setFixedSize(50, 30); setCursor(Qt::PointingHandCursor); setToolTip("Pattern / Song mode (L)"); }
    void setSong(bool s) { if (m_song != s) { m_song = s; update(); } }
    bool song() const { return m_song; }
Q_SIGNALS:
    void modeChanged(bool song);
protected:
    void paintEvent(QPaintEvent*) override; void mousePressEvent(QMouseEvent*) override;
private:
    bool m_song = false;
};

class TransportBar : public QWidget {
    Q_OBJECT
public:
    explicit TransportBar(App& app, QWidget* parent = nullptr);
    void refresh(uint32_t flags);
    void uiTick();
    QComboBox* snapCombo() const { return m_snap; }
    static QComboBox* makeSnapCombo(QWidget* parent);
    static int snapDivisionFromIndex(int idx);
    static int indexFromSnapDivision(int div);
    void addWindowButton(QToolButton* b) { m_winLayout->addWidget(b); }
    QToolButton* typingButton() const { return m_typing; }   // typing keyboard -> piano keyboard toggle (owned by MainWindow's action)
    void setHint(const QString& text);
protected:
    bool eventFilter(QObject* o, QEvent* e) override;
private:
    void rebuildPatternCombo();
    App& m_app; QToolButton *m_play, *m_stop, *m_record, *m_loop, *m_metro, *m_countIn, *m_punch, *m_loopRec, *m_typing; ModeSwitch* m_mode; QLabel *m_hint, *m_timeSec; DragValue *m_bpm, *m_tsNum, *m_tsDen; QComboBox *m_snap, *m_pattern, *m_recMode; QLabel *m_position, *m_stats, *m_midiLed; Knob* m_masterVol; QToolButton* m_newPattern;
    int m_lastMidiActivity = 0; bool m_updating = false; QHBoxLayout* m_winLayout;
};

} // namespace mt
