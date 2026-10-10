// Main window: configurable dockable workspace (Browser / Playlist / Piano Roll / Channel Rack / Mixer), menus,
// transport toolbar, status bar, editor window management and the typing keyboard.
#pragma once
#include "ui/App.h"
#include <QMainWindow>
#include <QDockWidget>
#include <QLabel>
#include <QTimer>
#include <QStackedWidget>
#include <QToolButton>
#include <map>
#include <set>

class QSplitter;
class QToolBar;
namespace mt {
class TransportBar;
class BrowserPanel;
class ProjectSamplesPanel;
class ChannelRackPanel;
class PianoRollPanel;
class PlaylistPanel;
class MixerPanel;
class PluginWindow;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(App& app, QWidget* parent = nullptr);
    void openEditor(Id ownerId, const QString& title = QString());
    void startupChecks();
    bool saveScreenshot(const QString& path);
    void setForceClose(bool f) { m_forceClose = f; }
    MixerPanel* mixer() const { return m_mixer; }
    PianoRollPanel* pianoRoll() const { return m_pianoRoll; }
    ChannelRackPanel* rackPanel() const { return m_rack; }
    PlaylistPanel* playlist() const { return m_playlist; }
    BrowserPanel* browser() const { return m_browser; }
    ProjectSamplesPanel* samplesPanel() const { return m_samples; }
    void debugSearch(const QString& t);
    void debugLeftDock(int w) { resizeDocks({m_dBrowser}, {w}, Qt::Horizontal); }
    QString debugLeftSizes() const;
    QString debugLayout() const;
    void debugDock(Qt::DockWidgetArea a) {
        m_rackCandidate = a;
        dockRackAtCandidate();
    }

protected:
    void closeEvent(QCloseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void keyReleaseEvent(QKeyEvent*) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void buildMenus();
    void buildDocks();
    void onChanged(uint32_t flags);
    void uiTick();
    void updateTitle();
    bool maybeSave();
    void resetLayout();
    void applyDefaultLayout();
    void showHelp();
    void showMixerPage(bool on);
    void dockRackAtCandidate();
    void updateRackDockCandidate();
    void animateRackDock();
    void updateWorkspaceSeparators();
    void saveLayout();
    void scheduleLayoutSave();
    QMenu* createPopupMenu() override;
    QToolBar* m_toolBar = nullptr;
    void restoreRackDock();
    void placeRackInWorkspace(bool bottom);
    bool rackDockedAtBottom() const;
    QTimer m_layoutSaveTimer;
    bool m_layoutReady = false;
    void setTypingKeyboard(bool on);
    bool typingKeyEvent(QObject* o, QEvent* e);
    void hideEditorsOnWorkspaceClick(QObject* o, QEvent* e);
    void showDockIndicator(Qt::DockWidgetArea area);
    void hideDockIndicator();
    void showWorkspaceDock(QDockWidget* d);
    void enforceSingleEditor();
    void closeAllWindows();
    void syncWindowButtons();
    bool doOpen(const QString& path = QString());
    bool doSave();
    bool doSaveAs();
    int typingNote(int key) const;
    App& m_app;
    TransportBar* m_transport;
    BrowserPanel* m_browser;
    ChannelRackPanel* m_rack;
    PianoRollPanel* m_pianoRoll;
    PlaylistPanel* m_playlist;
    MixerPanel* m_mixer;
    QMainWindow* m_workspace = nullptr;
    class LeftPanels* m_leftSplit = nullptr;
    QStackedWidget* m_pages = nullptr;
    std::map<QString, QToolButton*> m_winButtons;
    bool m_typingKeyboard = false;
    bool m_forceClose = false;
    Qt::DockWidgetArea m_rackCandidate = Qt::NoDockWidgetArea;
    QTimer m_rackReleasePoll;
    bool m_rackDragging = false;
    QWidget* m_dockIndicator = nullptr;
    QRect m_lastRackGeom;
    int m_rackStillTicks = 0;
    QByteArray m_defaultWorkspaceState;
    QDockWidget *m_dBrowser, *m_dRack, *m_dPiano, *m_dPlaylist, *m_dMixer, *m_dSamples;
    ProjectSamplesPanel* m_samples;
    QLabel *m_status, *m_undoLabel;
    QTimer m_timer;
    std::map<Id, PluginWindow*> m_editors;
    std::map<Id, std::pair<QString, int>>
        m_pendingEditors; /* auto-open requests waiting for the engine to build the processor: id -> (title, ticks left) */
    std::set<int> m_heldKeys;
    int m_octave = 4;
    QByteArray m_defaultState;
};

}   // namespace mt
