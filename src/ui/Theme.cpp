#include <QLabel>
#include "ui/Theme.h"
#include "core/Types.h"
#include <QPixmap>
#include <QPainterPath>
#include <QEvent>
#include <QFrame>
#include <QSettings>
#include <QApplication>
#include <QMap>
#include <QWidget>
#include <QAbstractButton>
#include <cmath>
#include <map>
#include <tuple>

namespace mt::theme {

QWidget* barSeparator(QWidget* parent) {
    auto* f = new QFrame(parent);
    f->setFrameShape(QFrame::VLine);
    f->setStyleSheet("color:#2b3037;");
    f->setFixedHeight(18);
    return f;
}

// ---------------------------------------------------------------- palettes
static Palette mk(const char* id, const char* name, bool dark, const char* bg, const char* panel, const char* panelLight,
    const char* surface, const char* raised, const char* hover, const char* border, const char* grid, const char* gridStrong,
    const char* text, const char* textDim, const char* accent, const char* accent2, const char* green, const char* red,
    const char* playhead, const char* glow = "#aab2c4") {
    Palette p;
    p.id = id;
    p.name = name;
    p.dark = dark;
    p.bg = QColor(bg);
    p.panel = QColor(panel);
    p.panelLight = QColor(panelLight);
    p.surface = QColor(surface);
    p.raised = QColor(raised);
    p.hover = QColor(hover);
    p.border = QColor(border);
    p.grid = QColor(grid);
    p.gridStrong = QColor(gridStrong);
    p.text = QColor(text);
    p.textDim = QColor(textDim);
    p.accent = QColor(accent);
    p.accent2 = QColor(accent2);
    p.green = QColor(green);
    p.red = QColor(red);
    p.playhead = QColor(playhead);
    p.glow = QColor(glow);
    return p;
}

const std::vector<Palette>& presets() {
    static const std::vector<Palette> all = {
        //    id          name        dark  bg         panel      panelLight surface    raised     hover      border     grid       gridStrong text       textDim    accent     accent2    green      red        playhead
        // Vybrant: the website's look - near-black, cool greys, a soft white for "on", the one warm colour kept for the playhead
        mk("vybrant", "Vybrant", true, "#05070a", "#13161b", "#191d23", "#0f1216", "#1b1f26", "#222730", "#232831", "#1a1e24", "#272c35",
            "#eef1f5", "#8b94a1", "#e6e9ee", "#8b9cc4", "#5fc08a", "#e5675c", "#f2a93b"),
        // Obsidian: black glass with a violet undertone in every layer; a luminous blue-violet for "on", a deep amethyst
        // underneath it (clips, wires, the base of checked buttons), violet light at the top bar, a lilac playhead
        mk("obsidian", "Obsidian", true, "#07060b", "#121019", "#191623", "#0b0a11", "#1d1929", "#271f38", "#231d30", "#15121d", "#251f33",
            "#ece8f5", "#8c84a3", "#985cff", "#6d2ed8", "#3ecf9a", "#ff5c7c", "#d9c8ff", "#8c6af5"),
        mk("midnight", "Midnight", true, "#15171b", "#1c1f24", "#24282e", "#111316", "#23272d", "#252930", "#2b3037", "#22262b", "#30363d",
            "#d3d7dd", "#7e8691", "#f0a13a", "#55a0f0", "#5fc074", "#e05a5a", "#ffd266"),
        mk("graphite", "Graphite", true, "#191919", "#212121", "#2a2a2a", "#131313", "#282828", "#2e2e2e", "#333333", "#242424", "#3a3a3a",
            "#dcdcdc", "#8b8b8b", "#c8c8c8", "#7f9cb5", "#6fb37a", "#d96a6a", "#e8e8e8"),
        mk("ocean", "Ocean", true, "#121a21", "#182430", "#1f2f3d", "#0d141a", "#1e2e3c", "#213445", "#25394a", "#1b2a36", "#2c4257",
            "#cfdde8", "#7690a3", "#43c8c0", "#4a9bd8", "#5bbf8a", "#e0666b", "#8ee6df"),
        mk("forest", "Forest", true, "#151a15", "#1c231c", "#242d24", "#101410", "#232c23", "#283228", "#2e392e", "#212a21", "#384538",
            "#d6dcd4", "#7f8c7d", "#9ccb52", "#59b08a", "#6dc26d", "#dd6a5a", "#cfe98a"),
        mk("plum", "Plum", true, "#17141c", "#1f1b26", "#282232", "#120f17", "#272130", "#2d2637", "#342c3f", "#241e2c", "#3d3449",
            "#dcd6e2", "#8a819a", "#c07be0", "#6f8fe0", "#69c091", "#e0667f", "#e2b6f2"),
        mk("ember", "Ember", true, "#1b1614", "#241d1a", "#2e2521", "#141010", "#2b221e", "#332822", "#3a2e28", "#271f1c", "#463831",
            "#e4dad3", "#9a8a80", "#e8732f", "#d8a13a", "#6dbf7a", "#e05a5a", "#ffb56b"),
        mk("daylight", "Daylight", false, "#eef0f3", "#e3e6ea", "#d7dbe0", "#fbfcfd", "#dfe3e8", "#d3d8de", "#c2c8d0", "#dde1e6", "#b9c0c9",
            "#23272d", "#6b727c", "#c47a12", "#2f6fb5", "#3d8f52", "#c04a4a", "#b07a10"),
    };
    return all;
}
const Palette* preset(const QString& id) {
    for (auto& p : presets())
        if (p.id == id) return &p;
    return nullptr;
}

static Palette g_palette = presets().front();
const Palette& current() { return g_palette; }
static int g_revision = 0;
int revision() { return g_revision; }
void setPalette(const Palette& p) {
    g_palette = p;
    ++g_revision;
}

// The editable roles, in the order the theme editor lists them.
static const struct {
    const char* key;
    const char* label;
    QColor Palette::* member;
} kRoles[] = {
    {"bg", "Background", &Palette::bg},
    {"panel", "Panel", &Palette::panel},
    {"panelLight", "Panel (light)", &Palette::panelLight},
    {"surface", "Inputs / lists", &Palette::surface},
    {"raised", "Buttons", &Palette::raised},
    {"hover", "Hover", &Palette::hover},
    {"border", "Borders", &Palette::border},
    {"grid", "Grid", &Palette::grid},
    {"gridStrong", "Grid (bar)", &Palette::gridStrong},
    {"text", "Text", &Palette::text},
    {"textDim", "Text (dim)", &Palette::textDim},
    {"accent", "Accent", &Palette::accent},
    {"accent2", "Accent 2", &Palette::accent2},
    {"green", "Green", &Palette::green},
    {"red", "Red", &Palette::red},
    {"playhead", "Playhead", &Palette::playhead},
    {"glow", "Top bar glow", &Palette::glow},
};
QStringList paletteRoleNames() {
    QStringList l;
    for (auto& r : kRoles) l << r.key;
    return l;
}
QString paletteRoleLabel(const QString& role) {
    for (auto& r : kRoles)
        if (role == r.key) return r.label;
    return role;
}
QColor paletteRole(const Palette& p, const QString& role) {
    for (auto& r : kRoles)
        if (role == r.key) return p.*(r.member);
    return QColor();
}
void setPaletteRole(Palette& p, const QString& role, const QColor& c) {
    for (auto& r : kRoles)
        if (role == r.key) p.*(r.member) = c;
}

// ---------------------------------------------------------------- user themes
static const char* kUserPrefix = "user:";
bool isUserTheme(const QString& id) { return id.startsWith(kUserPrefix); }
std::vector<Palette> userThemes() {
    std::vector<Palette> out;
    QSettings s;
    s.beginGroup("theme");
    s.beginGroup("userThemes");
    for (auto& key : s.childGroups()) {
        s.beginGroup(key);
        Palette p = presets().front();
        p.id = kUserPrefix + key;
        p.name = s.value("name", key).toString();
        p.dark = s.value("dark", true).toBool();
        for (auto& role : paletteRoleNames())
            if (s.contains(role)) {
                QColor c(s.value(role).toString());
                if (c.isValid()) setPaletteRole(p, role, c);
            }
        s.endGroup();
        out.push_back(p);
    }
    s.endGroup();
    s.endGroup();
    return out;
}
void saveUserTheme(const Palette& p) {
    QString key = p.id;
    if (isUserTheme(key)) key = key.mid(qstrlen(kUserPrefix));
    QSettings s;
    s.beginGroup("theme");
    s.beginGroup("userThemes");
    s.beginGroup(key);
    s.setValue("name", p.name);
    s.setValue("dark", p.dark);
    for (auto& role : paletteRoleNames()) s.setValue(role, paletteRole(p, role).name());
    s.endGroup();
    s.endGroup();
    s.endGroup();
}
void deleteUserTheme(const QString& id) {
    if (!isUserTheme(id)) return;
    QSettings s;
    s.beginGroup("theme");
    s.beginGroup("userThemes");
    s.remove(id.mid(qstrlen(kUserPrefix)));
    s.endGroup();
    s.endGroup();
}

void saveTheme(const Palette& p) {
    QSettings s;
    s.beginGroup("theme");
    s.setValue("preset", p.id);
    s.setValue("radius", radius());
    // Only colours that differ from the preset are stored, so tweaking a preset later still reaches existing users.
    // A user theme has no preset behind it - it is written in full by saveUserTheme - so nothing is diffed here.
    const Palette* base = isUserTheme(p.id) ? nullptr : preset(p.id);
    s.beginGroup("custom");
    s.remove("");
    if (base)
        for (auto& role : paletteRoleNames()) {
            QColor c = paletteRole(p, role);
            if (c != paletteRole(*base, role)) s.setValue(role, c.name());
        }
    s.endGroup();
    s.endGroup();
}
Palette loadSavedTheme() {
    QSettings s;
    s.beginGroup("theme");
    QString want = s.value("preset", "vybrant").toString();
    Palette p = presets().front();
    if (isUserTheme(want)) {
        for (auto& u : userThemes())
            if (u.id == want) {
                p = u;
                break;
            }
    } else if (const Palette* base = preset(want)) p = *base;
    s.beginGroup("custom");
    for (auto& role : paletteRoleNames())
        if (s.contains(role)) {
            QColor c(s.value(role).toString());
            if (c.isValid()) setPaletteRole(p, role, c);
        }
    s.endGroup();
    s.endGroup();
    return p;
}
void applyTheme(const Palette& p) {
    setPalette(p);
    if (auto* app = qobject_cast<QApplication*>(QCoreApplication::instance())) {
        app->setStyleSheet(stylesheet());
        for (QWidget* w : app->allWidgets()) {
            QVariant t = w->property("mtQss");
            if (t.isValid()) w->setStyleSheet(css(t.toString()));
            QVariant g = w->property("mtGlyph");
            if (g.isValid())
                if (auto* b = qobject_cast<QAbstractButton*>(w)) {
                    int sz = w->property("mtGlyphSize").toInt();
                    b->setIcon(glyphIcon((Glyph)g.toInt(), iconColor(), sz));
                }
            w->update();
        }
    }
}

// ---------------------------------------------------------------- stylesheet
// Written with @role@ placeholders and filled from the palette, so every rule follows the active theme. A few shades
// (checked chrome, list selection, scrollbars) are mixed from the base colours rather than being roles to pick.
static QMap<QString, QString> themeTokens() {
    const Palette& t = current();
    const QColor bar = mix(t.bg, t.panel, 0.55);
    return QMap<QString, QString>{
        {"bg", t.bg.name()},
        {"panel", t.panel.name()},
        {"panelLight", t.panelLight.name()},
        {"surface", t.surface.name()},
        {"raised", t.raised.name()},
        {"hover", t.hover.name()},
        {"border", t.border.name()},
        {"text", t.text.name()},
        {"textDim", t.textDim.name()},
        {"accent", t.accent.name()},
        {"accent2", t.accent2.name()},
        {"bar", bar.name()},
        {"menu", mix(t.panel, t.panelLight, 0.4).name()},
        {"checkedBg",
            QString("qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %1, stop:1 %2)").arg(activeTop().name(), activeBottom().name())},
        {"checkedFg", t.text.name()},
        {"checkedBorder", mix(bar, t.text, 0.3).name()},
        {"listSel", mix(t.bg, t.accent, t.dark ? 0.42 : 0.55).name()},   // selection follows the primary accent, not accent2
        {"listSelFg", t.dark ? t.text.name() : t.bg.name()},
        {"scroll", mix(t.border, t.textDim, 0.25).name()},
        {"scrollHover", mix(t.border, t.textDim, 0.6).name()},
        {"disabled", mix(t.bg, t.textDim, 0.55).name()},
        {"altRow", mix(t.surface, t.panel, 0.35).name()},
        {"rowHover", mix(t.surface, t.hover, 0.7).name()},
        {"focus", mix(t.border, t.textDim, 0.5).name()},
        {"radius", QString("%1px").arg(radius())},
        {"pill", QString("%1px").arg(radius() * 2)},   // buttons and tabs read as pills, like the website's
        {"glow", mix(bar, t.glow, t.dark ? 0.3 : 0.0).name()},   // smoky light at the top bar
        {"mono", "'Geist Mono', monospace"},
    };
}
static double g_radius = -1;
double radius() {
    if (g_radius < 0) g_radius = std::clamp(QSettings().value("theme/radius", 2.0).toDouble(), 0.0, 12.0);
    return g_radius;
}
void setRadius(double r) { g_radius = std::clamp(r, 0.0, 12.0); }
QString css(const QString& tmpl) {
    QString out = tmpl;
    auto v = themeTokens();
    for (auto it = v.constBegin(); it != v.constEnd(); ++it) out.replace("@" + it.key() + "@", it.value());
    return out;
}
void styled(QWidget* w, const QString& tmpl) {
    if (!w) return;
    w->setProperty("mtQss", tmpl);
    w->setStyleSheet(css(tmpl));
}
QLabel* dimLabel(const QString& text, QWidget* parent, const QString& extraCss) {
    auto* l = new QLabel(text, parent);
    styled(l, "color:@textDim@;" + (extraCss.isEmpty() ? QString() : " " + extraCss));
    return l;
}

QString stylesheet() {
    QString sheet = R"(
QMainWindow, QDialog, QWidget { background: @bg@; color: @text@; font-size: 9pt; }
QMenuBar { background: @bar@; border-bottom: 1px solid @border@; padding: 2px 4px; } QMenuBar::item { padding: 4px 10px; border-radius: @pill@; color: @textDim@; } QMenuBar::item:selected { background: @hover@; color: @text@; }
QMenu { background: @menu@; border: 1px solid @border@; border-radius: @pill@; padding: 6px; } QMenu::item { padding: 6px 24px 6px 12px; border-radius: @radius@; } QMenu::item:selected { background: @hover@; } QMenu::separator { height: 1px; background: @border@; margin: 5px 8px; }
QToolBar { background: qradialgradient(cx:0.5, cy:-0.4, radius:1.1, fx:0.5, fy:-0.4, stop:0 @glow@, stop:1 @bar@); border: none; border-bottom: 1px solid @border@; spacing: 4px; padding: 4px 8px; }
#transport, #transport QWidget { background: transparent; } #transport QToolButton:checked { background: @checkedBg@; } /* the id rule outranks QToolButton:checked */
QToolButton { background: transparent; border: 1px solid transparent; border-radius: @pill@; padding: 3px 8px; color: @text@; }
QToolButton:hover { background: @hover@; border-color: @border@; } QToolButton:pressed { background: @border@; }
QToolButton:checked { background: @checkedBg@; color: @checkedFg@; border-color: @checkedBorder@; } QToolButton:disabled { color: @disabled@; }
QToolButton::menu-indicator { image: none; width: 0; }
QPushButton { background: transparent; border: 1px solid @border@; border-radius: @pill@; padding: 5px 16px; color: @text@; } QPushButton:hover { background: @hover@; border-color: @focus@; } QPushButton:pressed { background: @panel@; } QPushButton:checked { background: @checkedBg@; color: @checkedFg@; border-color: @checkedBorder@; } QPushButton:disabled { color: @disabled@; }
QPushButton:default { background: @text@; color: @bg@; border-color: @text@; } QPushButton:default:hover { background: #ffffff; } QPushButton:default:disabled { background: @raised@; color: @disabled@; border-color: @border@; }
QComboBox { background: @panel@; border: 1px solid @border@; border-radius: @pill@; padding: 2px 20px 2px 10px; color: @text@; min-height: 20px; } QComboBox:hover { border-color: @focus@; }
QComboBox QAbstractItemView { background: @menu@; border: 1px solid @border@; selection-background-color: @hover@; outline: none; padding: 4px; }
QLineEdit, QSpinBox, QDoubleSpinBox, QTextEdit, QPlainTextEdit { background: @surface@; border: 1px solid @border@; border-radius: @radius@; padding: 3px 8px; color: @text@; selection-background-color: @accent@; selection-color: @bg@; } QLineEdit:focus, QPlainTextEdit:focus { border-color: @focus@; }
QDockWidget { titlebar-close-icon: none; titlebar-normal-icon: none; font-family: @mono@; font-size: 7.5pt; color: @textDim@; }
QDockWidget::title { background: @bar@; padding: 5px 10px; border-bottom: 1px solid @border@; text-align: left; }
QDockWidget::close-button, QDockWidget::float-button { background: transparent; border: none; border-radius: @radius@; } QDockWidget::close-button:hover, QDockWidget::float-button:hover { background: @border@; }
QTabBar::tab { background: transparent; border: 1px solid transparent; border-radius: @pill@; padding: 4px 14px; margin: 2px 2px; color: @textDim@; } QTabBar::tab:selected { background: @hover@; border-color: @border@; color: @text@; } QTabBar::tab:hover { color: @text@; }
QTabWidget::pane { border: 1px solid @border@; border-radius: @radius@; top: -1px; }
QScrollBar:vertical { background: transparent; width: 8px; margin: 0; } QScrollBar::handle:vertical { background: @scroll@; min-height: 24px; border-radius: 3px; margin: 2px 2px; } QScrollBar::handle:vertical:hover { background: @scrollHover@; }
QScrollBar:horizontal { background: transparent; height: 8px; margin: 0; } QScrollBar::handle:horizontal { background: @scroll@; min-width: 24px; border-radius: 3px; margin: 2px 2px; } QScrollBar::handle:horizontal:hover { background: @scrollHover@; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; } QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QTreeView, QListView, QTableView { background: @surface@; border: 1px solid @border@; alternate-background-color: @altRow@; outline: none; } QTreeView::item { padding: 2px 0; } QTreeView::item:selected, QListView::item:selected, QTableView::item:selected { background: @listSel@; color: @listSelFg@; } QTreeView::item:hover { background: @rowHover@; } QTreeView::item, QListView::item { border-radius: @radius@; }
QHeaderView::section { background: @panel@; border: none; border-right: 1px solid @border@; border-bottom: 1px solid @border@; padding: 4px 8px; color: @textDim@; font-family: @mono@; font-size: 7.5pt; }
QStatusBar { background: @bar@; border-top: 1px solid @border@; color: @textDim@; font-family: @mono@; font-size: 7.5pt; } QStatusBar::item { border: none; }
QSplitter::handle { background: @border@; } QSplitter::handle:horizontal { width: 1px; } QSplitter::handle:vertical { height: 1px; } QMainWindow::separator { background: @bg@; width: 6px; height: 6px; } QMainWindow::separator:hover { background: @border@; }
QGroupBox { border: 1px solid @border@; border-radius: @pill@; margin-top: 10px; padding-top: 8px; } QGroupBox::title { subcontrol-origin: margin; left: 10px; color: @textDim@; font-family: @mono@; font-size: 7.5pt; }
QCheckBox::indicator, QRadioButton::indicator { width: 13px; height: 13px; } QCheckBox::indicator:checked { background: @accent@; border: 1px solid @accent@; border-radius: @radius@; } QCheckBox::indicator:unchecked { background: @surface@; border: 1px solid @focus@; border-radius: @radius@; }
QSlider::groove:horizontal { height: 3px; background: @border@; border-radius: 1px; } QSlider::handle:horizontal { width: 12px; margin: -5px 0; background: @text@; border-radius: 6px; }
QToolTip { background: @menu@; color: @text@; border: 1px solid @border@; border-radius: @radius@; padding: 5px 8px; }
QProgressBar { background: @surface@; border: 1px solid @border@; border-radius: @pill@; text-align: center; height: 12px; } QProgressBar::chunk { background: @accent@; border-radius: @pill@; }
QLabel { background: transparent; }
)";
    return css(sheet);
}

namespace {
class RoundedFrameOverlay : public QWidget {
public:
    explicit RoundedFrameOverlay(QWidget* host)
        : QWidget(host)
        , m_host(host) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        host->installEventFilter(this);
        setGeometry(host->rect());
        raise();
    }
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == m_host && (e->type() == QEvent::Resize || e->type() == QEvent::ChildAdded || e->type() == QEvent::Show)) {
            setGeometry(m_host->rect());
            raise();
        }
        return false;
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath outer;
        outer.addRect(QRectF(rect()).adjusted(-1, -1, 1, 1));
        QPainterPath inner;
        inner.addRoundedRect(r, radius(), radius());
        p.fillPath(outer.subtracted(inner), bg());   // hide the host's square corners
        p.setPen(QPen(border(), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r, radius(), radius());
    }

private:
    QWidget* m_host;
};
}   // namespace
void roundedFrame(QWidget* host) { new RoundedFrameOverlay(host); }

