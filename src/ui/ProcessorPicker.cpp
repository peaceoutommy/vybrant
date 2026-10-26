#include "ui/ProcessorPicker.h"
#include "ui/Theme.h"
#include "dsp/NativeRegistry.h"
#include <QButtonGroup>
#include <QCryptographicHash>
#include <QFile>
#include <QPixmapCache>
#include <QStandardPaths>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

namespace mt {

namespace {
enum Role { KeyRole = Qt::UserRole, VendorRole, FormatRole, CategoryRole, HeaderRole, CountRole, ThumbRole };
const QStringList kEffectCats = {"Dynamics", "EQ / Filter", "Time", "Modulation", "Distortion", "Utility", "Other"};
const QStringList kInstrumentCats = {"Synths", "Keys", "Samplers", "Drums", "Other"};
const QColor kStar(0xf5, 0xc5, 0x42);

QColor categoryColor(const QString& c) {
    static const QHash<QString, QColor> colors = {{"Dynamics", QColor(0xe8, 0xa2, 0x4a)}, {"EQ / Filter", QColor(0x5a, 0xb0, 0xf0)},
        {"Time", QColor(0x9b, 0x7c, 0xf0)}, {"Modulation", QColor(0x4f, 0xd1, 0xa5)}, {"Distortion", QColor(0xf0, 0x6a, 0x5a)},
        {"Utility", QColor(0xa0, 0xa8, 0xb0)}, {"Synths", QColor(0xf0, 0xc7, 0x5a)}, {"Keys", QColor(0xe0, 0xd6, 0xc4)}, {"Samplers", QColor(0x6f, 0xc2, 0xe8)},
        {"Drums", QColor(0xf0, 0x8a, 0x5a)}};
    return colors.value(c, QColor(0x8a, 0x96, 0xa0));
}

QString nativeInstrumentCategory(const QString& key) {
    if (key == "native.sampler") return "Samplers";
    if (key == "native.drumsampler") return "Drums";
    if (key == "native.midiout" || key == "native.tonegen") return "Other";
    return "Synths";
}

// A small picture of what the processor does to a signal, drawn into r. The category picks the kind of picture, the name
// varies it (where the bell sits, saw or square, how many repeats), so cards of one category still look apart.
void drawSignal(QPainter& p, const QRectF& r, const QString& cat, const QString& name, const QColor& c) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const QPen pen(c, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin), dim(theme::withAlpha(c, 90), 1, Qt::SolidLine, Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const uint h = qHash(name);
    auto v = [h](int k) { return ((h >> (k * 5)) & 31) / 31.0; };   // a few stable 0..1 values per name
    const QString n = name.toLower();
    auto has = [&n](std::initializer_list<const char*> words) {
        return std::any_of(words.begin(), words.end(), [&n](const char* w) { return n.contains(QLatin1String(w)); });
    };
    auto at = [&](double x, double y) { return QPointF(r.left() + x * r.width(), r.top() + y * r.height()); };   // y 0 = top
    auto wave = [&](auto f, int steps = 96) {   // f: 0..1 -> -1..1
        QPainterPath w;
        for (int i = 0; i <= steps; ++i) {
            QPointF q = at(i / double(steps), 0.5 - 0.44 * f(i / double(steps)));
            i ? w.lineTo(q) : w.moveTo(q);
        }
        return w;
    };
    auto bars = [&](int count, auto height, double y0, bool centred) {   // vertical strokes; height(i) 0..1
        QPainterPath b;
        for (int i = 0; i < count; ++i) {
            double x = (i + 0.5) / count, hgt = height(i);
            b.moveTo(at(x, centred ? 0.5 - 0.46 * hgt : y0));
            b.lineTo(at(x, centred ? 0.5 + 0.46 * hgt : y0 - hgt * y0));
        }
        return b;
    };
    QPainterPath path;
    if (cat == "Dynamics") {   // the loud parts of a signal pulled down towards the threshold (dashed)
        const double th = 0.3 + 0.25 * v(0), ratio = n.contains("limit") || n.contains("clip") ? 50 : 2 + 6 * v(1), cyc = 5 + 3 * v(2);
        const bool gate = has({"gate", "expand"});
        auto in = [=](double t) { return (0.25 + 0.75 * std::pow(std::sin(M_PI * t), 0.6)) * std::sin(2 * M_PI * cyc * t); };
        p.setPen(QPen(theme::withAlpha(c, 110), 1, Qt::DashLine));
        p.drawLine(at(0, 0.5 - 0.44 * th), at(1, 0.5 - 0.44 * th));
        p.drawLine(at(0, 0.5 + 0.44 * th), at(1, 0.5 + 0.44 * th));
        p.setPen(dim);
        p.drawPath(wave(in, 160));
        p.setPen(pen);
        path = wave([=](double t) {
            double x = in(t), a = std::fabs(x);
            if (gate) a = a < th ? a * 0.25 : a;   // a gate / expander pushes the quiet parts down instead
            else if (a > th) a = th + (a - th) / ratio;
            return std::copysign(a, x);
        }, 160);
    } else if (cat == "EQ / Filter") {   // the frequency response the name suggests
        if (has({"high", "hp", "low cut"})) {
            path.moveTo(at(0, 0.98));
            path.cubicTo(at(0.18, 0.7), at(0.25, 0.3), at(0.42, 0.3));
            path.lineTo(at(1, 0.3));
        } else if (has({"low", "lp", "high cut"})) {
            path.moveTo(at(0, 0.3));
            path.lineTo(at(0.58, 0.3));
            path.cubicTo(at(0.75, 0.3), at(0.82, 0.7), at(1, 0.98));
        } else if (has({"notch"})) {
            path.moveTo(at(0, 0.3));
            path.lineTo(at(0.38, 0.3));
            path.cubicTo(at(0.47, 0.3), at(0.48, 0.98), at(0.5, 0.98));
            path.cubicTo(at(0.52, 0.98), at(0.53, 0.3), at(0.62, 0.3));
            path.lineTo(at(1, 0.3));
        } else if (has({"band"})) {
            path.moveTo(at(0, 0.95));
            path.cubicTo(at(0.35, 0.95), at(0.4, 0.12), at(0.5, 0.12));
            path.cubicTo(at(0.6, 0.12), at(0.65, 0.95), at(1, 0.95));
        } else if (has({"shel", "tilt"})) {
            path.moveTo(at(0, 0.25));
            path.lineTo(at(0.3, 0.25));
            path.cubicTo(at(0.5, 0.25), at(0.5, 0.72), at(0.7, 0.72));
            path.lineTo(at(1, 0.72));
        } else {   // a parametric band somewhere: boost or cut
            const double x0 = 0.25 + 0.5 * v(0), w = 0.1 + 0.12 * v(1), peak = v(2) < 0.7 ? 0.08 : 0.95;
            path.moveTo(at(0, 0.52));
            path.lineTo(at(x0 - 2 * w, 0.52));
            path.cubicTo(at(x0 - w, 0.52), at(x0 - w * 0.6, peak), at(x0, peak));
            path.cubicTo(at(x0 + w * 0.6, peak), at(x0 + w, 0.52), at(x0 + 2 * w, 0.52));
            path.lineTo(at(1, 0.52));
        }
    } else if (cat == "Time") {
        p.setPen(QPen(c, 2, Qt::SolidLine, Qt::RoundCap));
        if (has({"reverb", "verb", "room", "hall", "plate", "space", "convol"})) {   // a dense tail
            p.setPen(QPen(c, 1.2, Qt::SolidLine, Qt::RoundCap));
            const double decay = 3 + 3 * v(0);
            path = bars(28, [=](int i) { return std::exp(-decay * i / 28.0) * (0.55 + 0.45 * std::fabs(std::sin(i * 2.3 + 7 * v(1)))); }, 0.96, false);
        } else {   // repeats dying away
            const int count = 4 + int(v(0) * 3.99);
            const double fb = 0.55 + 0.25 * v(1);
            path = bars(count, [=](int i) { return std::pow(fb, i); }, 0.96, false);
        }
    } else if (cat == "Modulation") {   // the signal and a moving copy of it
        const double cyc = 1.5 + 2 * v(0), ph = 0.6 + 1.2 * v(1);
        p.setPen(dim);
        p.drawPath(wave([=](double t) { return std::sin(2 * M_PI * cyc * t + ph); }));
        p.setPen(pen);
        path = wave([=](double t) { return std::sin(2 * M_PI * cyc * t); });
    } else if (cat == "Distortion") {
        const double drive = 1.4 + 2.5 * v(0), cyc = 2 + v(1);
        if (has({"crush", "bit", "decimat"})) {   // stepped
            const double steps = 3 + int(v(2) * 3);
            path = wave([=](double t) { return std::round(std::sin(2 * M_PI * cyc * t) * steps) / steps; }, 240);
        } else if (has({"satur", "tube", "tape", "warm"}))
            path = wave([=](double t) { return std::tanh(drive * std::sin(2 * M_PI * cyc * t)) / std::tanh(drive); });
        else path = wave([=](double t) { return std::clamp(drive * std::sin(2 * M_PI * cyc * t), -1.0, 1.0); });
    } else if (cat == "Utility") {   // a level meter
        p.setPen(QPen(c, std::max(2.0, r.width() / 14), Qt::SolidLine, Qt::FlatCap));
        path = bars(5, [&](int i) { return 0.3 + 0.65 * v(i); }, 1.0, false);
    } else if (cat == "Synths") {   // an oscillator: saw, square or triangle (FM: a sine bent by another)
        const int cyc = 2 + int(v(1) * 2.99);
        const double shape = v(0);
        if (has({"fm"})) path = wave([=](double t) { return std::sin(2 * M_PI * cyc * t + 2.2 * std::sin(2 * M_PI * cyc * 3 * t)); }, 240);
        else if (shape < 0.45) {
            path.moveTo(at(0, 0.92));
            for (int k = 1; k <= cyc; ++k) {
                path.lineTo(at(k / double(cyc), 0.08));
                path.lineTo(at(k / double(cyc), 0.92));
            }
        } else if (shape < 0.75) {
            path.moveTo(at(0, 0.92));
            for (int k = 0; k < cyc; ++k) {
                double x = k / double(cyc), w = 1.0 / cyc;
                path.lineTo(at(x, 0.08));
                path.lineTo(at(x + w / 2, 0.08));
                path.lineTo(at(x + w / 2, 0.92));
                path.lineTo(at(x + w, 0.92));
            }
        } else path = wave([=](double t) { return 2 * std::fabs(2 * std::fmod(t * cyc + 0.25, 1.0) - 1) - 1; }, 48);
    } else if (cat == "Keys") {   // a stretch of keyboard
        const int white = 7 + int(v(0) * 3.99);
        const double w = 1.0 / white;
        for (int k = 0; k < white; ++k) path.addRect(QRectF(at(k * w, 0.04), at((k + 1) * w, 0.96)));
        p.drawPath(path);
        path = QPainterPath();
        p.setBrush(c);
        for (int k = 0; k < white - 1; ++k)
            if (k % 7 != 2 && k % 7 != 6) path.addRect(QRectF(at((k + 0.68) * w, 0.04), at((k + 1.32) * w, 0.6)));
    } else if (cat == "Samplers") {   // a recorded waveform
        const double f = 1.1 + 2 * v(0), ph = 6 * v(1), len = 18 + 8 * v(2);
        path = bars(18, [=](int i) { return (0.2 + 0.8 * std::fabs(std::sin(i * f + ph))) * std::max(0.08, 1 - i / len); }, 0, true);
    } else if (cat == "Drums") {   // hits and their decay
        const int hits = 2 + int(v(0) * 1.99);
        const double tail = 0.5 + 0.4 * v(1);
        path.moveTo(at(0, 0.92));
        for (int k = 0; k < hits; ++k) {
            double x0 = 0.04 + k / double(hits), w = tail / hits;
            path.lineTo(at(x0, 0.92));
            path.lineTo(at(x0, 0.1 + 0.25 * v(2 + k)));
            path.cubicTo(at(x0 + w * 0.15, 0.7), at(x0 + w * 0.45, 0.9), at(x0 + w, 0.92));
        }
        path.lineTo(at(1, 0.92));
    } else path = wave([=](double t) { return 0.6 * std::sin(2 * M_PI * (1 + v(0)) * t); }, 48);
    p.drawPath(path);
    p.restore();
}

// The plugin's own GUI, as captured the first time it was opened (PluginWindow), filling r from its top left corner, where
// most GUIs carry their name. False when there is no picture yet.
bool drawThumb(QPainter& p, const QRectF& r, const QString& file, bool lit) {
    QPixmap pm;
    if (file.isEmpty() || (!QPixmapCache::find(file, &pm) && !(pm.load(file) && QPixmapCache::insert(file, pm)))) return false;
    const QSizeF cover = QSizeF(pm.size()).scaled(r.size(), Qt::KeepAspectRatioByExpanding);
    const QRectF src(0, 0, r.width() * pm.width() / cover.width(), r.height() * pm.height() / cover.height());
    QPainterPath clip;
    clip.addRoundedRect(r, theme::radiusFor(r.height()), theme::radiusFor(r.height()));
    p.save();
    p.setClipPath(clip, Qt::IntersectClip);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setOpacity(lit ? 1.0 : 0.85);
    p.drawPixmap(r, pm, src);
    p.restore();
    return true;
}

void drawStar(QPainter& p, const QRectF& r, bool on, bool hot) {
    QPolygonF poly;
    for (int i = 0; i < 10; ++i) {
        double a = -M_PI / 2 + i * M_PI / 5, rad = (i % 2 ? 0.42 : 1.0) * r.width() / 2;
        poly << r.center() + QPointF(std::cos(a) * rad, std::sin(a) * rad);
    }
    p.setPen(QPen(on ? kStar : theme::withAlpha(theme::textDim(), hot ? 230 : 120), 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(on ? kStar : QColor(Qt::transparent));
    p.drawPolygon(poly);
}

// Format pill ending at `right`, vertically centred on cy; returns its rect
QRectF drawBadge(QPainter& p, qreal right, qreal cy, const QString& format) {
    const QString t = format.toUpper();
    QFont f = theme::smallFont();
    f.setPointSizeF(6.8);
    f.setBold(true);
    p.setFont(f);
    qreal w = QFontMetricsF(f).horizontalAdvance(t) + 10;
    QRectF b(right - w, cy - 7, w, 14);
    QColor c = format == "native" ? theme::accent() : theme::textDim();
    p.setPen(QPen(theme::withAlpha(c, 150), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(b, theme::radiusFor(14), theme::radiusFor(14));
    p.setPen(c);
    p.drawText(b, Qt::AlignCenter, t);
    return b;
}

class CardDelegate : public QStyledItemDelegate {
public:
    explicit CardDelegate(ProcessorPicker* picker)
        : QStyledItemDelegate(picker)
        , m_picker(picker) {}
    QRectF starRect(const QRect& r) const {
        return m_picker->cards() ? QRectF(r.right() - 27, r.top() + 12, 13, 13) : QRectF(r.right() - 22, r.center().y() - 6.5, 13, 13);
    }
    void paint(QPainter* p, const QStyleOptionViewItem& o, const QModelIndex& i) const override {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(o.rect).adjusted(0.5, 0.5, -0.5, -0.5);
        const QString name = i.data(Qt::DisplayRole).toString(), cat = i.data(CategoryRole).toString();
        if (i.data(HeaderRole).toBool()) {   // section title, then a hairline to the edge
            p->setFont(theme::boldFont());
            p->setPen(theme::text());
            QRectF tr = r.adjusted(6, 0, 0, 0);
            p->drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, name);
            qreal x = tr.left() + p->fontMetrics().horizontalAdvance(name) + 10;
            p->setPen(QPen(theme::border(), 1));
            p->drawLine(QPointF(x, r.center().y()), QPointF(r.right() - 6, r.center().y()));
            p->restore();
            return;
        }
        const bool sel = o.state & QStyle::State_Selected, hot = o.state & QStyle::State_MouseOver;
        const bool fav = m_picker->isFavorite(i.data(KeyRole).toString());
        const QColor cc = categoryColor(cat);
        const QString vendor = i.data(VendorRole).toString(), format = i.data(FormatRole).toString();
        if (m_picker->cards()) {
            QRectF card = r.adjusted(3, 3, -3, -3);
            p->setPen(QPen(sel ? theme::accent() : hot ? theme::gridStrong() : theme::border(), sel ? 1.5 : 1));
            p->setBrush(hot ? theme::hover() : theme::panelLight());
            p->drawRoundedRect(card, theme::radius(), theme::radius());
            QRectF scope(card.left() + 6, card.top() + 6, card.width() - 12, 46);
            p->setPen(Qt::NoPen);
            p->setBrush(theme::withAlpha(cc, sel || hot ? 48 : 28));
            p->drawRoundedRect(scope, theme::radiusFor(46), theme::radiusFor(46));
            if (!drawThumb(*p, scope, i.data(ThumbRole).toString(), sel || hot)) drawSignal(*p, scope.adjusted(10, 8, -30, -8), cat, name, cc);
            if (fav || hot) drawStar(*p, starRect(o.rect), fav, hot);
            QRectF bottom(card.left() + 8, card.bottom() - 20, card.width() - 16, 14);
            QRectF badge = drawBadge(*p, bottom.right(), bottom.center().y(), format);
            p->setFont(theme::boldFont());
            p->setPen(theme::text());
            QRectF nameR(card.left() + 8, scope.bottom() + 4, card.width() - 16, 18);
            p->drawText(nameR, Qt::AlignVCenter | Qt::AlignLeft, p->fontMetrics().elidedText(name, Qt::ElideRight, nameR.width()));
            p->setFont(theme::smallFont());
            p->setPen(theme::textDim());
            p->drawText(bottom.adjusted(0, 0, -badge.width() - 6, 0), Qt::AlignVCenter | Qt::AlignLeft,
                p->fontMetrics().elidedText(vendor, Qt::ElideRight, bottom.width() - badge.width() - 6));
        } else {
            QRectF row = r.adjusted(2, 1, -2, -1);
            if (sel || hot) {
                p->setPen(sel ? QPen(theme::withAlpha(theme::accent(), 160), 1) : Qt::NoPen);
                p->setBrush(sel ? theme::selection() : theme::hover());
                p->drawRoundedRect(row, theme::radiusFor(row.height()), theme::radiusFor(row.height()));
            }
            QRectF icon(row.left() + 6, row.top() + 4, 28, row.height() - 8);
            p->setPen(Qt::NoPen);
            p->setBrush(theme::withAlpha(cc, 36));
            p->drawRoundedRect(icon, theme::radiusFor(icon.height()), theme::radiusFor(icon.height()));
            if (!drawThumb(*p, icon, i.data(ThumbRole).toString(), sel || hot)) drawSignal(*p, icon.adjusted(4, 3, -4, -3), cat, name, cc);
            if (fav || hot) drawStar(*p, starRect(o.rect), fav, hot);
            QRectF badge = drawBadge(*p, row.right() - 28, row.center().y(), format);
            p->setFont(theme::smallFont());
            p->setPen(theme::textDim());
            QRectF catR(badge.left() - 100, row.top(), 92, row.height());
            p->drawText(catR, Qt::AlignVCenter | Qt::AlignRight, cat);
            qreal x = icon.right() + 10, room = catR.left() - 8 - x;
            p->setFont(theme::boldFont());
            p->setPen(theme::text());
            QString n = p->fontMetrics().elidedText(name, Qt::ElideRight, room);
            p->drawText(QRectF(x, row.top(), room, row.height()), Qt::AlignVCenter | Qt::AlignLeft, n);
            x += p->fontMetrics().horizontalAdvance(n) + 10;
            p->setFont(theme::uiFont());
            p->setPen(theme::textDim());
            p->drawText(QRectF(x, row.top(), catR.left() - 8 - x, row.height()), Qt::AlignVCenter | Qt::AlignLeft,
                p->fontMetrics().elidedText(vendor, Qt::ElideRight, catR.left() - 8 - x));
        }
        p->restore();
    }
    bool editorEvent(QEvent* e, QAbstractItemModel* m, const QStyleOptionViewItem& o, const QModelIndex& i) override {
        const bool release = e->type() == QEvent::MouseButtonRelease;
        if ((release || e->type() == QEvent::MouseButtonDblClick) && !i.data(HeaderRole).toBool() &&
            starRect(o.rect).adjusted(-4, -4, 4, 4).contains(static_cast<QMouseEvent*>(e)->position())) {
            if (release)   // later: the toggle rebuilds the items, so not from inside their own event
                QTimer::singleShot(0, m_picker, [p = m_picker, key = i.data(KeyRole).toString()] { p->toggleFavorite(key); });
            return true;
        }
        return QStyledItemDelegate::editorEvent(e, m, o, i);
    }

private:
    ProcessorPicker* m_picker;
};

class RailDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return {0, 26}; }
    void paint(QPainter* p, const QStyleOptionViewItem& o, const QModelIndex& i) const override {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        QRectF r = QRectF(o.rect).adjusted(2, 1, -2, -1);
        const bool sel = o.state & QStyle::State_Selected, hot = o.state & QStyle::State_MouseOver;
        if (sel || hot) {
            p->setPen(Qt::NoPen);
            p->setBrush(sel ? theme::selection() : theme::hover());
            p->drawRoundedRect(r, theme::radiusFor(r.height()), theme::radiusFor(r.height()));
        }
        const QString f = i.data(KeyRole).toString();
        const int n = i.data(CountRole).toInt();
        if (f == "@fav") drawStar(*p, QRectF(r.left() + 5, r.center().y() - 5, 10, 10), true, false);
        else {
            p->setPen(Qt::NoPen);
            p->setBrush(f.isEmpty() || f == "@recent" ? theme::textDim() : categoryColor(f));
            p->drawEllipse(QPointF(r.left() + 10, r.center().y()), 3.5, 3.5);
        }
        p->setFont(sel ? theme::boldFont() : theme::uiFont());
        p->setPen(n ? theme::text() : theme::textDim());
        p->drawText(r.adjusted(22, 0, -30, 0), Qt::AlignVCenter | Qt::AlignLeft, i.data(Qt::DisplayRole).toString());
        p->setFont(theme::smallFont());
        p->setPen(theme::textDim());
        p->drawText(r.adjusted(0, 0, -8, 0), Qt::AlignVCenter | Qt::AlignRight, QString::number(n));
        p->restore();
    }
};

enum class ViewIcon { Grid, List, Picture };
QIcon viewIcon(ViewIcon kind) {
    QPixmap pm(28, 28);
    pm.setDevicePixelRatio(2);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(theme::iconColor());
    if (kind == ViewIcon::Grid)
        for (int k = 0; k < 4; ++k) p.drawRoundedRect(QRectF(2 + (k % 2) * 6, 2 + (k / 2) * 6, 4.5, 4.5), 1, 1);
    else if (kind == ViewIcon::List)
        for (int k = 0; k < 3; ++k) p.drawRoundedRect(QRectF(2, 2.5 + k * 4, 10, 2), 0.8, 0.8);
    else {   // a framed picture: a sun over hills
        p.setPen(QPen(theme::iconColor(), 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(1.5, 2.5, 11, 9), 1.5, 1.5);
        p.setPen(Qt::NoPen);
        p.setBrush(theme::iconColor());
        p.drawEllipse(QPointF(9.5, 5.5), 1.2, 1.2);
        p.drawPolygon(QPolygonF({QPointF(2, 11), QPointF(5.5, 6.5), QPointF(8, 9.5), QPointF(9.5, 8), QPointF(12, 11)}));
    }
    return QIcon(pm);
}

// Cards stretch so each row is filled edge to edge; a section title takes a whole row. List rows span the view anyway.
void fitItems(QListWidget* w, bool cards) {
    const int sp = w->spacing(), room = w->viewport()->width() - sp - 6;   // slack: a wrapping QListView keeps a scrollbar's room
    const int cols = std::max(1, room / (150 + sp)), cardW = room / cols - sp;
    for (int r = 0; r < w->count(); ++r) {
        auto* it = w->item(r);
        const bool header = it->data(HeaderRole).toBool();
        it->setSizeHint(cards ? (header ? QSize(room - sp, 28) : QSize(cardW, 106)) : QSize(0, header ? 28 : 30));
    }
}
}   // namespace

QString ProcessorPicker::thumbnailPath(const QString& vendor, const QString& name) {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/thumbnails/" +
        QCryptographicHash::hash((vendor + '/' + name).toLower().toUtf8(), QCryptographicHash::Md5).toHex() + ".png";
}

// What a plugin does, from the categories it reports (VST3 "Fx|Dynamics", CLAP features, the LV2 class) and its name
QString ProcessorPicker::categoryOf(const PluginInfo& p) {
    const QStringList words = (p.subCategories + ' ' + p.name).toLower().split(QRegularExpression("[^a-z0-9]+"), Qt::SkipEmptyParts);
    auto any = [&](std::initializer_list<const char*> prefixes) {
        for (auto& w : words)
            for (auto* pre : prefixes)
                if (w.startsWith(QLatin1String(pre))) return true;
        return false;
    };
    if (p.category == "Instrument") {
        if (any({"drum", "percussion", "808", "909"})) return "Drums";
        if (any({"piano", "keys", "organ", "rhodes", "wurli", "clav", "epiano", "harpsi", "celest"})) return "Keys";
        if (any({"sampler", "sample", "rompler"})) return "Samplers";
        return "Synths";
    }
    if (any({"compress", "limit", "gate", "expand", "dynamic", "transient", "clipper", "deess"})) return "Dynamics";
    if (any({"eq", "equali", "filter", "lowpass", "highpass", "bandpass", "notch", "parametric", "shelf", "crossover"})) return "EQ / Filter";
    if (any({"delay", "reverb", "echo", "convol", "room", "hall", "plate"})) return "Time";
    if (any({"chorus", "flang", "phaser", "tremolo", "vibrato", "modulat", "rotary", "ensemble"})) return "Modulation";
    if (any({"distort", "saturat", "overdrive", "fuzz", "crush", "waveshap", "amplifier", "tube"})) return "Distortion";
    if (any({"analy", "meter", "utility", "gain", "mixer", "tool", "scope", "tuner", "width", "imager", "mono", "spectr"})) return "Utility";
    return "Other";
}

ProcessorPicker::ProcessorPicker(App& app, ProcessorKind kind, QWidget* parent)
    : QDialog(parent)
    , m_app(app)
    , m_kind(kind) {
    const bool inst = kind == ProcessorKind::Instrument;
    setWindowTitle(inst ? "Choose instrument" : "Choose effect");
    resize(900, 580);
    QSettings s;
    const QStringList favs = s.value("browser/favorites").toStringList();
    m_favorites = QSet<QString>(favs.begin(), favs.end());
    m_recent = s.value("picker/recent").toStringList();
    m_pictures = s.value("picker/pictures", true).toBool();
    for (auto& i : NativeRegistry::instance().all())
        if (i.kind == kind) m_entries.push_back({"native:" + i.key, i.name, "Vybrant", "native", inst ? nativeInstrumentCategory(i.key) : i.category});
    for (auto& p : m_app.pluginDb().plugins()) {
        if (p.failed || p.hidden || (p.category == "Instrument") != inst) continue;
        m_entries.push_back({"plugin:" + p.key(), p.name, p.vendor, p.format, categoryOf(p)});
        if (p.favorite) m_favorites.insert("plugin:" + p.key());   // starred in the plugin manager
    }
    std::sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });

