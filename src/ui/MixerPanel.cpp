#include "ui/MixerPanel.h"
#include "ui/InAppDrag.h"
#include <QMessageBox>
#include <QProgressDialog>
#include <QShortcut>
#include "ui/Theme.h"
#include "dsp/NativeRegistry.h"
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMenu>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QScrollBar>

namespace mt {

// A typed name sticks to the track; an empty one hands it back to the routed channel's name (App::syncMixerTrackNames)
static void renameTrack(MixerTrack& t, const QString& name) {
    t.customName = !name.trimmed().isEmpty();
    if (t.customName) t.name = name.trimmed();
}

// ---------------------------------------------------------------- picker
ProcessorPicker::ProcessorPicker(App& app, ProcessorKind kind, QWidget* parent)
    : QDialog(parent)
    , m_app(app)
    , m_kind(kind) {
    setWindowTitle(kind == ProcessorKind::Instrument ? "Choose instrument" : "Choose effect");
    resize(360, 460);
    auto* lay = new QVBoxLayout(this);
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText("Search...");
    lay->addWidget(m_search);
    m_list = new QListWidget(this);
    lay->addWidget(m_list, 1);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    lay->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& t) { rebuild(t); });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) { accept(); });
    connect(this, &QDialog::accepted, this, [this] {
        auto* it = m_list->currentItem();
        if (!it) return;
        QString v = it->data(Qt::UserRole).toString();
        if (v.startsWith("native:")) m_nativeKey = v.mid(7);
        else if (v.startsWith("plugin:")) {
            for (auto& p : m_app.pluginDb().plugins())
                if (p.key() == v.mid(7)) m_plugin = &p;
        }
    });
    rebuild(QString());
    m_search->setFocus();
}
void ProcessorPicker::rebuild(const QString& f) {
    m_list->clear();
    for (auto& i : NativeRegistry::instance().all()) {
        if (i.kind != m_kind) continue;
        if (!f.isEmpty() && !i.name.contains(f, Qt::CaseInsensitive) && !i.category.contains(f, Qt::CaseInsensitive)) continue;
        auto* it = new QListWidgetItem(QString("%1  —  %2").arg(i.name, i.category));
        it->setData(Qt::UserRole, "native:" + i.key);
        m_list->addItem(it);
    }
    for (auto& p : m_app.pluginDb().plugins()) {
        if (p.failed || p.hidden) continue;
        bool inst = p.category == "Instrument";
        if (inst != (m_kind == ProcessorKind::Instrument)) continue;
        if (!f.isEmpty() && !p.name.contains(f, Qt::CaseInsensitive) && !p.vendor.contains(f, Qt::CaseInsensitive)) continue;
        auto* it = new QListWidgetItem(QString("%1%2  —  %3 [%4]").arg(p.favorite ? "★ " : "", p.name, p.vendor, p.format.toUpper()));
        it->setData(Qt::UserRole, "plugin:" + p.key());
        it->setForeground(QColor("#ffd27f"));
        m_list->addItem(it);
    }
    if (m_list->count()) m_list->setCurrentRow(0);
}

