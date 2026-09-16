#include "ui/MainWindow.h"
#include "ui/TransportBar.h"
#include "ui/BrowserPanel.h"
#include "ui/ProjectSamplesPanel.h"
#include "ui/ChannelRackPanel.h"
#include "ui/PianoRollPanel.h"
#include "ui/PlaylistPanel.h"
#include "ui/MixerPanel.h"
#include "ui/PluginWindow.h"
#include "ui/dialogs/Dialogs.h"
#include "ui/Theme.h"
#include "core/Log.h"
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QSettings>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QMessageBox>
#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QFileInfo>
#include <QPixmap>
#include <QDebug>

namespace mt {

MainWindow::MainWindow(App& app, QWidget* parent) : QMainWindow(parent), m_app(app) {
    setWindowTitle("MusicThingy"); resize(1500, 900); setDockNestingEnabled(true); setDockOptions(AllowNestedDocks | AllowTabbedDocks | AnimatedDocks | GroupedDragging);
    setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea); setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
    m_workspace = new QMainWindow(this); m_workspace->setWindowFlags(Qt::Widget); m_workspace->setDockNestingEnabled(true); m_workspace->setDockOptions(AllowNestedDocks | AllowTabbedDocks | AnimatedDocks); auto* wc = new QWidget(m_workspace); wc->setFixedSize(0, 0); m_workspace->setCentralWidget(wc); m_pages = new QStackedWidget(this); m_pages->addWidget(m_workspace); setCentralWidget(m_pages);
    m_transport = new TransportBar(app, this); auto* tb = new QToolBar("Transport", this); tb->setObjectName("transportBar"); tb->setMovable(false); tb->addWidget(m_transport); addToolBar(Qt::TopToolBarArea, tb);
    buildDocks(); buildMenus();
    { // FL-style window toggles on the right of the toolbar
        auto mk = [&](const QString& key, theme::Glyph g, const QString& actionId, const QString& tip) { auto* b = new QToolButton(this); b->setIcon(theme::glyphIcon(g, QColor(0xc8, 0xcc, 0xd3))); b->setIconSize(QSize(15, 15)); b->setCheckable(true); b->setFocusPolicy(Qt::NoFocus); b->setToolTip(tip); b->setFixedSize(28, 22); connect(b, &QToolButton::clicked, this, [this, actionId] { if (auto* a = m_app.commands().action(actionId)) a->trigger(); }); m_transport->addWindowButton(b); m_winButtons[key] = b; };
        mk("playlist", theme::Glyph::Playlist, "view.playlist", "Playlist - arrange patterns, audio and automation clips into a song (F5)"); mk("pianoroll", theme::Glyph::PianoRoll, "view.pianoRoll", "Piano Roll - edit the notes of the selected channel (F7)"); mk("rack", theme::Glyph::ChannelRack, "view.rack", "Channel Rack - instruments and the step sequencer of the current pattern (F6)"); mk("mixer", theme::Glyph::Mixer, "view.mixer", "Mixer - effects, routing, sends and levels (F9)"); mk("browser", theme::Glyph::Browser, "view.browser", "Browser - samples, instruments, effects, plugins and presets (F8)"); mk("samples", theme::Glyph::Samples, "view.samples", "Project samples - every sample this project uses (Alt+S)");
        auto* plug = new QToolButton(this); plug->setIcon(theme::glyphIcon(theme::Glyph::Plugins, QColor(0xc8, 0xcc, 0xd3))); plug->setIconSize(QSize(15, 15)); plug->setFixedSize(28, 22); plug->setFocusPolicy(Qt::NoFocus); plug->setToolTip("Plugin manager - scan and organise VST2 / VST3 plugins"); connect(plug, &QToolButton::clicked, this, [this] { m_app.commands().action("options.plugins")->trigger(); }); m_transport->addWindowButton(plug);
        auto* gear = new QToolButton(this); gear->setIcon(theme::glyphIcon(theme::Glyph::Settings, QColor(0xc8, 0xcc, 0xd3))); gear->setIconSize(QSize(15, 15)); gear->setFixedSize(28, 22); gear->setFocusPolicy(Qt::NoFocus); gear->setToolTip("Audio settings - device, sample rate, buffer size"); connect(gear, &QToolButton::clicked, this, [this] { m_app.commands().action("options.audio")->trigger(); }); m_transport->addWindowButton(gear);
        auto* help = new QToolButton(this); help->setIcon(theme::glyphIcon(theme::Glyph::Help, QColor(0xc8, 0xcc, 0xd3))); help->setIconSize(QSize(15, 15)); help->setFixedSize(28, 22); help->setFocusPolicy(Qt::NoFocus); help->setToolTip("Quick help and keyboard shortcuts (F1)"); connect(help, &QToolButton::clicked, this, [this] { showHelp(); }); m_transport->addWindowButton(help);
    }
    qApp->installEventFilter(this);
    m_status = new QLabel(this); statusBar()->addWidget(m_status, 1); m_undoLabel = new QLabel(this); m_undoLabel->setStyleSheet("color:#8a919a;"); statusBar()->addPermanentWidget(m_undoLabel);
    connect(&m_app, &App::statusMessage, this, [this](const QString& m, int t) { if (t == 0) m_status->setText(m); else statusBar()->showMessage(m, t); });
    connect(&m_app, &App::requestOpenEditor, this, [this](Id id, const QString& t) { openEditor(id, t); });
    connect(&m_app, &App::projectLoaded, this, [this] { for (auto& [id, w] : m_editors) w->close(); m_editors.clear(); updateTitle(); m_pianoRoll->refresh(ChangeAll); MissingFilesDialog::checkAndPrompt(m_app, this); });
    connect(&m_app.model(), &ProjectModel::changed, this, &MainWindow::onChanged); connect(&m_app.model(), &ProjectModel::dirtyChanged, this, [this](bool) { updateTitle(); });
    connect(&m_app.model().undoStack(), &UndoStack::changed, this, [this] { auto& u = m_app.model().undoStack(); m_undoLabel->setText(u.canUndo() ? "Undo: " + u.undoText() : ""); m_app.commands().action("edit.undo")->setEnabled(u.canUndo()); m_app.commands().action("edit.redo")->setEnabled(u.canRedo()); });
    connect(&m_app, &App::midiLearnChanged, this, [this](bool on, const QString& n) { if (on) m_status->setText("MIDI learn: move a controller for '" + n + "' (Esc cancels)"); else m_status->clear(); });
    connect(&m_timer, &QTimer::timeout, this, &MainWindow::uiTick); m_timer.start(33);
    m_defaultState = saveState(); m_defaultWorkspaceState = m_workspace->saveState();
    QSettings s; if (s.contains("ui/geometry")) restoreGeometry(s.value("ui/geometry").toByteArray()); if (s.contains("ui/state")) restoreState(s.value("ui/state").toByteArray()); if (s.contains("ui/workspace")) m_workspace->restoreState(s.value("ui/workspace").toByteArray());
    updateTitle();
    QTimer::singleShot(0, this, [this] { if (!QSettings().contains("ui/state")) applyDefaultLayout(); if (qEnvironmentVariableIsSet("MT_DEBUG_LAYOUT")) { qDebug() << "win" << size() << "workspace" << m_workspace->geometry() << "hint" << m_workspace->sizeHint() << m_workspace->minimumSizeHint() << "rackdock" << m_dRack->geometry() << m_dRack->sizeHint() << m_dRack->minimumSizeHint() << "rack" << m_rack->sizeHint() << m_rack->minimumSizeHint() << "mixer" << m_mixer->sizeHint() << m_mixer->minimumSizeHint() << "playlistdock" << m_dPlaylist->geometry() << m_playlist->sizeHint() << m_playlist->minimumSizeHint(); } });
}

