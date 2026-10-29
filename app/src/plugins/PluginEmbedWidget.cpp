#include "plugins/PluginEmbedWidget.h"
#include <QResizeEvent>
#include <QKeyEvent>
#include <QLayout>
#include <cstdio>
#include <cmath>
#include <QGuiApplication>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <X11/Xlib.h>   // last: its macros (None, Bool...) clash with Qt headers
#undef KeyPress   // and with QEvent::KeyPress below
#endif

namespace mt {

PluginEmbedWidget::PluginEmbedWidget(QWidget* parent)
    : QWidget(parent) {
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(200, 100);
    connect(&m_idle, &QTimer::timeout, this, [this] {
        if (m_attached && onIdle) onIdle();
        if (m_attached) reclaimKeyboard();
    });
    m_snap.setSingleShot(true);
    m_snap.setInterval(250);   // once the window edge is let go, not against the drag
    connect(&m_snap, &QTimer::timeout, this, [this] {
        double dpr = devicePixelRatioF();
        QSize d = QSize((int)std::lround(m_snapSize.width() / dpr), (int)std::lround(m_snapSize.height() / dpr)) - size();
        if (QWidget* top = window(); top && !d.isNull()) top->resize(top->size() + d);
    });
}
PluginEmbedWidget::~PluginEmbedWidget() { doDetach(); }
void PluginEmbedWidget::showEvent(QShowEvent*) {
    if (!m_attached) QTimer::singleShot(0, this, [this] { doAttach(); });
}
void PluginEmbedWidget::hideEvent(QHideEvent*) {}
void PluginEmbedWidget::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    if (m_attached && !m_inPluginResize && onResized) {
        double dpr = devicePixelRatioF();
        onResized((int)std::lround(e->size().width() * dpr), (int)std::lround(e->size().height() * dpr));
    }
}
void PluginEmbedWidget::snapTo(int w, int h) {
    m_snapSize = QSize(w, h);
    m_snap.start();
}
// A key the editor takes (its text fields, its own shortcuts) must not also fire an app shortcut, so the editor gets it at
// ShortcutOverride, before shortcuts are matched
bool PluginEmbedWidget::event(QEvent* e) {
    if (e->type() == QEvent::ShortcutOverride && m_attached && onKey && !keepHostKeyboard && onKey(static_cast<QKeyEvent*>(e), true)) {
        m_keyTaken = true;
        e->accept();
        return true;
    }
    if (e->type() == QEvent::KeyPress && m_keyTaken) {
        m_keyTaken = false;
        return true;
    }
    return QWidget::event(e);
}
void PluginEmbedWidget::keyReleaseEvent(QKeyEvent* e) {
    if (m_attached && onKey && !keepHostKeyboard && onKey(e, false)) return;
    QWidget::keyReleaseEvent(e);
}
void PluginEmbedWidget::doAttach() {
    if (m_attached || !onAttach) return;
    double dpr = devicePixelRatioF();
    int w = (int)std::lround(width() * dpr), h = (int)std::lround(height() * dpr);
    if (onAttach(winId(), w, h)) {
        m_attached = true;
        if (w > 0 && h > 0) requestResize(w, h);
        m_idle.start(30);
    }
}
void PluginEmbedWidget::doDetach() {
    if (!m_attached) return;
    m_idle.stop();
    if (onDetach) onDetach();
    m_attached = false;
}
// yabridge moves X input focus onto this window when the editor is clicked, so key presses go to the plugin and the typing
// keyboard goes dead. Focus must not go back to any ancestor of the plugin window either: X delivers keys to the window under
// the pointer when it lies inside the focus window. So while typing mode is on, focus goes to a 1x1 native sibling instead.
void PluginEmbedWidget::reclaimKeyboard() {
    if (!keepHostKeyboard) return;
#ifdef _WIN32   // the editor's own child windows took the keyboard: give it back to the top-level window
    if (HWND f = GetFocus(); f && IsChild((HWND)winId(), f)) SetFocus((HWND)window()->winId());
#else
    auto* x11 = qApp->nativeInterface<QNativeInterface::QX11Application>();
    if (!x11) return;
    Display* d = x11->display();
    Window focus;
    int revert;
    XGetInputFocus(d, &focus, &revert);
    if (focus != (Window)winId() && focus != (Window)window()->winId()) return;
    if (!m_keySink) {
        m_keySink = new QWidget(window());
        m_keySink->setAttribute(Qt::WA_NativeWindow);
        m_keySink->setAttribute(Qt::WA_DontCreateNativeAncestors);
        m_keySink->setGeometry(0, 0, 1, 1);
        m_keySink->show();
        m_keySink->lower();   // under the editor: mapped (focusable) but never visible
    }
    XSetInputFocus(d, (Window)m_keySink->winId(), RevertToParent, CurrentTime);
    XFlush(d);
#endif
}
// Plugin sizes are physical pixels of the native window; the Qt widget is laid out in logical units (device pixel ratio)
void PluginEmbedWidget::requestResize(int w, int h) {
    double dpr = devicePixelRatioF();
    int lw = (int)std::lround(w / dpr), lh = (int)std::lround(h / dpr);
    if (qEnvironmentVariableIsSet("MT_DEBUG_EMBED"))
        fprintf(stderr, "embed requestResize %dx%d px -> %dx%d logical (dpr %.2f)\n", w, h, lw, lh, dpr);
    m_inPluginResize = true;
    m_pluginSize = QSize(lw, lh);
    setMinimumSize(lw, lh);
    updateGeometry();
    resize(lw, lh);
    // the layout's minimum (and so the window's) only updates on the next layout pass: activate it first, then fit the window to the new hint - shrinking included
    if (QWidget* top = window()) {
        if (top->layout()) top->layout()->activate();
        top->adjustSize();
    }
    m_inPluginResize = false;
}

}   // namespace mt
