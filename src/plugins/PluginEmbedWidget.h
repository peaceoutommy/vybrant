// A native X11 child window a plugin editor can attach to. Used by both VST2 and VST3 adapters.
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
    std::function<bool(unsigned long xid, int& w, int& h)> onAttach;
    std::function<void()> onDetach;
    std::function<void(int w, int h)> onResized;    // host -> plugin size change (user resizes)
    std::function<void()> onIdle;                   // periodic idle (VST2 editors)
    void requestResize(int w, int h);               // plugin -> host
    bool attached() const { return m_attached; }
    QSize sizeHint() const override { return m_pluginSize.isValid() ? m_pluginSize : QSize(400, 300); }
protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    QPaintEngine* paintEngine() const override { return nullptr; }
private:
    void doAttach(); void doDetach();
    bool m_attached = false; bool m_inPluginResize = false; QTimer m_idle; QSize m_pluginSize;
};

} // namespace mt