void MainWindow::buildDocks() {
    auto mkDock = [](QMainWindow* host, const QString& title, const QString& name, QWidget* w, Qt::DockWidgetArea area) { auto* d = new QDockWidget(title, host); d->setObjectName(name); d->setWidget(w); d->setAllowedAreas(Qt::AllDockWidgetAreas); d->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable); host->addDockWidget(area, d); return d; };
    m_browser = new BrowserPanel(m_app, this); m_dBrowser = mkDock(this, "Browser", "dockBrowser", m_browser, Qt::LeftDockWidgetArea); m_dBrowser->setMinimumWidth(220);
    m_playlist = new PlaylistPanel(m_app, this); m_dPlaylist = mkDock(m_workspace, "Playlist", "dockPlaylist", m_playlist, Qt::TopDockWidgetArea);
    m_pianoRoll = new PianoRollPanel(m_app, this); m_dPiano = mkDock(m_workspace, "Piano Roll", "dockPianoRoll", m_pianoRoll, Qt::TopDockWidgetArea);
    m_rack = new ChannelRackPanel(m_app, this); m_dRack = mkDock(this, "Channel Rack", "dockRack", m_rack, Qt::BottomDockWidgetArea);
    m_mixer = new MixerPanel(m_app, this); m_pages->addWidget(m_mixer); m_dMixer = nullptr;
    m_samples = new ProjectSamplesPanel(m_app, this); m_dSamples = mkDock(this, "Project Samples", "dockSamples", m_samples, Qt::RightDockWidgetArea); m_dSamples->setFloating(true); m_dSamples->hide();
    m_workspace->tabifyDockWidget(m_dPlaylist, m_dPiano); m_dPlaylist->raise();
    m_rack->setMinimumHeight(180); m_dRack->setMinimumHeight(200); resizeDocks({m_dBrowser}, {260}, Qt::Horizontal); resizeDocks({m_dRack}, {330}, Qt::Vertical);
    connect(m_browser, &BrowserPanel::openEditor, this, [this](Id id) { openEditor(id); });
    connect(m_rack, &ChannelRackPanel::openEditor, this, [this](Id id) { openEditor(id); });
    connect(m_dRack, &QDockWidget::topLevelChanged, this, [this](bool floating) { if (floating) { QSize s = m_dRack->size(); int w = std::min(s.width(), 900); if (s.width() > 950) QTimer::singleShot(0, m_dRack, [this, w, s] { m_dRack->resize(w, std::min(s.height(), 420)); }); } });
    connect(m_mixer, &MixerPanel::openEditor, this, [this](Id id, const QString& t) { openEditor(id, t); });
    connect(m_playlist, &PlaylistPanel::openPattern, this, [this](Id) { showWorkspaceDock(m_dPiano); m_pianoRoll->showChannel(m_app.project().selectedChannelId); });
    connect(m_playlist, &PlaylistPanel::openAudioClip, this, [this](Id id) { auto* e = new AudioClipEditor(m_app, id, this); e->show(); });
}