    auto* lay = new QVBoxLayout(this);
    auto* top = new QHBoxLayout;
    m_search = new QLineEdit(this);
    m_search->setObjectName("pickerSearch");
    m_search->setPlaceholderText("Search name, maker or type...");
    m_search->setClearButtonEnabled(true);
    m_search->installEventFilter(this);
    top->addWidget(m_search, 1);
    // format chips, only when there is more than one format to choose from
    QStringList formats;
    for (auto* f : {"native", "clap", "vst3", "lv2", "vst2"})
        if (std::any_of(m_entries.begin(), m_entries.end(), [f](const Entry& e) { return e.format == f; })) formats << f;
    if (formats.size() > 1) {
        auto* group = new QButtonGroup(this);
        formats.prepend(QString());
        for (auto& f : formats) {
            auto* b = new QToolButton(this);
            b->setText(f.isEmpty() ? "All" : f.toUpper());
            b->setCheckable(true);
            b->setChecked(f.isEmpty());
            theme::styled(b, "QToolButton { border: 1px solid @border@; border-radius: 9px; padding: 1px 9px; color: @textDim@; background: transparent; }"
                             "QToolButton:checked { border-color: @accent@; color: @text@; background: @hover@; }");
            group->addButton(b);
            top->addWidget(b);
            connect(b, &QToolButton::clicked, this, [this, f] {
                m_format = f;
                rebuild();
            });
        }
    }
    top->addSpacing(12);
    auto* pictures = new QToolButton(this);   // the plugins' own GUIs on the cards, or the drawings only
    pictures->setObjectName("pickerPictures");
    pictures->setIcon(viewIcon(ViewIcon::Picture));
    pictures->setToolTip("Show pictures of the plugins' own windows (taken the first time each one is opened)");
    pictures->setCheckable(true);
    pictures->setAutoRaise(true);
    pictures->setChecked(m_pictures);
    connect(pictures, &QToolButton::toggled, this, [this](bool on) {
        m_pictures = on;
        QSettings().setValue("picker/pictures", on);
        rebuild(true);
    });
    top->addWidget(pictures);
    auto* views = new QButtonGroup(this);
    for (bool grid : {true, false}) {
        auto* b = new QToolButton(this);
        b->setObjectName(grid ? "pickerCards" : "pickerList");
        b->setIcon(viewIcon(grid ? ViewIcon::Grid : ViewIcon::List));
        b->setToolTip(grid ? "Cards" : "List");
        b->setCheckable(true);
        b->setAutoRaise(true);
        views->addButton(b);
        top->addWidget(b);
        connect(b, &QToolButton::clicked, this, [this, grid] { setCards(grid); });
        (grid ? m_cardsBtn : m_listBtn) = b;
    }
    lay->addLayout(top);

