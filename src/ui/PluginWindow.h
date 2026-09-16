// Editor windows for instruments, effects and hosted plugins. Generic parameter editor + custom native editors + plugin GUIs.
#pragma once
#include "ui/App.h"
#include "ui/widgets/Controls.h"
#include <QWidget>
#include <QScrollArea>
#include <QLabel>
#include <QToolButton>
#include <QTimer>
#include <functional>
class QComboBox; class QCheckBox;

namespace mt {

// Knob/combo/toggle grid for a ParameterSet. 'filter' decides which parameters are shown.
class GenericParamPanel : public QWidget {
    Q_OBJECT
public:
    GenericParamPanel(App& app, Id ownerId, IProcessor* proc, std::function<bool(const ParamDesc&)> filter = nullptr, int columns = 6, QWidget* parent = nullptr);
    void sync();   // pull values from the processor (automation / plugin changes)
private:
    struct Row { uint32_t id; Knob* knob = nullptr; QLabel* value = nullptr; QComboBox* combo = nullptr; QCheckBox* check = nullptr; };
    App& m_app; Id m_owner; IProcessor* m_proc; std::vector<Row> m_rows; bool m_updating = false;
};

class PluginWindow : public QWidget {
    Q_OBJECT
public:
    PluginWindow(App& app, Id ownerId, const QString& title, QWidget* parent = nullptr);
    Id ownerId() const { return m_owner; }
    void refreshTitle(const QString& t);
protected:
    void closeEvent(QCloseEvent*) override;
private:
    void build(); void tick(); void showPresetMenu();
    App& m_app; Id m_owner; IProcessor* m_proc = nullptr; QWidget* m_body = nullptr; GenericParamPanel* m_generic = nullptr; QLabel* m_title; QToolButton *m_preset, *m_bypass, *m_genericToggle; QTimer m_timer; bool m_useGeneric = false; QString m_titleText;
};

// Editor for an audio clip: trim, gain, fades, stretch, pitch, reverse, normalize, slices
class AudioClipEditor : public QWidget {
    Q_OBJECT
public:
    AudioClipEditor(App& app, Id clipId, QWidget* parent = nullptr);
private:
    App& m_app; Id m_clip;
};

} // namespace mt
