// Original dark theme used across the UI (no third-party artwork). Flat, low-contrast surfaces with one warm accent.
#pragma once
#include <QColor>
#include <QString>
#include <QFont>
#include <QPainter>
#include <QIcon>
#include <QCursor>

namespace mt::theme {
inline QColor bg() { return QColor(0x15, 0x17, 0x1b); }
inline QColor panel() { return QColor(0x1c, 0x1f, 0x24); }
inline QColor panelLight() { return QColor(0x24, 0x28, 0x2e); }
inline QColor border() { return QColor(0x2b, 0x30, 0x37); }
inline QColor grid() { return QColor(0x22, 0x26, 0x2b); }
inline QColor gridStrong() { return QColor(0x30, 0x36, 0x3d); }
inline QColor text() { return QColor(0xd3, 0xd7, 0xdd); }
inline QColor textDim() { return QColor(0x7e, 0x86, 0x91); }
inline QColor accent() { return QColor(0xf0, 0xa1, 0x3a); }
inline QColor accent2() { return QColor(0x55, 0xa0, 0xf0); }
inline QColor green() { return QColor(0x5f, 0xc0, 0x74); }
inline QColor red() { return QColor(0xe0, 0x5a, 0x5a); }
inline QColor playhead() { return QColor(0xff, 0xd2, 0x66); }
inline QColor selection() { return QColor(0xf0, 0xa1, 0x3a, 70); }
inline QFont smallFont() { QFont f; f.setPointSizeF(8.0); return f; }
inline QFont uiFont() { QFont f; f.setPointSizeF(9.0); return f; }
inline QFont boldFont() { QFont f; f.setPointSizeF(9.0); f.setBold(true); return f; }
QString stylesheet();
inline void drawPanel(QPainter& p, const QRectF& r, const QColor& c, qreal radius = 3.0) { p.setPen(Qt::NoPen); p.setBrush(c); p.drawRoundedRect(r, radius, radius); }
inline QColor lighter(const QColor& c, int f = 130) { return c.lighter(f); }
inline QColor withAlpha(QColor c, int a) { c.setAlpha(a); return c; }
QString noteName(int midi);
QString tickToBBT(int64_t tick, int tsNum, int tsDen);
// Painted glyph icons (play, stop, record, metronome, loop...) so the toolbar has no text-heavy buttons
enum class Glyph { Play, Stop, Record, Metronome, Pause, Playlist, PianoRoll, ChannelRack, Mixer, Browser, Samples, Plugins, Settings, CountIn, Punch, Takes, Help };
QIcon glyphIcon(Glyph g, const QColor& color, int size = 14);
QCursor deleteCursor();   // eraser cursor shown while right-drag deleting
QCursor muteCursor();     // cursor for the mute mode (double right-click + drag / mute tool)
}
