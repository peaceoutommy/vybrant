#include "ui/Theme.h"
#include "core/Types.h"
#include <QPixmap>
#include <QPainterPath>
#include <QFrame>
#include <cmath>
#include <map>
#include <tuple>

namespace mt::theme {

QWidget* barSeparator(QWidget* parent) { auto* f = new QFrame(parent); f->setFrameShape(QFrame::VLine); f->setStyleSheet("color:#2b3037;"); f->setFixedHeight(18); return f; }

QString stylesheet() {
    return R"(
QMainWindow, QDialog, QWidget { background: #15171b; color: #d3d7dd; font-size: 9pt; }
QMenuBar { background: #1a1d22; border-bottom: 1px solid #2b3037; padding: 1px; } QMenuBar::item { padding: 3px 8px; border-radius: 3px; } QMenuBar::item:selected { background: #2b3037; }
QMenu { background: #1e2126; border: 1px solid #2b3037; padding: 4px; } QMenu::item { padding: 4px 22px 4px 12px; border-radius: 3px; } QMenu::item:selected { background: #2f4a6b; } QMenu::separator { height: 1px; background: #2b3037; margin: 4px 6px; }
QToolBar { background: #1a1d22; border: none; border-bottom: 1px solid #2b3037; spacing: 3px; padding: 2px 4px; }
QToolButton { background: transparent; border: 1px solid transparent; border-radius: 3px; padding: 3px 7px; color: #c8ccd3; }
QToolButton:hover { background: #252930; border-color: #2b3037; } QToolButton:pressed { background: #2b3037; }
QToolButton:checked { background: #3a2e18; color: #f0a13a; border-color: #5a4520; } QToolButton:disabled { color: #4f5660; }
QToolButton::menu-indicator { image: none; width: 0; }
QPushButton { background: #23272d; border: 1px solid #2b3037; border-radius: 3px; padding: 4px 12px; color: #d3d7dd; } QPushButton:hover { background: #2a2f36; } QPushButton:pressed { background: #1c1f24; } QPushButton:checked { background: #3a2e18; color: #f0a13a; border-color: #5a4520; } QPushButton:disabled { color: #5b6370; }
QComboBox { background: #1c1f24; border: 1px solid #2b3037; border-radius: 3px; padding: 2px 18px 2px 7px; color: #d3d7dd; min-height: 20px; } QComboBox:hover { border-color: #3a4048; }
QComboBox QAbstractItemView { background: #1e2126; border: 1px solid #2b3037; selection-background-color: #2f4a6b; outline: none; }
QLineEdit, QSpinBox, QDoubleSpinBox, QTextEdit, QPlainTextEdit { background: #111316; border: 1px solid #2b3037; border-radius: 3px; padding: 2px 5px; color: #d3d7dd; selection-background-color: #f0a13a; selection-color: #15171b; } QLineEdit:focus { border-color: #4a5260; }
QDockWidget { titlebar-close-icon: none; titlebar-normal-icon: none; font-size: 8.5pt; font-weight: bold; color: #9aa2ad; }
QDockWidget::title { background: #1a1d22; padding: 3px 8px; border-bottom: 1px solid #2b3037; text-align: left; }
QDockWidget::close-button, QDockWidget::float-button { background: transparent; border: none; border-radius: 2px; } QDockWidget::close-button:hover, QDockWidget::float-button:hover { background: #2b3037; }
QTabBar::tab { background: #1a1d22; border: 1px solid #2b3037; border-bottom: none; padding: 4px 14px; margin-right: 1px; color: #9aa2ad; } QTabBar::tab:selected { background: #23272d; color: #d3d7dd; border-top: 2px solid #f0a13a; } QTabBar::tab:hover { color: #d3d7dd; }
QScrollBar:vertical { background: transparent; width: 9px; margin: 0; } QScrollBar::handle:vertical { background: #2f353d; min-height: 24px; border-radius: 4px; margin: 2px 1px; } QScrollBar::handle:vertical:hover { background: #3d4550; }
QScrollBar:horizontal { background: transparent; height: 9px; margin: 0; } QScrollBar::handle:horizontal { background: #2f353d; min-width: 24px; border-radius: 4px; margin: 1px 2px; } QScrollBar::handle:horizontal:hover { background: #3d4550; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; } QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QTreeView, QListView, QTableView { background: #17191e; border: 1px solid #23272d; alternate-background-color: #191c21; outline: none; } QTreeView::item { padding: 1px 0; } QTreeView::item:selected, QListView::item:selected, QTableView::item:selected { background: #2f4a6b; color: #ffffff; } QTreeView::item:hover { background: #20242a; }
QHeaderView::section { background: #1c1f24; border: none; border-right: 1px solid #2b3037; border-bottom: 1px solid #2b3037; padding: 3px 6px; color: #9aa2ad; font-weight: bold; }
QStatusBar { background: #1a1d22; border-top: 1px solid #2b3037; color: #9aa2ad; } QStatusBar::item { border: none; }
QSplitter::handle { background: #1a1d22; } QMainWindow::separator { background: #1a1d22; width: 3px; height: 3px; } QMainWindow::separator:hover { background: #2b3037; }
QGroupBox { border: 1px solid #2b3037; border-radius: 4px; margin-top: 8px; padding-top: 6px; } QGroupBox::title { subcontrol-origin: margin; left: 8px; color: #7e8691; }
QCheckBox::indicator, QRadioButton::indicator { width: 12px; height: 12px; } QCheckBox::indicator:checked { background: #f0a13a; border: 1px solid #f0a13a; border-radius: 2px; } QCheckBox::indicator:unchecked { background: #111316; border: 1px solid #3a4048; border-radius: 2px; }
QSlider::groove:horizontal { height: 3px; background: #111316; border-radius: 1px; } QSlider::handle:horizontal { width: 10px; margin: -5px 0; background: #d3d7dd; border-radius: 5px; }
QToolTip { background: #1e2126; color: #d3d7dd; border: 1px solid #3a4048; padding: 3px; }
QProgressBar { background: #111316; border: 1px solid #2b3037; border-radius: 3px; text-align: center; height: 12px; } QProgressBar::chunk { background: #f0a13a; border-radius: 2px; }
QLabel { background: transparent; }
)";
}

QString noteName(int midi) { static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}; return QString("%1%2").arg(names[((midi % 12) + 12) % 12]).arg(midi / 12 - 1); }
QString tickToBBT(int64_t tick, int tsNum, int tsDen) { TimeSignature ts{tsNum, tsDen}; int64_t bar = ts.ticksPerBar(), beat = ts.ticksPerBeat(); if (tick < 0) tick = 0; return QString("%1:%2:%3").arg(tick / bar + 1, 3, 10, QChar('0')).arg((tick % bar) / beat + 1).arg(tick % beat, 3, 10, QChar('0')); }

// Selecting a tool puts that tool's own icon under the pointer. The glyph is drawn white over a dark halo so it stays
// readable on both the light key column and the dark grid.
QCursor toolCursor(Glyph g, int hotX, int hotY) {
    // Cached: the hover handlers ask for this on every mouse move, and building one costs nine pixmap blits.
    static std::map<std::tuple<int, int, int>, QCursor> cache;
    auto key = std::make_tuple((int)g, hotX, hotY);
    auto it = cache.find(key); if (it != cache.end()) return it->second;
    const int sz = 22;
    QPixmap light = glyphIcon(g, QColor(0xff, 0xff, 0xff), sz).pixmap(sz, sz);
    QPixmap dark = glyphIcon(g, QColor(0x0a, 0x0c, 0x0f), sz).pixmap(sz, sz);
    qreal r = light.devicePixelRatio(); if (r <= 0) r = 1;
    QPixmap pm(light.width() + (int)(2 * r), light.height() + (int)(2 * r)); pm.setDevicePixelRatio(r); pm.fill(Qt::transparent);
    QPainter p(&pm);
    for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) if (dx || dy) p.drawPixmap(1 + dx, 1 + dy, dark);
    p.drawPixmap(1, 1, light); p.end();
    return cache.emplace(key, QCursor(pm, hotX, hotY)).first->second;
}
QCursor deleteCursor() {
    static QCursor cur = [] { QPixmap pm(24, 24); pm.fill(Qt::transparent); QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing); p.setPen(QPen(Qt::black, 3.5, Qt::SolidLine, Qt::RoundCap)); p.drawLine(6, 6, 18, 18); p.drawLine(18, 6, 6, 18); p.setPen(QPen(QColor(0xe0, 0x5a, 0x5a), 2, Qt::SolidLine, Qt::RoundCap)); p.drawLine(6, 6, 18, 18); p.drawLine(18, 6, 6, 18); return QCursor(pm, 12, 12); }();
    return cur;
}
QCursor muteCursor() {
    static QCursor cur = [] { QPixmap pm(24, 24); pm.fill(Qt::transparent); QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing); p.setPen(QPen(Qt::black, 2)); p.setBrush(QColor(0xf0, 0xa1, 0x3a)); p.drawEllipse(QRectF(3, 3, 18, 18)); QFont f; f.setPointSizeF(9); f.setBold(true); p.setFont(f); p.setPen(Qt::black); p.drawText(QRect(3, 3, 18, 18), Qt::AlignCenter, "M"); return QCursor(pm, 12, 12); }();
    return cur;
}
QIcon glyphIcon(Glyph g, const QColor& color, int size) {
    QPixmap pm(size * 2, size * 2); pm.setDevicePixelRatio(2); pm.fill(Qt::transparent); QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing); p.setPen(Qt::NoPen); p.setBrush(color); double s = size;
    switch (g) {
    case Glyph::Play: { QPainterPath path; path.moveTo(s * 0.25, s * 0.15); path.lineTo(s * 0.85, s * 0.5); path.lineTo(s * 0.25, s * 0.85); path.closeSubpath(); p.drawPath(path); break; }
    case Glyph::Stop: p.drawRoundedRect(QRectF(s * 0.22, s * 0.22, s * 0.56, s * 0.56), 1.5, 1.5); break;
    case Glyph::Record: p.drawEllipse(QRectF(s * 0.2, s * 0.2, s * 0.6, s * 0.6)); break;
    case Glyph::Pause: p.drawRoundedRect(QRectF(s * 0.22, s * 0.18, s * 0.2, s * 0.64), 1, 1); p.drawRoundedRect(QRectF(s * 0.58, s * 0.18, s * 0.2, s * 0.64), 1, 1); break;
    case Glyph::Playlist: { p.setBrush(color); p.drawRoundedRect(QRectF(s * 0.12, s * 0.2, s * 0.45, s * 0.16), 1, 1); p.drawRoundedRect(QRectF(s * 0.35, s * 0.42, s * 0.53, s * 0.16), 1, 1); p.drawRoundedRect(QRectF(s * 0.2, s * 0.64, s * 0.4, s * 0.16), 1, 1); break; }
    case Glyph::PianoRoll: {   // a keyboard column on the left with note bars beside it
        p.setBrush(color); p.drawRoundedRect(QRectF(s * 0.08, s * 0.12, s * 0.22, s * 0.76), 1.5, 1.5);
        p.setBrush(QColor(0x15, 0x17, 0x1b)); for (int i = 0; i < 3; ++i) p.drawRect(QRectF(s * 0.08, s * (0.26 + i * 0.22), s * 0.13, s * 0.09));
        p.setBrush(color); const double ny[3] = {0.20, 0.44, 0.66}; const double nw[3] = {0.40, 0.58, 0.30};
        for (int i = 0; i < 3; ++i) p.drawRoundedRect(QRectF(s * 0.38, s * ny[i], s * nw[i], s * 0.15), 1.5, 1.5); break; }
    case Glyph::ChannelRack: { for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) { bool on = (r + c) % 2 == 0; p.setBrush(on ? color : theme::withAlpha(color, 90)); p.drawRoundedRect(QRectF(s * (0.12 + c * 0.2), s * (0.18 + r * 0.24), s * 0.15, s * 0.17), 1, 1); } break; }
    case Glyph::Mixer: { p.setPen(QPen(color, 1.5)); for (int i = 0; i < 3; ++i) { double x = s * (0.25 + i * 0.25); p.drawLine(QPointF(x, s * 0.15), QPointF(x, s * 0.85)); } p.setPen(Qt::NoPen); p.setBrush(color); double ys[3] = {0.55, 0.3, 0.65}; for (int i = 0; i < 3; ++i) p.drawRoundedRect(QRectF(s * (0.25 + i * 0.25) - s * 0.11, s * ys[i] - s * 0.07, s * 0.22, s * 0.14), 1, 1); break; }
    case Glyph::Browser: { QPainterPath path; path.moveTo(s * 0.1, s * 0.25); path.lineTo(s * 0.38, s * 0.25); path.lineTo(s * 0.46, s * 0.35); path.lineTo(s * 0.9, s * 0.35); path.lineTo(s * 0.9, s * 0.8); path.lineTo(s * 0.1, s * 0.8); path.closeSubpath(); p.drawPath(path); break; }
    case Glyph::Samples: { p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap)); double hs[7] = {0.3, 0.7, 0.45, 0.85, 0.5, 0.65, 0.35}; for (int i = 0; i < 7; ++i) { double x = s * (0.15 + i * 0.117); p.drawLine(QPointF(x, s * (0.5 - hs[i] * 0.35)), QPointF(x, s * (0.5 + hs[i] * 0.35))); } break; }
    case Glyph::Plugins: { p.drawRoundedRect(QRectF(s * 0.2, s * 0.4, s * 0.6, s * 0.35), 2, 2); p.drawRect(QRectF(s * 0.32, s * 0.15, s * 0.1, s * 0.3)); p.drawRect(QRectF(s * 0.58, s * 0.15, s * 0.1, s * 0.3)); p.drawRect(QRectF(s * 0.45, s * 0.72, s * 0.1, s * 0.15)); break; }
    case Glyph::Settings: { p.setPen(QPen(color, 2.2)); p.setBrush(Qt::NoBrush); p.drawEllipse(QRectF(s * 0.3, s * 0.3, s * 0.4, s * 0.4)); for (int i = 0; i < 8; ++i) { double a = i * M_PI / 4; p.drawLine(QPointF(s * 0.5 + std::cos(a) * s * 0.28, s * 0.5 + std::sin(a) * s * 0.28), QPointF(s * 0.5 + std::cos(a) * s * 0.42, s * 0.5 + std::sin(a) * s * 0.42)); } break; }
    case Glyph::CountIn: { QFont f; f.setPointSizeF(s * 0.5); f.setBold(true); p.setFont(f); p.setPen(color); p.drawText(QRectF(0, 0, s, s), Qt::AlignCenter, "3·2"); break; }
    case Glyph::Punch: { p.setPen(QPen(color, 1.5)); p.drawLine(QPointF(s * 0.15, s * 0.5), QPointF(s * 0.85, s * 0.5)); p.setPen(Qt::NoPen); p.drawRect(QRectF(s * 0.3, s * 0.25, s * 0.08, s * 0.5)); p.drawRect(QRectF(s * 0.62, s * 0.25, s * 0.08, s * 0.5)); p.setBrush(QColor(0xe0, 0x5a, 0x5a)); p.drawEllipse(QRectF(s * 0.42, s * 0.42, s * 0.16, s * 0.16)); break; }
    case Glyph::Takes: { p.setPen(QPen(color, 1.5)); p.setBrush(Qt::NoBrush); p.drawArc(QRectF(s * 0.2, s * 0.2, s * 0.6, s * 0.6), 30 * 16, 300 * 16); QPainterPath a; a.moveTo(s * 0.72, s * 0.12); a.lineTo(s * 0.82, s * 0.36); a.lineTo(s * 0.58, s * 0.36); a.closeSubpath(); p.setPen(Qt::NoPen); p.setBrush(color); p.drawPath(a); break; }
    case Glyph::TypingKeys: { // small piano: 4 white keys with 3 black keys
        p.setPen(QPen(color, 1.2)); p.setBrush(Qt::NoBrush); p.drawRoundedRect(QRectF(s * 0.1, s * 0.2, s * 0.8, s * 0.6), 1.5, 1.5); for (int i = 1; i < 4; ++i) p.drawLine(QPointF(s * (0.1 + 0.2 * i), s * 0.5), QPointF(s * (0.1 + 0.2 * i), s * 0.8));
        p.setPen(Qt::NoPen); p.setBrush(color); for (int i = 0; i < 4; ++i) if (i != 2) p.drawRect(QRectF(s * (0.1 + 0.2 * i) + s * 0.13, s * 0.2, s * 0.14, s * 0.32)); break; }
    case Glyph::Help: { QFont f; f.setPointSizeF(s * 0.65); f.setBold(true); p.setFont(f); p.setPen(color); p.drawText(QRectF(0, 0, s, s), Qt::AlignCenter, "?"); break; }
    case Glyph::Pencil: { QPainterPath tip; tip.moveTo(s * 0.14, s * 0.86); tip.lineTo(s * 0.33, s * 0.79); tip.lineTo(s * 0.21, s * 0.67); tip.closeSubpath(); p.drawPath(tip);
        QPainterPath body; body.moveTo(s * 0.26, s * 0.62); body.lineTo(s * 0.68, s * 0.20); body.lineTo(s * 0.82, s * 0.34); body.lineTo(s * 0.40, s * 0.76); body.closeSubpath(); p.drawPath(body); break; }
    case Glyph::Marquee: { QPen pen(color, 1.3); pen.setStyle(Qt::CustomDashLine); pen.setDashPattern({2, 2}); p.setPen(pen); p.setBrush(Qt::NoBrush); p.drawRect(QRectF(s * 0.17, s * 0.20, s * 0.66, s * 0.60)); break; }
    case Glyph::Eraser: { QPainterPath e; e.moveTo(s * 0.20, s * 0.62); e.lineTo(s * 0.52, s * 0.22); e.lineTo(s * 0.82, s * 0.44); e.lineTo(s * 0.50, s * 0.84); e.closeSubpath(); p.drawPath(e);
        p.setPen(QPen(withAlpha(QColor(0x15, 0x17, 0x1b), 200), 1.2)); p.drawLine(QPointF(s * 0.35, s * 0.73), QPointF(s * 0.66, s * 0.33)); p.setPen(Qt::NoPen);
        p.setBrush(color); p.drawRect(QRectF(s * 0.12, s * 0.86, s * 0.76, s * 0.07)); break; }
    case Glyph::Slice: { QPen pen(color, 1.1); pen.setStyle(Qt::CustomDashLine); pen.setDashPattern({2, 2}); p.setPen(pen); p.drawLine(QPointF(s * 0.5, s * 0.06), QPointF(s * 0.5, s * 0.94)); p.setPen(Qt::NoPen);
        QPainterPath blade; blade.moveTo(s * 0.12, s * 0.34); blade.lineTo(s * 0.64, s * 0.62); blade.lineTo(s * 0.56, s * 0.74); blade.lineTo(s * 0.08, s * 0.46); blade.closeSubpath(); p.drawPath(blade);
        p.setPen(QPen(color, 2.0, Qt::SolidLine, Qt::RoundCap)); p.drawLine(QPointF(s * 0.68, s * 0.68), QPointF(s * 0.88, s * 0.80)); p.setPen(Qt::NoPen); break; }
    case Glyph::MuteSpeaker: { p.drawRect(QRectF(s * 0.12, s * 0.38, s * 0.16, s * 0.24));
        QPainterPath cone; cone.moveTo(s * 0.28, s * 0.38); cone.lineTo(s * 0.52, s * 0.16); cone.lineTo(s * 0.52, s * 0.84); cone.lineTo(s * 0.28, s * 0.62); cone.closeSubpath(); p.drawPath(cone);
        p.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap)); p.drawLine(QPointF(s * 0.62, s * 0.34), QPointF(s * 0.90, s * 0.66)); p.drawLine(QPointF(s * 0.90, s * 0.34), QPointF(s * 0.62, s * 0.66)); p.setPen(Qt::NoPen); break; }
    case Glyph::Ghost: { QPainterPath gh; gh.moveTo(s * 0.20, s * 0.86); gh.lineTo(s * 0.20, s * 0.46); gh.arcTo(QRectF(s * 0.20, s * 0.14, s * 0.60, s * 0.64), 180, -180); gh.lineTo(s * 0.80, s * 0.86);
        gh.lineTo(s * 0.68, s * 0.74); gh.lineTo(s * 0.56, s * 0.86); gh.lineTo(s * 0.44, s * 0.74); gh.lineTo(s * 0.32, s * 0.86); gh.closeSubpath();
        p.setBrush(withAlpha(color, 150)); p.drawPath(gh); p.setBrush(QColor(0x15, 0x17, 0x1b)); p.drawEllipse(QRectF(s * 0.33, s * 0.40, s * 0.11, s * 0.14)); p.drawEllipse(QRectF(s * 0.56, s * 0.40, s * 0.11, s * 0.14)); p.setBrush(color); break; }
    case Glyph::Repeat: { p.setPen(QPen(color, 1.5)); p.setBrush(Qt::NoBrush); p.drawArc(QRectF(s * 0.18, s * 0.18, s * 0.64, s * 0.64), 200 * 16, 300 * 16); p.setPen(Qt::NoPen); p.setBrush(color);
        QPainterPath a; a.moveTo(s * 0.30, s * 0.10); a.lineTo(s * 0.30, s * 0.40); a.lineTo(s * 0.06, s * 0.25); a.closeSubpath(); p.drawPath(a); break; }
    case Glyph::Metronome: { QPainterPath path; path.moveTo(s * 0.35, s * 0.15); path.lineTo(s * 0.65, s * 0.15); path.lineTo(s * 0.85, s * 0.85); path.lineTo(s * 0.15, s * 0.85); path.closeSubpath(); p.setBrush(Qt::NoBrush); p.setPen(QPen(color, 1.5)); p.drawPath(path); p.drawLine(QPointF(s * 0.5, s * 0.8), QPointF(s * 0.7, s * 0.3)); break; }
    }
    return QIcon(pm);
}

}
