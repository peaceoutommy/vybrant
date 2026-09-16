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
protected:
    bool eventFilter(QObject* o, QEvent* e) override;
private:
    void rebuildPatternCombo();
    App& m_app; QToolButton *m_play, *m_stop, *m_record, *m_songMode, *m_patMode, *m_loop, *m_metro, *m_countIn, *m_punch, *m_loopRec; DragValue *m_bpm, *m_tsNum, *m_tsDen; QComboBox *m_snap, *m_pattern, *m_recMode; QLabel *m_position, *m_stats, *m_midiLed; Knob* m_masterVol; QToolButton* m_newPattern;
    int m_lastMidiActivity = 0; bool m_updating = false; QHBoxLayout* m_winLayout;
};

} // namespace mt
