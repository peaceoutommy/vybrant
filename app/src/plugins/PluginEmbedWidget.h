// A native child window (X11 on Linux, HWND on Windows) a plugin editor can attach to. Used by the VST2, VST3 and CLAP adapters.
#pragma once
#include <QWidget>
#include <QTimer>
#include <functional>

namespace mt {

class PluginEmbedWidget : public QWidget {
    Q_OBJECT
public:
    explicit PluginEmbedWidget(QWidget* parent = nullptr);
    ~PluginEmbedWidget() override;
    // Called once the native window exists (on first show)
    std::function<bool(WId wid, int& w, int& h)> onAttach;
    std::function<void()> onDetach;
    std::function<void(int w, int h)> onResized;   // host -> plugin size change (user resizes)
    std::function<void()> onIdle;   // periodic idle (VST2 editors)
    std::function<bool(QKeyEvent* e, bool down)> onKey;   // keys while the editor window has focus; true = the editor took it
    void requestResize(int w, int h);   // plugin -> host
    void snapTo(int w, int h);   // after a user resize: fit the window to the size the editor allows (stays resizable)
    bool attached() const { return m_attached; }
    static inline bool keepHostKeyboard = false;   // typing keyboard on: keys must reach the app, not the plugin editor
    QSize sizeHint() const override { return m_pluginSize.isValid() ? m_pluginSize : QSize(400, 300); }

protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    bool event(QEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    QPaintEngine* paintEngine() const override { return nullptr; }

private:
    void doAttach();
    void doDetach();
    void reclaimKeyboard();
    bool m_attached = false;
    bool m_inPluginResize = false;
    bool m_keyTaken = false;
    QTimer m_idle, m_snap;
    QSize m_snapSize;
    QSize m_pluginSize;
    QWidget* m_keySink = nullptr;
};

}   // namespace mt