void MainWindow::buildMenus() {
    auto& reg = m_app.commands(); auto* mb = menuBar();
    auto* file = mb->addMenu("&File");
    file->addAction(reg.add("file.new", "New", "File", QKeySequence::New, [this] { if (maybeSave()) m_app.newProject(); }, this));
    auto* tpl = file->addMenu("New from template"); connect(tpl, &QMenu::aboutToShow, this, [this, tpl] { tpl->clear(); for (auto& t : m_app.templates()) tpl->addAction(QFileInfo(t).completeBaseName(), [this, t] { if (maybeSave()) m_app.newFromTemplate(t); }); if (tpl->isEmpty()) tpl->addAction("(no templates - use File > Save as template)")->setEnabled(false); });
    file->addAction(reg.add("file.open", "Open...", "File", QKeySequence::Open, [this] { doOpen(); }, this));
    auto* recent = file->addMenu("Open recent"); connect(recent, &QMenu::aboutToShow, this, [this, recent] { recent->clear(); for (auto& r : m_app.recentFiles()) recent->addAction(r, [this, r] { doOpen(r); }); if (recent->isEmpty()) recent->addAction("(empty)")->setEnabled(false); });
    file->addAction(reg.add("file.save", "Save", "File", QKeySequence::Save, [this] { doSave(); }, this)); file->addAction(reg.add("file.saveAs", "Save as...", "File", QKeySequence::SaveAs, [this] { doSaveAs(); }, this));
    file->addAction(reg.add("file.saveTemplate", "Save as template...", "File", QKeySequence(), [this] { bool ok; QString n = QInputDialog::getText(this, "Save template", "Template name:", QLineEdit::Normal, m_app.projectTitle(), &ok); if (ok && !n.isEmpty()) { m_app.saveTemplate(n); statusBar()->showMessage("Template saved", 3000); } }, this));
    file->addSeparator(); file->addAction(reg.add("file.export", "Export audio...", "File", QKeySequence("Ctrl+R"), [this] { ExportDialog d(m_app, this); d.exec(); }, this, false, QKeySequence("Ctrl+Shift+R")));
    file->addAction(reg.add("file.collect", "Collect project samples", "File", QKeySequence(), [this] { m_app.collectSamples(); }, this)); file->addAction(reg.add("file.missing", "Find missing samples...", "File", QKeySequence(), [this] { if (!MissingFilesDialog::checkAndPrompt(m_app, this)) statusBar()->showMessage("No missing samples", 3000); }, this));
    file->addAction(reg.add("file.info", "Project info...", "File", QKeySequence("F11"), [this] { ProjectInfoDialog d(m_app, this); d.exec(); }, this)); file->addSeparator(); file->addAction(reg.add("file.quit", "Quit", "File", QKeySequence::Quit, [this] { close(); }, this));
    auto* edit = mb->addMenu("&Edit");
    edit->addAction(reg.add("edit.undo", "Undo", "Edit", QKeySequence::Undo, [this] { m_app.model().undoStack().undo(); }, this)); edit->addAction(reg.add("edit.redo", "Redo", "Edit", QKeySequence("Ctrl+Alt+Z"), [this] { m_app.model().undoStack().redo(); }, this, false, QKeySequence("Ctrl+Y")));
    edit->addSeparator(); edit->addAction(reg.add("edit.addChannel", "Add channel (Synth)", "Edit", QKeySequence("Ctrl+Shift+N"), [this] { m_app.addChannel(ChannelType::Synth, "native.synth"); }, this)); edit->addAction(reg.add("edit.addPattern", "New pattern", "Edit", QKeySequence("F4"), [this] { m_app.addPattern(); }, this)); edit->addAction(reg.add("edit.clonePattern", "Clone current pattern", "Edit", QKeySequence("Ctrl+Shift+C"), [this] { m_app.clonePattern(m_app.project().selectedPatternId); }, this));
    edit->addAction(reg.add("edit.nextPattern", "Next pattern", "Edit", QKeySequence(Qt::KeypadModifier | Qt::Key_Plus), [this] { auto& p = m_app.project(); for (size_t i = 0; i + 1 < p.patterns.size(); ++i) if (p.patterns[i].id == p.selectedPatternId) { m_app.selectPattern(p.patterns[i + 1].id); return; } }, this)); edit->addAction(reg.add("edit.prevPattern", "Previous pattern", "Edit", QKeySequence(Qt::KeypadModifier | Qt::Key_Minus), [this] { auto& p = m_app.project(); for (size_t i = 1; i < p.patterns.size(); ++i) if (p.patterns[i].id == p.selectedPatternId) { m_app.selectPattern(p.patterns[i - 1].id); return; } }, this));
    edit->addAction(reg.add("edit.renamePattern", "Rename current pattern...", "Edit", QKeySequence("F2"), [this] { auto* p = m_app.project().pattern(m_app.project().selectedPatternId); if (!p) return; bool ok; QString n = QInputDialog::getText(this, "Rename pattern", "Name:", QLineEdit::Normal, p->name, &ok); if (ok && !n.isEmpty()) m_app.renamePattern(p->id, n); }, this));
    auto* tr = mb->addMenu("&Transport");
    tr->addAction(reg.add("transport.play", "Play / Stop", "Transport", QKeySequence(Qt::Key_Space), [this] { if (m_app.engine().isPlaying()) m_app.stop(); else m_app.play(); }, this));
    tr->addAction(reg.add("transport.pause", "Play / Pause", "Transport", QKeySequence("Ctrl+Space"), [this] { if (m_app.engine().isPlaying()) m_app.pause(); else m_app.play(); }, this));
    tr->addAction(reg.add("transport.stop", "Stop (twice: back to start)", "Transport", QKeySequence(), [this] { m_app.stop(); }, this));
    tr->addAction(reg.add("transport.escape", "Cancel / close focused window", "Transport", QKeySequence(Qt::Key_Escape), [this] { if (m_app.midiLearning()) m_app.cancelMidiLearn(); else if (m_dRack->isFloating() && m_dRack->isVisible() && m_dRack->isActiveWindow()) m_dRack->hide(); else if (m_pages->currentWidget() == m_mixer) showMixerPage(false); }, this));
    tr->addAction(reg.add("transport.record", "Record", "Transport", QKeySequence("R"), [this] { m_app.record(!m_app.engine().isRecording()); }, this));
    tr->addAction(reg.add("transport.toStart", "Go to start", "Transport", QKeySequence(Qt::Key_Home), [this] { m_app.seekToStart(); }, this));
    tr->addAction(reg.add("transport.songMode", "Toggle song / pattern mode", "Transport", QKeySequence("L"), [this] { m_app.setSongMode(!m_app.project().transport.songMode); }, this));
    tr->addAction(reg.add("transport.loop", "Toggle loop", "Transport", QKeySequence("Ctrl+L"), [this] { m_app.setLoop(!m_app.project().transport.loopEnabled); }, this));
    tr->addAction(reg.add("transport.metronome", "Toggle metronome", "Transport", QKeySequence("Ctrl+M"), [this] { m_app.setMetronome(!m_app.project().transport.metronome); }, this));
    tr->addAction(reg.add("transport.countIn", "Toggle count-in", "Transport", QKeySequence("Ctrl+P"), [this] { m_app.setCountIn(!m_app.project().transport.countIn); }, this));
    tr->addAction(reg.add("transport.typing", "Typing keyboard to piano keyboard", "Transport", QKeySequence("Ctrl+T"), [this] { m_typingKeyboard = !m_typingKeyboard; statusBar()->showMessage(m_typingKeyboard ? "Typing keyboard: on" : "Typing keyboard: off", 1500); }, this));
    tr->addAction(reg.add("transport.tempoUp", "Tempo +1", "Transport", QKeySequence("Ctrl+Up"), [this] { m_app.setBpm(m_app.project().transport.bpm + 1); }, this)); tr->addAction(reg.add("transport.tempoDown", "Tempo -1", "Transport", QKeySequence("Ctrl+Down"), [this] { m_app.setBpm(m_app.project().transport.bpm - 1); }, this));
    tr->addSeparator(); tr->addAction(reg.add("transport.panic", "Panic (all notes off)", "Transport", QKeySequence("Ctrl+Shift+P"), [this] { m_app.panic(); }, this));
    tr->addAction(reg.add("transport.octaveUp", "Typing keyboard octave up", "Transport", QKeySequence("Ctrl+]"), [this] { m_octave = std::min(8, m_octave + 1); statusBar()->showMessage(QString("Typing keyboard octave: %1").arg(m_octave), 1500); }, this)); tr->addAction(reg.add("transport.octaveDown", "Typing keyboard octave down", "Transport", QKeySequence("Ctrl+["), [this] { m_octave = std::max(0, m_octave - 1); statusBar()->showMessage(QString("Typing keyboard octave: %1").arg(m_octave), 1500); }, this));
    auto* view = mb->addMenu("&View");
    auto addDockAction = [&](const QString& id, QDockWidget* d, const QKeySequence& k, bool inWorkspace) { auto* a = reg.add(id, d->windowTitle(), "View", k, [this, d, inWorkspace] { bool shown = d->isVisible() && !d->visibleRegion().isEmpty() && (!inWorkspace || m_pages->currentWidget() == m_workspace); if (shown) { if (inWorkspace && m_dPlaylist->isVisible() && m_dPiano->isVisible() && !d->isFloating()) { d->hide(); } else d->hide(); } else { if (inWorkspace) showWorkspaceDock(d); else { d->show(); d->raise(); d->widget()->setFocus(); } } syncWindowButtons(); }, this); view->addAction(a); };
    addDockAction("view.playlist", m_dPlaylist, QKeySequence("F5"), true); addDockAction("view.rack", m_dRack, QKeySequence("F6"), false); addDockAction("view.pianoRoll", m_dPiano, QKeySequence("F7"), true); addDockAction("view.browser", m_dBrowser, QKeySequence("F8"), false);
    view->addAction(reg.add("view.mixer", "Mixer", "View", QKeySequence("F9"), [this] { showMixerPage(m_pages->currentWidget() != m_mixer); }, this));
    view->addAction(reg.add("view.samples", "Project samples", "View", QKeySequence("Alt+S"), [this] { if (m_dSamples->isVisible()) m_dSamples->hide(); else { if (m_dSamples->isFloating() && m_dSamples->width() < 200) m_dSamples->resize(600, 320); m_dSamples->show(); m_dSamples->raise(); } }, this));
    view->addAction(reg.add("view.closeAll", "Close all windows", "View", QKeySequence("F12"), [this] { closeAllWindows(); }, this));
    view->addSeparator(); view->addAction(reg.add("view.resetLayout", "Reset workspace layout", "View", QKeySequence(), [this] { resetLayout(); }, this));
    auto* opt = mb->addMenu("&Options");
    opt->addAction(reg.add("options.audio", "Audio settings...", "Options", QKeySequence(), [this] { AudioSettingsDialog d(m_app, this); d.exec(); }, this)); opt->addAction(reg.add("options.midi", "MIDI settings...", "Options", QKeySequence("F10"), [this] { MidiSettingsDialog d(m_app, this); d.exec(); }, this)); opt->addAction(reg.add("options.plugins", "Plugin manager...", "Options", QKeySequence(), [this] { PluginManagerDialog d(m_app, this); d.exec(); }, this)); opt->addAction(reg.add("options.shortcuts", "Keyboard shortcuts...", "Options", QKeySequence(), [this] { ShortcutsDialog d(m_app, this); d.exec(); }, this));
    auto* help = mb->addMenu("&Help"); help->addAction(reg.add("help.help", "Quick help", "Help", QKeySequence("F1"), [this] { showHelp(); }, this)); help->addAction(reg.add("help.about", "About MusicThingy", "Help", QKeySequence(), [this] { QMessageBox::about(this, "About MusicThingy", "<b>MusicThingy</b> - a Linux-native digital audio workstation.<br>Pattern-based sequencing, piano roll, playlist arrangement, mixer with routing/sends/sidechain, native instruments and effects, VST2/VST3 hosting, automation, recording and offline rendering.<br><br>Audio: PortAudio (ALSA/JACK/PipeWire). MIDI: ALSA sequencer."); }, this));
}