// ---------------------------------------------------------------- slot list
EffectSlotList::EffectSlotList(App& app, Id trackId, QWidget* parent)
    : QWidget(parent)
    , m_app(app)
    , m_track(trackId) {
    setAcceptDrops(true);
    setMouseTracking(true);
    setMinimumHeight(3 * 18);
    setMaximumHeight(10 * 18 + 2);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
}
void EffectSlotList::refresh() { update(); }
void EffectSlotList::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), theme::surface());
    const MixerTrack* t = m_app.project().mixerTrack(m_track);
    if (!t) return;
    int rows = height() / 18;
    p.setFont(theme::smallFont());
    for (int r = 0; r < rows; ++r) {
        int slot = r + m_scroll;
        QRect rr(1, r * 18 + 1, width() - 2, 16);
        if (slot < (int)t->effects.size()) {
            const EffectSlot& e = t->effects[slot];
            bool crashed = false;
            if (auto* pd = m_app.project().plugin(e.pluginInstanceId)) crashed = pd->crashed || pd->missing;
            bool available = m_app.processor(e.id) != nullptr;
            QColor c = e.bypass ? theme::grid() : (e.pluginInstanceId ? QColor(0x4a, 0x3c, 0x22) : QColor(0x22, 0x3c, 0x30));
            if (crashed || !available) c = QColor(0x4a, 0x24, 0x24);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawRoundedRect(rr, theme::kRadius, theme::kRadius);
            p.setPen(Qt::NoPen);
            p.setBrush(e.bypass ? theme::textDim() : theme::green());
            p.drawEllipse(QRect(rr.left() + 3, rr.top() + 5, 6, 6));
            p.setPen(e.bypass ? theme::textDim() : theme::text());
            p.drawText(rr.adjusted(12, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft,
                p.fontMetrics().elidedText(e.name.isEmpty() ? e.processorKey : e.name, Qt::ElideRight, rr.width() - 14));
            if (m_dragging && m_dragSlot == slot) {
                p.setPen(QPen(theme::accent(), 1));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(rr, theme::kRadius, theme::kRadius);
            }
        } else {
            bool hot = slot == m_hoverSlot;
            p.setPen(QPen(hot ? theme::withAlpha(theme::accent(), 140) : theme::grid(), 1));
            p.setBrush(hot ? theme::withAlpha(theme::text(), 18) : Qt::NoBrush);
            p.drawRoundedRect(rr, theme::kRadius, theme::kRadius);
            if (hot) {
                p.setPen(theme::textDim());
                p.drawText(rr, Qt::AlignCenter, "+");
            }
        }   // empty slot: hover shows where an effect can go (double-click / drop)
    }
}
void EffectSlotList::pick(int slot) {
    ProcessorPicker dlg(m_app, ProcessorKind::Effect, this);
    if (dlg.exec() != QDialog::Accepted) return;
    const MixerTrack* t = m_app.project().mixerTrack(m_track);
    if (!t) return;
    Id existing = slot < (int)t->effects.size() ? t->effects[slot].id : 0;
    if (existing) m_app.removeEffect(m_track, existing);
    Id id = 0;
    if (!dlg.nativeKey().isEmpty()) id = m_app.addEffect(m_track, dlg.nativeKey(), slot);
    else if (dlg.plugin()) id = m_app.addPluginEffect(m_track, *dlg.plugin(), slot);
    if (id) {
        t = m_app.project().mixerTrack(m_track);
        for (auto& e : t->effects)
            if (e.id == id) Q_EMIT openEditor(id, e.name);
    }
}
void EffectSlotList::mousePressEvent(QMouseEvent* e) {
    int slot = slotAt(e->pos().y());
    const MixerTrack* t = m_app.project().mixerTrack(m_track);
    if (!t) return;
    m_pressPos = e->pos();
    if (e->button() == Qt::RightButton) {
        menu(slot, e->globalPosition().toPoint());
        return;
    }
    if (slot < (int)t->effects.size()) {
        if (e->pos().x() < 12) {
            m_app.setEffectBypass(m_track, t->effects[slot].id, !t->effects[slot].bypass);
            return;
        }
        m_dragSlot = slot;
        if (e->button() == Qt::LeftButton) Q_EMIT openEditor(t->effects[slot].id, t->effects[slot].name);
    }
}
void EffectSlotList::mouseMoveEvent(QMouseEvent* e) {
    {
        int hs = e->pos().y() / 18 + m_scroll;
        const MixerTrack* t = m_app.project().mixerTrack(m_track);
        if (!t || hs < (int)t->effects.size()) hs = -1;
        if (hs != m_hoverSlot) {
            m_hoverSlot = hs;
            update();
        }
    }
    if (m_dragSlot < 0 || !(e->buttons() & Qt::LeftButton)) return;
    if ((e->pos() - m_pressPos).manhattanLength() > 6) m_dragging = true;
    if (m_dragging) update();
}
void EffectSlotList::mouseReleaseEvent(QMouseEvent* e) {
    if (m_dragging && m_dragSlot >= 0) {
        const MixerTrack* t = m_app.project().mixerTrack(m_track);
        int target = clamp(slotAt(e->pos().y()), 0, t ? (int)t->effects.size() - 1 : 0);
        if (t && target != m_dragSlot && m_dragSlot < (int)t->effects.size()) m_app.moveEffect(m_track, t->effects[m_dragSlot].id, target);
    }
    m_dragging = false;
    m_dragSlot = -1;
    update();
}
void EffectSlotList::mouseDoubleClickEvent(QMouseEvent* e) {
    int slot = slotAt(e->pos().y());
    const MixerTrack* t = m_app.project().mixerTrack(m_track);
    if (!t) return;
    if (slot >= (int)t->effects.size()) pick(slot);
}
void EffectSlotList::wheelEvent(QWheelEvent* e) {
    const MixerTrack* t = m_app.project().mixerTrack(m_track);
    int rows = height() / 18;
    int maxScroll = t ? std::max(0, (int)t->effects.size() + 1 - rows) : 0;
    m_scroll = clamp(m_scroll + (e->angleDelta().y() > 0 ? -1 : 1), 0, maxScroll);
    update();
    e->accept();
}
void EffectSlotList::menu(int slot, const QPoint& global) {
    const MixerTrack* t = m_app.project().mixerTrack(m_track);
    if (!t) return;
    QMenu m(this);
    if (slot < (int)t->effects.size()) {
        EffectSlot e = t->effects[slot];
        Id id = e.id;
        m.addAction("Open editor", [this, id, e] { Q_EMIT openEditor(id, e.name); });
        m.addAction(e.bypass ? "Enable" : "Bypass", [this, id, b = !e.bypass] { m_app.setEffectBypass(m_track, id, b); });
        m.addAction("Replace...", [this, slot] { pick(slot); });
        m.addAction("Remove", [this, id] { m_app.removeEffect(m_track, id); });
        m.addSeparator();
        m.addAction("Move up", [this, id, slot] { m_app.moveEffect(m_track, id, std::max(0, slot - 1)); });
        m.addAction("Move down", [this, id, slot] { m_app.moveEffect(m_track, id, slot + 1); });
        m.addAction("Mix (wet/dry)...", [this, id, e] {
            bool ok;
            int v = QInputDialog::getInt(this, "Effect mix", "Wet %:", (int)(e.mix * 100), 0, 100, 1, &ok);
            if (ok) m_app.setEffectMix(m_track, id, v / 100.f);
        });
        if (auto* pd = m_app.project().plugin(e.pluginInstanceId)) {
            if (pd->crashed)
                m.addAction("Reload crashed plugin", [this, pid = e.pluginInstanceId] {
                    m_app.engine().registry().crashFlag(pid).store(false);
                    if (auto* p = m_app.project().plugin(pid)) p->crashed = false;
                    m_app.model().notify(ChangeMixer);
                });
        }
    } else m.addAction("Add effect...", [this, slot] { pick(slot); });
    m.exec(global);
}
void EffectSlotList::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasFormat(kItemMime)) e->acceptProposedAction();   // effects / plugins / presets only, no files
}
void EffectSlotList::dragMoveEvent(QDragMoveEvent* e) { e->acceptProposedAction(); }
void EffectSlotList::dropEvent(QDropEvent* e) {
    auto [kind, val] = dropItem(e->mimeData());
    int slot = slotAt(e->position().toPoint().y());
    const MixerTrack* t = m_app.project().mixerTrack(m_track);
    if (!t) return;
    if (slot > (int)t->effects.size()) slot = -1;
    if (kind == "native") {
        auto* info = NativeRegistry::instance().info(val);
        if (info && info->kind == ProcessorKind::Effect) m_app.addEffect(m_track, val, slot);
    } else if (kind == "plugin") {
        for (auto& p : m_app.pluginDb().plugins())
            if (p.key() == val && p.category != "Instrument") m_app.addPluginEffect(m_track, p, slot);
    } else if (kind == "preset" && slot >= 0 && slot < (int)t->effects.size()) m_app.loadPreset(t->effects[slot].id, val);
    e->acceptProposedAction();
}

