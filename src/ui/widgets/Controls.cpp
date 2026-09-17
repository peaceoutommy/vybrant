#include "ui/widgets/Controls.h"
#include "ui/Theme.h"
#include "core/Types.h"
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QToolTip>
#include <QInputDialog>
#include <QtMath>

namespace mt {

// ---------------------------------------------------------------- Knob
Knob::Knob(QWidget* parent, int size) : QWidget(parent), m_size(size), m_color(theme::accent()) { setCursor(Qt::PointingHandCursor); setMouseTracking(false); setMinimumSize(sizeHint()); }
QSize Knob::sizeHint() const { return {m_showLabel ? std::max(m_size + 8, 66) : m_size + 8, m_size + (m_showLabel ? 14 : 2)}; }
void Knob::setValue(float v, bool emitSignal) { v = clamp(v, 0.f, 1.f); if (std::fabs(v - m_value) < 1e-6f) return; m_value = v; update(); if (emitSignal) Q_EMIT valueChanged(v); }
void Knob::paintEvent(QPaintEvent*) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    int d = std::min(width() - 4, height() - (m_showLabel ? 14 : 2)); QRectF r((width() - d) / 2.0, 1, d, d);
    p.setPen(QPen(theme::border(), 1)); p.setBrush(theme::panel()); p.drawEllipse(r.adjusted(3, 3, -3, -3));
    const double startAngle = 225, span = -270; QRectF arc = r.adjusted(1.5, 1.5, -1.5, -1.5);
    p.setPen(QPen(theme::border(), 2, Qt::SolidLine, Qt::RoundCap)); p.drawArc(arc, (int)(startAngle * 16), (int)(span * 16));
    p.setPen(QPen(m_color, 2, Qt::SolidLine, Qt::RoundCap));
    if (m_bipolar) { double mid = startAngle + span * 0.5; double a = span * (m_value - 0.5); p.drawArc(arc, (int)(mid * 16), (int)(a * 16)); }
    else p.drawArc(arc, (int)(startAngle * 16), (int)(span * m_value * 16));
    double ang = qDegreesToRadians(startAngle + span * m_value); QPointF c = r.center(); double rad = d / 2.0 - 4;
    p.setPen(QPen(theme::text(), 1.5, Qt::SolidLine, Qt::RoundCap)); p.drawLine(c + QPointF(std::cos(ang) * rad * 0.45, -std::sin(ang) * rad * 0.45), c + QPointF(std::cos(ang) * rad, -std::sin(ang) * rad));
    if (m_showLabel && !m_label.isEmpty()) { p.setPen(theme::textDim()); p.setFont(theme::smallFont()); QString t = p.fontMetrics().elidedText(m_label, Qt::ElideRight, width() - 2); p.drawText(QRect(0, height() - 13, width(), 12), Qt::AlignCenter, t); }
}
void Knob::showTip() { QString t = valueText ? valueText(m_value) : QString::number(m_value * 100, 'f', 0) + "%"; if (!m_label.isEmpty()) t = m_label + ": " + t; QToolTip::showText(mapToGlobal(QPoint(width(), 0)), t, this); }
void Knob::mousePressEvent(QMouseEvent* e) { if (e->button() == Qt::RightButton) { Q_EMIT contextRequested(e->globalPosition().toPoint()); return; } if (e->button() != Qt::LeftButton) return; m_dragging = true; m_lastPos = e->pos(); m_dragStart = m_value; Q_EMIT dragStarted(); showTip(); }
void Knob::mouseMoveEvent(QMouseEvent* e) { if (!m_dragging) return; int dy = m_lastPos.y() - e->pos().y(); int dx = e->pos().x() - m_lastPos.x(); m_lastPos = e->pos(); float sens = (e->modifiers() & Qt::ShiftModifier) ? 0.0005f : 0.005f; setValue(m_value + (dy + dx) * sens, true); showTip(); }
void Knob::mouseReleaseEvent(QMouseEvent*) { if (m_dragging) { m_dragging = false; Q_EMIT dragFinished(); QToolTip::hideText(); } }
void Knob::mouseDoubleClickEvent(QMouseEvent*) { Q_EMIT dragStarted(); setValue(m_default, true); Q_EMIT dragFinished(); }
void Knob::wheelEvent(QWheelEvent* e) { float step = (e->modifiers() & Qt::ShiftModifier) ? 0.002f : 0.02f; Q_EMIT dragStarted(); setValue(m_value + (e->angleDelta().y() > 0 ? step : -step), true); Q_EMIT dragFinished(); showTip(); e->accept(); }

