// Mixer: strips with insert effect chains, sends, routing, faders, pans and meters. Master strip is always visible.
#pragma once
#include "ui/App.h"
#include "ui/widgets/Controls.h"
#include <QWidget>
#include <QScrollArea>
#include <QLabel>
#include <QToolButton>
#include <QDialog>
#include <QLineEdit>
#include <QListWidget>

namespace mt {

// Searchable picker for native effects/instruments and scanned plugins
class ProcessorPicker : public QDialog {
    Q_OBJECT
public:
    ProcessorPicker(App& app, ProcessorKind kind, QWidget* parent = nullptr);
    QString nativeKey() const { return m_nativeKey; } const PluginInfo* plugin() const { return m_plugin; }
private:
    void rebuild(const QString& filter);
    App& m_app; ProcessorKind m_kind; QLineEdit* m_search; QListWidget* m_list; QString m_nativeKey; const PluginInfo* m_plugin = nullptr;
};

class EffectSlotList : public QWidget {
    Q_OBJECT
public:
    EffectSlotList(App& app, Id trackId, QWidget* parent = nullptr);
    void refresh(); QSize sizeHint() const override { return {90, 6 * 18}; }
Q_SIGNALS:
    void openEditor(Id slotId, const QString& title);
protected:
    void paintEvent(QPaintEvent*) override; void mousePressEvent(QMouseEvent*) override; void mouseDoubleClickEvent(QMouseEvent*) override; void mouseMoveEvent(QMouseEvent*) override; void mouseReleaseEvent(QMouseEvent*) override; void wheelEvent(QWheelEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override; void dragMoveEvent(QDragMoveEvent*) override; void dropEvent(QDropEvent*) override;
private:
    int slotAt(int y) const { return (y / 18) + m_scroll; } void pick(int slot); void menu(int slot, const QPoint& global);
    App& m_app; Id m_track; int m_scroll = 0; int m_dragSlot = -1; QPoint m_pressPos; bool m_dragging = false;
};

class MixerStrip : public QWidget {
    Q_OBJECT
public:
    MixerStrip(App& app, Id trackId, QWidget* parent = nullptr);
    Id trackId() const { return m_track; }
    void refresh(); void uiTick(); void setSelected(bool s) { m_selected = s; update(); }
Q_SIGNALS:
    void selected(Id trackId); void openEditor(Id slotId, const QString& title);
protected:
    void paintEvent(QPaintEvent*) override; void mousePressEvent(QMouseEvent*) override; void mouseDoubleClickEvent(QMouseEvent*) override; void contextMenuEvent(QContextMenuEvent*) override;
private:
    void showRouteMenu(); void showSendsMenu();
    App& m_app; Id m_track; bool m_selected = false; bool m_updating = false;
    QLabel* m_name; QToolButton *m_input, *m_route, *m_sends; EffectSlotList* m_slots; Knob* m_pan; Fader* m_fader; LevelMeter* m_meter; LedButton *m_mute, *m_solo, *m_arm; QLabel* m_latency; float m_faderStart = 0, m_panStart = 0;
};

class MixerPanel : public QWidget {
    Q_OBJECT
public:
    explicit MixerPanel(App& app, QWidget* parent = nullptr);
    void refresh(uint32_t flags); void uiTick();
    Id selectedTrack() const { return m_selected; } void selectTrack(Id id);
    QSize sizeHint() const override { return {900, 340}; } QSize minimumSizeHint() const override { return {300, 200}; }
Q_SIGNALS:
    void openEditor(Id slotId, const QString& title);
private:
    void rebuild();
    App& m_app; QScrollArea* m_scroll; QWidget* m_stripsHost; std::vector<MixerStrip*> m_strips; MixerStrip* m_master = nullptr; Id m_selected = 0;
};

} // namespace mt
