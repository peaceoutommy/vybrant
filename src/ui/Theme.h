// Original dark theme used across the UI (no third-party artwork).
#pragma once
#include <QColor>
#include <QString>
#include <QFont>
#include <QPainter>

namespace mt::theme {
inline QColor bg() { return QColor(0x1b, 0x1e, 0x23); }
inline QColor panel() { return QColor(0x24, 0x28, 0x2e); }
inline QColor panelLight() { return QColor(0x2d, 0x32, 0x39); }
inline QColor border() { return QColor(0x3a, 0x40, 0x48); }
inline QColor grid() { return QColor(0x2f, 0x34, 0x3b); }
inline QColor gridStrong() { return QColor(0x40, 0x46, 0x4f); }
inline QColor text() { return QColor(0xd6, 0xda, 0xe0); }
inline QColor textDim() { return QColor(0x8a, 0x91, 0x9a); }
inline QColor accent() { return QColor(0xf2, 0xa9, 0x3b); }
inline QColor accent2() { return QColor(0x4e, 0xa3, 0xf1); }
inline QColor green() { return QColor(0x5d, 0xc2, 0x6d); }
inline QColor red() { return QColor(0xe2, 0x5c, 0x5c); }
inline QColor playhead() { return QColor(0xff, 0xd7, 0x6a); }
inline QColor selection() { return QColor(0xf2, 0xa9, 0x3b, 90); }
inline QFont smallFont() { QFont f; f.setPointSizeF(8.0); return f; }
inline QFont uiFont() { QFont f; f.setPointSizeF(9.0); return f; }
inline QFont boldFont() { QFont f; f.setPointSizeF(9.0); f.setBold(true); return f; }
QString stylesheet();
inline void drawPanel(QPainter& p, const QRectF& r, const QColor& c, qreal radius = 3.0) { p.setPen(Qt::NoPen); p.setBrush(c); p.drawRoundedRect(r, radius, radius); }
inline QColor lighter(const QColor& c, int f = 130) { return c.lighter(f); }
inline QColor withAlpha(QColor c, int a) { c.setAlpha(a); return c; }
QString noteName(int midi);
QString tickToBBT(int64_t tick, int tsNum, int tsDen);
}
