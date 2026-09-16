#include "ui/Theme.h"
#include "core/Types.h"

namespace mt::theme {

QString stylesheet() {
    return R"(
QMainWindow, QDialog, QWidget { background: #1b1e23; color: #d6dae0; font-size: 9pt; }
QMenuBar { background: #22262c; } QMenuBar::item:selected { background: #3a4048; }
QMenu { background: #262a30; border: 1px solid #3a4048; } QMenu::item:selected { background: #3d4756; } QMenu::separator { height: 1px; background: #3a4048; margin: 4px 8px; }
QToolBar { background: #22262c; border: none; spacing: 4px; padding: 2px; }
QToolButton { background: #2d3239; border: 1px solid #3a4048; border-radius: 3px; padding: 3px 6px; color: #d6dae0; }
QToolButton:hover { background: #3a4048; } QToolButton:checked { background: #f2a93b; color: #1b1e23; border-color: #f2a93b; } QToolButton:pressed { background: #4e5866; }
QPushButton { background: #2d3239; border: 1px solid #3a4048; border-radius: 3px; padding: 4px 10px; color: #d6dae0; } QPushButton:hover { background: #3a4048; } QPushButton:checked { background: #f2a93b; color: #1b1e23; } QPushButton:disabled { color: #6a717a; }
QComboBox { background: #2d3239; border: 1px solid #3a4048; border-radius: 3px; padding: 2px 6px; color: #d6dae0; } QComboBox QAbstractItemView { background: #262a30; selection-background-color: #3d4756; }
QLineEdit, QSpinBox, QDoubleSpinBox, QTextEdit, QPlainTextEdit { background: #15181c; border: 1px solid #3a4048; border-radius: 3px; padding: 2px 4px; color: #d6dae0; selection-background-color: #f2a93b; selection-color: #1b1e23; }
QDockWidget { titlebar-close-icon: none; titlebar-normal-icon: none; } QDockWidget::title { background: #2a2f36; padding: 4px 8px; border-bottom: 1px solid #3a4048; font-weight: bold; }
QDockWidget::close-button, QDockWidget::float-button { background: #3a4048; border-radius: 2px; }
QTabBar::tab { background: #22262c; border: 1px solid #3a4048; padding: 4px 10px; } QTabBar::tab:selected { background: #2d3239; border-bottom-color: #f2a93b; }
QScrollBar:vertical { background: #1b1e23; width: 12px; margin: 0; } QScrollBar::handle:vertical { background: #3a4048; min-height: 20px; border-radius: 4px; margin: 2px; } QScrollBar::handle:vertical:hover { background: #4e5866; }
QScrollBar:horizontal { background: #1b1e23; height: 12px; margin: 0; } QScrollBar::handle:horizontal { background: #3a4048; min-width: 20px; border-radius: 4px; margin: 2px; } QScrollBar::handle:horizontal:hover { background: #4e5866; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QTreeView, QListView, QTableView { background: #1f2227; border: 1px solid #3a4048; alternate-background-color: #22262c; } QTreeView::item:selected, QListView::item:selected, QTableView::item:selected { background: #3d4756; }
QHeaderView::section { background: #2a2f36; border: 1px solid #3a4048; padding: 3px; }
QStatusBar { background: #22262c; border-top: 1px solid #3a4048; }
QSplitter::handle { background: #2a2f36; } QGroupBox { border: 1px solid #3a4048; border-radius: 4px; margin-top: 8px; padding-top: 6px; } QGroupBox::title { subcontrol-origin: margin; left: 8px; color: #8a919a; }
QCheckBox::indicator, QRadioButton::indicator { width: 12px; height: 12px; } QCheckBox::indicator:checked { background: #f2a93b; border: 1px solid #f2a93b; border-radius: 2px; } QCheckBox::indicator:unchecked { background: #15181c; border: 1px solid #3a4048; border-radius: 2px; }
QSlider::groove:horizontal { height: 4px; background: #15181c; border-radius: 2px; } QSlider::handle:horizontal { width: 12px; margin: -5px 0; background: #d6dae0; border-radius: 6px; }
QToolTip { background: #262a30; color: #d6dae0; border: 1px solid #3a4048; }
QProgressBar { background: #15181c; border: 1px solid #3a4048; border-radius: 3px; text-align: center; } QProgressBar::chunk { background: #f2a93b; }
)";
}

QString noteName(int midi) { static const char* names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"}; return QString("%1%2").arg(names[((midi % 12) + 12) % 12]).arg(midi / 12 - 1); }
QString tickToBBT(int64_t tick, int tsNum, int tsDen) { TimeSignature ts{tsNum, tsDen}; int64_t bar = ts.ticksPerBar(), beat = ts.ticksPerBeat(); if (tick < 0) tick = 0; return QString("%1:%2:%3").arg(tick / bar + 1, 3, 10, QChar('0')).arg((tick % bar) / beat + 1).arg(tick % beat, 3, 10, QChar('0')); }

}