QString noteName(int midi) {
    static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return QString("%1%2").arg(names[((midi % 12) + 12) % 12]).arg(midi / 12);
}   // FL-style octaves: MIDI 60 is C5, MIDI 0 is C0 (no octave -1)

// Selecting a tool puts that tool's own icon under the pointer. The glyph is drawn white over a dark halo so it stays
// readable on both the light key column and the dark grid.
QCursor toolCursor(Glyph g, int hotX, int hotY) {
    // Cached: the hover handlers ask for this on every mouse move, and building one costs nine pixmap blits.
    static std::map<std::tuple<int, int, int>, QCursor> cache;
    auto key = std::make_tuple((int)g, hotX, hotY);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    const int sz = 22;
    QPixmap light = glyphIcon(g, QColor(0xff, 0xff, 0xff), sz).pixmap(sz, sz);
    QPixmap dark = glyphIcon(g, QColor(0x0a, 0x0c, 0x0f), sz).pixmap(sz, sz);
    qreal r = light.devicePixelRatio();
    if (r <= 0) r = 1;
    QPixmap pm(light.width() + (int)(2 * r), light.height() + (int)(2 * r));
    pm.setDevicePixelRatio(r);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            if (dx || dy) p.drawPixmap(1 + dx, 1 + dy, dark);
    p.drawPixmap(1, 1, light);
    p.end();
    return cache.emplace(key, QCursor(pm, hotX, hotY)).first->second;
}
void setGlyph(QAbstractButton* b, Glyph g, int size) {
    if (!b) return;
    b->setProperty("mtGlyph", (int)g);
    b->setProperty("mtGlyphSize", size);
    b->setIcon(glyphIcon(g, iconColor(), size));
    b->setIconSize(QSize(size, size));
}
QIcon glyphIcon(Glyph g, const QColor& color, int size) {
    QPixmap pm(size * 2, size * 2);
    pm.setDevicePixelRatio(2);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    double s = size;
    switch (g) {
    case Glyph::Play: {
        QPainterPath path;
        path.moveTo(s * 0.25, s * 0.15);
        path.lineTo(s * 0.85, s * 0.5);
        path.lineTo(s * 0.25, s * 0.85);
        path.closeSubpath();
        p.drawPath(path);
        break;
    }
    case Glyph::Stop: p.drawRoundedRect(QRectF(s * 0.22, s * 0.22, s * 0.56, s * 0.56), 1.5, 1.5); break;
    case Glyph::Record: p.drawEllipse(QRectF(s * 0.2, s * 0.2, s * 0.6, s * 0.6)); break;
    case Glyph::Pause:
        p.drawRoundedRect(QRectF(s * 0.22, s * 0.18, s * 0.2, s * 0.64), 1, 1);
        p.drawRoundedRect(QRectF(s * 0.58, s * 0.18, s * 0.2, s * 0.64), 1, 1);
        break;
    case Glyph::Playlist: {
        p.setBrush(color);
        p.drawRoundedRect(QRectF(s * 0.12, s * 0.2, s * 0.45, s * 0.16), 1, 1);
        p.drawRoundedRect(QRectF(s * 0.35, s * 0.42, s * 0.53, s * 0.16), 1, 1);
        p.drawRoundedRect(QRectF(s * 0.2, s * 0.64, s * 0.4, s * 0.16), 1, 1);
        break;
    }
    case Glyph::PianoRoll: {   // a keyboard column on the left with note bars beside it
        p.setBrush(color);
        p.drawRoundedRect(QRectF(s * 0.08, s * 0.12, s * 0.22, s * 0.76), 1.5, 1.5);
        p.setBrush(QColor(0x15, 0x17, 0x1b));
        for (int i = 0; i < 3; ++i) p.drawRect(QRectF(s * 0.08, s * (0.26 + i * 0.22), s * 0.13, s * 0.09));
        p.setBrush(color);
        const double ny[3] = {0.20, 0.44, 0.66};
        const double nw[3] = {0.40, 0.58, 0.30};
        for (int i = 0; i < 3; ++i) p.drawRoundedRect(QRectF(s * 0.38, s * ny[i], s * nw[i], s * 0.15), 1.5, 1.5);
        break;
    }
    case Glyph::ChannelRack: {
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c) {
                bool on = (r + c) % 2 == 0;
                p.setBrush(on ? color : theme::withAlpha(color, 90));
                p.drawRoundedRect(QRectF(s * (0.12 + c * 0.2), s * (0.18 + r * 0.24), s * 0.15, s * 0.17), 1, 1);
            }
        break;
    }
    case Glyph::Mixer: {
        p.setPen(QPen(color, 1.5));
        for (int i = 0; i < 3; ++i) {
            double x = s * (0.25 + i * 0.25);
            p.drawLine(QPointF(x, s * 0.15), QPointF(x, s * 0.85));
        }
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        double ys[3] = {0.55, 0.3, 0.65};
        for (int i = 0; i < 3; ++i)
            p.drawRoundedRect(QRectF(s * (0.25 + i * 0.25) - s * 0.11, s * ys[i] - s * 0.07, s * 0.22, s * 0.14), 1, 1);
        break;
    }
    case Glyph::Browser: {
        QPainterPath path;
        path.moveTo(s * 0.1, s * 0.25);
        path.lineTo(s * 0.38, s * 0.25);
        path.lineTo(s * 0.46, s * 0.35);
        path.lineTo(s * 0.9, s * 0.35);
        path.lineTo(s * 0.9, s * 0.8);
        path.lineTo(s * 0.1, s * 0.8);
        path.closeSubpath();
        p.drawPath(path);
        break;
    }
    case Glyph::Samples: {
        p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap));
        double hs[7] = {0.3, 0.7, 0.45, 0.85, 0.5, 0.65, 0.35};
        for (int i = 0; i < 7; ++i) {
            double x = s * (0.15 + i * 0.117);
            p.drawLine(QPointF(x, s * (0.5 - hs[i] * 0.35)), QPointF(x, s * (0.5 + hs[i] * 0.35)));
        }
        break;
    }
    case Glyph::Plugins: {
        p.drawRoundedRect(QRectF(s * 0.2, s * 0.4, s * 0.6, s * 0.35), 2, 2);
        p.drawRect(QRectF(s * 0.32, s * 0.15, s * 0.1, s * 0.3));
        p.drawRect(QRectF(s * 0.58, s * 0.15, s * 0.1, s * 0.3));
        p.drawRect(QRectF(s * 0.45, s * 0.72, s * 0.1, s * 0.15));
        break;
    }
    case Glyph::Settings: {
        p.setPen(QPen(color, 2.2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(s * 0.3, s * 0.3, s * 0.4, s * 0.4));
        for (int i = 0; i < 8; ++i) {
            double a = i * M_PI / 4;
            p.drawLine(QPointF(s * 0.5 + std::cos(a) * s * 0.28, s * 0.5 + std::sin(a) * s * 0.28),
                QPointF(s * 0.5 + std::cos(a) * s * 0.42, s * 0.5 + std::sin(a) * s * 0.42));
        }
        break;
    }
    case Glyph::CountIn: {
        QFont f;
        f.setPointSizeF(s * 0.5);
        f.setBold(true);
        p.setFont(f);
        p.setPen(color);
        p.drawText(QRectF(0, 0, s, s), Qt::AlignCenter, "3·2");
        break;
    }
    case Glyph::Punch: {
        p.setPen(QPen(color, 1.5));
        p.drawLine(QPointF(s * 0.15, s * 0.5), QPointF(s * 0.85, s * 0.5));
        p.setPen(Qt::NoPen);
        p.drawRect(QRectF(s * 0.3, s * 0.25, s * 0.08, s * 0.5));
        p.drawRect(QRectF(s * 0.62, s * 0.25, s * 0.08, s * 0.5));
        p.setBrush(QColor(0xe0, 0x5a, 0x5a));
        p.drawEllipse(QRectF(s * 0.42, s * 0.42, s * 0.16, s * 0.16));
        break;
    }
    case Glyph::Takes: {
        p.setPen(QPen(color, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(s * 0.2, s * 0.2, s * 0.6, s * 0.6), 30 * 16, 300 * 16);
        QPainterPath a;
        a.moveTo(s * 0.72, s * 0.12);
        a.lineTo(s * 0.82, s * 0.36);
        a.lineTo(s * 0.58, s * 0.36);
        a.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawPath(a);
        break;
    }
    case Glyph::TypingKeys: {   // small piano: 4 white keys with 3 black keys
        p.setPen(QPen(color, 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(s * 0.1, s * 0.2, s * 0.8, s * 0.6), 1.5, 1.5);
        for (int i = 1; i < 4; ++i) p.drawLine(QPointF(s * (0.1 + 0.2 * i), s * 0.5), QPointF(s * (0.1 + 0.2 * i), s * 0.8));
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        for (int i = 0; i < 4; ++i)
            if (i != 2) p.drawRect(QRectF(s * (0.1 + 0.2 * i) + s * 0.13, s * 0.2, s * 0.14, s * 0.32));
        break;
    }
    case Glyph::Help: {
        QFont f;
        f.setPointSizeF(s * 0.65);
        f.setBold(true);
        p.setFont(f);
        p.setPen(color);
        p.drawText(QRectF(0, 0, s, s), Qt::AlignCenter, "?");
        break;
    }
    case Glyph::Pencil: {   // the draw tool: a plain arrow cursor (the 'pencil' name is historical)
        QPainterPath a;
        a.moveTo(s * 0.26, s * 0.12);
        a.lineTo(s * 0.26, s * 0.80);
        a.lineTo(s * 0.43, s * 0.64);
        a.lineTo(s * 0.55, s * 0.90);
        a.lineTo(s * 0.66, s * 0.85);
        a.lineTo(s * 0.54, s * 0.60);
        a.lineTo(s * 0.76, s * 0.58);
        a.closeSubpath();
        p.setPen(QPen(QColor(0x15, 0x17, 0x1b), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(color);
        p.drawPath(a);
        p.setPen(Qt::NoPen);
        break;
    }
    case Glyph::Marquee: {
        QPen pen(color, 1.3);
        pen.setStyle(Qt::CustomDashLine);
        pen.setDashPattern({2, 2});
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(s * 0.17, s * 0.20, s * 0.66, s * 0.60));
        break;
    }
    case Glyph::Eraser: {
        QPainterPath e;
        e.moveTo(s * 0.20, s * 0.62);
        e.lineTo(s * 0.52, s * 0.22);
        e.lineTo(s * 0.82, s * 0.44);
        e.lineTo(s * 0.50, s * 0.84);
        e.closeSubpath();
        p.drawPath(e);
        p.setPen(QPen(withAlpha(QColor(0x15, 0x17, 0x1b), 200), 1.2));
        p.drawLine(QPointF(s * 0.35, s * 0.73), QPointF(s * 0.66, s * 0.33));
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRect(QRectF(s * 0.12, s * 0.86, s * 0.76, s * 0.07));
        break;
    }
    case Glyph::Slice: {   // scissors
        QPen pen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(s * 0.30, s * 0.16), QPointF(s * 0.72, s * 0.72));
        p.drawLine(QPointF(s * 0.70, s * 0.16), QPointF(s * 0.28, s * 0.72));   // blades
        p.drawEllipse(QRectF(s * 0.12, s * 0.66, s * 0.26, s * 0.26));
        p.drawEllipse(QRectF(s * 0.62, s * 0.66, s * 0.26, s * 0.26));   // handles
        p.setPen(Qt::NoPen);
        break;
    }
    case Glyph::MuteSpeaker: {
        p.drawRect(QRectF(s * 0.12, s * 0.38, s * 0.16, s * 0.24));
        QPainterPath cone;
        cone.moveTo(s * 0.28, s * 0.38);
        cone.lineTo(s * 0.52, s * 0.16);
        cone.lineTo(s * 0.52, s * 0.84);
        cone.lineTo(s * 0.28, s * 0.62);
        cone.closeSubpath();
        p.drawPath(cone);
        p.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(s * 0.62, s * 0.34), QPointF(s * 0.90, s * 0.66));
        p.drawLine(QPointF(s * 0.90, s * 0.34), QPointF(s * 0.62, s * 0.66));
        p.setPen(Qt::NoPen);
        break;
    }
    case Glyph::Ghost: {
        QPainterPath gh;
        gh.moveTo(s * 0.20, s * 0.86);
        gh.lineTo(s * 0.20, s * 0.46);
        gh.arcTo(QRectF(s * 0.20, s * 0.14, s * 0.60, s * 0.64), 180, -180);
        gh.lineTo(s * 0.80, s * 0.86);
        gh.lineTo(s * 0.68, s * 0.74);
        gh.lineTo(s * 0.56, s * 0.86);
        gh.lineTo(s * 0.44, s * 0.74);
        gh.lineTo(s * 0.32, s * 0.86);
        gh.closeSubpath();
        p.setBrush(withAlpha(color, 150));
        p.drawPath(gh);
        p.setBrush(QColor(0x15, 0x17, 0x1b));
        p.drawEllipse(QRectF(s * 0.33, s * 0.40, s * 0.11, s * 0.14));
        p.drawEllipse(QRectF(s * 0.56, s * 0.40, s * 0.11, s * 0.14));
        p.setBrush(color);
        break;
    }
    case Glyph::Magnet: {   // a horseshoe magnet, poles up: the body, a gap, then the pole tips
        QPainterPath u;
        u.moveTo(s * 0.27, s * 0.33);
        u.lineTo(s * 0.27, s * 0.52);
        u.arcTo(QRectF(s * 0.27, s * 0.29, s * 0.46, s * 0.46), 180, 180);
        u.lineTo(s * 0.73, s * 0.33);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(color, s * 0.17, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
        p.drawPath(u);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRect(QRectF(s * 0.185, s * 0.12, s * 0.17, s * 0.15));
        p.drawRect(QRectF(s * 0.645, s * 0.12, s * 0.17, s * 0.15));
        break;
    }
    case Glyph::Repeat: {
        p.setPen(QPen(color, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(s * 0.18, s * 0.18, s * 0.64, s * 0.64), 200 * 16, 300 * 16);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        QPainterPath a;
        a.moveTo(s * 0.30, s * 0.10);
        a.lineTo(s * 0.30, s * 0.40);
        a.lineTo(s * 0.06, s * 0.25);
        a.closeSubpath();
        p.drawPath(a);
        break;
    }
    case Glyph::Metronome: {
        QPainterPath path;
        path.moveTo(s * 0.35, s * 0.15);
        path.lineTo(s * 0.65, s * 0.15);
        path.lineTo(s * 0.85, s * 0.85);
        path.lineTo(s * 0.15, s * 0.85);
        path.closeSubpath();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(color, 1.5));
        p.drawPath(path);
        p.drawLine(QPointF(s * 0.5, s * 0.8), QPointF(s * 0.7, s * 0.3));
        break;
    }
    }
    return QIcon(pm);
}

}   // namespace mt::theme