// ---------------------------------------------------------------- Fader
Fader::Fader(QWidget* parent) : QWidget(parent) { setCursor(Qt::PointingHandCursor); }
void Fader::setValue(float v, bool emitSignal) { v = clamp(v, 0.f, 1.f); if (std::fabs(v - m_value) < 1e-6f) return; m_value = v; update(); if (emitSignal) Q_EMIT valueChanged(v); }
void Fader::paintEvent(QPaintEvent*) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    int trackX = width() / 2; int top = 8, bottom = height() - 8; p.setPen(QPen(theme::surface(), 3, Qt::SolidLine, Qt::RoundCap)); p.drawLine(trackX, top, trackX, bottom);
    // unity mark (0.64 = 0.8/1.25)
    int unityY = bottom - (int)((bottom - top) * 0.64f); p.setPen(QPen(theme::textDim(), 1)); p.drawLine(trackX - 8, unityY, trackX + 8, unityY);
    int y = bottom - (int)((bottom - top) * m_value); p.setPen(QPen(theme::accent(), 3, Qt::SolidLine, Qt::RoundCap)); p.drawLine(trackX, bottom, trackX, y);
    QRectF h(trackX - 8, y - 5, 16, 10); p.setPen(QPen(QColor(0x3a, 0x40, 0x48), 1)); p.setBrush(QColor(0x2e, 0x33, 0x3a)); p.drawRoundedRect(h, 2, 2); p.setPen(theme::text()); p.drawLine(QPointF(h.left() + 3, h.center().y()), QPointF(h.right() - 3, h.center().y()));
}
void Fader::mousePressEvent(QMouseEvent* e) { if (e->button() == Qt::RightButton) { Q_EMIT contextRequested(e->globalPosition().toPoint()); return; } if (e->button() != Qt::LeftButton) return; m_dragging = true; m_lastY = e->pos().y(); m_dragStart = m_value; Q_EMIT dragStarted(); }
void Fader::mouseMoveEvent(QMouseEvent* e) { if (!m_dragging) return; int dy = m_lastY - e->pos().y(); m_lastY = e->pos().y(); float range = (float)(height() - 16); float sens = (e->modifiers() & Qt::ShiftModifier) ? 0.1f : 1.f; setValue(m_value + dy / range * sens, true); if (valueText) QToolTip::showText(mapToGlobal(QPoint(width(), e->pos().y())), valueText(m_value), this); }
void Fader::mouseReleaseEvent(QMouseEvent*) { if (m_dragging) { m_dragging = false; Q_EMIT dragFinished(); QToolTip::hideText(); } }
void Fader::mouseDoubleClickEvent(QMouseEvent*) { Q_EMIT dragStarted(); setValue(m_default, true); Q_EMIT dragFinished(); }
void Fader::wheelEvent(QWheelEvent* e) { Q_EMIT dragStarted(); setValue(m_value + (e->angleDelta().y() > 0 ? 0.02f : -0.02f), true); Q_EMIT dragFinished(); e->accept(); }

// ---------------------------------------------------------------- LevelMeter
LevelMeter::LevelMeter(QWidget* parent, bool horizontal) : QWidget(parent), m_horizontal(horizontal) {}
void LevelMeter::setLevels(float l, float r, bool clipped) {
    m_l = std::max(l, m_l * 0.85f); m_r = std::max(r, m_r * 0.85f);
    if (l >= m_holdL) { m_holdL = l; m_holdCount = 30; } if (r >= m_holdR) m_holdR = r; if (m_holdCount > 0) m_holdCount--; else { m_holdL *= 0.95f; m_holdR *= 0.95f; }
    if (clipped) m_clip = true; update();
}
static float meterPos(float lin) { float db = gainToDb(lin); return clamp((db + 60.f) / 66.f, 0.f, 1.f); }   // -60 .. +6 dB
void LevelMeter::paintEvent(QPaintEvent*) {
    QPainter p(this); p.fillRect(rect(), theme::bg());
    auto drawBar = [&](QRect r, float lv, float hold) {
        float f = meterPos(lv); QLinearGradient g; if (m_horizontal) { g = QLinearGradient(r.left(), 0, r.right(), 0); } else { g = QLinearGradient(0, r.bottom(), 0, r.top()); }
        g.setColorAt(0, theme::green()); g.setColorAt(0.75, theme::green()); g.setColorAt(0.87, theme::accent()); g.setColorAt(1.0, theme::red());
        QRect fill = m_horizontal ? QRect(r.left(), r.top(), (int)(r.width() * f), r.height()) : QRect(r.left(), r.bottom() - (int)(r.height() * f), r.width(), (int)(r.height() * f));
        p.fillRect(fill, g);
        float h = meterPos(hold); p.setPen(theme::text()); if (m_horizontal) { int x = r.left() + (int)(r.width() * h); p.drawLine(x, r.top(), x, r.bottom()); } else { int y = r.bottom() - (int)(r.height() * h); p.drawLine(r.left(), y, r.right(), y); }
    };
    int clipH = 5;
    if (m_horizontal) { int h = (height() - 1) / 2; drawBar(QRect(0, 0, width() - clipH - 1, h), m_l, m_holdL); drawBar(QRect(0, h + 1, width() - clipH - 1, h), m_r, m_holdR); p.fillRect(QRect(width() - clipH, 0, clipH, height()), m_clip ? theme::red() : theme::panelLight()); }
    else { int w = (width() - 1) / 2; drawBar(QRect(0, clipH + 1, w, height() - clipH - 1), m_l, m_holdL); drawBar(QRect(w + 1, clipH + 1, w, height() - clipH - 1), m_r, m_holdR); p.fillRect(QRect(0, 0, width(), clipH), m_clip ? theme::red() : theme::panelLight()); }
}