void MainWindow::onChanged(uint32_t flags) {
    m_transport->refresh(flags); m_rack->refresh(flags); m_pianoRoll->refresh(flags); m_playlist->refresh(flags); m_mixer->refresh(flags); m_browser->refresh(flags); m_samples->refresh(flags);
    for (auto it = m_editors.begin(); it != m_editors.end();) { Id id = it->first; bool exists = m_app.project().channel(id) != nullptr; for (auto& t : m_app.project().mixerTracks) for (auto& e : t.effects) if (e.id == id) exists = true; if (!exists) { it->second->close(); it = m_editors.erase(it); } else ++it; }
}
void MainWindow::uiTick() { m_transport->uiTick(); m_rack->uiTick(); m_pianoRoll->uiTick(); m_playlist->uiTick(); m_mixer->uiTick(); m_browser->uiTick(); syncWindowButtons(); }
void MainWindow::updateTitle() { setWindowTitle(QString("%1%2 - MusicThingy").arg(m_app.projectTitle(), m_app.model().isDirty() ? "*" : "")); }

void MainWindow::openEditor(Id id, const QString& titleIn) {
    QString title = titleIn; if (title.isEmpty()) { if (auto* c = m_app.project().channel(id)) title = c->name; for (auto& t : m_app.project().mixerTracks) for (auto& e : t.effects) if (e.id == id) title = t.name + ": " + e.name; }
    auto it = m_editors.find(id); if (it != m_editors.end()) { it->second->refreshTitle(title); it->second->show(); it->second->raise(); it->second->activateWindow(); return; }
    if (!m_app.processor(id)) { statusBar()->showMessage("No editor: processor is not loaded (missing plugin?)", 3000); return; }
    auto* w = new PluginWindow(m_app, id, title, this); m_editors[id] = w; connect(w, &QObject::destroyed, this, [this, id] { m_editors.erase(id); }); w->show();
}

