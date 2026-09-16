#include "ui/Theme.h"
#include "core/Types.h"
#include <QPixmap>
#include <QPainterPath>

namespace mt::theme {

QString stylesheet() {
    return R"(
QMainWindow, QDialog, QWidget { background: #15171b; color: #d3d7dd; font-size: 8.5pt; }
QMenuBar { background: #1a1d22; border-bottom: 1px solid #2b3037; padding: 1px; } QMenuBar::item { padding: 3px 8px; border-radius: 3px; } QMenuBar::item:selected { background: #2b3037; }
QMenu { background: #1e2126; border: 1px solid #2b3037; padding: 4px; } QMenu::item { padding: 4px 22px 4px 12px; border-radius: 3px; } QMenu::item:selected { background: #2f4a6b; } QMenu::separator { height: 1px; background: #2b3037; margin: 4px 6px; }
QToolBar { background: #1a1d22; border: none; border-bottom: 1px solid #2b3037; spacing: 3px; padding: 2px 4px; }
QToolButton { background: transparent; border: 1px solid transparent; border-radius: 3px; padding: 2px 5px; color: #c8ccd3; }
QToolButton:hover { background: #252930; border-color: #2b3037; } QToolButton:pressed { background: #2b3037; }
QToolButton:checked { background: #3a2e18; color: #f0a13a; border-color: #5a4520; } QToolButton:disabled { color: #4f5660; }
QToolButton::menu-indicator { image: none; width: 0; }
QPushButton { background: #23272d; border: 1px solid #2b3037; border-radius: 3px; padding: 3px 10px; color: #d3d7dd; } QPushButton:hover { background: #2a2f36; } QPushButton:pressed { background: #1c1f24; } QPushButton:checked { background: #3a2e18; color: #f0a13a; border-color: #5a4520; } QPushButton:disabled { color: #5b6370; }
QComboBox { background: #1c1f24; border: 1px solid #2b3037; border-radius: 3px; padding: 1px 18px 1px 6px; color: #d3d7dd; min-height: 18px; } QComboBox:hover { border-color: #3a4048; }
QComboBox QAbstractItemView { background: #1e2126; border: 1px solid #2b3037; selection-background-color: #2f4a6b; outline: none; }
QLineEdit, QSpinBox, QDoubleSpinBox, QTextEdit, QPlainTextEdit { background: #111316; border: 1px solid #2b3037; border-radius: 3px; padding: 2px 5px; color: #d3d7dd; selection-background-color: #f0a13a; selection-color: #15171b; } QLineEdit:focus { border-color: #4a5260; }
QDockWidget { titlebar-close-icon: none; titlebar-normal-icon: none; font-size: 8pt; font-weight: bold; color: #9aa2ad; }
QDockWidget::title { background: #1a1d22; padding: 3px 8px; border-bottom: 1px solid #2b3037; text-align: left; }
QDockWidget::close-button, QDockWidget::float-button { background: transparent; border: none; border-radius: 2px; } QDockWidget::close-button:hover, QDockWidget::float-button:hover { background: #2b3037; }
QTabBar::tab { background: #1a1d22; border: 1px solid #2b3037; border-bottom: none; padding: 3px 12px; margin-right: 1px; color: #9aa2ad; } QTabBar::tab:selected { background: #23272d; color: #d3d7dd; border-top: 2px solid #f0a13a; } QTabBar::tab:hover { color: #d3d7dd; }
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

QIcon glyphIcon(Glyph g, const QColor& color, int size) {
    QPixmap pm(size * 2, size * 2); pm.setDevicePixelRatio(2); pm.fill(Qt::transparent); QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing); p.setPen(Qt::NoPen); p.setBrush(color); double s = size;
    switch (g) {
    case Glyph::Play: { QPainterPath path; path.moveTo(s * 0.25, s * 0.15); path.lineTo(s * 0.85, s * 0.5); path.lineTo(s * 0.25, s * 0.85); path.closeSubpath(); p.drawPath(path); break; }
    case Glyph::Stop: p.drawRoundedRect(QRectF(s * 0.22, s * 0.22, s * 0.56, s * 0.56), 1.5, 1.5); break;
    case Glyph::Record: p.drawEllipse(QRectF(s * 0.2, s * 0.2, s * 0.6, s * 0.6)); break;
    case Glyph::Pause: p.drawRoundedRect(QRectF(s * 0.22, s * 0.18, s * 0.2, s * 0.64), 1, 1); p.drawRoundedRect(QRectF(s * 0.58, s * 0.18, s * 0.2, s * 0.64), 1, 1); break;
    case Glyph::Metronome: { QPainterPath path; path.moveTo(s * 0.35, s * 0.15); path.lineTo(s * 0.65, s * 0.15); path.lineTo(s * 0.85, s * 0.85); path.lineTo(s * 0.15, s * 0.85); path.closeSubpath(); p.setBrush(Qt::NoBrush); p.setPen(QPen(color, 1.5)); p.drawPath(path); p.drawLine(QPointF(s * 0.5, s * 0.8), QPointF(s * 0.7, s * 0.3)); break; }
    }
    return QIcon(pm);
}

}