// ---------------------------------------------------------------- port & wires
void RoutePort::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QPoint c = nodeCenter();
    QColor col = m_lit ? theme::accent() : m_color;
    p.setPen(QPen(theme::border(), 1));
    p.setBrush(theme::surface());
    p.drawRoundedRect(QRectF(2, 0, width() - 4, height() - 1), 3, 3);
    p.setPen(QPen(col.darker(m_lit ? 100 : 130), 1.5));
    p.setBrush(m_lit ? col : theme::withAlpha(col, 90));
    p.drawEllipse(QRectF(c.x() - 5, c.y() - 5, 10, 10));   // the node
    if (m_sends) {
        p.setPen(Qt::NoPen);
        p.setBrush(theme::accent2());
        p.drawEllipse(QRectF(c.x() + 7, c.y() - 7, 5, 5));
    }   // has sends
    p.setPen(m_lit ? theme::text() : theme::textDim());
    p.setFont(theme::smallFont());
    if (!m_text.isEmpty())
        p.drawText(
            QRect(2, 3, width() - 4, 12), Qt::AlignCenter, p.fontMetrics().elidedText("\u2192 " + m_text, Qt::ElideRight, width() - 8));
}
void MixerWires::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    for (auto& w : m_wires) {   // wires leave the node downwards, run along the bottom and come back up into the target node
        int dy = 9 + std::min(8, std::abs(w.to.x() - w.from.x()) / 60);
        QPainterPath path(w.from);
        path.cubicTo(w.from + QPoint(0, dy), w.to + QPoint(0, dy), w.to);
        QPen pen(w.color, w.intoSelected ? 1.2 : 2.0);
        if (w.dashed) pen.setStyle(Qt::DashLine);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.setPen(Qt::NoPen);
        p.setBrush(w.color);
        p.drawEllipse(QRectF(w.to.x() - 3, w.to.y() - 3, 6, 6));
    }   // arrival dot
}

