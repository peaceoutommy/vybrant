// Waveform display of an audio asset with optional start marker, loop region, envelope overlay and file drop.
// Used by the sampler editor, the audio clip editor and the sample editor.
#pragma once
#include "ui/Theme.h"
#include "audio/AudioFile.h"
#include <QWidget>
#include <QPainter>
#include <QMouseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QFileInfo>
#include <functional>

namespace mt {

class WaveView : public QWidget {
public:
    explicit WaveView(QWidget* parent = nullptr)
        : QWidget(parent) {
        setMinimumHeight(120);
        setAcceptDrops(true);
        setMouseTracking(true);
    }
    std::function<AudioAssetPtr()> asset;
    std::function<void(const QString&)> onDrop;
    std::function<float()> startFrac;
    std::function<void(float)> setStart;
    // Optional processing view: the drawn waveform reflects the instrument's current settings
    std::function<float(double)> envelope;   // gain multiplier at fraction 0..1 of the (already reversed) sample
    std::function<bool()> reversed;   // play backwards
    std::function<std::pair<float, float>()> loop;   // loop region (fractions), enabled when first < second
    std::function<QString()> caption;   // extra info line
    std::function<bool()> showEnvelope;   // draw the envelope line + translucent fill (only once the user has touched the envelope)
    std::function<std::pair<double, double>()> view;   // visible part of the file (fractions); the whole file when unset
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), theme::surface());
        auto a = asset ? asset() : nullptr;
        if (!a || a->frames == 0) {
            p.setPen(theme::textDim());
            p.drawText(rect(), Qt::AlignCenter, "Drop an audio file here or use Load");
            return;
        }
        bool rev = reversed && reversed();
        float st = startFrac ? startFrac() : 0.f;
        std::pair<float, float> lp = loop ? loop() : std::make_pair(0.f, 0.f);
        bool hasLoop = lp.first < lp.second;
        QRect wave = rect().adjusted(0, 16, 0, 0);
        auto [v0, v1] = view ? view() : std::make_pair(0.0, 1.0);
        double span = v1 - v0;
        auto fx = [&](double f) { return (int)((f - v0) / span * wave.width()); };   // file fraction -> x
        double spp = (double)a->frames * span / wave.width();
        const auto& lv = a->levelFor(spp);
        int mid = wave.center().y();
        int hh = wave.height() / 2 - 2;
        // background regions: before the start point (skipped) and the loop region
        if (st > 0) p.fillRect(QRect(wave.left(), wave.top(), fx(st), wave.height()), QColor(0x1a, 0x1c, 0x21));
        if (hasLoop)
            p.fillRect(
                QRect(wave.left() + fx(lp.first), wave.top(), fx(lp.second) - fx(lp.first), wave.height()), QColor(0x22, 0x2c, 0x38));
        p.setPen(theme::grid());
        p.drawLine(wave.left(), mid, wave.right(), mid);
        for (int x = 0; x < wave.width(); ++x) {
            double frac = v0 + span * x / wave.width();
            double srcFrac = rev ? 1.0 - frac : frac;
            int64_t b0 = (int64_t)(srcFrac * a->frames / lv.samplesPerBin),
                    b1 = std::max<int64_t>(b0 + 1, (int64_t)((srcFrac + span / wave.width()) * a->frames / lv.samplesPerBin));
            if (rev) std::swap(b0, b1), b0 = std::max<int64_t>(0, b1 - std::max<int64_t>(1, (int64_t)(spp / lv.samplesPerBin)));
            float mx = -1, mn = 1;
            for (int64_t b = b0; b < b1 && b < (int64_t)lv.maxL.size(); ++b) {
                if (b < 0) continue;
                mx = std::max(mx, std::max(lv.maxL[b], lv.maxR[b]));
                mn = std::min(mn, std::min(lv.minL[b], lv.minR[b]));
            }
            if (mx < mn) continue;
            float g = envelope ? envelope(frac) : 1.f;
            bool active = frac >= st;
            QColor c = active ? QColor("#5ad1c9") : QColor(0x3a, 0x45, 0x50);
            if (g > 1.f && active) c = c.lighter(110 + (int)std::min(60.f, (g - 1) * 40));
            p.setPen(c);
            p.drawLine(x, mid - (int)(clamp(mx * g, -1.f, 1.f) * hh), x, mid - (int)(clamp(mn * g, -1.f, 1.f) * hh));
        }
        if (envelope && (!showEnvelope || showEnvelope())) {   // envelope: line over the sound with a translucent body underneath it
            QPolygonF env;
            for (int x = 0; x <= wave.width(); x += 2)
                env << QPointF(x, mid - clamp(envelope(v0 + span * x / wave.width()), 0.f, 1.f) * hh);
            QPolygonF body = env;
            body << QPointF(wave.width(), mid) << QPointF(0, mid);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(theme::withAlpha(theme::accent(), 34));
            p.drawPolygon(body);
            p.setPen(QPen(theme::withAlpha(theme::accent(), 200), 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawPolyline(env);
            p.setRenderHint(QPainter::Antialiasing, false);
        }
        int sx = fx(st);
        p.setPen(QPen(theme::accent(), 2));
        p.drawLine(sx, wave.top(), sx, wave.bottom());
        if (hasLoop) {
            p.setPen(QPen(theme::accent2(), 1, Qt::DashLine));
            int lx1 = fx(lp.first), lx2 = fx(lp.second);
            p.drawLine(lx1, wave.top(), lx1, wave.bottom());
            p.drawLine(lx2, wave.top(), lx2, wave.bottom());
        }
        p.setPen(theme::textDim());
        p.setFont(theme::smallFont());
        QString info = QString("%1  |  %2 s  |  %3 Hz  |  %4%5")
                           .arg(QFileInfo(a->path).fileName())
                           .arg(a->seconds(), 0, 'f', 2)
                           .arg((int)a->originalSampleRate)
                           .arg(a->channels == 1 ? "mono" : "stereo")
                           .arg(rev ? "  |  reversed" : "");
        if (caption) info += "  |  " + caption();
        p.drawText(4, 12, info);
    }
    void enterEvent(QEnterEvent*) override { setCursor(setStart ? Qt::PointingHandCursor : Qt::ArrowCursor); }   // click sets the start
    void mousePressEvent(QMouseEvent* e) override {
        if (setStart && e->button() == Qt::LeftButton) {
            setStart(clamp((float)e->pos().x() / width(), 0.f, 1.f));
            update();
        }
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (setStart && (e->buttons() & Qt::LeftButton)) {
            setStart(clamp((float)e->pos().x() / width(), 0.f, 1.f));
            update();
        }
    }
    void dragEnterEvent(QDragEnterEvent* e) override {
        if (e->mimeData()->hasUrls()) e->acceptProposedAction();
    }
    void dropEvent(QDropEvent* e) override {
        for (auto& u : e->mimeData()->urls()) {
            QString p = u.toLocalFile();
            if (AudioFile::isAudioFile(p) && onDrop) {
                onDrop(p);
                break;
            }
        }
    }
};

}   // namespace mt