bool MainWindow::maybeSave() { if (!m_app.model().isDirty()) return true; auto r = QMessageBox::question(this, "Unsaved changes", "Save changes to " + m_app.projectTitle() + "?", QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel); if (r == QMessageBox::Cancel) return false; if (r == QMessageBox::Save) return doSave(); return true; }
bool MainWindow::doOpen(const QString& pathIn) { if (!maybeSave()) return false; QString path = pathIn; if (path.isEmpty()) path = QFileDialog::getOpenFileName(this, "Open project", QString(), "MusicThingy project (*.mtp)"); if (path.isEmpty()) return false; return m_app.openProject(path); }
bool MainWindow::doSave() { if (m_app.model().filePath().isEmpty()) return doSaveAs(); return m_app.saveProject(); }
bool MainWindow::doSaveAs() { QString path = QFileDialog::getSaveFileName(this, "Save project", m_app.projectTitle() + ".mtp", "MusicThingy project (*.mtp)"); if (path.isEmpty()) return false; if (!path.endsWith(".mtp")) path += ".mtp"; bool ok = m_app.saveProjectAs(path); updateTitle(); return ok; }
void MainWindow::resetLayout() { restoreState(m_defaultState); m_workspace->restoreState(m_defaultWorkspaceState); for (auto* d : {m_dBrowser, m_dPlaylist, m_dPiano, m_dRack}) d->show(); m_dPlaylist->raise(); applyDefaultLayout(); }
// FL-style default: Browser docked left, Playlist / Piano Roll in the main area, Channel Rack floating over the
// workspace and the Mixer in its own floating window along the bottom.
void MainWindow::applyDefaultLayout() {
    resizeDocks({m_dBrowser}, {260}, Qt::Horizontal);
    QRect g = geometry(); int left = g.left() + 300, top = g.top() + 110;
    m_dRack->setFloating(true); m_dRack->move(left, top); m_dRack->show(); m_dRack->raise(); QTimer::singleShot(0, m_dRack, [this] { m_dRack->resize(m_rack->widthForSteps(16), 330); });
    showMixerPage(false);
}
void MainWindow::showMixerPage(bool on) { m_pages->setCurrentWidget(on ? static_cast<QWidget*>(m_mixer) : static_cast<QWidget*>(m_workspace)); if (on && m_dRack->isFloating()) m_dRack->hide(); syncWindowButtons(); }
void MainWindow::showWorkspaceDock(QDockWidget* d) { showMixerPage(false); d->show(); d->raise(); d->widget()->setFocus(); }
void MainWindow::closeAllWindows() { for (auto& [id, w] : m_editors) w->close(); if (m_dRack->isFloating()) m_dRack->hide(); if (m_dSamples->isFloating()) m_dSamples->hide(); showMixerPage(false); }
void MainWindow::syncWindowButtons() {
    auto set = [this](const QString& k, bool on) { auto it = m_winButtons.find(k); if (it != m_winButtons.end() && it->second->isChecked() != on) it->second->setChecked(on); };
    bool mixer = m_pages->currentWidget() == m_mixer; set("mixer", mixer); set("playlist", !mixer && m_dPlaylist->isVisible() && !m_dPlaylist->visibleRegion().isEmpty()); set("pianoroll", !mixer && m_dPiano->isVisible() && !m_dPiano->visibleRegion().isEmpty()); set("rack", m_dRack->isVisible()); set("browser", m_dBrowser->isVisible()); set("samples", m_dSamples->isVisible());
}
void MainWindow::startupChecks() { if (m_app.hasRecoveryFile()) { auto r = QMessageBox::question(this, "Recover project", "An autosaved recovery file from a previous session was found. Recover it?", QMessageBox::Yes | QMessageBox::No); if (r == QMessageBox::Yes) m_app.recoverAutosave(); else QFile::remove(m_app.autosavePath()); } }
void MainWindow::closeEvent(QCloseEvent* e) { if (!m_forceClose && !maybeSave()) { e->ignore(); return; } QSettings s; s.setValue("ui/geometry", saveGeometry()); s.setValue("ui/state", saveState()); s.setValue("ui/workspace", m_workspace->saveState()); for (auto& [id, w] : m_editors) w->close(); e->accept(); }
bool MainWindow::saveScreenshot(const QString& path) { QPixmap pm = grab(); bool ok = pm.save(path); for (auto* d : {m_dRack}) if (d->isFloating() && d->isVisible()) { QString p = path; p.insert(p.lastIndexOf('.'), "_" + d->objectName()); d->grab().save(p); } return ok; }