    auto* body = new QHBoxLayout;
    m_rail = new QListWidget(this);
    m_rail->setObjectName("pickerRail");
    m_rail->setFixedWidth(160);
    m_rail->setItemDelegate(new RailDelegate(m_rail));
    m_rail->viewport()->setAttribute(Qt::WA_Hover);
    m_rail->setFocusPolicy(Qt::NoFocus);
    auto railItem = [this](const QString& label, const QString& filter) {
        auto* it = new QListWidgetItem(label, m_rail);
        it->setData(KeyRole, filter);
    };
    railItem("All", QString());
    railItem("Favorites", "@fav");
    railItem("Recently used", "@recent");
    for (auto& c : inst ? kInstrumentCats : kEffectCats)
        if (std::any_of(m_entries.begin(), m_entries.end(), [&c](const Entry& e) { return e.category == c; })) railItem(c, c);
    m_rail->setCurrentRow(0);
    connect(m_rail, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* it) {
        m_category = it ? it->data(KeyRole).toString() : QString();
        rebuild();
    });
    body->addWidget(m_rail);
    m_items = new QListWidget(this);
    m_items->setObjectName("pickerItems");
    m_items->setItemDelegate(new CardDelegate(this));
    m_items->setResizeMode(QListView::Adjust);
    m_items->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_items->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_items->verticalScrollBar()->setSingleStep(24);
    m_items->setMouseTracking(true);
    m_items->viewport()->setAttribute(Qt::WA_Hover);
    m_items->viewport()->installEventFilter(this);
    connect(m_items, &QListWidget::itemActivated, this, &ProcessorPicker::choose);
    body->addWidget(m_items, 1);
    lay->addLayout(body, 1);

