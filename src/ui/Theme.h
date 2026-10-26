// Theming for the whole UI (no third-party artwork). One palette drives the stylesheet and every painted widget.
#pragma once
#include <QColor>
#include <QString>
#include <algorithm>
#include <QFont>
#include <QPainter>
#include <QIcon>
#include <QCursor>
#include <QStringList>
#include <vector>
class QWidget;

QT_BEGIN_NAMESPACE
class QWidget;
class QAbstractButton;
QT_END_NAMESPACE

class QLabel;

namespace mt::theme {
inline constexpr int kBarH = 26;   // shared control height for the transport / playlist / piano roll toolbars
QWidget* barSeparator(QWidget* parent);   // 1px vertical rule between toolbar groups

// A theme is this handful of base colours. Everything the UI paints reads them through the accessors below at paint
// time, so switching theme only has to swap the palette and repaint - no widget needs to know a theme changed.
struct Palette {
    QString id, name;
    QColor bg;   // window background, behind everything
    QColor panel;   // panel / toolbar surfaces
    QColor panelLight;   // a step lighter than panel (headers, raised rows)
    QColor surface;   // deepest surface: text fields, list backgrounds, meters
    QColor raised;   // push buttons and other raised chrome
    QColor hover;   // hover background
    QColor border;   // hairlines between surfaces
    QColor grid;   // faint editor grid line
    QColor gridStrong;   // bar / octave grid line
    QColor text;   // primary text
    QColor textDim;   // secondary text and labels
    QColor accent;   // the one warm highlight: checked state, selection, progress
    QColor accent2;   // secondary highlight: list selection, clips
    QColor green, red;   // status colours (armed, clipping, ...)
    QColor playhead;
    bool dark = true;   // lets painted widgets pick a contrasting shade where it matters
};

const Palette& current();
const std::vector<Palette>& presets();
const Palette* preset(const QString& id);
void setPalette(const Palette& p);   // swap the palette only
int revision();   // bumped on every palette change: cached painters compare it and re-render
void applyTheme(const Palette& p);   // swap it, rebuild the stylesheet and repaint every window
void saveTheme(const Palette& p);   // persist to QSettings
Palette loadSavedTheme();   // restore the saved preset plus any per-colour overrides
// User themes live in QSettings alongside the built-in presets and behave the same everywhere they are listed.
std::vector<Palette> userThemes();
void saveUserTheme(const Palette& p);
void deleteUserTheme(const QString& id);
bool isUserTheme(const QString& id);
QStringList paletteRoleNames();   // editable role keys, in display order
QString paletteRoleLabel(const QString& role);
QColor paletteRole(const Palette& p, const QString& role);
void setPaletteRole(Palette& p, const QString& role, const QColor& c);

inline QColor bg() { return current().bg; }
inline QColor panel() { return current().panel; }
inline QColor panelLight() { return current().panelLight; }
inline QColor surface() { return current().surface; }
inline QColor raised() { return current().raised; }
inline QColor hover() { return current().hover; }
inline QColor border() { return current().border; }
inline QColor grid() { return current().grid; }
inline QColor gridStrong() { return current().gridStrong; }
inline QColor text() { return current().text; }
inline QColor textDim() { return current().textDim; }
inline QColor accent() { return current().accent; }
inline QColor accent2() { return current().accent2; }
inline QColor green() { return current().green; }
inline QColor red() { return current().red; }
inline QColor playhead() { return current().playhead; }
inline bool isDark() { return current().dark; }
inline QColor mix(const QColor& a, const QColor& b, double f) {
    return QColor(
        (int)(a.red() * (1 - f) + b.red() * f), (int)(a.green() * (1 - f) + b.green() * f), (int)(a.blue() * (1 - f) + b.blue() * f));
}
// Active (checked) button fill: a gradient that deepens downward, from the toolbar shade pulled toward the text colour (lighter on
// dark themes, darker on light ones) to a cooler, deeper shade tinted with accent2
inline QColor activeTop() { return mix(mix(bg(), panel(), 0.55), text(), isDark() ? 0.20 : 0.08); }
inline QColor activeBottom() { return mix(mix(mix(bg(), panel(), 0.55), accent2(), isDark() ? 0.3 : 0.22), text(), isDark() ? 0.0 : 0.16); }
inline QBrush activeBrush(const QRectF& r) {
    QLinearGradient g(r.topLeft(), r.bottomLeft());
    g.setColorAt(0, activeTop());
    g.setColorAt(1, activeBottom());
    return g;
}
// Icons are painted in one flat colour that contrasts with the chrome, so a checked button only changes its background.
// Alternating row shade: a step away from panel, in whichever direction the theme needs.
inline QColor altRow() { return current().dark ? current().panel.darker(118) : current().panel.lighter(105); }
inline QColor iconColor() { return current().dark ? QColor(0xe8, 0xeb, 0xef) : QColor(0x23, 0x27, 0x2d); }
inline QColor selection() {
    QColor c = current().accent;
    c.setAlpha(70);
    return c;
}
inline QFont smallFont() {
    QFont f;
    f.setPointSizeF(8.0);
    return f;
}
inline QFont uiFont() {
    QFont f;
    f.setPointSizeF(9.0);
    return f;
}
inline QFont boldFont() {
    QFont f;
    f.setPointSizeF(9.0);
    f.setBold(true);
    return f;
}
QString stylesheet();
QString css(const QString& tmpl);   // expands @role@ tokens against the active palette
void styled(QWidget* w, const QString& tmpl);   // css() + remembers the template so a theme change re-applies it
QLabel* dimLabel(const QString& text, QWidget* parent, const QString& extraCss = QString());   // secondary text: names, units, hints
inline void drawPanel(QPainter& p, const QRectF& r, const QColor& c, qreal radius = 3.0) {
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    p.drawRoundedRect(r, radius, radius);
}
inline QColor lighter(const QColor& c, int f = 130) { return c.lighter(f); }
inline QColor withAlpha(QColor c, int a) {
    c.setAlpha(a);
    return c;
}
QString noteName(int midi);
// Corner radius shared by painted buttons, cells, clips and the stylesheet's boxes (@radius@): a setting, Options > Theme
double radius();
void setRadius(double r);   // not saved (saveTheme does that); follow with applyTheme(current()) to show it
// Radius for small painted shapes: use the full radius unless the shape is too short for it
inline double radiusFor(double h) { return std::min(radius(), h / 2.5); }
// Gives a square widget (a scroll area, a painted view) rounded corners and a 1px border: a mouse-transparent overlay
// covers the corners with the panel background and strokes the frame, so the widget itself needs no changes
void roundedFrame(QWidget* host);
// Painted glyph icons (play, stop, record, metronome, loop...) so the toolbar has no text-heavy buttons
enum class Glyph {
    Play,
    Stop,
    Record,
    Metronome,
    Pause,
    Playlist,
    PianoRoll,
    ChannelRack,
    Mixer,
    Browser,
    Samples,
    Plugins,
    Settings,
    CountIn,
    Punch,
    Takes,
    Help,
    TypingKeys,
    Pencil,
    Marquee,
    Eraser,
    Slice,
    MuteSpeaker,
    Ghost,
    Repeat,
    Magnet   // snap to grid
};   // editor tool icons (playlist / piano roll)
QIcon glyphIcon(Glyph g, const QColor& color, int size = 14);
QCursor toolCursor(Glyph g, int hotX = 11, int hotY = 11);   // the active editor tool, drawn as the mouse cursor
// Icons are pixmaps, so they cannot follow a palette change on their own: remember which glyph a button carries and
// applyTheme() repaints it. Use this instead of setIcon(glyphIcon(...)) for any chrome icon.
void setGlyph(QAbstractButton* b, Glyph g, int size);
}   // namespace mt::theme
