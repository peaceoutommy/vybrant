// Editor windows for instruments, effects and hosted plugins. Generic parameter editor + custom native editors + plugin GUIs.
#pragma once
#include "ui/App.h"
#include "ui/widgets/Controls.h"
#include <QWidget>
#include <QScrollArea>
#include <QLabel>
#include <QToolButton>
#include <QTimer>
#include <QPointer>
#include <functional>
class QComboBox;
class QCheckBox;

namespace mt {

// Knob/combo/toggle grid for a ParameterSet. 'filter' decides which parameters are shown.
class GenericParamPanel : public QWidget {
    Q_OBJECT
public:
    // cellWidth > 0: every cell gets that fixed width (choices span two cells), so panels stacked in one column line up
    GenericParamPanel(App& app, Id ownerId, IProcessor* proc, std::function<bool(const ParamDesc&)> filter = nullptr, int columns = 6,
        QWidget* parent = nullptr, int cellWidth = 0);
    static constexpr int kCellH = 40 + 2 * 14 + 2;   // knob 40 + name + value
    static constexpr int kCellW = 92;   // shared by the sampler and audio clip editors so their knob grids match
    void sync();   // pull values from the processor (automation / plugin changes)
Q_SIGNALS:
    void edited();   // the user changed a value here (knob drag, combo, checkbox)
private:
    struct Row {
        uint32_t id;
        Knob* knob = nullptr;
        QLabel* value = nullptr;
        QComboBox* combo = nullptr;
        QCheckBox* check = nullptr;
    };
    App& m_app;
    Id m_owner;
    IProcessor* m_proc;
    std::vector<Row> m_rows;
    bool m_updating = false;
};

class PluginWindow : public QWidget {
    Q_OBJECT
public:
    PluginWindow(App& app, Id ownerId, const QString& title, QWidget* parent = nullptr);
    Id ownerId() const { return m_owner; }
    void refreshTitle(const QString& t);

protected:
    void closeEvent(QCloseEvent*) override;
    // hidden by a workspace click: no UI sync (each one can be a plugin IPC round trip) until shown again
    void hideEvent(QHideEvent*) override { m_timer.stop(); }
    void showEvent(QShowEvent*) override {
        if (!m_timer.isActive()) m_timer.start(33);
    }

private:
    void build();
    void tick();
    void showPresetMenu();
    Id pluginInstanceId() const;   // of the channel or effect slot this window edits (0 = native processor)
    // a picture of the GUI for the picker: captured every 1.5 s until two in a row look the same (the GUI has drawn)
    void grabThumbnail(int gen);
    void offerThumbnail(const QImage& img);
    Q_SLOT void portalScreenshot(uint code, const QVariantMap& results);
    struct {
        QPointer<QWidget> editor;
        QString file;
        int gen = 0, attempt = 0;
        QImage last;   // the previous capture, small
    } m_thumb;
    App& m_app;
    Id m_owner;
    IProcessor* m_proc = nullptr;
    QWidget* m_body = nullptr;
    GenericParamPanel* m_generic = nullptr;
    QLabel* m_title;
    QToolButton *m_preset, *m_bypass, *m_genericToggle, *m_bridge;
    QTimer m_timer;
    bool m_useGeneric = false;
    QString m_titleText;
};

// Editor for an audio clip, laid out like the sampler editor: waveform (click sets the start), knob groups, toggles
class AudioClipEditor : public QWidget {
    Q_OBJECT
public:
    AudioClipEditor(App& app, Id clipId, QWidget* parent = nullptr);
    // sample-only details (waveform, play, normalize, trims) for a project sample no clip uses yet
    AudioClipEditor(App& app, Id clipId, QWidget* parent, Id sourceId);

private:
    App& m_app;
    Id m_clip;
    bool m_previewing = false;
    bool m_lengthDirty = false;   // the next commit also writes m_draft.length (set by Fit tempo)
    Clip m_draft;   // working copy the knobs edit live; committed to the project on release
    AudioSource m_draftSrc;
};

}   // namespace mt