    auto* bottom = new QHBoxLayout;
    bottom->addWidget(theme::dimLabel("Double-click or Enter to add  ·  click the star to keep it under Favorites", this));
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    bb->button(QDialogButtonBox::Ok)->setText(inst ? "Use" : "Add");
    connect(bb, &QDialogButtonBox::accepted, this, [this] { choose(m_items->currentItem()); });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    bottom->addWidget(bb);
    lay->addLayout(bottom);

    connect(m_search, &QLineEdit::textChanged, this, [this] { rebuild(); });
    setCards(QSettings().value("picker/view", "cards").toString() != "list");
    m_search->setFocus();
}

void ProcessorPicker::setCards(bool on) {
    m_cards = on;
    QSettings().setValue("picker/view", on ? "cards" : "list");
    m_cardsBtn->setChecked(on);
    m_listBtn->setChecked(!on);
    m_items->setFlow(on ? QListView::LeftToRight : QListView::TopToBottom);
    m_items->setWrapping(on);
    m_items->setSpacing(on ? 2 : 0);
    rebuild(true);
}

void ProcessorPicker::toggleFavorite(const QString& key) {
    const bool on = !m_favorites.contains(key);
    on ? (void)m_favorites.insert(key) : (void)m_favorites.remove(key);
    QSettings().setValue("browser/favorites", QStringList(m_favorites.begin(), m_favorites.end()));
    if (key.startsWith("plugin:")) m_app.pluginDb().setFavorite(key.mid(7), on);   // the plugin manager's star too
    Q_EMIT m_app.favoritesChanged();
    rebuild(true);
}