// ---------------------------------------------------------------- strip
MixerStrip::MixerStrip(App& app, Id trackId, QWidget* parent)
    : QWidget(parent)
    , m_app(app)
    , m_track(trackId) {
    setFixedWidth(96);
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(3, 3, 3, 3);
    lay->setSpacing(3);
    m_name = new QLabel(this);
    m_name->setAlignment(Qt::AlignCenter);
    m_name->setStyleSheet("font-weight:bold; padding:2px; border-radius:3px;");
    m_name->setToolTip("Double-click to rename (empty name: follow the routed channel). Click to select (then right-click another strip to route/send).");
    lay->addWidget(m_name);
    m_input = new QToolButton(this);
    m_input->setText("In: -");
    m_input->setStyleSheet("QToolButton { padding: 1px 4px; }");
    m_input->setToolTip("Hardware input for recording / monitoring");
    m_input->setPopupMode(QToolButton::InstantPopup);
    lay->addWidget(m_input);
    m_slots = new EffectSlotList(app, trackId, this);
    lay->addWidget(m_slots, 3);
    connect(m_slots, &EffectSlotList::openEditor, this, &MixerStrip::openEditor);
    auto* mid = new QHBoxLayout();
    mid->setSpacing(2);
    m_pan = new Knob(this, 26);
    m_pan->setBipolar(true);
    m_pan->setLabel("Pan");
    m_pan->setShowLabel(false);
    m_pan->setDefault(0.5f);
    m_pan->valueText = [](float v) { return QString("Pan %1").arg((v - 0.5f) * 2, 0, 'f', 2); };
    mid->addWidget(m_pan);
    m_latency = theme::dimLabel(QString(), this, "font-size:7pt;");
    m_latency->setToolTip("Latency compensation delay applied to this track");
    mid->addWidget(m_latency);
    lay->addLayout(mid);
    auto* fl = new QHBoxLayout();
    fl->setSpacing(2);
    m_fader = new Fader(this);
    m_fader->valueText = [](float v) { return QString::number(gainToDb(v * 1.25f / 0.8f), 'f', 1) + " dB"; };
    fl->addWidget(m_fader);
    m_meter = new LevelMeter(this);
    fl->addWidget(m_meter);
    lay->addLayout(fl, 2);
    auto* bl = new QHBoxLayout();
    bl->setSpacing(2);
    m_mute = new LedButton("M", theme::red(), this);
    m_solo = new LedButton("S", theme::accent(), this);
    m_arm = new LedButton("R", QColor(0xe2, 0x5c, 0x5c), this);
    m_arm->setToolTip("Arm for recording (needs an input)");
    bl->addWidget(m_mute);
    bl->addWidget(m_solo);
    bl->addWidget(m_arm);
    lay->addLayout(bl);
    m_route = new QToolButton(this);
    m_route->hide();
    m_sends = new QToolButton(this);
    m_sends->hide();   // menus only; the port row below is the visible control
    m_port = new RoutePort(this);
    m_port->setToolTip(
        "Output: click to choose where this track goes. Right-click for sends & sidechains. Select a track to see its wires.");
    lay->addWidget(m_port);
    connect(m_port, &RoutePort::routeClicked, this, [this] { showRouteMenu(); });
    connect(m_port, &RoutePort::sendsClicked, this, [this] { showSendsMenu(); });
    connect(m_pan, &Knob::dragStarted, this, [this] {
        if (auto* t = m_app.project().mixerTrack(m_track)) m_panStart = t->pan;
    });
    connect(m_pan, &Knob::valueChanged, this, [this](float v) {
        if (!m_updating) m_app.setMixerPan(m_track, v * 2 - 1, false);
    });
    connect(m_pan, &Knob::dragFinished, this, [this] {
        if (auto* t = m_app.project().mixerTrack(m_track)) {
            float v = t->pan;
            t->pan = m_panStart;
            m_app.setMixerPan(m_track, v, true);
        }
    });
    connect(m_pan, &Knob::contextRequested, this, [this](const QPoint& g) {
        QMenu m;
        m.addAction("Create automation clip", [this] { m_app.createAutomationLane({ParamTarget::MixerPan, m_track, 0, 0}, QString()); });
        m.addAction("Link to MIDI controller...",
            [this] { m_app.startMidiLearn({ParamTarget::MixerPan, m_track, 0, 0}, m_name->text() + " Pan"); });
        m.exec(g);
    });
    connect(m_fader, &Fader::dragStarted, this, [this] {
        if (auto* t = m_app.project().mixerTrack(m_track)) m_faderStart = t->volume;
    });
    connect(m_fader, &Fader::valueChanged, this, [this](float v) {
        if (!m_updating) m_app.setMixerVolume(m_track, v * 1.25f, false);
    });
    connect(m_fader, &Fader::dragFinished, this, [this] {
        if (auto* t = m_app.project().mixerTrack(m_track)) {
            float v = t->volume;
            t->volume = m_faderStart;
            m_app.setMixerVolume(m_track, v, true);
        }
    });
    connect(m_fader, &Fader::contextRequested, this, [this](const QPoint& g) {
        QMenu m;
        m.addAction("Create automation clip", [this] { m_app.createAutomationLane({ParamTarget::MixerVolume, m_track, 0, 0}, QString()); });
        m.addAction("Link to MIDI controller...",
            [this] { m_app.startMidiLearn({ParamTarget::MixerVolume, m_track, 0, 0}, m_name->text() + " Volume"); });
        m.addAction("Reset to 0 dB", [this] { m_app.setMixerVolume(m_track, 0.8f, true); });
        m.exec(g);
    });
    connect(m_mute, &LedButton::toggled, this,
        [this](bool b) { m_app.edit<MixerTrack>(m_track, b ? "Mute track" : "Unmute track", [b](MixerTrack& t) { t.mute = b; }); });
    connect(m_solo, &LedButton::toggled, this,
        [this](bool b) { m_app.edit<MixerTrack>(m_track, "Solo track", [b](MixerTrack& t) { t.solo = b; }); });
    connect(m_arm, &LedButton::toggled, this,
        [this](bool b) { m_app.edit<MixerTrack>(m_track, b ? "Arm track" : "Disarm track", [b](MixerTrack& t) { t.armed = b; }); });
    connect(m_route, &QToolButton::clicked, this, [this] { showRouteMenu(); });
    connect(m_sends, &QToolButton::clicked, this, [this] { showSendsMenu(); });
    connect(m_input, &QToolButton::clicked, this, [this] {
        QMenu m;
        auto* none = m.addAction("None",
            [this] { m_app.edit<MixerTrack>(m_track, "Track input", [](MixerTrack& t) { t.inputChannel = -1, t.armed = false; }); });
        none->setCheckable(true);
        const auto& cfg = m_app.devices().config();
        const MixerTrack* t = m_app.project().mixerTrack(m_track);
        none->setChecked(t && t->inputChannel < 0);
        // FL: every input on its own (mono) and the stereo pairs; picking one arms the track
        auto add = [&](int i, bool stereo) {
            auto* a = m.addAction(
                stereo ? QString("Input %1+%2 (stereo)").arg(i + 1).arg(i + 2) : QString("Input %1 (mono)").arg(i + 1), [this, i, stereo] {
                    m_app.edit<MixerTrack>(
                        m_track, "Track input", [i, stereo](MixerTrack& t) { t.inputChannel = i, t.inputStereo = stereo, t.armed = true; });
                });
            a->setCheckable(true);
            a->setChecked(t && t->inputChannel == i && t->inputStereo == stereo);
        };
        for (int i = 0; i < cfg.inputChannels; ++i) add(i, false);
        if (cfg.inputChannels > 1) m.addSeparator();
        for (int i = 0; i + 1 < cfg.inputChannels; i += 2) add(i, true);
        if (cfg.inputChannels == 0) m.addAction("(no input device - see Audio Settings)")->setEnabled(false);
        m.exec(m_input->mapToGlobal(QPoint(0, m_input->height())));
    });
    refresh();
}
void MixerStrip::refresh() {
    const MixerTrack* t = m_app.project().mixerTrack(m_track);
    if (!t) return;
    m_updating = true;
    m_name->setText(t->freezeTrackId ? t->name + " \u2744" : t->name);   // snowflake: frozen
    // Kept as a theme template so a live theme change re-applies it; the track colour is this strip's own, not the theme's.
    theme::styled(m_name,
        QString("font-weight:bold; font-size:8pt; padding:2px; border-radius:3px; background:%1; color:%2; border-bottom: 2px solid %3;")
            .arg(m_selected ? "@hover@" : "@panel@", m_selected ? "@accent@" : "@text@", t->color.name()));
    m_input->setText(t->inputChannel < 0 ? "In: -"
            : t->inputStereo             ? QString("In %1+%2").arg(t->inputChannel + 1).arg(t->inputChannel + 2)
                                         : QString("In %1").arg(t->inputChannel + 1));
    m_pan->setValue((t->pan + 1) * 0.5f);
    m_fader->setValue(t->volume / 1.25f);
    m_mute->setChecked(t->mute);
    m_solo->setChecked(t->solo);
    m_arm->setChecked(t->armed);
    const MixerTrack* out = m_app.project().mixerTrack(t->outputTrackId);
    m_route->setText(t->isMaster
            ? "→ Out"
            : QString("→ %1").arg(out ? (out->isMaster ? "Mst" : QString::number(m_app.project().mixerIndex(out->id))) : "?"));
    int ns = (int)t->sends.size();
    m_sends->setText(ns ? QString("S%1").arg(ns) : "S");
    if (m_port)
        m_port->setState(t->isMaster ? QString() : (out && !out->isMaster ? out->name : QString()), t->color, m_selected,
            ns > 0);   // label only for non-default routing
    auto& tc = m_app.engine().registry().track(m_track);
    int d = tc.generatorDelay.delay;
    m_latency->setText(d > 0 ? QString("+%1").arg(d) : "");
    m_slots->refresh();
    m_updating = false;
    update();
}
void MixerStrip::uiTick() {
    auto* tc = m_app.engine().registry().trackIfExists(m_track);
    if (!tc) return;
    bool clip = tc->meter.clipped.exchange(false);
    m_meter->setLevels(tc->meter.peakL.load(), tc->meter.peakR.load(), clip);
    m_updating = true;
    m_fader->setValue(tc->volume.load() / 1.25f);
    m_pan->setValue((tc->pan.load() + 1) * 0.5f);
    m_updating = false;
}
void MixerStrip::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(theme::border(), 1));
    p.setBrush(m_selected ? theme::hover() : theme::panel());
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), theme::kRadius, theme::kRadius);   // each strip is a rounded card
}
void MixerStrip::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) Q_EMIT selected(m_track, e->modifiers());
    QWidget::mousePressEvent(e);
}
void MixerStrip::mouseDoubleClickEvent(QMouseEvent* e) {
    if (m_name->geometry().contains(e->pos())) {
        const MixerTrack* t = m_app.project().mixerTrack(m_track);
        if (auto n = askName(this, "Rename track", t ? t->name : "", true))
            m_app.edit<MixerTrack>(m_track, "Rename mixer track", [n = *n](MixerTrack& x) { renameTrack(x, n); });
    }
}
void MixerStrip::showRouteMenu() {
    QMenu m;
    const MixerTrack* t = m_app.project().mixerTrack(m_track);
    if (!t || t->isMaster) {
        m.addAction("Master goes to the audio device")->setEnabled(false);
        m.exec(m_port->mapToGlobal(QPoint(0, m_port->height())));
        return;
    }
    for (auto& o : m_app.project().mixerTracks) {
        if (o.id == m_track) continue;
        auto* a = m.addAction(o.name, [this, id = o.id] { m_app.routeTrack(m_track, id); });
        a->setCheckable(true);
        a->setChecked(o.id == t->outputTrackId);
    }
    m.exec(m_port->mapToGlobal(QPoint(0, m_port->height())));
}
void MixerStrip::showSendsMenu() {
    QMenu m;
    const MixerTrack* t = m_app.project().mixerTrack(m_track);
    if (!t) return;
    for (auto& s : t->sends) {
        const MixerTrack* d = m_app.project().mixerTrack(s.targetTrackId);
        QString label = QString("%1 %2  %3 %4")
                            .arg(s.sidechain ? "SC →" : "→", d ? d->name : "?", QString::number((int)(s.level * 100)) + "%",
                                s.preFader ? "(pre)" : "(post)");
        auto* sub = m.addMenu(label);
        Id to = s.targetTrackId;
        sub->addAction("Level...", [this, to, lv = s.level] {
            bool ok;
            int v = QInputDialog::getInt(this, "Send level", "%:", (int)(lv * 100), 0, 100, 1, &ok);
            if (ok) m_app.setSendLevel(m_track, to, v / 100.f, true);
        });
        sub->addAction(s.preFader ? "Make post-fader" : "Make pre-fader", [this, to] {
            m_app.edit<MixerTrack>(m_track, "Send pre/post", [to](MixerTrack& x) {
                for (auto& sd : x.sends)
                    if (sd.targetTrackId == to) sd.preFader = !sd.preFader;
            });
        });
        sub->addAction(s.sidechain ? "Make normal send" : "Make sidechain", [this, to] {
            m_app.edit<MixerTrack>(m_track, "Send sidechain", [to](MixerTrack& x) {
                for (auto& sd : x.sends)
                    if (sd.targetTrackId == to) sd.sidechain = !sd.sidechain;
            });
        });
        sub->addAction("Automate level", [this, to] { m_app.createAutomationLane({ParamTarget::MixerSend, m_track, to, 0}, QString()); });
        sub->addAction("Remove", [this, to] { m_app.removeSend(m_track, to); });
    }
    if (!t->sends.empty()) m.addSeparator();
    auto* add = m.addMenu("Add send to");
    auto* sc = m.addMenu("Add sidechain to");
    for (auto& o : m_app.project().mixerTracks) {
        if (o.id == m_track || o.isMaster) continue;
        add->addAction(o.name, [this, id = o.id] { m_app.addSend(m_track, id, false); });
        sc->addAction(o.name, [this, id = o.id] { m_app.addSend(m_track, id, true); });
    }
    m.exec(m_port->mapToGlobal(QPoint(0, m_port->height())));
}
void MixerStrip::contextMenuEvent(QContextMenuEvent* e) {
    QMenu m;
    const MixerTrack* t = m_app.project().mixerTrack(m_track);
    if (!t) return;
    auto* panel = qobject_cast<MixerPanel*>(
        parentWidget() ? parentWidget()->parentWidget() ? parentWidget()->parentWidget()->parentWidget() : nullptr : nullptr);
    Id sel = 0;
    {
        QWidget* w = this;
        while (w && !qobject_cast<MixerPanel*>(w)) w = w->parentWidget();
        if (auto* mp = qobject_cast<MixerPanel*>(w)) sel = mp->selectedTrack();
    }
    (void)panel;
    if (sel && sel != m_track) {
        const MixerTrack* s = m_app.project().mixerTrack(sel);
        QString sn = s ? s->name : "selected";
        m.addAction(QString("Route '%1' output to here").arg(sn), [this, sel] { m_app.routeTrack(sel, m_track); });
        m.addAction(QString("Send '%1' to here").arg(sn), [this, sel] { m_app.addSend(sel, m_track, false); });
        m.addAction(QString("Sidechain '%1' to here").arg(sn), [this, sel] { m_app.addSend(sel, m_track, true); });
        m.addSeparator();
    }
    m.addAction("Rename...", [this, t] {
        if (auto n = askName(this, "Rename track", t->name, true))
            m_app.edit<MixerTrack>(m_track, "Rename mixer track", [n = *n](MixerTrack& x) { renameTrack(x, n); });
    });
    m.addAction("Color...", [this, t] {
        QColor c = QColorDialog::getColor(t->color, this);
        if (c.isValid()) m_app.edit<MixerTrack>(m_track, "Track color", [c](MixerTrack& x) { x.color = c; });
    });
    m.addAction("Add mixer track", [this] { m_app.addMixerTrack(); });
    {
        MixerPanel* mp = nullptr;
        {
            QWidget* w = this;
            while (w && !(mp = qobject_cast<MixerPanel*>(w))) w = w->parentWidget();
        }
        std::vector<Id> many = mp ? mp->selectedTracks() : std::vector<Id>{};
        int deletable = 0;
        for (Id id : many)
            if (auto* x = m_app.project().mixerTrack(id))
                if (!x->isMaster) ++deletable;
        if (mp && deletable > 1 && std::find(many.begin(), many.end(), m_track) != many.end()) {
            m.addAction(QString("Delete %1 selected tracks").arg(deletable), [mp] { mp->deleteSelectedTracks(); });
            m.addAction("Color selected tracks...", [this, many, t] {
                QColor c = QColorDialog::getColor(t->color, this);
                if (!c.isValid()) return;
                for (Id id : many) m_app.edit<MixerTrack>(id, "Track color", [c](MixerTrack& x) { x.color = c; });
            });
        } else if (!t->isMaster) m.addAction("Delete track", [this] { m_app.deleteMixerTrack(m_track); });
    }
    if (t->freezeTrackId) m.addAction("Unfreeze", [this] { m_app.unfreezeMixerTrack(m_track); });
    else if (!t->isMaster)
        m.addAction("Freeze (render to audio)", [this] {
             QProgressDialog dlg("Freezing...", "Cancel", 0, 1000, this);
             dlg.setWindowModality(Qt::WindowModal);
             QString err;
             bool ok = m_app.freezeMixerTrack(
                 m_track,
                 [&](double v) {
                     dlg.setValue((int)(v * 1000));
                     return !dlg.wasCanceled();
                 },
                 &err);
             if (!ok && !dlg.wasCanceled()) QMessageBox::warning(this, "Freeze failed", err);
         })->setToolTip("Renders this track (after its effects, before its fader) to audio and stops running its instruments and effects");
    m.addAction("Reset clip indicator", [this] { m_meter->resetClip(); });
    m.exec(e->globalPos());
}

