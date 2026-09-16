#include "plugins/PluginEmbedWidget.h"
#include <QResizeEvent>

namespace mt {

PluginEmbedWidget::PluginEmbedWidget(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_NativeWindow); setAttribute(Qt::WA_DontCreateNativeAncestors); setAttribute(Qt::WA_PaintOnScreen); setAttribute(Qt::WA_NoSystemBackground); setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus); setMinimumSize(200, 100);
    connect(&m_idle, &QTimer::timeout, this, [this] { if (m_attached && onIdle) onIdle(); });
}
PluginEmbedWidget::~PluginEmbedWidget() { doDetach(); }
void PluginEmbedWidget::showEvent(QShowEvent*) { if (!m_attached) QTimer::singleShot(0, this, [this] { doAttach(); }); }
void PluginEmbedWidget::hideEvent(QHideEvent*) {}
void PluginEmbedWidget::resizeEvent(QResizeEvent* e) { QWidget::resizeEvent(e); if (m_attached && !m_inPluginResize && onResized) onResized(e->size().width(), e->size().height()); }
void PluginEmbedWidget::doAttach() {
    if (m_attached || !onAttach) return;
    int w = width(), h = height(); unsigned long xid = (unsigned long)winId();
    if (onAttach(xid, w, h)) { m_attached = true; if (w > 0 && h > 0) requestResize(w, h); m_idle.start(30); }
}
void PluginEmbedWidget::doDetach() { if (!m_attached) return; m_idle.stop(); if (onDetach) onDetach(); m_attached = false; }
void PluginEmbedWidget::requestResize(int w, int h) { m_inPluginResize = true; setMinimumSize(w, h); resize(w, h); if (parentWidget()) parentWidget()->adjustSize(); m_inPluginResize = false; }

} // namespace mt
