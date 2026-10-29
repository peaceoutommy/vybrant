#include "ui/MainWindow.h"
#include "plugins/PluginEmbedWidget.h"
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
#include <QSplitter>
#include <QMenu>
#include <QDialog>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QFileInfo>
#include <QDir>
#include <QPixmap>
#include <QScreen>
#include <QWindow>
#include "License.h"
#include "AppImage.h"
#include <QActionGroup>
#include <QCursor>
#include <QVariantAnimation>
#include <QElapsedTimer>
#include "ui/Anim.h"
#include <cstdio>
#include <QDebug>

namespace mt {
// The editor workspace: a nested QMainWindow whose docks (playlist, piano roll, rack) must not offer Qt's right-click dock toggle menu
// Bump when the dock structure changes in a way old saved states can't be restored into (e.g. the rack moving between areas)
static constexpr int kLayoutVersion = 1;   // 1 = rack docked via the split top area (older bottom-area states are migrated at startup)
class WorkspaceWindow : public QMainWindow {
public:
    using QMainWindow::QMainWindow;
    QMenu* createPopupMenu() override { return nullptr; }
};

// Browser | project strip: two independent panels, not a splitter. Each has its own width (with a min and a max) and its own
// grip; dragging a grip changes that panel only and grows/shrinks the dock by the same amount, so the other panel never moves.
// When the main window resizes the dock from its outer edge, the change goes to the strip alone.
class LeftPanels : public QWidget {
public:
    static constexpr int kGrip = 5;
    struct Bounds {
        int min, max;
    };
    LeftPanels(QWidget* browser, QWidget* strip, QWidget* parent)
        : QWidget(parent)
        , m_browser(browser)
        , m_strip(strip) {
        browser->setParent(this);
        strip->setParent(this);
        m_grip = new Grip(this, [this](int dx) { setBrowserWidth(m_browserW + dx); });
        updateBounds();
    }
    void setBrowserWidth(int w) {
        m_browserW = std::clamp(w, m_bb.min, m_bb.max);
        requestDockWidth();
    }
    void setStripWidth(int w) {
        m_stripW = std::clamp(w, m_sb.min, m_sb.max);
        requestDockWidth();
    }
    int browserWidth() const { return m_browserW; }
    int stripWidth() const { return m_stripW; }
    void setPanelsVisible(bool browser, bool strip) {
        m_browser->setVisible(browser);
        m_strip->setVisible(strip);
        m_grip->setVisible(browser && strip);
        requestDockWidth();
    }
    std::function<void(int)> onWantWidth;   // ask the main window to give the dock this width
    std::function<void()> onChanged;   // widths changed (the main window saves the layout)
    void resizeEvent(QResizeEvent*) override { relayout(); }

private:
    struct Grip : QWidget {
        std::function<void(int)> onDrag;
        int last = 0;
        Grip(QWidget* p, std::function<void(int)> f)
            : QWidget(p)
            , onDrag(std::move(f)) {
            setCursor(Qt::SplitHCursor);
            setFixedWidth(kGrip);
        }
        void mousePressEvent(QMouseEvent* e) override { last = e->globalPosition().toPoint().x(); }
        void mouseMoveEvent(QMouseEvent* e) override {
            if (!(e->buttons() & Qt::LeftButton)) return;
            int x = e->globalPosition().toPoint().x();
            onDrag(x - last);
            last = x;
        }
        void paintEvent(QPaintEvent*) override {
            QPainter p(this);
            p.fillRect(rect(), theme::withAlpha(theme::border(), 120));
        }
    };
    // isHidden, not isVisible: the panels' own state, which still counts while the dock itself is being shown or hidden
    int stripX() const { return m_browser->isHidden() ? 0 : m_browserW + kGrip; }
    int widthFor(int stripW) const { return !m_strip->isHidden() ? stripX() + stripW : m_browser->isHidden() ? 0 : m_browserW; }
    // The dock can only be as wide as browser + strip-max and as narrow as browser + strip-min: the main window's separator
    // then stops exactly at the limits instead of overshooting and snapping back on release.
    void updateBounds() {
        setMinimumWidth(widthFor(m_sb.min));
        setMaximumWidth(widthFor(m_sb.max));
    }
    void requestDockWidth() {
        int sw = m_stripW;
        updateBounds();   // new bounds resize the panel at once (showing / hiding a panel): not a width the strip should keep
        m_stripW = sw;
        m_wantW = widthFor(m_stripW);
        if (onWantWidth) onWantWidth(m_wantW);
        if (width() == m_wantW) relayout();
    }   // otherwise the dock resize delivers the resizeEvent
    // The strip always takes whatever the dock gives beyond the browser (the bounds keep that within the strip's min/max)
    void relayout() {
        int h = height();
        if (!m_strip->isHidden()) {
            int sw = std::clamp(width() - stripX(), m_sb.min, m_sb.max);
            if (sw != m_stripW) {
                m_stripW = sw;
                if (onChanged) onChanged();
            }
        }
        m_browser->setGeometry(0, 0, m_browserW, h);
        if (!m_strip->isHidden()) {
            m_grip->setGeometry(m_browserW, 0, kGrip, h);
            m_strip->setGeometry(stripX(), 0, m_stripW, h);
        }
    }
    QWidget* m_browser;
    QWidget* m_strip;
    Grip* m_grip;
    int m_browserW = 260, m_stripW = 180, m_wantW = -1;
    Bounds m_bb{200, 520}, m_sb{180, 300};
};

// Window buttons in the menu bar row (the window draws no system title bar): minimize, maximize / restore and close, as
// coloured dots like the desktop's own; the hovered one shows what it does
class WindowButtons : public QWidget {
public:
    WindowButtons(QWidget* window, QWidget* parent)
        : QWidget(parent)
        , m_window(window) {
        setMouseTracking(true);
        setFixedSize(3 * kStep + 10, 22);
        setStyleSheet("background: transparent;");   // the menu bar shows through, not the window colour
    }

private:
    static constexpr int kStep = 22;
    static constexpr double kR = 6;
    QWidget* m_window;
    int m_hot = -1;
    QPointF centre(int i) const { return QPointF(5 + kStep * i + kStep / 2.0, height() / 2.0); }
    int at(const QPoint& p) const {
        for (int i = 0; i < 3; ++i)
            if (QLineF(centre(i), p).length() <= kR + 3) return i;
        return -1;
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        static const QColor colors[] = {QColor(0xf5, 0xc5, 0x18), QColor(0xb0, 0x30, 0xff), QColor(0xff, 0x45, 0x45)};
        for (int i = 0; i < 3; ++i) {
            QPointF c = centre(i);
            p.setPen(Qt::NoPen);
            p.setBrush(colors[i]);
            p.drawEllipse(c, kR, kR);
            if (i != m_hot) continue;
            p.setPen(QPen(QColor(0, 0, 0, 170), 1.4, Qt::SolidLine, Qt::RoundCap));
            p.setBrush(Qt::NoBrush);
            if (i == 0) p.drawLine(c + QPointF(-3, 0), c + QPointF(3, 0));
            else if (i == 1) p.drawRect(QRectF(c - QPointF(2.5, 2.5), QSizeF(5, 5)));
            else {
                p.drawLine(c + QPointF(-2.5, -2.5), c + QPointF(2.5, 2.5));
                p.drawLine(c + QPointF(-2.5, 2.5), c + QPointF(2.5, -2.5));
            }
        }
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (int h = at(e->pos()); h != m_hot) {
            m_hot = h;
            setCursor(h >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
            update();
        }
    }
    void leaveEvent(QEvent*) override {
        m_hot = -1;
        update();
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        switch (at(e->pos())) {
        case 0: m_window->showMinimized(); break;
        case 1: m_window->isMaximized() ? m_window->showNormal() : m_window->showMaximized(); break;
        case 2: m_window->close(); break;
        }
    }
};

MainWindow::MainWindow(App& app, QWidget* parent)
    : QMainWindow(parent)
    , m_app(app) {
    setWindowTitle("Vybrant");
    resize(1500, 900);
    setDockNestingEnabled(true);
    setDockOptions(AllowNestedDocks | AllowTabbedDocks | AnimatedDocks | GroupedDragging);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
    setCorner(Qt::TopRightCorner, Qt::TopDockWidgetArea);
    m_workspace = new WorkspaceWindow(this);
    m_workspace->setWindowFlags(Qt::Widget);
    m_workspace->setDockNestingEnabled(true);
    m_workspace->setDockOptions(AllowNestedDocks | AllowTabbedDocks | AnimatedDocks);
    auto* wc = new QWidget(m_workspace);
    wc->setFixedSize(0, 0);
    m_workspace->setCentralWidget(wc);
    m_pages = new QStackedWidget(this);
    m_pages->addWidget(m_workspace);
    setCentralWidget(m_pages);
    m_transport = new TransportBar(app, this);
    m_transport->setObjectName("transport");   // see-through: the bar's glow shows behind it
    auto* tb = new QToolBar("Transport", this);
    tb->setObjectName("transportBar");
    tb->setMovable(false);
    tb->setContextMenuPolicy(Qt::PreventContextMenu);
    tb->addWidget(m_transport);
    addToolBar(Qt::TopToolBarArea, tb);
    m_toolBar = tb;
    buildDocks();
    buildMenus();
    setupWindowChrome();
    {   // FL-style window toggles on the right of the toolbar
        auto mk = [&](const QString& key, theme::Glyph g, const QString& actionId, const QString& tip) {
            auto* b = new QToolButton(this);
            theme::setGlyph(b, g, 21);
            b->setCheckable(true);
            b->setFocusPolicy(Qt::NoFocus);
            b->setToolTip(tip);
            b->setFixedSize(34, theme::kBarH);
            connect(b, &QToolButton::clicked, this, [this, actionId] {
                if (auto* a = m_app.commands().action(actionId)) a->trigger();
            });
            m_transport->addWindowButton(b);
            m_winButtons[key] = b;
        };
        mk("playlist", theme::Glyph::Playlist, "view.playlist", "Playlist - arrange patterns, audio and automation clips into a song (F5)");
        mk("pianoroll", theme::Glyph::PianoRoll, "view.pianoRoll", "Piano Roll - edit the notes of the selected channel (F7)");
        mk("rack", theme::Glyph::ChannelRack, "view.rack", "Channel Rack - instruments and the step sequencer of the current pattern (F6)");
        mk("mixer", theme::Glyph::Mixer, "view.mixer", "Mixer - effects, routing, sends and levels (F9)");
        mk("browser", theme::Glyph::Browser, "view.browser", "Browser - samples, instruments, effects, plugins and presets (F8)");
        mk("samples", theme::Glyph::Samples, "view.samples",
            "Project panel - the samples, patterns and automation in this project (Alt+S)");
        auto* plug = new QToolButton(this);
        theme::setGlyph(plug, theme::Glyph::Plugins, 21);
        plug->setFixedSize(34, theme::kBarH);
        plug->setFocusPolicy(Qt::NoFocus);
        plug->setToolTip("Plugin manager - scan and organise VST2 / VST3 / CLAP plugins");
        connect(plug, &QToolButton::clicked, this, [this] { m_app.commands().action("options.plugins")->trigger(); });
        m_transport->addWindowButton(plug);
        auto* gear = new QToolButton(this);
        theme::setGlyph(gear, theme::Glyph::Settings, 21);
        gear->setFixedSize(34, theme::kBarH);
        gear->setFocusPolicy(Qt::NoFocus);
        gear->setToolTip("Audio settings - device, sample rate, buffer size");
        connect(gear, &QToolButton::clicked, this, [this] { m_app.commands().action("options.audio")->trigger(); });
        m_transport->addWindowButton(gear);
        auto* help = new QToolButton(this);
        theme::setGlyph(help, theme::Glyph::Help, 21);
        help->setFixedSize(34, theme::kBarH);
        help->setFocusPolicy(Qt::NoFocus);
        help->setToolTip("Quick help and keyboard shortcuts (F1)");
        connect(help, &QToolButton::clicked, this, [this] { showHelp(); });
        m_transport->addWindowButton(help);
    }
    qApp->installEventFilter(this);
    connect(&m_rackReleasePoll, &QTimer::timeout, this, [this] {
        if (!m_dRack->isFloating() || !m_dRack->isVisible()) {
            m_rackReleasePoll.stop();
            hideDockIndicator();
            return;
        }
        QRect g = m_dRack->frameGeometry();
        if (g == m_lastRackGeom) {
            bool buttonsUp = !(QApplication::mouseButtons() & Qt::LeftButton);
            if (++m_rackStillTicks > (m_rackDragging ? 25 : 8) && buttonsUp) {
                m_rackReleasePoll.stop();
                m_rackDragging = false;
                if (m_rackCandidate != Qt::NoDockWidgetArea) dockRackAtCandidate();
                else hideDockIndicator();
            }
        } else {
            m_rackStillTicks = 0;
            m_lastRackGeom = g;
        }
    });
    m_status = new QLabel(this);
    statusBar()->addWidget(m_status, 1);
    m_undoLabel = theme::dimLabel(QString(), this);
    statusBar()->addPermanentWidget(m_undoLabel);
    connect(&m_app, &App::statusMessage, this, [this](const QString& m, int t) {
        if (t == 0) m_status->setText(m);
        else statusBar()->showMessage(m, t);
    });
    // A freshly added plugin has no processor yet - the engine builds it on its next poll, and a bridged plugin can take
    // seconds to boot Wine. Queue the request and let uiTick open the editor once the processor actually exists.
    connect(&m_app, &App::requestOpenEditor, this, [this](Id id, const QString& t) {
        if (!m_editors.count(id)) m_pendingEditors[id] = {t, 900};
    });
    connect(&m_app, &App::projectLoaded, this, [this] {
        for (auto& [id, w] : m_editors) w->close();
        m_editors.clear();
        updateTitle();
        m_pianoRoll->refresh(ChangeAll);
        MissingFilesDialog::checkAndPrompt(m_app, this);
    });
    connect(&m_app.model(), &ProjectModel::changed, this, &MainWindow::onChanged);
    connect(&m_app.model(), &ProjectModel::dirtyChanged, this, [this](bool) { updateTitle(); });
    connect(&m_app.model().undoStack(), &UndoStack::changed, this, [this] {
        auto& u = m_app.model().undoStack();
        m_undoLabel->setText(u.canUndo() ? "Undo: " + u.undoText() : "");
        m_app.commands().action("edit.undo")->setEnabled(u.canUndo());
        m_app.commands().action("edit.redo")->setEnabled(u.canRedo());
    });
    connect(&m_app, &App::midiLearnChanged, this, [this](bool on, const QString& n) {
        if (on) m_status->setText("MIDI learn: move a controller for '" + n + "' (Esc cancels)");
        else m_status->clear();
    });
    connect(&m_timer, &QTimer::timeout, this, &MainWindow::uiTick);
    m_timer.start(33);
    // Layout persistence: any dock move / float / resize schedules a save (1 s later), and a periodic save catches the rest -
    // the layout survives even when the app is killed instead of closed
    m_layoutSaveTimer.setSingleShot(true);
    m_layoutSaveTimer.setInterval(1000);
    connect(&m_layoutSaveTimer, &QTimer::timeout, this, &MainWindow::saveLayout);
    for (auto* d : {m_dRack, m_dBrowser, m_dPlaylist, m_dPiano}) {
        connect(d, &QDockWidget::dockLocationChanged, this, [this](Qt::DockWidgetArea) { scheduleLayoutSave(); });
        connect(d, &QDockWidget::topLevelChanged, this, [this](bool) { scheduleLayoutSave(); });
        connect(d, &QDockWidget::visibilityChanged, this, [this](bool) { scheduleLayoutSave(); });
    }
    {
        auto* periodic = new QTimer(this);
        periodic->setInterval(15000);
        connect(periodic, &QTimer::timeout, this, [this] {
            if (m_layoutReady) saveLayout();
        });
        periodic->start();
    }
    m_defaultState = saveState();
    m_defaultWorkspaceState = m_workspace->saveState();
    // order matters: the rack must already be a child of the right main window before either state is restored - restoring a
    // state that references the rack and then moving the rack leaves a dangling layout item behind (crash on first show)
    QSettings s;
    // Saved dock state from an incompatible layout scheme is discarded rather than restored into the wrong structure
    if (s.value("ui/layoutVersion", 1).toInt() != kLayoutVersion) {
        for (const char* k : {"ui/state", "ui/workspace", "ui/rackArea", "ui/rackHeight", "ui/rackVisible"}) s.remove(k);
    }
    if (s.contains("ui/geometry")) restoreGeometry(s.value("ui/geometry").toByteArray());
    restoreRackDock();
    // the main window's state can hold a placeholder for the rack from when it floated there, and restoreState matches docks by
    // name anywhere in the widget tree - a docked rack must not answer to it, or it floats again
    QString rackName = m_dRack->objectName();
    if (!m_dRack->isFloating()) m_dRack->setObjectName(QString());
    if (s.contains("ui/state")) restoreState(s.value("ui/state").toByteArray());
    m_dRack->setObjectName(rackName);
    m_toolBar->show();
    m_toolBar->toggleViewAction()->setVisible(false);
    if (s.contains("ui/workspace")) m_workspace->restoreState(s.value("ui/workspace").toByteArray());
    if (!m_dRack->isFloating() &&
        m_workspace->dockWidgetArea(m_dRack) == Qt::BottomDockWidgetArea) { /* layouts saved before the rack moved to the split top area */
        int h = s.value("ui/rackHeight", 0).toInt();
        QTimer::singleShot(0, this, [this, h] {
            placeRackInWorkspace(true);
            if (h > 60) m_workspace->resizeDocks({m_dRack}, {h}, Qt::Vertical);
            updateWorkspaceSeparators();
        });
    }
    enforceSingleEditor();
    updateTitle();
    QTimer::singleShot(0, this, [this] {
        {
            QSettings st;
            setLeftPanels(st.value("ui/browserVisible", true).toBool(), st.value("ui/samplesVisible", true).toBool());
            m_leftSplit->setBrowserWidth(st.value("ui/browserWidth", 260).toInt());
            m_leftSplit->setStripWidth(st.value("ui/stripWidth", 180).toInt());
        }
        if (!QSettings().contains("ui/state")) applyDefaultLayout();
        m_layoutReady = true;
        if (qEnvironmentVariableIsSet("MT_DEBUG_LAYOUT")) {
            fprintf(stderr, "layout: browser %d,%d %dx%d vis=%d | samples %d,%d %dx%d vis=%d floating=%d area=%d\n", m_dBrowser->x(),
                m_dBrowser->y(), m_dBrowser->width(), m_dBrowser->height(), (int)m_dBrowser->isVisible(), m_dSamples->x(), m_dSamples->y(),
                m_dSamples->width(), m_dSamples->height(), (int)m_dSamples->isVisible(), (int)m_dSamples->isFloating(),
                (int)dockWidgetArea(m_dSamples));
            qDebug() << "win" << size() << "workspace" << m_workspace->geometry() << "hint" << m_workspace->sizeHint()
                     << m_workspace->minimumSizeHint() << "rackdock" << m_dRack->geometry() << m_dRack->sizeHint()
                     << m_dRack->minimumSizeHint() << "rack" << m_rack->sizeHint() << m_rack->minimumSizeHint() << "mixer"
                     << m_mixer->sizeHint() << m_mixer->minimumSizeHint() << "playlistdock" << m_dPlaylist->geometry()
                     << m_playlist->sizeHint() << m_playlist->minimumSizeHint();
        }
    });
}

void MainWindow::buildDocks() {
    auto mkDock = [](QMainWindow* host, const QString& title, const QString& name, QWidget* w, Qt::DockWidgetArea area) {
        auto* d = new QDockWidget(title, host);
        d->setObjectName(name);
        d->setWidget(w);
        d->setAllowedAreas(Qt::AllDockWidgetAreas);
        d->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
        host->addDockWidget(area, d);
        return d;
    };
    // Browser and the project strip share one dock through a splitter: the strip resizes against the browser with the inner
    // handle, and the dock's outer edge only resizes the strip (the browser keeps its width)
    m_browser = new BrowserPanel(m_app, this);
    m_samples = new ProjectSamplesPanel(m_app, this);
    m_leftSplit = new LeftPanels(m_browser, m_samples, this);
    m_dBrowser = mkDock(this, "Browser", "dockBrowser", m_leftSplit, Qt::LeftDockWidgetArea);
    m_dSamples = m_dBrowser;
    m_leftSplit->onWantWidth = [this](int w) {
        resizeDocks({m_dBrowser}, {w}, Qt::Horizontal);
        scheduleLayoutSave();
    };
    m_leftSplit->onChanged = [this] { scheduleLayoutSave(); };
    m_playlist = new PlaylistPanel(m_app, this);
    m_dPlaylist = mkDock(m_workspace, "Playlist", "dockPlaylist", m_playlist, Qt::TopDockWidgetArea);
    m_pianoRoll = new PianoRollPanel(m_app, this);
    m_dPiano = mkDock(m_workspace, "Piano Roll", "dockPianoRoll", m_pianoRoll, Qt::TopDockWidgetArea);
    // fixed panels don't need a title bar: the browser / samples / playlist / piano roll are where they are (the rack keeps its bar - that's how it floats and docks)
    auto noTitle = [](QDockWidget* d) {
        auto* empty = new QWidget(d);
        empty->setFixedHeight(0);
        d->setTitleBarWidget(empty);
    };
    m_rack = new ChannelRackPanel(m_app, this);
    m_dRack = mkDock(this, "Channel Rack", "dockRack", m_rack, Qt::BottomDockWidgetArea);
    m_mixer = new MixerPanel(m_app, this);
    m_pages->addWidget(m_mixer);
    m_dMixer = nullptr;
    for (auto* d : {m_dBrowser, m_dPlaylist, m_dPiano}) noTitle(d);
    m_workspace->tabifyDockWidget(m_dPlaylist, m_dPiano);
    m_dPiano->hide();
    m_dPlaylist->show();
    m_dPlaylist->raise();
    m_rack->setMinimumHeight(180);
    m_dRack->setMinimumHeight(200);
    resizeDocks({m_dBrowser}, {260}, Qt::Horizontal);
    resizeDocks({m_dRack}, {330}, Qt::Vertical);
    connect(m_browser, &BrowserPanel::openEditor, this, [this](Id id) { openEditor(id); });
    connect(m_rack, &ChannelRackPanel::openEditor, this, [this](Id id) { openEditor(id); });
    connect(m_rack, &ChannelRackPanel::openPianoRoll, this, [this](Id id) {
        m_app.selectChannel(id);
        closeAllWindows(); /* rack, editors, mixer page out of the way: the piano roll takes the stage */
        showWorkspaceDock(m_dPiano);
        m_pianoRoll->showChannel(id);
        m_dPiano->raise();
        m_pianoRoll->setFocus();
        syncWindowButtons();
    });
    QTimer::singleShot(0, this, [this] {
        m_dRack->setAllowedAreas(Qt::NoDockWidgetArea);
        updateWorkspaceSeparators();
    });   // edge-based docking only (see eventFilter)
    connect(m_dRack, &QDockWidget::topLevelChanged, this, [this](bool floating) {
        QTimer::singleShot(0, this, [this] { updateWorkspaceSeparators(); });
        if (floating) m_dRack->setAllowedAreas(Qt::NoDockWidgetArea);
        if (floating) {
            QSize s = m_dRack->size();
            int w = std::min(s.width(), 900);
            if (s.width() > 950) QTimer::singleShot(0, m_dRack, [this, w, s] { m_dRack->resize(w, std::min(s.height(), 420)); });
        }
    });
    connect(m_mixer, &MixerPanel::openEditor, this, [this](Id id, const QString& t) { openEditor(id, t); });
    connect(m_playlist, &PlaylistPanel::openPattern, this, [this](Id) {
        showWorkspaceDock(m_dPiano);
        m_pianoRoll->showChannel(m_app.project().selectedChannelId);
    });
    connect(m_playlist, &PlaylistPanel::openAudioClip, this, [this](Id id) {
        auto* e = new AudioClipEditor(m_app, id, this);
        e->show();
    });
    // project panel: a sample's details are the playlist's clip editor (or the sample alone / its channel), and it can point
    // out where the sample sits in the playlist
    connect(m_samples, &ProjectSamplesPanel::openAudioClip, this, [this](Id id) { (new AudioClipEditor(m_app, id, this))->show(); });
    connect(m_samples, &ProjectSamplesPanel::openSample, this, [this](Id sid) { (new AudioClipEditor(m_app, 0, this, sid))->show(); });
    connect(m_samples, &ProjectSamplesPanel::openEditor, this, [this](Id ch) { openEditor(ch); });
    connect(m_samples, &ProjectSamplesPanel::picked, m_playlist->view(), &PlaylistView::pickSource);
    connect(&m_app, &App::revealInProject, m_samples, &ProjectSamplesPanel::reveal);
    connect(m_samples, &ProjectSamplesPanel::showOnPlaylist, this, [this](const std::vector<Id>& ids) {
        if (m_pages->currentWidget() != m_workspace) showMixerPage(false);
        showWorkspaceDock(m_dPlaylist);
        m_playlist->view()->revealClips(ids);
    });
}

void MainWindow::buildMenus() {
    auto& reg = m_app.commands();
    auto* mb = menuBar();
    auto* file = mb->addMenu("&File");
    file->addAction(reg.add(
        "file.new", "New", "File", QKeySequence::New,
        [this] {
            if (maybeSave()) m_app.newProject();
        },
        this));
    auto* tpl = file->addMenu("New from template");
    connect(tpl, &QMenu::aboutToShow, this, [this, tpl] {
        tpl->clear();
        for (auto& t : m_app.templates())
            tpl->addAction(QFileInfo(t).completeBaseName(), [this, t] {
                if (maybeSave()) m_app.newFromTemplate(t);
            });
        if (tpl->isEmpty()) tpl->addAction("(no templates - use File > Save as template)")->setEnabled(false);
    });
    file->addAction(reg.add("file.open", "Open...", "File", QKeySequence::Open, [this] { doOpen(); }, this));
    auto* recent = file->addMenu("Open recent");
    connect(recent, &QMenu::aboutToShow, this, [this, recent] {
        recent->clear();
        for (auto& r : m_app.recentFiles()) recent->addAction(r, [this, r] { doOpen(r); });
        if (recent->isEmpty()) recent->addAction("(empty)")->setEnabled(false);
    });
    file->addAction(reg.add("file.save", "Save", "File", QKeySequence::Save, [this] { doSave(); }, this));
    file->addAction(reg.add("file.saveAs", "Save as...", "File", QKeySequence::SaveAs, [this] { doSaveAs(); }, this));
    auto* backups = file->addMenu("Open backup");
    connect(backups, &QMenu::aboutToShow, this, [this, backups] {
        backups->clear();
        auto list = m_app.backups();
        for (auto& b : list)
            backups->addAction(QFileInfo(b).completeBaseName().section('_', -2).replace('_', ' '), [this, b] {
                if (!maybeSave()) return;
                QString orig = m_app.model().filePath();
                if (m_app.openProject(b)) {
                    m_app.model().setFilePath(orig);
                    m_app.model().setDirty(true);
                    updateTitle();
                    statusBar()->showMessage("Backup loaded - Ctrl+S saves it over the project", 5000);
                }
            });
        if (list.isEmpty())
            backups->addAction(m_app.model().filePath().isEmpty() ? "(save the project to enable backups)" : "(no backups yet)")
                ->setEnabled(false);
    });
    file->addAction(reg.add(
        "file.saveTemplate", "Save as template...", "File", QKeySequence(),
        [this] {
            if (auto n = askName(this, "Save template", m_app.projectTitle())) {
                m_app.saveTemplate(*n);
                statusBar()->showMessage("Template saved", 3000);
            }
        },
        this));
    file->addSeparator();
    file->addAction(reg.add(
        "file.export", "Export audio...", "File", QKeySequence("Ctrl+R"),
        [this] {
            ExportDialog d(m_app, this);
            d.exec();
        },
        this, false, QKeySequence("Ctrl+Shift+R")));
    file->addAction(reg.add(
        "file.importMidi", "Import MIDI file...", "File", QKeySequence(),
        [this] {
            QString p = QFileDialog::getOpenFileName(this, "Import MIDI file", QString(), "MIDI files (*.mid *.midi)");
            if (!p.isEmpty()) m_app.importMidiFile(p);
        },
        this));
    file->addAction(reg.add(
        "file.exportMidi", "Export MIDI file...", "File", QKeySequence(),
        [this] {
            QString p = QFileDialog::getSaveFileName(this, "Export MIDI file", m_app.projectTitle() + ".mid", "MIDI file (*.mid)");
            if (!p.isEmpty()) m_app.exportMidiFile(p);
        },
        this));
    file->addAction(reg.add("file.collect", "Collect project samples", "File", QKeySequence(), [this] { m_app.collectSamples(); }, this));
    file->addAction(reg.add(
        "file.missing", "Find missing samples...", "File", QKeySequence(),
        [this] {
            if (!MissingFilesDialog::checkAndPrompt(m_app, this)) statusBar()->showMessage("No missing samples", 3000);
        },
        this));
    file->addAction(reg.add(
        "file.info", "Project info...", "File", QKeySequence("F11"),
        [this] {
            ProjectInfoDialog d(m_app, this);
            d.exec();
        },
        this));
    file->addSeparator();
    file->addAction(reg.add("file.quit", "Quit", "File", QKeySequence::Quit, [this] { close(); }, this));
    auto* edit = mb->addMenu("&Edit");
    edit->addAction(reg.add("edit.undo", "Undo", "Edit", QKeySequence::Undo, [this] { m_app.model().undoStack().undo(); }, this));
    edit->addAction(reg.add(
        "edit.redo", "Redo", "Edit", QKeySequence("Ctrl+Alt+Z"), [this] { m_app.model().undoStack().redo(); }, this, false,
        QKeySequence("Ctrl+Y")));
    edit->addSeparator();
    edit->addAction(reg.add(
        "edit.addChannel", "Add channel (Synth)", "Edit", QKeySequence("Ctrl+Shift+N"),
        [this] { m_app.addChannel(ChannelType::Synth, "native.synth"); }, this));
    edit->addAction(reg.add("edit.addMixerTrack", "Add mixer track", "Edit", QKeySequence(), [this] { m_app.addMixerTrack(); }, this));
    edit->addAction(reg.add("edit.addPattern", "New pattern", "Edit", QKeySequence("F4"), [this] { m_app.addPattern(); }, this));
    edit->addAction(reg.add(
        "edit.clonePattern", "Clone current pattern", "Edit", QKeySequence("Ctrl+Shift+C"),
        [this] { m_app.clonePattern(m_app.project().selectedPatternId); }, this));
    edit->addAction(reg.add(
        "edit.nextPattern", "Next pattern", "Edit", QKeySequence(Qt::KeypadModifier | Qt::Key_Plus),
        [this] {
            auto& p = m_app.project();
            for (size_t i = 0; i + 1 < p.patterns.size(); ++i)
                if (p.patterns[i].id == p.selectedPatternId) {
                    m_app.selectPattern(p.patterns[i + 1].id);
                    return;
                }
        },
        this));
    edit->addAction(reg.add(
        "edit.prevPattern", "Previous pattern", "Edit", QKeySequence(Qt::KeypadModifier | Qt::Key_Minus),
        [this] {
            auto& p = m_app.project();
            for (size_t i = 1; i < p.patterns.size(); ++i)
                if (p.patterns[i].id == p.selectedPatternId) {
                    m_app.selectPattern(p.patterns[i - 1].id);
                    return;
                }
        },
        this));
    edit->addAction(reg.add(
        "edit.renamePattern", "Rename current pattern...", "Edit", QKeySequence("F2"),
        [this] {
            auto* p = m_app.project().pattern(m_app.project().selectedPatternId);
            if (!p) return;
            if (auto n = askName(this, "Rename pattern", p->name)) m_app.renamePattern(p->id, *n);
        },
        this));
    auto* tr = mb->addMenu("&Transport");
    tr->addAction(reg.add(
        "transport.play", "Play / Stop", "Transport", QKeySequence(Qt::Key_Space),
        [this] {
            if (m_app.engine().isPlaying()) m_app.stop();
            else m_app.play();
        },
        this));
    tr->addAction(reg.add(
        "transport.pause", "Play / Pause", "Transport", QKeySequence("Ctrl+Space"),
        [this] {
            if (m_app.engine().isPlaying()) m_app.pause();
            else m_app.play();
        },
        this));
    tr->addAction(reg.add("transport.stop", "Stop (twice: back to start)", "Transport", QKeySequence(), [this] { m_app.stop(); }, this));
    tr->addAction(reg.add(
        "transport.escape", "Cancel / close focused window", "Transport", QKeySequence(Qt::Key_Escape),
        [this] {
            if (m_app.midiLearning()) m_app.cancelMidiLearn();
            else if (m_dRack->isFloating() && m_dRack->isVisible() && m_dRack->isActiveWindow()) m_dRack->hide();
            else if (m_pages->currentWidget() == m_mixer) showMixerPage(false);
        },
        this));
    tr->addAction(reg.add(
        "transport.record", "Record", "Transport", QKeySequence("R"), [this] { m_app.record(!m_app.engine().isRecording()); }, this));
    tr->addAction(
        reg.add("transport.toStart", "Go to start", "Transport", QKeySequence(Qt::Key_Home), [this] { m_app.seekToStart(); }, this));
    tr->addAction(reg.add(
        "transport.songMode", "Toggle song / pattern mode", "Transport", QKeySequence("L"),
        [this] { m_app.setSongMode(!m_app.project().transport.songMode); }, this));
    tr->addAction(reg.add(
        "transport.loop", "Toggle loop", "Transport", QKeySequence("Ctrl+L"),
        [this] { m_app.setLoop(!m_app.project().transport.loopEnabled); }, this));
    tr->addAction(reg.add(
        "transport.metronome", "Toggle metronome", "Transport", QKeySequence("Ctrl+M"),
        [this] { m_app.setMetronome(!m_app.project().transport.metronome); }, this));
    tr->addAction(reg.add(
        "transport.countIn", "Toggle count-in", "Transport", QKeySequence("Ctrl+P"),
        [this] { m_app.setCountIn(!m_app.project().transport.countIn); }, this));
    {
        auto* a = reg.add(
            "transport.typing", "Typing keyboard to piano keyboard", "Transport", QKeySequence("Ctrl+T"),
            [this] { setTypingKeyboard(!m_typingKeyboard); }, this, true);
        tr->addAction(a);
        QToolButton* b = m_transport->typingButton();
        connect(b, &QToolButton::clicked, this, [this](bool on) { setTypingKeyboard(on); });
        m_typingKeyboard = QSettings().value("ui/typingKeyboard", false).toBool();
        PluginEmbedWidget::keepHostKeyboard = m_typingKeyboard;
        a->setChecked(m_typingKeyboard);
        b->setChecked(m_typingKeyboard);
    }
    tr->addAction(reg.add(
        "transport.tempoUp", "Tempo +1", "Transport", QKeySequence("Ctrl+Up"), [this] { m_app.setBpm(m_app.bpmAtPlayhead() + 1); }, this));
    tr->addAction(reg.add(
        "transport.tempoDown", "Tempo -1", "Transport", QKeySequence("Ctrl+Down"), [this] { m_app.setBpm(m_app.bpmAtPlayhead() - 1); },
        this));
    tr->addSeparator();
    tr->addAction(reg.add(
        "transport.panic", "Panic (all notes off)", "Transport", QKeySequence("Ctrl+H"), [this] { m_app.panic(); }, this, false,
        QKeySequence("Ctrl+Shift+P")));
    tr->addAction(reg.add(
        "transport.nextBar", "Next bar", "Transport", QKeySequence(Qt::KeypadModifier | Qt::Key_Asterisk),
        [this] {
            TempoMap m = m_app.project().transport.tempoMap();
            m_app.seek(m.barStart(m.barAt((Tick)m_app.engine().positionTicks()) + 1));
        },
        this));
    tr->addAction(reg.add(
        "transport.prevBar", "Previous bar", "Transport", QKeySequence(Qt::KeypadModifier | Qt::Key_Slash),
        [this] {
            TempoMap m = m_app.project().transport.tempoMap();
            m_app.seek(std::max<Tick>(0, m.barStart(m.barAt((Tick)m_app.engine().positionTicks() - 1))));
        },
        this));
    tr->addAction(reg.add(
        "transport.octaveUp", "Typing keyboard octave up", "Transport", QKeySequence("Ctrl+]"),
        [this] {
            m_octave = std::min(8, m_octave + 1);
            statusBar()->showMessage(QString("Typing keyboard octave: %1").arg(m_octave), 1500);
        },
        this));
    tr->addAction(reg.add(
        "transport.octaveDown", "Typing keyboard octave down", "Transport", QKeySequence("Ctrl+["),
        [this] {
            m_octave = std::max(0, m_octave - 1);
            statusBar()->showMessage(QString("Typing keyboard octave: %1").arg(m_octave), 1500);
        },
        this));
    auto* view = mb->addMenu("&View");
    auto addDockAction = [&](const QString& id, QDockWidget* d, const QKeySequence& k, bool inWorkspace) {
        auto* a = reg.add(
            id, d->windowTitle(), "View", k,
            [this, d, inWorkspace] {
                bool shown = d->isVisible() && !d->visibleRegion().isEmpty() && (!inWorkspace || m_pages->currentWidget() == m_workspace);
                if (inWorkspace) showWorkspaceDock(d); /* switch, never toggle off: the workspace always shows one of the two editors */
                else if (shown) d->hide();
                else {
                    d->show();
                    d->raise();
                    d->widget()->setFocus();
                }
                syncWindowButtons();
            },
            this);
        view->addAction(a);
    };
    addDockAction("view.playlist", m_dPlaylist, QKeySequence("F5"), true);
    addDockAction("view.rack", m_dRack, QKeySequence("F6"), false);
    addDockAction("view.pianoRoll", m_dPiano, QKeySequence("F7"), true);
    view->addAction(reg.add(
        "view.browser", "Browser", "View", QKeySequence("F8"), [this] { setLeftPanels(m_browser->isHidden(), !m_samples->isHidden()); },
        this));
    view->addAction(
        reg.add("view.mixer", "Mixer", "View", QKeySequence("F9"), [this] { showMixerPage(m_pages->currentWidget() != m_mixer); }, this));
    view->addAction(reg.add(
        "view.samples", "Project panel (samples / patterns / automation)", "View", QKeySequence("Alt+S"),
        [this] { setLeftPanels(!m_browser->isHidden(), m_samples->isHidden()); }, this));
    view->addAction(reg.add("view.closeAll", "Close all windows", "View", QKeySequence("F12"), [this] { closeAllWindows(); }, this));
    view->addSeparator();
    view->addAction(reg.add("view.resetLayout", "Reset workspace layout", "View", QKeySequence(), [this] { resetLayout(); }, this));
    auto* opt = mb->addMenu("&Options");
    opt->addAction(reg.add(
        "options.audio", "Audio settings...", "Options", QKeySequence(),
        [this] {
            AudioSettingsDialog d(m_app, this);
            d.exec();
        },
        this));
    opt->addAction(reg.add(
        "options.midi", "MIDI settings...", "Options", QKeySequence("F10"),
        [this] {
            MidiSettingsDialog d(m_app, this);
            d.exec();
        },
        this));
    opt->addAction(reg.add(
        "options.plugins", "Plugin manager...", "Options", QKeySequence(),
        [this] {
            PluginManagerDialog d(m_app, this);
            d.exec();
        },
        this));
    opt->addAction(reg.add(
        "options.theme", "Theme...", "Options", QKeySequence(),
        [this] {
            ThemeDialog d(this);
            d.exec();
        },
        this));
    {
        auto* pv = opt->addMenu("Note preview (Piano Roll)");
        auto* grp = new QActionGroup(pv);
        const char* labels[] = {
            "When placing or re-pitching notes, only while stopped (default)", "Always, even while the song plays", "Never"};
        for (int i = 0; i < 3; ++i) {
            auto* a = pv->addAction(labels[i]);
            a->setCheckable(true);
            a->setActionGroup(grp);
            a->setChecked(QSettings().value("pianoroll/preview", 0).toInt() == i);
            connect(a, &QAction::triggered, this, [i] { QSettings().setValue("pianoroll/preview", i); });
        }
    }
    opt->addAction(reg.add(
        "options.autosave", "Autosave / backups...", "Options", QKeySequence(),
        [this] {
            AutosaveDialog d(m_app, this);
            d.exec();
        },
        this));
    opt->addAction(reg.add(
        "options.shortcuts", "Keyboard shortcuts...", "Options", QKeySequence(),
        [this] {
            ShortcutsDialog d(m_app, this);
            d.exec();
        },
        this));
    auto* help = mb->addMenu("&Help");
    help->addAction(reg.add("help.help", "Quick help", "Help", QKeySequence("F1"), [this] { showHelp(); }, this));
    help->addAction(reg.add(
        "help.license", "License...", "Help", QKeySequence(),
        [this] {
            std::unique_ptr<QDialog> d(license::makeDialog(this, false));
            d->exec();
        },
        this));
    // AppImage only: add this copy to the applications menu (it asks on the first start), or take it out again
    auto* menuEntry = help->addAction(QString(), [this] {
        QString err;
        if (appimage::installed()) appimage::uninstall();
        else if (!appimage::install(appimage::running(), &err)) QMessageBox::warning(this, "Install Vybrant", err);
    });
    connect(help, &QMenu::aboutToShow, this, [menuEntry] {
        menuEntry->setText(appimage::installed() ? "Remove from applications menu" : "Add to applications menu");
        menuEntry->setVisible(appimage::installed() || !appimage::running().isEmpty());
    });
    help->addAction(reg.add(
        "help.about", "About Vybrant", "Help", QKeySequence(),
        [this] {
            QMessageBox::about(this, "About Vybrant",
                "<b>Vybrant</b> - a digital audio workstation for Linux and Windows.<br>Pattern-based sequencing, piano roll, playlist arrangement, mixer with routing/sends/sidechain, native instruments and effects, VST2/VST3/CLAP hosting, automation, recording and offline rendering.<br><br>Audio: PortAudio (ALSA/JACK/PipeWire on Linux, WASAPI/DirectSound/MME on Windows). MIDI: ALSA sequencer / WinMM.");
        },
        this));
}

void MainWindow::onChanged(uint32_t flags) {
    if ((flags & (ChangeStructure | ChangeChannels)) && m_dRack->isFloating() && m_dRack->isVisible()) {
        int need = m_rack->heightForContent() + 24;
        if (m_dRack->height() < need)
            m_dRack->resize(m_dRack->width(), std::min(need, screen() ? screen()->availableGeometry().height() - 100 : need));
    }
    m_transport->refresh(flags);
    m_rack->refresh(flags);
    m_pianoRoll->refresh(flags);
    m_playlist->refresh(flags);
    m_mixer->refresh(flags);
    m_browser->refresh(flags);
    m_samples->refresh(flags);
    for (auto it = m_editors.begin(); it != m_editors.end();) {
        Id id = it->first;
        bool exists = m_app.project().channel(id) != nullptr;
        for (auto& t : m_app.project().mixerTracks)
            for (auto& e : t.effects)
                if (e.id == id) exists = true;
        if (!exists) {
            it->second->close();
            it = m_editors.erase(it);
        } else ++it;
    }
}
void MainWindow::uiTick() {
    m_transport->uiTick();
    m_rack->uiTick();
    m_pianoRoll->uiTick();
    m_playlist->uiTick();
    m_mixer->uiTick();
    m_browser->uiTick();
    syncWindowButtons();
    enforceSingleEditor();
    for (auto it = m_pendingEditors.begin(); it != m_pendingEditors.end();) {
        if (m_app.processor(it->first)) {
            Id id = it->first;
            QString title = it->second.first;
            it = m_pendingEditors.erase(it);
            openEditor(id, title);
        } else if (--it->second.second <= 0) it = m_pendingEditors.erase(it);   // gave up: the plugin failed to load
        else ++it;
    }
}
void MainWindow::setupWindowChrome() {
    setWindowFlag(Qt::FramelessWindowHint);
    setMouseTracking(true);   // the edge band is this window's own (its contents margins): cursor shapes and resizing
    setContentsMargins(4, 4, 4, 4);
    auto* mb = menuBar();
    m_titleLabel = theme::dimLabel(windowTitle(), mb);
    m_titleLabel->setAlignment(Qt::AlignCenter);
    m_titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_windowButtons = new WindowButtons(this, mb);   // placed by placeTitle (QMenuBar never lays out a corner widget here)
}
void MainWindow::placeTitle() {
    auto* mb = menuBar();
    if (!m_titleLabel) return;
    int left = 0;
    for (QAction* a : mb->actions()) left = std::max(left, mb->actionGeometry(a).right() + 12);
    m_windowButtons->move(mb->width() - m_windowButtons->width() - 6, (mb->height() - m_windowButtons->height()) / 2);
    int right = m_windowButtons->x() - 12;
    QFontMetrics fm(m_titleLabel->font());
    int w = std::min(fm.horizontalAdvance(windowTitle()) + 8, std::max(0, right - left));
    int x = std::clamp(mb->width() / 2 - w / 2, left, std::max(left, right - w));
    m_titleLabel->setText(fm.elidedText(windowTitle(), Qt::ElideRight, w));
    m_titleLabel->setGeometry(x, 0, w, mb->height());
    m_titleLabel->setVisible(w > 40);
}
Qt::Edges MainWindow::edgesAt(const QPoint& p) const {
    Qt::Edges e;
    if (isMaximized() || isFullScreen()) return e;
    const int m = 6;
    if (p.x() < m) e |= Qt::LeftEdge;
    if (p.x() >= width() - m) e |= Qt::RightEdge;
    if (p.y() < m) e |= Qt::TopEdge;
    if (p.y() >= height() - m) e |= Qt::BottomEdge;
    return e;
}
void MainWindow::mouseMoveEvent(QMouseEvent* e) {
    Qt::Edges ed = edgesAt(e->pos());
    bool h = ed & (Qt::LeftEdge | Qt::RightEdge), v = ed & (Qt::TopEdge | Qt::BottomEdge);
    if (h && v)
        setCursor(ed == (Qt::LeftEdge | Qt::TopEdge) || ed == (Qt::RightEdge | Qt::BottomEdge) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
    else if (h) setCursor(Qt::SizeHorCursor);
    else if (v) setCursor(Qt::SizeVerCursor);
    else unsetCursor();
    QMainWindow::mouseMoveEvent(e);
}
void MainWindow::mousePressEvent(QMouseEvent* e) {
    if (Qt::Edges ed = edgesAt(e->pos()); ed && e->button() == Qt::LeftButton && windowHandle()) {
        windowHandle()->startSystemResize(ed);
        return;
    }
    QMainWindow::mousePressEvent(e);
}
void MainWindow::paintEvent(QPaintEvent* e) {
    QMainWindow::paintEvent(e);
    if (isMaximized() || isFullScreen()) return;
    QPainter p(this);   // a hairline frame: without the system's, the window would melt into a dark desktop
    p.setPen(theme::border());
    p.drawRect(rect().adjusted(0, 0, -1, -1));
}
void MainWindow::updateTitle() { setWindowTitle(QString("%1%2 - Vybrant").arg(m_app.projectTitle(), m_app.model().isDirty() ? "*" : "")); }

void MainWindow::openEditor(Id id, const QString& titleIn) {
    QString title = titleIn;
    if (title.isEmpty()) {
        if (auto* c = m_app.project().channel(id)) title = c->name;
        for (auto& t : m_app.project().mixerTracks)
            for (auto& e : t.effects)
                if (e.id == id) title = t.name + ": " + e.name;
    }
    auto it = m_editors.find(id);
    if (it != m_editors.end()) {
        it->second->refreshTitle(title);
        it->second->show();
        it->second->raise();
        it->second->activateWindow();
        return;
    }
    if (!m_app.processor(id)) {
        statusBar()->showMessage("No editor: processor is not loaded (missing plugin?)", 3000);
        return;
    }
    auto* w = new PluginWindow(m_app, id, title, this);
    m_editors[id] = w;
    connect(w, &QObject::destroyed, this, [this, id] { m_editors.erase(id); });
    const QRect area = screen()->availableGeometry();   // left alone, the window manager puts it top left
    const QPoint centred = area.center() - w->rect().center();
    w->move(std::max(centred.x(), area.left()), std::max(centred.y(), area.top()));   // too big to fit: its title bar stays on screen
    w->show();
}

bool MainWindow::maybeSave() {
    if (!m_app.model().isDirty()) return true;
    auto r = QMessageBox::question(this, "Unsaved changes", "Save changes to " + m_app.projectTitle() + "?",
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Save) return doSave();
    return true;
}
bool MainWindow::doOpen(const QString& pathIn) {
    if (!maybeSave()) return false;
    QString path = pathIn;
    if (path.isEmpty()) path = QFileDialog::getOpenFileName(this, "Open project", App::projectsDir(), "Vybrant project (*.mtp)");
    if (path.isEmpty()) return false;
    return m_app.openProject(path);
}
bool MainWindow::doSave() {
    if (m_app.model().filePath().isEmpty()) return doSaveAs();
    return m_app.saveProject();
}
bool MainWindow::doSaveAs() {
    const QString cur = m_app.model().filePath();   // a saved song's "Save as" starts in its own folder
    const QString start = (cur.isEmpty() ? App::projectsDir() : QFileInfo(cur).absolutePath()) + "/" + m_app.projectTitle() + ".mtp";
    QString path = QFileDialog::getSaveFileName(this, "Save project", start, "Vybrant project (*.mtp)");
    if (path.isEmpty()) return false;
    if (!path.endsWith(".mtp")) path += ".mtp";
    const QString target = App::projectFolderPath(path);   // the dialog asked about the file it showed, not this one
    if (target != path && QFileInfo::exists(target) &&
        QMessageBox::question(this, "Save project",
            "A project named " + QFileInfo(target).completeBaseName() + " already exists. Replace it?") != QMessageBox::Yes)
        return false;
    QDir().mkpath(QFileInfo(target).absolutePath());
    bool ok = m_app.saveProjectAs(target);
    updateTitle();
    return ok;
}
void MainWindow::resetLayout() {
    restoreState(m_defaultState);
    m_workspace->restoreState(m_defaultWorkspaceState);
    for (auto* d : {m_dPlaylist, m_dPiano, m_dRack}) d->show();
    setLeftPanels(true, true);
    m_dPlaylist->raise();
    applyDefaultLayout();
}
// The browser and the project strip share one dock but show and hide independently; the dock is there while either one is
void MainWindow::setLeftPanels(bool browser, bool samples) {
    m_dBrowser->setVisible(browser || samples);   // first: the panels can only size a shown dock
    m_leftSplit->setPanelsVisible(browser, samples);
    QSettings s;
    s.setValue("ui/browserVisible", browser);
    s.setValue("ui/samplesVisible", samples);
    syncWindowButtons();
}
// FL-style default: Browser docked left, Playlist / Piano Roll in the main area, Channel Rack floating over the
// workspace and the Mixer in its own floating window along the bottom.
void MainWindow::applyDefaultLayout() {
    m_leftSplit->setBrowserWidth(260);
    m_leftSplit->setStripWidth(180);
    QRect g = geometry();
    int left = g.left() + 300, top = g.top() + 110;
    m_dRack->setFloating(true);
    m_dRack->move(left, top);
    m_dRack->show();
    m_dRack->raise();
    QTimer::singleShot(0, m_dRack, [this] { m_dRack->resize(m_rack->widthForSteps(16), m_rack->heightForContent() + 24); });
    showMixerPage(false);
}
void MainWindow::showMixerPage(bool on) {
    m_pages->setCurrentWidget(on ? static_cast<QWidget*>(m_mixer) : static_cast<QWidget*>(m_workspace));
    if (on && m_dRack->isFloating()) m_dRack->hide();
    syncWindowButtons();
}
// Playlist and Piano Roll share one tabified slot, but the toolbar buttons are the only intended way to switch between
// them. Hiding the sibling leaves a single tab, which Qt drops the tab bar for on its own - hiding the tab bar directly
// would stop QDockWidget::raise() from switching tabs at all.
void MainWindow::showWorkspaceDock(QDockWidget* d) {
    showMixerPage(false);
    if (d == m_dPlaylist) m_dPiano->hide();
    else if (d == m_dPiano) m_dPlaylist->hide();
    d->show();
    d->raise();
    d->widget()->setFocus();
}
// Only one of the two editors is ever docked in the workspace, which is what keeps Qt from drawing a dock tab bar for
// them. Restoring a saved layout (or a drag) can bring both back, so the invariant is re-checked rather than set once.
void MainWindow::enforceSingleEditor() {
    if (!m_dPlaylist->isVisible() || !m_dPiano->isVisible()) return;
    if (m_dPlaylist->isFloating() || m_dPiano->isFloating()) return;   // torn off on purpose: leave it alone
    (m_dPiano->visibleRegion().isEmpty() ? m_dPiano : m_dPlaylist)->hide();   // keep whichever is actually on top
}
void MainWindow::closeAllWindows() {
    for (auto& [id, w] : m_editors) w->close();
    if (m_dRack->isFloating()) m_dRack->hide();
    showMixerPage(false);
}
void MainWindow::syncWindowButtons() {
    auto set = [this](const QString& k, bool on) {
        auto it = m_winButtons.find(k);
        if (it != m_winButtons.end() && it->second->isChecked() != on) it->second->setChecked(on);
    };
    bool mixer = m_pages->currentWidget() == m_mixer;
    set("mixer", mixer);
    set("playlist", !mixer && m_dPlaylist->isVisible() && !m_dPlaylist->visibleRegion().isEmpty());
    set("pianoroll", !mixer && m_dPiano->isVisible() && !m_dPiano->visibleRegion().isEmpty());
    set("rack", m_dRack->isVisible());
    set("browser", m_browser->isVisible());
    set("samples", m_samples->isVisible());
}
void MainWindow::startupChecks() {
    if (m_app.hasRecoveryFile()) {
        auto r = QMessageBox::question(this, "Recover project", "An autosaved recovery file from a previous session was found. Recover it?",
            QMessageBox::Yes | QMessageBox::No);
        if (r == QMessageBox::Yes) m_app.recoverAutosave();
        else QFile::remove(m_app.autosavePath());
    }
}
void MainWindow::closeEvent(QCloseEvent* e) {
    if (!m_forceClose && !maybeSave()) {
        e->ignore();
        return;
    }
    saveLayout();
    for (auto& [id, w] : m_editors) w->close();
    e->accept();
}
void MainWindow::scheduleLayoutSave() {
    if (m_layoutReady) m_layoutSaveTimer.start();
}
// Everything about the window arrangement: geometry, dock states of both main windows, where the rack is docked (top /
// bottom of the workspace / floating), the browser and project-strip widths and which of the two are shown
void MainWindow::saveLayout() {
    if (!m_layoutReady || m_dRack->minimumHeight() == m_dRack->maximumHeight()) return;   // not while the rack is mid-animation
    QSettings s;
    s.setValue("ui/layoutVersion", kLayoutVersion);
    s.setValue("ui/geometry", saveGeometry());
    s.setValue("ui/state", saveState());
    s.setValue("ui/workspace", m_workspace->saveState());
    bool inWorkspace = !m_dRack->isFloating() && m_workspace->isAncestorOf(m_dRack);
    s.setValue("ui/rackArea", !inWorkspace ? "floating" : rackDockedAtBottom() ? "bottom" : "top");
    s.setValue("ui/rackVisible", m_dRack->isVisible());
    s.setValue("ui/rackHeight", m_dRack->height());
    s.setValue("ui/browserWidth", m_leftSplit->browserWidth());
    s.setValue("ui/stripWidth", m_leftSplit->stripWidth());
    s.setValue("ui/browserVisible", !m_browser->isHidden());
    s.setValue("ui/samplesVisible", !m_samples->isHidden());
}
// The rack lives in the main window while floating and moves into the workspace when docked; restoreState can only place it
// if it is already a child of the right window, so re-dock it first (no animation) when the saved layout had it docked
void MainWindow::restoreRackDock() {
    QSettings s;
    QString where = s.value("ui/rackArea", "floating").toString();
    if (where != "top" && where != "bottom") return;
    Qt::DockWidgetArea area = where == "top" ? Qt::TopDockWidgetArea : Qt::BottomDockWidgetArea;
    m_dRack->setFloating(false);
    placeRackInWorkspace(area == Qt::BottomDockWidgetArea);
    m_dRack->setVisible(s.value("ui/rackVisible", true).toBool());
    int h = s.value("ui/rackHeight", 0).toInt();
    if (h > 60)
        QTimer::singleShot(0, this, [this, h] {
            m_workspace->resizeDocks({m_dRack}, {h}, Qt::Vertical);
            updateWorkspaceSeparators();
        });
    QTimer::singleShot(0, this, [this] {
        m_dRack->setAllowedAreas(Qt::NoDockWidgetArea);
        updateWorkspaceSeparators();
    });
}
// The rack always lives in the workspace's top dock area, split above or below the editors (playlist / piano roll tabs):
// putting it in the bottom area would leave Qt's empty central widget between the two and draw two resizable separators
void MainWindow::placeRackInWorkspace(bool bottom) {
    m_dRack->setAllowedAreas(Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea);
    if (bottom) {
        m_workspace->addDockWidget(Qt::TopDockWidgetArea, m_dRack, Qt::Vertical);
        return;
    }   // appended below whatever the area holds; the editor tabs stay intact
    // above the editors: split against whichever editor is shown (a hidden one would turn the split into a tab), then re-tab the other one
    QDockWidget* ref = (m_dPlaylist->isVisible() || !m_dPiano->isVisible()) ? m_dPlaylist : m_dPiano;
    QDockWidget* other = ref == m_dPlaylist ? m_dPiano : m_dPlaylist;
    bool pianoOnTop = m_dPiano->isVisible() && !m_dPiano->visibleRegion().isEmpty();
    m_workspace->addDockWidget(Qt::TopDockWidgetArea, m_dRack);
    m_workspace->splitDockWidget(m_dRack, ref, Qt::Vertical);
    m_workspace->tabifyDockWidget(ref, other);
    (pianoOnTop ? m_dPiano : m_dPlaylist)->raise();
}
bool MainWindow::rackDockedAtBottom() const {
    if (m_workspace->dockWidgetArea(m_dRack) == Qt::BottomDockWidgetArea) return true;
    const QDockWidget* ref = m_dPlaylist->isVisible() ? m_dPlaylist : m_dPiano;
    return m_dRack->geometry().top() > ref->geometry().top();
}
bool MainWindow::saveScreenshot(const QString& path) {
    QPixmap pm = grab();
    bool ok = pm.save(path);
    if (QWidget* m = QApplication::activeModalWidget()) {
        QString p = path;
        p.insert(p.lastIndexOf('.'), "_dialog");
        m->grab().save(p);
    }
    int ei = 0;
    for (auto& [id, w] : m_editors)
        if (w && w->isVisible()) {
            QString p = path;
            p.insert(p.lastIndexOf('.'), QString("_editor%1").arg(ei++));
            w->grab().save(p);
        }
    for (auto* d : {m_dRack})
        if (d->isFloating() && d->isVisible()) {
            QString p = path;
            p.insert(p.lastIndexOf('.'), "_" + d->objectName());
            d->grab().save(p);
        }
    return ok;
}

int MainWindow::typingNote(int key) const {
    static const char lower[] = "zsxdcvgbhnjm,l.;/";
    static const char upper[] = "q2w3er5t6y7ui9o0p";
    const char* l = strchr(lower, (char)tolower(key));
    if (l && *l) return m_octave * 12 + 12 + (int)(l - lower);
    const char* u = strchr(upper, (char)tolower(key));
    if (u && *u) return (m_octave + 1) * 12 + 12 + (int)(u - upper);
    return -1;
}
void MainWindow::keyPressEvent(QKeyEvent* e) { QMainWindow::keyPressEvent(e); }
void MainWindow::keyReleaseEvent(QKeyEvent* e) { QMainWindow::keyReleaseEvent(e); }
void MainWindow::setTypingKeyboard(bool on) {
    if (m_typingKeyboard == on) return;
    m_typingKeyboard = on;
    QSettings().setValue("ui/typingKeyboard", on);
    PluginEmbedWidget::keepHostKeyboard = on;
    if (!on) {
        for (int k : m_heldKeys) {
            int n = typingNote(k);
            if (n >= 0) m_app.noteOff(n);
        }
        m_heldKeys.clear();
    }
    if (auto* a = m_app.commands().action("transport.typing")) a->setChecked(on);
    m_transport->typingButton()->setChecked(on);
    statusBar()->showMessage(
        on ? "Typing keyboard: keys play the selected channel (Ctrl+T to return to shortcuts)" : "Typing keyboard off: keys are shortcuts",
        2000);
}
// Typing keyboard mode: while on, unmodified letter/number keys play the selected channel and never reach the editors'
// single-key shortcuts (P, B, E, D, R, L, 1..9 ...). Off, the same keys are plain keybinds. Text fields always win.
bool MainWindow::typingKeyEvent(QObject* o, QEvent* e) {
    if (!m_typingKeyboard) return false;
    QEvent::Type t = e->type();
    if (t != QEvent::KeyPress && t != QEvent::KeyRelease && t != QEvent::ShortcutOverride) return false;
    // Plugin editors are separate Qt::Tool top-levels, so their focus widget's window() is the PluginWindow rather than
    // this one. They still have to count as ours: every command is a Qt::ApplicationShortcut, so without this the letter
    // keys skip typing mode and fire the shortcuts instead whenever a plugin editor holds focus.
    QWidget* f = QApplication::focusWidget();
    // Ours: the main window, plugin/sound editors, floating docks (rack, project samples) - anything whose parent chain
    // leads back here, except dialogs (settings, file pickers) and popup menus, where letters keep their normal meaning.
    auto ours = [this](QWidget* x) {
        QWidget* win = x ? x->window() : nullptr;
        if (!win) return false;
        if (win == this || qobject_cast<PluginWindow*>(win)) return true;
        if (qobject_cast<QDialog*>(win) || qobject_cast<QMenu*>(win)) return false;
        for (QWidget* w = win->parentWidget(); w; w = w->parentWidget())
            if (w == this) return true;
        return false;
    };
    if (!ours(qobject_cast<QWidget*>(o)) && !ours(f)) return false;
    auto* combo = qobject_cast<QComboBox*>(f);
    bool textual = qobject_cast<QLineEdit*>(f) || qobject_cast<QTextEdit*>(f) || qobject_cast<QPlainTextEdit*>(f) ||
        qobject_cast<QAbstractSpinBox*>(f) || (combo && combo->isEditable());
    if (textual) return false;   // a plain selector combo (loop mode, filter type...) is not a text field: letters still play
    auto* ke = static_cast<QKeyEvent*>(e);
    if (ke->modifiers() & ~(Qt::KeypadModifier | Qt::ShiftModifier)) return false;
    int k = ke->key();
    if (k < 0x20 || k >= 0x7f) return false;
    int note = typingNote(k);
    if (note < 0 || note > 127) return false;
    if (t == QEvent::ShortcutOverride) {
        ke->accept();
        return true;
    }
    if (ke->isAutoRepeat()) return true;
    if (t == QEvent::KeyPress) {
        if (!m_heldKeys.count(k)) {
            m_heldKeys.insert(k);
            m_app.noteOn(note);
        }
    } else if (m_heldKeys.count(k)) {
        m_heldKeys.erase(k);
        m_app.noteOff(note);
    }
    return true;
}

// FL behaviour: clicking in the main workspace (playlist / piano roll / mixer) closes the floating Channel Rack; the Browser is exempt so you can browse sounds while the rack stays open.
// Clicking the workspace (playlist, piano roll, rack, mixer - the main window or its floating panels) puts away the editor
// windows: plugin / instrument windows are hidden (reopening the channel's editor shows them again as they were) and clip
// editors close (their edits are already committed). Runs before the clicked widget, so a click that opens an editor still does.
void MainWindow::hideEditorsOnWorkspaceClick(QObject* o, QEvent* e) {
    if (e->type() != QEvent::MouseButtonPress) return;
    auto* w = qobject_cast<QWidget*>(o);
    QWidget* win = w ? w->window() : nullptr;
    // a popup (a dropdown's list, a menu) is no workspace click, even when it belongs to an editor through its parents
    if (!win || win->windowType() == Qt::Popup || qobject_cast<PluginWindow*>(win) || qobject_cast<AudioClipEditor*>(win) ||
        qobject_cast<QDialog*>(win))
        return;
    bool ours = win == this;
    for (QWidget* p = win->parentWidget(); p && !ours; p = p->parentWidget()) ours = p == this;
    if (!ours) return;
    for (auto* pw : findChildren<PluginWindow*>())
        if (pw->isVisible()) pw->hide();
    for (auto* ce : findChildren<AudioClipEditor*>())
        if (ce->isVisible()) ce->close();
}
bool MainWindow::eventFilter(QObject* o, QEvent* e) {
    if (typingKeyEvent(o, e)) return true;
    if (o == this && (e->type() == QEvent::WindowTitleChange || e->type() == QEvent::Resize)) placeTitle();
    if (o == this && e->type() == QEvent::WindowStateChange) {   // maximized: no edge band to resize from
        int m = isMaximized() || isFullScreen() ? 0 : 4;
        setContentsMargins(m, m, m, m);
    }
    if (o == menuBar()) {
        auto* me = static_cast<QMouseEvent*>(e);
        if (e->type() == QEvent::Resize || e->type() == QEvent::LayoutRequest) placeTitle();
        else if ((e->type() == QEvent::MouseButtonPress || e->type() == QEvent::MouseButtonDblClick) && me->button() == Qt::LeftButton &&
            !menuBar()->actionAt(me->pos()) && windowHandle()) {   // empty menu bar space: the title bar
            if (e->type() == QEvent::MouseButtonDblClick) isMaximized() ? showNormal() : showMaximized();
            else windowHandle()->startSystemMove();
            return true;
        }
    }
    hideEditorsOnWorkspaceClick(o, e);
    // Dock the floating rack by its *own* edges. On X11 floating docks have native window-manager decorations, so the
    // drag itself is done by the WM: Qt only sees non-client-area press/move/release events and the resulting
    // QEvent::Move. We track the drag through those, show an indicator while an edge is touched and dock on release.
    if (o == m_dRack && (e->type() == QEvent::NonClientAreaMouseButtonPress || e->type() == QEvent::MouseButtonPress))
        m_rackDragging = true;   // docked or floating: a title-bar press starts a drag
    if (o == m_dRack && m_dRack->isFloating()) {
        if (m_rackDragging && (e->type() == QEvent::Move || e->type() == QEvent::NonClientAreaMouseMove || e->type() == QEvent::MouseMove))
            updateRackDockCandidate();
        if (e->type() == QEvent::NonClientAreaMouseButtonRelease || e->type() == QEvent::MouseButtonRelease) {
            m_rackDragging = false;
            if (m_rackCandidate != Qt::NoDockWidgetArea) dockRackAtCandidate();
            else hideDockIndicator();
        }
    }
    if (e->type() == QEvent::MouseButtonPress && m_dRack && m_dRack->isFloating() && m_dRack->isVisible()) {
        QWidget* w = qobject_cast<QWidget*>(o);
        if (w && w->window() == this && !m_dRack->isAncestorOf(w)) {
            QWidget* p = w;
            bool inMain = false;
            while (p) {
                if (p == m_dBrowser || p == m_dSamples) return QMainWindow::eventFilter(o, e);
                if (p == m_pages) {
                    inMain = true;
                    break;
                }
                p = p->parentWidget();
            }
            if (inMain) m_dRack->hide();
        }
    }
    return QMainWindow::eventFilter(o, e);
}
// Slide the freshly docked rack open instead of letting the layout jump (undocking is already animated by Qt)
// The workspace is a QMainWindow whose central widget is empty, so Qt still offers a resize separator along its bottom edge.
// Only show separators when the rack is actually docked there (then the playlist/rack split is a real resize).
QString MainWindow::debugLeftSizes() const {
    return QString("dock=%1 browser=%2 strip=%3 panel=%4")
        .arg(m_dBrowser->width())
        .arg(m_leftSplit->browserWidth())
        .arg(m_leftSplit->stripWidth())
        .arg(m_leftSplit->width());
}
QString MainWindow::debugLayout() const {
    bool inWs = !m_dRack->isFloating() && m_workspace->isAncestorOf(m_dRack);
    return QString("rack=%1 visible=%2 height=%3 browser=%4 strip=%5 stripVisible=%6 browserVisible=%7")
        .arg(inWs ? (rackDockedAtBottom() ? "bottom" : "top") : "floating")
        .arg(m_dRack->isVisible())
        .arg(m_dRack->height())
        .arg(m_leftSplit->browserWidth())
        .arg(m_leftSplit->stripWidth())
        .arg(m_samples->isVisible())
        .arg(m_browser->isVisible());
}
QMenu* MainWindow::createPopupMenu() {
    return nullptr;
}   // no Qt toolbar/dock toggle menu on right-click: one wrong click there hid the whole toolbar
void MainWindow::updateWorkspaceSeparators() {
    bool rackDocked = m_dRack && !m_dRack->isFloating() && m_dRack->parentWidget() && m_workspace->isAncestorOf(m_dRack);
    m_workspace->setStyleSheet(
        rackDocked ? QString() : QStringLiteral("QMainWindow::separator { width: 0px; height: 0px; margin: 0; padding: 0; }"));
}
void MainWindow::animateRackDock() {
    int h = m_rack->heightForContent() + 24;
    bool dbg = qEnvironmentVariableIsSet("MT_DEBUG_DOCK");
    auto* a = new QVariantAnimation(this);
    a->setDuration(anim::kDurationMs * 2);
    a->setEasingCurve(QEasingCurve::OutCubic);
    a->setStartValue(1);
    a->setEndValue(h);
    connect(a, &QVariantAnimation::valueChanged, this, [this, dbg](const QVariant& v) {
        int hh = v.toInt();
        m_dRack->setFixedHeight(hh);
        m_workspace->resizeDocks({m_dRack}, {hh}, Qt::Vertical);
        if (dbg) fprintf(stderr, "dock anim %d -> rack %d playlist y %d\n", hh, m_dRack->height(), m_dPlaylist->y());
    });
    connect(a, &QVariantAnimation::finished, this, [this, h] {
        updateWorkspaceSeparators();
        m_dRack->setMinimumHeight(0);
        m_dRack->setMaximumHeight(QWIDGETSIZE_MAX);
        m_workspace->resizeDocks({m_dRack}, {h}, Qt::Vertical);
        m_workspace->setDockOptions(AllowNestedDocks | AllowTabbedDocks | AnimatedDocks);
    });
    QTimer::singleShot(30, a, [a] { a->start(QAbstractAnimation::DeleteWhenStopped); });
}
void MainWindow::debugSearch(const QString& t) {
    QElapsedTimer et;
    et.start();
    m_browser->debugSearch(t);
    fprintf(stderr, "search '%s' took %lld ms\n", t.toUtf8().constData(), (long long)et.elapsed());
}
void MainWindow::updateRackDockCandidate() {
    QRect r = m_dRack->frameGeometry();
    QRect main = geometry();
    const int th = 16;
    bool overlapsX = r.right() > main.left() + 40 && r.left() < main.right() - 40;
    int topLimit = main.top() + menuBar()->height() + m_transport->height() + 6;
    Qt::DockWidgetArea c = (overlapsX && r.bottom() >= main.bottom() - th) ? Qt::BottomDockWidgetArea
        : (overlapsX && r.top() <= topLimit + th)                          ? Qt::TopDockWidgetArea
                                                                           : Qt::NoDockWidgetArea;
    if (c != m_rackCandidate) {
        m_rackCandidate = c;
        if (c == Qt::NoDockWidgetArea) hideDockIndicator();
        else showDockIndicator(c);
    }
    if (!m_rackReleasePoll.isActive()) m_rackReleasePoll.start(40);
}
// Translucent overlay painted inside the main window (a separate window can be suppressed by the WM during a drag)
class DockIndicatorOverlay : public QWidget {
public:
    explicit DockIndicatorOverlay(QWidget* parent)
        : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(theme::withAlpha(theme::accent(), 230), 3));
        p.setBrush(theme::withAlpha(theme::accent(), 60));
        p.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 6, 6);
        p.setPen(theme::accent());
        QFont f = theme::boldFont();
        f.setPointSizeF(12);
        p.setFont(f);
        p.drawText(rect(), Qt::AlignCenter, "Release to dock the Channel Rack here");
    }
};
void MainWindow::showDockIndicator(Qt::DockWidgetArea area) {
    if (!m_dockIndicator) m_dockIndicator = new DockIndicatorOverlay(this);
    QRect c = m_workspace->geometry();
    c.moveTopLeft(m_workspace->mapTo(this, QPoint(0, 0)));
    int h = std::min(c.height() / 2, m_dRack->height() + 30);
    QRect target = area == Qt::BottomDockWidgetArea ? QRect(c.left(), c.bottom() - h, c.width(), h)
                                                    : QRect(c.left(), c.top(), c.width(), h);
    m_dockIndicator->setGeometry(target);
    m_dockIndicator->show();
    m_dockIndicator->raise();
    m_dockIndicator->update();
}
void MainWindow::hideDockIndicator() {
    if (m_dockIndicator) m_dockIndicator->hide();
}
void MainWindow::dockRackAtCandidate() {
    Qt::DockWidgetArea area = m_rackCandidate;
    m_rackCandidate = Qt::NoDockWidgetArea;
    m_rackReleasePoll.stop();
    m_rackDragging = false;
    hideDockIndicator();
    if (area == Qt::NoDockWidgetArea || !m_dRack->isFloating()) return;
    QTimer::singleShot(0, this, [this, area] {
        if (!m_dRack->isFloating()) return;
        // Qt's own dock animation would first snap the layout to the rack's full height; we run our own instead:
        // the rack enters collapsed (1 px) so the editors don't jump, then slides open to its content height
        m_workspace->setDockOptions(AllowNestedDocks | AllowTabbedDocks);
        m_dRack->hide();
        m_dRack->setAllowedAreas(Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea);
        m_dRack->setFixedHeight(1); /* explicit min+max: overrides the content's minimum size hint */
        placeRackInWorkspace(area == Qt::BottomDockWidgetArea);
        m_dRack->setFloating(false);
        m_workspace->resizeDocks({m_dRack}, {1}, Qt::Vertical);
        m_dRack->show();
        animateRackDock();
        QTimer::singleShot(0, this, [this] { m_dRack->setAllowedAreas(Qt::NoDockWidgetArea); });
    });
}
void MainWindow::showHelp() {
    QMessageBox::information(this, "Quick help", R"(<b>Workflow</b><br>
Channel Rack: click steps to program beats (right-click erases, wheel = velocity, Ctrl+wheel = pitch). Up/Down select channels, Alt+Up/Down move them, Alt+C clones, Alt+Del deletes, 1-0 mute / Ctrl+1-0 solo the first ten, Ctrl+Shift+Left/Right shift steps. Double-click a channel name to open its instrument. Drag samples/plugins from the Browser onto the rack.<br>
Piano Roll: draw notes with the pencil (P), select (E), erase (D), mute (T). Drag note edges to resize, Ctrl+Shift+drag duplicates, Ctrl+Q quantizes, Ctrl+L legato, Ctrl+G glue, Ctrl+D deselect, Shift+I invert, Alt+M mute, Ctrl+Up/Down octave, PgUp/PgDn zoom, G/K previous/next channel. Ctrl+drag the ruler selects a time area: Ctrl+B duplicates its notes to the right, Delete clears it. Ctrl+wheel zooms.<br>
Playlist: click to place the selected pattern (P), select (E), delete (D), slice (C), mute (T); Insert slices at the playhead, Ctrl+Del / Ctrl+Ins remove / insert the loop area, Shift+arrows move clips, Ctrl+C/X/V copy, cut and paste at the playhead. Ctrl+Alt+C consolidates the selected pattern clips to one audio clip (the clips are muted and kept). Drag the ruler to set the loop region. Double-click audio clips for stretch/pitch/fades.<br>
Mixer: double-click an empty slot to add an effect, click to open, drag slots to reorder. Select a strip, then right-click another strip to route / send / sidechain to it.<br>
Automation: right-click any knob or fader &gt; "Create automation clip". Edit points in the Playlist (double-click adds, right-click for curve types).<br>
Recording: choose an input on a mixer strip (that arms the strip), press Record (R) to arm the transport, then Play. In song mode audio lands in the playlist on a track linked to the strip; with loop recording every loop pass becomes a take, the newest on top and the older ones muted below it. Live MIDI / typing keyboard plays the selected channel; while recording, notes go into the current pattern (song mode places a pattern clip where recording started). Stop, then Ctrl+Z undoes the whole pass.<br>
Typing keyboard (toolbar piano button / Ctrl+T): when on, Z-M play the lower octave and Q-P the upper one on the selected channel and letter keys are not shortcuts; Ctrl+[ / Ctrl+] change octave. Ctrl+H = panic.<br><br>
<b>Transport</b>: Space play/stop, Ctrl+Space play/pause, Stop twice = back to start, R record, L song/pattern mode, Ctrl+M metronome, Ctrl+P count-in, Ctrl+T typing keyboard on/off, Num +/- next/previous pattern.<br>
<b>Windows</b>: F5 Playlist, F6 Channel Rack, F7 Piano Roll, F8 Browser, F9 Mixer (full page), Alt+S project samples, F10 MIDI settings, F11 song info, F12 close all, Esc closes the focused window. Tools: P pencil, B brush, E select, D delete, C slice, T mute. Shortcuts are customisable under Options.)");
}

}   // namespace mt