int MainWindow::typingNote(int key) const {
    static const char lower[] = "zsxdcvgbhnjm,l.;/"; static const char upper[] = "q2w3er5t6y7ui9o0p";
    const char* l = strchr(lower, (char)tolower(key)); if (l && *l) return m_octave * 12 + 12 + (int)(l - lower);
    const char* u = strchr(upper, (char)tolower(key)); if (u && *u) return (m_octave + 1) * 12 + 12 + (int)(u - upper);
    return -1;
}
void MainWindow::keyPressEvent(QKeyEvent* e) {
    QWidget* f = QApplication::focusWidget(); bool textual = qobject_cast<QLineEdit*>(f) || qobject_cast<QTextEdit*>(f) || qobject_cast<QPlainTextEdit*>(f) || qobject_cast<QAbstractSpinBox*>(f);
    if (m_typingKeyboard && !textual && e->modifiers() == Qt::NoModifier && !e->isAutoRepeat()) { int k = e->key(); if (k >= 0x20 && k < 0x7f) { int note = typingNote(k); if (note >= 0 && note <= 127) { if (!m_heldKeys.count(k)) { m_heldKeys.insert(k); m_app.noteOn(note); } e->accept(); return; } } }
    QMainWindow::keyPressEvent(e);
}
void MainWindow::keyReleaseEvent(QKeyEvent* e) { if (e->isAutoRepeat()) { QMainWindow::keyReleaseEvent(e); return; } int k = e->key(); if (m_heldKeys.count(k)) { m_heldKeys.erase(k); int note = typingNote(k); if (note >= 0) m_app.noteOff(note); e->accept(); return; } QMainWindow::keyReleaseEvent(e); }

