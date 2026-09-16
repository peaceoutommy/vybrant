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

namespace mt {
class TransportBar; class BrowserPanel; class ChannelRackPanel; class PianoRollPanel; class PlaylistPanel; class MixerPanel; class PluginWindow;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(App& app, QWidget* parent = nullptr);
    void openEditor(Id ownerId, const QString& title = QString());
    void startupChecks();
    bool saveScreenshot(const QString& path);
protected:
    void closeEvent(QCloseEvent*) override; void keyPressEvent(QKeyEvent*) override; void keyReleaseEvent(QKeyEvent*) override; bool eventFilter(QObject* o, QEvent* e) override;
private:
    void buildMenus(); void buildDocks(); void onChanged(uint32_t flags); void uiTick(); void updateTitle(); bool maybeSave(); void resetLayout(); void applyDefaultLayout(); void showHelp(); void showMixerPage(bool on); void showWorkspaceDock(QDockWidget* d); void closeAllWindows(); void syncWindowButtons();
    bool doOpen(const QString& path = QString()); bool doSave(); bool doSaveAs();
    int typingNote(int key) const;
    App& m_app; TransportBar* m_transport; BrowserPanel* m_browser; ChannelRackPanel* m_rack; PianoRollPanel* m_pianoRoll; PlaylistPanel* m_playlist; MixerPanel* m_mixer;
    QMainWindow* m_workspace = nullptr; QStackedWidget* m_pages = nullptr; std::map<QString, QToolButton*> m_winButtons; bool m_typingKeyboard = true; QByteArray m_defaultWorkspaceState; QDockWidget *m_dBrowser, *m_dRack, *m_dPiano, *m_dPlaylist, *m_dMixer; QLabel *m_status, *m_undoLabel; QTimer m_timer; std::map<Id, PluginWindow*> m_editors; std::set<int> m_heldKeys; int m_octave = 4; QByteArray m_defaultState;
};

} // namespace mt