// ---------------------------------------------------------------- panel
MixerPanel::MixerPanel(App& app, QWidget* parent)
    : QWidget(parent)
    , m_app(app) {
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(2, 2, 2, 2);
    lay->setSpacing(4);
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_stripsHost = new QWidget(this);
    auto* hl = new QHBoxLayout(m_stripsHost);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(2);
    // "+" column after the last insert: adds a mixer track (also in the strips' context menu and Edit > Add mixer track)
    m_addBtn = new QToolButton(m_stripsHost);
    m_addBtn->setText("+");
    m_addBtn->setToolTip("Add a mixer track");
    m_addBtn->setFixedWidth(28);
    m_addBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    m_addBtn->setFocusPolicy(Qt::NoFocus);
    connect(m_addBtn, &QToolButton::clicked, this, [this] {
        Id id = m_app.addMixerTrack();
        selectTrack(id);
        QTimer::singleShot(0, this, [this] { m_scroll->horizontalScrollBar()->setValue(m_scroll->horizontalScrollBar()->maximum()); });
    });
    hl->addWidget(m_addBtn);
    hl->addStretch(1);
    m_scroll->setWidget(m_stripsHost);
    lay->addWidget(m_scroll, 1);
    m_stripsHost->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_stripsHost, &QWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        if (m_stripsHost->childAt(p)) return;
        QMenu m;
        m.addAction("Add mixer track", [this] {
            Id id = m_app.addMixerTrack();
            selectTrack(id);
        });
        m.exec(m_stripsHost->mapToGlobal(p));
    });
    auto* del = new QShortcut(QKeySequence(Qt::Key_Delete), this);
    del->setContext(Qt::WidgetWithChildrenShortcut);
    connect(del, &QShortcut::activated, this, &MixerPanel::deleteSelectedTracks);
    auto* all = new QShortcut(QKeySequence::SelectAll, this);
    all->setContext(Qt::WidgetWithChildrenShortcut);
    connect(all, &QShortcut::activated, this, &MixerPanel::selectAllTracks);
    setFocusPolicy(Qt::ClickFocus);
    m_wires = new MixerWires(this);
    m_wires->raise();
    connect(m_scroll->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] { updateWires(); });
    rebuild();
}
// Wires for the selected track: solid = its output, dashed = its sends (sidechains in red), thin = tracks feeding into it
void MixerPanel::updateWires() {
    if (!m_wires) return;
    m_wires->setGeometry(rect());
    m_wires->raise();
    std::vector<MixerWires::Wire> wires;
    auto& proj = m_app.project();
    auto stripFor = [&](Id id) -> MixerStrip* {
        if (m_master && m_master->trackId() == id) return m_master;
        for (auto* s : m_strips)
            if (s->trackId() == id) return s;
        return nullptr;
    };
    auto node = [&](MixerStrip* s) { return s->port()->mapTo(this, s->port()->nodeCenter()); };
    auto visible = [&](MixerStrip* s) {
        QPoint pt = node(s);
        return s == m_master || (pt.x() >= m_scroll->x() && pt.x() <= m_scroll->x() + m_scroll->viewport()->width());
    };
    MixerStrip* sel = stripFor(m_selected);
    const MixerTrack* t = proj.mixerTrack(m_selected);
    if (sel && t) {   // going to Master is the default and needs no wire; anything else is worth showing
        const MixerTrack* out = proj.mixerTrack(t->outputTrackId);
        if (!t->isMaster && out && !out->isMaster)
            if (MixerStrip* d = stripFor(out->id))
                if (visible(sel) && visible(d)) wires.push_back({node(sel), node(d), theme::accent(), false, false});
        for (auto& sd : t->sends)
            if (MixerStrip* d = stripFor(sd.targetTrackId))
                if (visible(sel) && visible(d))
                    wires.push_back({node(sel), node(d), sd.sidechain ? theme::red() : theme::accent2(), true, false});
    }
    m_wires->setWires(std::move(wires));
}
void MixerPanel::rebuild() {
    for (auto* s : m_strips) delete s;
    m_strips.clear();
    if (m_master) {
        delete m_master;
        m_master = nullptr;
    }
    auto* hl = static_cast<QHBoxLayout*>(m_stripsHost->layout());
    for (auto& t : m_app.project().mixerTracks) {
        if (t.isMaster) continue;
        auto* s = new MixerStrip(m_app, t.id, m_stripsHost);
        hl->insertWidget(hl->indexOf(m_addBtn), s);
        m_strips.push_back(s);
        connect(s, &MixerStrip::selected, this, qOverload<Id, Qt::KeyboardModifiers>(&MixerPanel::selectTrack));
        connect(s, &MixerStrip::openEditor, this, &MixerPanel::openEditor);
    }
    if (Id mid = m_app.project().masterId()) {
        m_master = new MixerStrip(m_app, mid, this);
        static_cast<QHBoxLayout*>(layout())->insertWidget(0, m_master); /* Master is the first strip, on the left */
        m_master->layout()->setContentsMargins(3, 3, 3,
            3 +
                m_scroll->horizontalScrollBar()
                    ->sizeHint()
                    .height()); /* keep its port level with the inserts' (which sit above the scrollbar) */
        connect(m_master, &MixerStrip::selected, this, qOverload<Id, Qt::KeyboardModifiers>(&MixerPanel::selectTrack));
        connect(m_master, &MixerStrip::openEditor, this, &MixerPanel::openEditor);
    }
    if (!m_selected || !m_app.project().mixerTrack(m_selected)) m_selected = m_app.project().masterId();
    for (auto it = m_multi.begin(); it != m_multi.end();) {
        if (!m_app.project().mixerTrack(*it)) it = m_multi.erase(it);
        else ++it;
    }
    m_multi.insert(m_selected);
    for (auto* s : m_strips) s->setSelected(m_multi.count(s->trackId()));
    if (m_master) m_master->setSelected(m_multi.count(m_master->trackId()));
}
void MixerPanel::selectTrack(Id id) { selectTrack(id, Qt::NoModifier); }
// Ctrl+click toggles a strip, Shift+click selects the range from the primary strip; plain click selects one (FL: Ctrl+Shift+click adds)
void MixerPanel::selectTrack(Id id, Qt::KeyboardModifiers mods) {
    if (mods & Qt::ShiftModifier) {
        auto& tr = m_app.project().mixerTracks;
        int a = -1, b = -1;
        for (size_t i = 0; i < tr.size(); ++i) {
            if (tr[i].id == m_selected) a = (int)i;
            if (tr[i].id == id) b = (int)i;
        }
        if (a < 0) a = b;
        if (!(mods & Qt::ControlModifier)) m_multi.clear();
        for (int i = std::min(a, b); i <= std::max(a, b) && i >= 0; ++i) m_multi.insert(tr[i].id);
    } else if (mods & Qt::ControlModifier) {
        if (m_multi.count(id) && m_multi.size() > 1) {
            m_multi.erase(id);
            if (m_selected == id) id = *m_multi.begin();
        } else m_multi.insert(id);
    } else {
        m_multi.clear();
        m_multi.insert(id);
    }
    m_selected = id;
    m_multi.insert(id);
    for (auto* s : m_strips) {
        s->setSelected(m_multi.count(s->trackId()));
        s->refresh();
    }
    if (m_master) {
        m_master->setSelected(m_multi.count(m_master->trackId()));
        m_master->refresh();
    }
    QTimer::singleShot(0, this, [this] { updateWires(); });
}
std::vector<Id> MixerPanel::selectedTracks() const {
    std::vector<Id> v;
    if (m_selected) v.push_back(m_selected);
    for (Id id : m_multi)
        if (id != m_selected) v.push_back(id);
    return v;
}
void MixerPanel::selectAllTracks() {
    m_multi.clear();
    for (auto& t : m_app.project().mixerTracks)
        if (!t.isMaster) m_multi.insert(t.id);
    if (!m_multi.empty()) selectTrack(m_multi.count(m_selected) ? m_selected : *m_multi.begin(), Qt::ControlModifier);
}
void MixerPanel::deleteSelectedTracks() {
    std::vector<Id> ids = selectedTracks();
    int n = 0;
    for (Id id : ids)
        if (auto* t = m_app.project().mixerTrack(id))
            if (!t->isMaster) ++n;
    if (n == 0) return;
    if (n > 1 &&
        QMessageBox::question(this, "Delete mixer tracks",
            QString("Delete %1 mixer tracks? Channels routed to them go back to the Master.").arg(n)) != QMessageBox::Yes)
        return;
    for (Id id : ids) m_app.deleteMixerTrack(id);
    m_multi.clear();
}
void MixerPanel::refresh(uint32_t flags) {
    bool structural = (flags & (ChangeStructure | ChangeAll)) != 0;
    size_t n = 0;
    for (auto& t : m_app.project().mixerTracks)
        if (!t.isMaster) ++n;
    if (structural || n != m_strips.size()) {
        bool same = n == m_strips.size();
        if (same) {
            size_t i = 0;
            for (auto& t : m_app.project().mixerTracks) {
                if (t.isMaster) continue;
                if (m_strips[i++]->trackId() != t.id) {
                    same = false;
                    break;
                }
            }
        }
        if (!same) rebuild();
    }
    for (auto* s : m_strips) s->refresh();
    if (m_master) m_master->refresh();
    QTimer::singleShot(0, this, [this] { updateWires(); });
}
void MixerPanel::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    QTimer::singleShot(0, this, [this] { updateWires(); });
}
void MixerPanel::uiTick() {
    for (auto* s : m_strips) s->uiTick();
    if (m_master) m_master->uiTick();
}

}   // namespace mt