void ProcessorPicker::rebuild(bool keepPlace) {
    const QStringList words = m_search->text().toLower().split(' ', Qt::SkipEmptyParts);
    std::vector<const Entry*> hits;
    for (auto& e : m_entries) {
        if (!m_format.isEmpty() && e.format != m_format) continue;
        const QString hay = (e.name + ' ' + e.vendor + ' ' + e.category + ' ' + e.format).toLower();
        if (std::all_of(words.begin(), words.end(), [&hay](const QString& w) { return hay.contains(w); })) hits.push_back(&e);
    }
    auto inFilter = [this](const Entry& e, const QString& f) {
        return f.isEmpty() || (f == "@fav" ? m_favorites.contains(e.key) : f == "@recent" ? m_recent.contains(e.key) : e.category == f);
    };
    for (int r = 0; r < m_rail->count(); ++r) {   // counts follow the search, so the rail shows where the matches are
        auto* it = m_rail->item(r);
        const QString f = it->data(KeyRole).toString();
        it->setData(CountRole, (int)std::count_if(hits.begin(), hits.end(), [&](const Entry* e) { return inFilter(*e, f); }));
    }

    const QString keep = keepPlace && m_items->currentItem() ? m_items->currentItem()->data(KeyRole).toString() : QString();
    const int scroll = m_items->verticalScrollBar()->value();
    m_items->clear();
    auto addSection = [&](const QString& title, const std::vector<const Entry*>& list) {
        if (list.empty()) return;
        auto* h = new QListWidgetItem(title, m_items);
        h->setData(HeaderRole, true);
        h->setFlags(Qt::NoItemFlags);
        for (auto* e : list) {
            auto* it = new QListWidgetItem(e->name, m_items);
            it->setData(KeyRole, e->key);
            it->setData(VendorRole, e->vendor);
            it->setData(FormatRole, e->format);
            it->setData(CategoryRole, e->category);
            const QString thumb = m_pictures && e->key.startsWith("plugin:") ? thumbnailPath(e->vendor, e->name) : QString();
            const bool hasThumb = !thumb.isEmpty() && QFile::exists(thumb);
            it->setData(ThumbRole, hasThumb ? thumb : QString());
            it->setToolTip(QString("%1<b>%2</b><br>%3  ·  %4  ·  %5")
                    .arg(hasThumb ? QString("<img src=\"%1\" width=\"360\"><br>").arg(thumb.toHtmlEscaped()) : QString(),
                        e->name.toHtmlEscaped(), e->vendor.toHtmlEscaped(), e->category, e->format.toUpper()));
        }
    };
    // with nothing typed, "All" opens on what was used last and the favourites; both are also in their own category
    const bool overview = m_category.isEmpty() && words.isEmpty();
    if (overview || m_category == "@recent") {
        std::vector<const Entry*> rec;
        for (auto& k : m_recent)
            for (auto* e : hits)
                if (e->key == k) rec.push_back(e);
        addSection("Recently used", rec);
    }
    if (overview || m_category == "@fav") {
        std::vector<const Entry*> fav;
        std::copy_if(hits.begin(), hits.end(), std::back_inserter(fav), [this](const Entry* e) { return m_favorites.contains(e->key); });
        addSection("Favorites", fav);
    }
    if (!m_category.startsWith('@'))
        for (auto& c : m_kind == ProcessorKind::Instrument ? kInstrumentCats : kEffectCats) {
            if (!m_category.isEmpty() && m_category != c) continue;
            std::vector<const Entry*> list;
            std::copy_if(hits.begin(), hits.end(), std::back_inserter(list), [&c](const Entry* e) { return e->category == c; });
            addSection(c, list);
        }
    if (!m_items->count()) {
        auto* h = new QListWidgetItem(m_category == "@fav" ? "No favorites yet: click the star on a card" : "Nothing matches", m_items);
        h->setData(HeaderRole, true);
        h->setFlags(Qt::NoItemFlags);
    }
    fitItems(m_items, m_cards);

    QListWidgetItem* cur = nullptr;
    for (int r = 0; r < m_items->count() && !cur; ++r) {
        auto* it = m_items->item(r);
        if (!it->data(HeaderRole).toBool() && (keep.isEmpty() || it->data(KeyRole).toString() == keep)) cur = it;
    }
    if (!cur && !keep.isEmpty())
        for (int r = 0; r < m_items->count() && !cur; ++r)
            if (!m_items->item(r)->data(HeaderRole).toBool()) cur = m_items->item(r);
    m_items->setCurrentItem(cur);
    if (keepPlace) {
        m_items->doItemsLayout();
        m_items->verticalScrollBar()->setValue(scroll);
    } else m_items->scrollToTop();
}

void ProcessorPicker::choose(QListWidgetItem* it) {
    if (!it || it->data(HeaderRole).toBool()) return;
    const QString key = it->data(KeyRole).toString();
    if (key.startsWith("native:")) m_nativeKey = key.mid(7);
    else
        for (auto& p : m_app.pluginDb().plugins())
            if ("plugin:" + p.key() == key) m_plugin = &p;
    if (m_nativeKey.isEmpty() && !m_plugin) return;
    m_recent.removeAll(key);
    m_recent.prepend(key);
    QSettings().setValue("picker/recent", m_recent.mid(0, 12));
    accept();
}

bool ProcessorPicker::eventFilter(QObject* o, QEvent* e) {
    if (o == m_search && e->type() == QEvent::KeyPress) {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->key() == Qt::Key_Down) {
            m_items->setFocus();
            return true;
        }
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
            choose(m_items->currentItem());
            return true;
        }
    }
    if (o == m_items->viewport() && e->type() == QEvent::Resize) fitItems(m_items, m_cards);
    return QDialog::eventFilter(o, e);
}

}   // namespace mt
