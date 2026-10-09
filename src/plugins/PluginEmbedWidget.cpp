#include "plugins/PluginEmbedWidget.h"
#include <QResizeEvent>
#include <QLayout>
#include <cstdio>
#include <cmath>
#include <QGuiApplication>
#include <X11/Xlib.h>   // last: its macros (None, Bool...) clash with Qt headers

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
void PluginEmbedWidget::doAttach() {
    if (m_attached || !onAttach) return;
    double dpr = devicePixelRatioF();
    int w = (int)std::lround(width() * dpr), h = (int)std::lround(height() * dpr);
    unsigned long xid = (unsigned long)winId();
    if (onAttach(xid, w, h)) {
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
}
// Plugin sizes are physical pixels of the native X11 window; the Qt widget is laid out in logical units (device pixel ratio)
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
