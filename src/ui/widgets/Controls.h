// Reusable control widgets: rotary knob, fader, level meter, drag-editable number box.
#pragma once
#include <QWidget>
#include <QString>
#include <functional>

class QLabel;

namespace mt {

class Knob : public QWidget {
    Q_OBJECT
public:
    explicit Knob(QWidget* parent = nullptr, int size = 36);
    float value() const { return m_value; }   // normalized 0..1
    void setValue(float v, bool emitSignal = false);
    void setDefault(float d) { m_default = d; }
    void setLabel(const QString& l) {
        m_label = l;
        if (toolTip().isEmpty()) setToolTip(l);
        setMinimumSize(sizeHint());
        update();
    }
    void setBipolar(bool b) {
        m_bipolar = b;
        update();
    }
    void setColor(const QColor& c) {
        m_color = c;
        update();
    }
    void setShowLabel(bool s) {
        m_showLabel = s;
        setMinimumSize(sizeHint());
        updateGeometry();
    }
    std::function<QString(float)> valueText;   // normalized -> display
    std::function<float(float)> snap;   // optional detent: raw normalized -> shown/emitted value (Shift bypasses it)
    // gentle detent at whole steps of a linear [min, max] range: within 'width' of a whole value it lands on it, otherwise free
    static std::function<float(float)> wholeStepSnap(float min, float max, float width = 0.12f);
    QSize sizeHint() const override;
Q_SIGNALS:
    void valueChanged(float v);
    void dragStarted();
    void dragFinished();
    void contextRequested(const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    void showTip();
    float m_value = 0.5f, m_default = 0.5f;
    QString m_label;
    bool m_bipolar = false, m_dragging = false, m_showLabel = true;
    QPoint m_lastPos;
    float m_dragStart = 0;
    float m_raw = 0;   // unsnapped drag position, so a detent never traps the knob
    int m_size;
    QColor m_color;
};

class Fader : public QWidget {
    Q_OBJECT
public:
    explicit Fader(QWidget* parent = nullptr);
    float value() const { return m_value; }   // normalized 0..1 (1.25 max volume maps to 1.0)
    void setValue(float v, bool emitSignal = false);
    void setDefault(float d) { m_default = d; }
    std::function<QString(float)> valueText;
    QSize sizeHint() const override { return {28, 110}; }
    QSize minimumSizeHint() const override { return {28, 40}; }
Q_SIGNALS:
    void valueChanged(float v);
    void dragStarted();
    void dragFinished();
    void contextRequested(const QPoint& globalPos);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    float m_value = 0.64f, m_default = 0.64f;
    bool m_dragging = false;
    int m_lastY = 0;
    float m_dragStart = 0;
};

class LevelMeter : public QWidget {
    Q_OBJECT
public:
    explicit LevelMeter(QWidget* parent = nullptr, bool horizontal = false);
    void setLevels(float peakL, float peakR, bool clipped = false);
    void resetClip() {
        m_clip = false;
        update();
    }
    QSize sizeHint() const override { return m_horizontal ? QSize(80, 10) : QSize(14, 110); }
    QSize minimumSizeHint() const override { return m_horizontal ? QSize(30, 6) : QSize(14, 40); }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override { resetClip(); }

private:
    float m_l = 0, m_r = 0, m_holdL = 0, m_holdR = 0;
    int m_holdCount = 0;
    bool m_clip = false, m_horizontal;
};

// Number box edited by vertical drag / wheel / double-click text entry
class DragValue : public QWidget {
    Q_OBJECT
public:
    explicit DragValue(QWidget* parent = nullptr);
    void setRange(double min, double max, double step, int decimals) {
        m_min = min;
        m_max = max;
        m_step = step;
        m_decimals = decimals;
        update();
    }
    void setValue(double v, bool emitSignal = false);
    double value() const { return m_value; }
    void setSuffix(const QString& s) {
        m_suffix = s;
        update();
    }
    void setPrefix(const QString& s) {
        m_prefix = s;
        update();
    }
    QSize sizeHint() const override { return {64, 22}; }
Q_SIGNALS:
    void valueChanged(double v);
    void editingFinished();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    double stepAt(int x) const;
    double m_value = 0, m_min = 0, m_max = 100, m_step = 1;
    int m_decimals = 0;
    QString m_suffix, m_prefix;
    bool m_dragging = false;
    int m_lastY = 0;
    double m_accum = 0, m_dragStep = 1;
};

// Small LED-style toggle button
class LedButton : public QWidget {
    Q_OBJECT
public:
    explicit LedButton(const QString& text, const QColor& onColor, QWidget* parent = nullptr);
    bool isChecked() const { return m_checked; }
    void setChecked(bool c) {
        if (m_checked != c) {
            m_checked = c;
            update();
        }
    }
    QSize sizeHint() const override { return {m_w, 20}; }
    void setWidth(int w) {
        m_w = w;
        updateGeometry();
    }
Q_SIGNALS:
    void toggled(bool);
    void rightClicked();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;

private:
    QString m_text;
    QColor m_on;
    bool m_checked = false;
    int m_w = 26;
};

// Double-click a knob's value label to type a number (Enter applies, Escape cancels). The first number in the typed text is
// passed on, so "3", "+3 st" and "-1,5" all work.
void makeValueEditable(QLabel* label, std::function<void(double)> onValue);

}   // namespace mt