// ---------------------------------------------------------------- DragValue
DragValue::DragValue(QWidget* parent) : QWidget(parent) { setCursor(Qt::SizeVerCursor); }
void DragValue::setValue(double v, bool emitSignal) { v = clamp(v, m_min, m_max); double q = std::pow(10.0, -m_decimals); v = std::round(v / q) * q; if (std::fabs(v - m_value) < 1e-9) return; m_value = v; update(); if (emitSignal) Q_EMIT valueChanged(v); }
void DragValue::paintEvent(QPaintEvent*) { QPainter p(this); p.setRenderHint(QPainter::Antialiasing); theme::drawPanel(p, rect().adjusted(0, 0, -1, -1), theme::surface()); p.setPen(QPen(theme::border(), 1)); p.setBrush(Qt::NoBrush); p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 3, 3); p.setPen(theme::accent()); QFont f = theme::boldFont(); f.setPointSizeF(9.5); p.setFont(f); p.drawText(rect(), Qt::AlignCenter, m_prefix + QString::number(m_value, 'f', m_decimals) + m_suffix); }
// The step depends on where the cursor is: over the integer digits = whole units, over the decimals = 1/10^decimals
double DragValue::stepAt(int x) const {
    if (m_decimals <= 0) return std::max(m_step, 1.0);
    QString text = m_prefix + QString::number(m_value, 'f', m_decimals) + m_suffix; QFont ff = theme::boldFont(); ff.setPointSizeF(9.5); QFontMetrics fm(ff); int w = fm.horizontalAdvance(text); int left = (width() - w) / 2; int dot = text.lastIndexOf('.'); int dotX = left + fm.horizontalAdvance(text.left(dot + 1));
    return x >= dotX ? std::pow(10.0, -m_decimals) : 1.0;
}
void DragValue::mousePressEvent(QMouseEvent* e) { if (e->button() == Qt::LeftButton) { m_dragging = true; m_lastY = e->pos().y(); m_accum = 0; m_dragStep = stepAt(e->pos().x()); } }
void DragValue::mouseMoveEvent(QMouseEvent* e) { if (!m_dragging) return; int dy = m_lastY - e->pos().y(); m_lastY = e->pos().y(); double st = m_dragStep; if (e->modifiers() & Qt::ShiftModifier) st *= 0.1; m_accum += dy * st * 0.5; if (std::fabs(m_accum) >= st) { double steps = std::trunc(m_accum / st); setValue(m_value + steps * st, true); m_accum -= steps * st; } }
void DragValue::mouseReleaseEvent(QMouseEvent*) { if (m_dragging) { m_dragging = false; Q_EMIT editingFinished(); } }
void DragValue::mouseDoubleClickEvent(QMouseEvent*) { bool ok; double v = QInputDialog::getDouble(this, "Enter value", m_prefix, m_value, m_min, m_max, m_decimals, &ok); if (ok) { setValue(v, true); Q_EMIT editingFinished(); } }
void DragValue::wheelEvent(QWheelEvent* e) { double s = stepAt(e->position().toPoint().x()); if (e->modifiers() & Qt::ShiftModifier) s *= 0.1; if (m_decimals == 0) s = std::max(s, 1.0); setValue(m_value + (e->angleDelta().y() > 0 ? s : -s), true); Q_EMIT editingFinished(); e->accept(); }

// ---------------------------------------------------------------- LedButton
LedButton::LedButton(const QString& text, const QColor& onColor, QWidget* parent) : QWidget(parent), m_text(text), m_on(onColor) { setCursor(Qt::PointingHandCursor); setToolTip(text); }
void LedButton::paintEvent(QPaintEvent*) { QPainter p(this); p.setRenderHint(QPainter::Antialiasing); QRectF r = rect().adjusted(0, 0, -1, -1); p.setPen(QPen(m_checked ? m_on.darker(120) : theme::border(), 1)); p.setBrush(m_checked ? m_on : theme::panel()); p.drawRoundedRect(r, 2, 2); p.setPen(m_checked ? theme::bg() : theme::textDim()); p.setFont(theme::smallFont()); p.drawText(r, Qt::AlignCenter, m_text); }
void LedButton::mousePressEvent(QMouseEvent* e) { if (e->button() == Qt::RightButton) { Q_EMIT rightClicked(); return; } m_checked = !m_checked; update(); Q_EMIT toggled(m_checked); }

} // namespace mt