// FL behaviour: clicking in the main workspace (playlist / piano roll / browser) closes the floating Channel Rack.
bool MainWindow::eventFilter(QObject* o, QEvent* e) {
    if (e->type() == QEvent::MouseButtonPress && m_dRack && m_dRack->isFloating() && m_dRack->isVisible()) {
        QWidget* w = qobject_cast<QWidget*>(o); if (w && w->window() == this && !m_dRack->isAncestorOf(w)) { QWidget* p = w; bool inMain = false; while (p) { if (p == m_pages || p == m_dBrowser) { inMain = true; break; } p = p->parentWidget(); } if (inMain) m_dRack->hide(); }
    }
    return QMainWindow::eventFilter(o, e);
}
void MainWindow::showHelp() {
    QMessageBox::information(this, "Quick help", R"(<b>Workflow</b><br>
Channel Rack: click steps to program beats (right-click erases, wheel = velocity, Ctrl+wheel = pitch). Double-click a channel name to open its instrument. Drag samples/plugins from the Browser onto the rack.<br>
Piano Roll: draw notes with the pencil (1), select (2), erase (3). Drag note edges to resize, Ctrl+Shift+drag duplicates, Ctrl+Q quantizes, Ctrl+L legato. Ctrl+wheel zooms.<br>
Playlist: click to place the selected pattern, drag to move, edges to resize, slice tool (4) splits, mute tool (5). Drag the ruler to set the loop region. Double-click audio clips for stretch/pitch/fades.<br>
Mixer: double-click an empty slot to add an effect, click to open, drag slots to reorder. Select a strip, then right-click another strip to route / send / sidechain to it.<br>
Automation: right-click any knob or fader &gt; "Create automation clip". Edit points in the Playlist (double-click adds, right-click for curve types).<br>
Recording: choose an input on a mixer strip, arm it (R), press Record. Live MIDI / typing keyboard plays the selected channel; with Record on, notes go into the current pattern.<br>
Typing keyboard: Z-M lower octave, Q-P upper octave, Ctrl+[ / Ctrl+] change octave.<br><br>
<b>Transport</b>: Space play/stop, Ctrl+Space play/pause, Stop twice = back to start, R record, L song/pattern mode, Ctrl+M metronome, Ctrl+P count-in, Ctrl+T typing keyboard on/off, Num +/- next/previous pattern.<br>
<b>Windows</b>: F5 Playlist, F6 Channel Rack, F7 Piano Roll, F8 Browser, F9 Mixer (full page), Alt+S project samples, F10 MIDI settings, F11 song info, F12 close all, Esc closes the focused window. Tools: P pencil, B brush, E select, D delete, C slice, T mute. Shortcuts are customisable under Options.)");
}

} // namespace mt
