// Mixer: strips with insert effect chains, sends, routing, faders, pans and meters. Master strip is always visible.
#pragma once
#include "ui/App.h"
#include "ui/widgets/Controls.h"
#include <QWidget>
#include <set>
#include <QScrollArea>
#include <QLabel>
#include <QToolButton>
#include <QDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>

class QToolButton;
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
    void paintEvent(QPaintEvent*) override; void mousePressEvent(QMouseEvent*) override; void mouseDoubleClickEvent(QMouseEvent*) override; void mouseMoveEvent(QMouseEvent*) override; void mouseReleaseEvent(QMouseEvent*) override; void wheelEvent(QWheelEvent*) override; void leaveEvent(QEvent*) override { m_hoverSlot = -1; update(); }
    void dragEnterEvent(QDragEnterEvent*) override; void dragMoveEvent(QDragMoveEvent*) override; void dropEvent(QDropEvent*) override;
private:
    int slotAt(int y) const { return (y / 18) + m_scroll; } void pick(int slot); void menu(int slot, const QPoint& global);
    App& m_app; Id m_track; int m_scroll = 0; int m_dragSlot = -1; QPoint m_pressPos; bool m_dragging = false; int m_hoverSlot = -1;
};

// Bottom "port" of a strip: the node the routing wires attach to. Click = output routing, right-click = sends.
class RoutePort : public QWidget {
    Q_OBJECT
public:
    explicit RoutePort(QWidget* parent = nullptr) : QWidget(parent) { setFixedHeight(42); setCursor(Qt::PointingHandCursor); setMouseTracking(true); }
    QPoint nodeCenter() const { return QPoint(width() / 2, 24); }   // label above, node in the middle, wire lane below
    void setState(const QString& text, const QColor& color, bool lit, bool hasSends) { m_text = text; m_color = color; m_lit = lit; m_sends = hasSends; update(); }
Q_SIGNALS:
    void routeClicked(); void sendsClicked();
protected:
    void paintEvent(QPaintEvent*) override; void mousePressEvent(QMouseEvent* e) override { if (e->button() == Qt::LeftButton) Q_EMIT routeClicked(); else if (e->button() == Qt::RightButton) Q_EMIT sendsClicked(); }
private:
    QString m_text; QColor m_color; bool m_lit = false, m_sends = false;
};

// Transparent overlay over the whole mixer drawing the wires of the selected track (output + sends, and what feeds it)
class MixerWires : public QWidget {
    Q_OBJECT
public:
    explicit MixerWires(QWidget* parent = nullptr) : QWidget(parent) { setAttribute(Qt::WA_TransparentForMouseEvents); setAttribute(Qt::WA_NoSystemBackground); }
    struct Wire { QPoint from, to; QColor color; bool dashed; bool intoSelected; };
    void setWires(std::vector<Wire> w) { m_wires = std::move(w); update(); }
protected:
    void paintEvent(QPaintEvent*) override;
private:
    std::vector<Wire> m_wires;
};

class MixerStrip : public QWidget {
    Q_OBJECT
public:
    MixerStrip(App& app, Id trackId, QWidget* parent = nullptr);
    Id trackId() const { return m_track; }
    RoutePort* port() const { return m_port; }
    void refresh(); void uiTick(); void setSelected(bool s) { m_selected = s; update(); }
Q_SIGNALS:
    void selected(Id trackId, Qt::KeyboardModifiers mods); void openEditor(Id slotId, const QString& title);
protected:
    void paintEvent(QPaintEvent*) override; void mousePressEvent(QMouseEvent*) override; void mouseDoubleClickEvent(QMouseEvent*) override; void contextMenuEvent(QContextMenuEvent*) override;
private:
    void showRouteMenu(); void showSendsMenu();
    App& m_app; Id m_track; bool m_selected = false; bool m_updating = false;
    QLabel* m_name; QToolButton *m_input, *m_route, *m_sends; RoutePort* m_port = nullptr; EffectSlotList* m_slots; Knob* m_pan; Fader* m_fader; LevelMeter* m_meter; LedButton *m_mute, *m_solo, *m_arm; QLabel* m_latency; float m_faderStart = 0, m_panStart = 0;
};

class MixerPanel : public QWidget {
    Q_OBJECT
public:
    explicit MixerPanel(App& app, QWidget* parent = nullptr);
    void refresh(uint32_t flags); void uiTick();
    Id selectedTrack() const { return m_selected; } void selectTrack(Id id); void selectTrack(Id id, Qt::KeyboardModifiers mods);
    std::vector<Id> selectedTracks() const;   // every highlighted strip (primary first)
protected:
    void resizeEvent(QResizeEvent*) override;
public:
    void deleteSelectedTracks(); void selectAllTracks();
    QSize sizeHint() const override { return {900, 340}; } QSize minimumSizeHint() const override { return {300, 200}; }
Q_SIGNALS:
    void openEditor(Id slotId, const QString& title);
private:
    void rebuild();
    App& m_app; QScrollArea* m_scroll; QWidget* m_stripsHost; std::vector<MixerStrip*> m_strips; MixerStrip* m_master = nullptr; QToolButton* m_addBtn = nullptr; MixerWires* m_wires = nullptr; Id m_selected = 0; std::set<Id> m_multi;
    void updateWires();
};

} // namespace mt
