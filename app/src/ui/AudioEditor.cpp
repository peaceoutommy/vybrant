#include "ui/AudioEditor.h"
#include "ui/App.h"
#include "ui/widgets/WaveView.h"
#include <QDialog>
#include <QBoxLayout>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QInputDialog>
#include <QMessageBox>
#include <QStandardPaths>
#include <QDir>
#include <QKeyEvent>
#include <QWheelEvent>
#include <algorithm>
#include <numeric>

namespace mt {

namespace sampleedit {
template <class F> static void eachChannel(AudioAsset& a, F f) {   // mono assets keep 'right' empty
    f(a.left);
    if (a.channels > 1) f(a.right);
}
void trim(AudioAsset& a, int64_t from, int64_t to) {
    eachChannel(a, [&](std::vector<float>& v) {
        v.erase(v.begin() + to, v.end());
        v.erase(v.begin(), v.begin() + from);
    });
    a.frames = (int64_t)a.left.size();
}
void erase(AudioAsset& a, int64_t from, int64_t to) {
    eachChannel(a, [&](std::vector<float>& v) { v.erase(v.begin() + from, v.begin() + to); });
    a.frames = (int64_t)a.left.size();
}
void silence(AudioAsset& a, int64_t from, int64_t to) {
    eachChannel(a, [&](std::vector<float>& v) { std::fill(v.begin() + from, v.begin() + to, 0.f); });
}
void fade(AudioAsset& a, int64_t from, int64_t to, bool in) {
    double n = (double)std::max<int64_t>(1, to - from - 1);
    eachChannel(a, [&](std::vector<float>& v) {
        for (int64_t i = from; i < to; ++i) v[i] *= (float)((in ? i - from : to - 1 - i) / n);
    });
}
void gain(AudioAsset& a, int64_t from, int64_t to, float g) {
    eachChannel(a, [&](std::vector<float>& v) {
        for (int64_t i = from; i < to; ++i) v[i] *= g;
    });
}
void normalize(AudioAsset& a, int64_t from, int64_t to) {
    float peak = 0.f;
    eachChannel(a, [&](std::vector<float>& v) {
        for (int64_t i = from; i < to; ++i) peak = std::max(peak, std::fabs(v[i]));
    });
    if (peak > 0.f) gain(a, from, to, 1.f / peak);
}
void reverse(AudioAsset& a, int64_t from, int64_t to) {
    eachChannel(a, [&](std::vector<float>& v) { std::reverse(v.begin() + from, v.begin() + to); });
}
void removeDc(AudioAsset& a, int64_t from, int64_t to) {
    if (to <= from) return;
    eachChannel(a, [&](std::vector<float>& v) {
        float mean = (float)(std::accumulate(v.begin() + from, v.begin() + to, 0.0) / (double)(to - from));
        for (int64_t i = from; i < to; ++i) v[i] -= mean;
    });
}
void trimSilence(AudioAsset& a, float threshold) {
    auto loud = [&](int64_t i) { return std::fabs(a.left[i]) > threshold || (a.channels > 1 && std::fabs(a.right[i]) > threshold); };
    int64_t s = 0, e = a.frames;
    while (s < e && !loud(s)) ++s;
    while (e > s && !loud(e - 1)) --e;
    if (s < e) trim(a, s, e);   // all silent: left alone rather than emptied
}
}   // namespace sampleedit

// The editor's waveform: drag selects, the wheel zooms at the cursor, Shift+wheel scrolls
class EditWave : public WaveView {
public:
    double v0 = 0, v1 = 1;   // visible part (fractions of the sample)
    int64_t selA = 0, selB = 0;   // selection anchor and end (frames); equal = nothing selected
    std::function<void()> viewChanged;
    explicit EditWave(QWidget* parent)
        : WaveView(parent) {
        setAcceptDrops(false);
        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(640, 220);
        view = [this] { return std::make_pair(v0, v1); };
        loop = [this] {   // the selection, drawn as WaveView's highlighted region
            auto [a, b] = selection();
            double n = (double)std::max<int64_t>(1, frames());
            return std::make_pair((float)(a / n), (float)(b / n));
        };
    }
    int64_t frames() const {
        auto a = asset ? asset() : nullptr;
        return a ? a->frames : 0;
    }
    std::pair<int64_t, int64_t> selection() const { return {std::min(selA, selB), std::max(selA, selB)}; }
    void setView(double from, double span) {
        span = std::clamp(span, std::min(1.0, 1000.0 / (double)std::max<int64_t>(1, frames())), 1.0);   // zoomed in: 1000 frames across
        v0 = std::clamp(from, 0.0, 1.0 - span);
        v1 = v0 + span;
        if (viewChanged) viewChanged();
        update();
    }
    int64_t frameAt(double x) const {
        return std::clamp<int64_t>((int64_t)((v0 + (v1 - v0) * x / std::max(1, width())) * (double)frames()), 0, frames());
    }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        selA = selB = frameAt(e->position().x());
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        if (!(e->buttons() & Qt::LeftButton)) return;
        selB = frameAt(e->position().x());
        update();
    }
    void wheelEvent(QWheelEvent* e) override {
        double span = v1 - v0, steps = (e->angleDelta().y() + e->angleDelta().x()) / 120.0;
        if (e->modifiers() & Qt::ShiftModifier || e->angleDelta().x()) {
            setView(v0 - steps * span / 8, span);
            return;
        }
        double at = v0 + span * e->position().x() / std::max(1, width()), zoomed = span * std::pow(0.8, steps);
        setView(at - (at - v0) * zoomed / span, zoomed);   // the point under the cursor stays put
    }
};

class AudioEditor : public QDialog {
public:
    AudioEditor(App& app, QWidget* parent, Id sourceId, Id channelId, const QString& path, std::shared_ptr<AudioAsset> asset)
        : QDialog(parent)
        , m_app(app)
        , m_source(sourceId)
        , m_channel(channelId)
        , m_path(path)
        , m_asset(std::move(asset))
        , m_saved(m_asset) {
        setWindowTitle("Edit audio: " + QFileInfo(path).fileName());
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(10, 8, 10, 10);
        lay->setSpacing(6);
        auto button = [this](QBoxLayout* row, const QString& text, const QString& tip, std::function<void()> fn) {
            auto* b = new QPushButton(text, this);
            b->setToolTip(tip);
            b->setFocusPolicy(Qt::NoFocus);   // Space, Delete and Ctrl+Z stay with the editor
            b->setAutoDefault(false);
            connect(b, &QPushButton::clicked, this, fn);
            row->addWidget(b);
        };
        m_wave = new EditWave(this);
        m_wave->asset = [this] { return AudioAssetPtr(m_asset); };
        m_wave->caption = [this] {
            auto [a, b] = m_wave->selection();
            double sr = std::max(1.0, m_asset->sampleRate);
            return a == b ? QString("drag to select, wheel to zoom, Shift+wheel to scroll")
                          : QString("selection %1 - %2 s").arg(a / sr, 0, 'f', 3).arg(b / sr, 0, 'f', 3);
        };
        auto* scroll = new QScrollBar(Qt::Horizontal, this);
        constexpr int kSteps = 100000;   // scrollbar resolution over the whole sample
        m_wave->viewChanged = [this, scroll] {
            const QSignalBlocker block(scroll);
            int page = (int)((m_wave->v1 - m_wave->v0) * kSteps);
            scroll->setRange(0, kSteps - page);
            scroll->setPageStep(page);
            scroll->setValue((int)(m_wave->v0 * kSteps));
        };
        connect(scroll, &QScrollBar::valueChanged, this, [this](int v) { m_wave->setView((double)v / kSteps, m_wave->v1 - m_wave->v0); });
        m_wave->setView(0, 1);

        auto* top = new QHBoxLayout();
        button(top, "▶ Play", "Play the selection, or the whole sample (Space)", [this] { play(); });
        button(top, "■ Stop", "Stop playing (Space)", [this] { m_app.stopPreview(); });
        button(top, "Undo", "Undo the last edit (Ctrl+Z)", [this] { undo(); });
        button(top, "Zoom out", "Show the whole sample", [this] { m_wave->setView(0, 1); });
        top->addStretch(1);
        lay->addLayout(top);
        lay->addWidget(m_wave, 1);
        lay->addWidget(scroll);
        auto* cuts = new QHBoxLayout();
        button(cuts, "Trim", "Keep only the selection", [this] { edit(sampleedit::trim, true); });
        button(cuts, "Delete", "Cut out the selection (Del)", [this] { edit(sampleedit::erase, true); });
        button(cuts, "Silence", "Silence the selection, or the whole sample", [this] { edit(sampleedit::silence, false); });
        button(cuts, "Fade in", "Fade in over the selection, or the whole sample",
            [this] { edit([](AudioAsset& a, int64_t f, int64_t t) { sampleedit::fade(a, f, t, true); }, false); });
        button(cuts, "Fade out", "Fade out over the selection, or the whole sample",
            [this] { edit([](AudioAsset& a, int64_t f, int64_t t) { sampleedit::fade(a, f, t, false); }, false); });
        button(cuts, "Trim silence", "Cut the silence (below -60 dB) at the start and the end",
            [this] { edit([](AudioAsset& a, int64_t, int64_t) { sampleedit::trimSilence(a, dbToGain(-60.f)); }, false); });
        lay->addLayout(cuts);
        auto* fx = new QHBoxLayout();
        button(
            fx, "Normalize", "Raise the peak to 0 dB: the selection, or the whole sample", [this] { edit(sampleedit::normalize, false); });
        button(fx, "Gain...", "Change the level of the selection, or the whole sample", [this] {
            bool ok = false;
            double db = QInputDialog::getDouble(this, "Gain", "dB:", 0, -60, 24, 1, &ok);
            if (ok) edit([db](AudioAsset& a, int64_t f, int64_t t) { sampleedit::gain(a, f, t, dbToGain((float)db)); }, false);
        });
        button(fx, "Reverse", "Reverse the selection, or the whole sample", [this] { edit(sampleedit::reverse, false); });
        button(
            fx, "Remove DC", "Remove the DC offset of the selection, or the whole sample", [this] { edit(sampleedit::removeDc, false); });
        lay->addLayout(fx);
        auto* bottom = new QHBoxLayout();
        bottom->addStretch(1);
        button(bottom, "Apply", "Save the result as a new file next to the project and use it instead of the original (undoable)",
            [this] { apply(); });
        button(bottom, "Close", "", [this] { reject(); });
        lay->addLayout(bottom);
    }
    void reject() override {
        if (m_asset != m_saved && QMessageBox::question(this, "Edit audio", "Close without applying the changes?") != QMessageBox::Yes)
            return;
        if (playing()) m_app.stopPreview();
        QDialog::reject();
    }

protected:
    static bool ownKey(QKeyEvent* e) { return e->matches(QKeySequence::Undo) || e->key() == Qt::Key_Delete || e->key() == Qt::Key_Space; }
    bool event(QEvent* e) override {   // the app's shortcuts are application-wide: claim the keys the editor uses before they fire
        if (e->type() == QEvent::ShortcutOverride && ownKey(static_cast<QKeyEvent*>(e))) {
            e->accept();
            return true;
        }
        return QDialog::event(e);
    }
    void keyPressEvent(QKeyEvent* e) override {
        if (e->matches(QKeySequence::Undo)) undo();
        else if (e->key() == Qt::Key_Delete) edit(sampleedit::erase, true);
        else if (e->key() == Qt::Key_Space) playing() ? m_app.stopPreview() : play();
        else QDialog::keyPressEvent(e);
    }

private:
    // Each edit works on a fresh copy, so the undo stack holds the previous versions and 'unapplied' is a pointer compare
    void edit(const std::function<void(AudioAsset&, int64_t, int64_t)>& op, bool needsSelection) {
        auto [from, to] = m_wave->selection();
        if (from == to) {
            if (needsSelection) return;
            from = 0, to = m_asset->frames;
        }
        m_undo.push_back(m_asset);
        if (m_undo.size() > 32) m_undo.erase(m_undo.begin());   // every step holds a whole copy of the sample
        m_asset = std::make_shared<AudioAsset>(*m_asset);
        op(*m_asset, from, to);
        m_asset->buildPeaks();
        shown(m_undo.back()->frames);
    }
    void undo() {
        if (m_undo.empty()) return;
        int64_t frames = m_asset->frames;
        m_asset = m_undo.back();
        m_undo.pop_back();
        shown(frames);
    }
    void shown(int64_t framesBefore) {
        if (m_asset->frames != framesBefore) {   // selection and view were fractions of the old length
            m_wave->selA = m_wave->selB = 0;
            m_wave->setView(0, 1);
        }
        m_wave->update();
    }
    bool playing() const { return m_preview && m_app.previewBuffer() == m_preview && m_app.engine().previewPlaying(); }
    void play() {
        auto [from, to] = m_wave->selection();
        if (from == to) from = 0, to = m_asset->frames;
        if (to <= from) return;
        auto b = std::make_shared<PreviewBuffer>();
        b->path = m_path;
        b->fileRate = m_asset->sampleRate;
        b->channels = m_asset->channels;
        b->frames = to - from;
        b->left.assign(m_asset->left.begin() + from, m_asset->left.begin() + to);
        if (b->channels > 1) b->right.assign(m_asset->right.begin() + from, m_asset->right.begin() + to);
        b->available.store(b->frames);
        b->done.store(true);
        m_app.stopPreview();
        m_preview = b;
        m_app.playPreview(b);
    }
    void apply() {
        const AudioAsset& a = *m_asset;
        if (a.frames <= 0) return;
        // like recordings: a folder next to the project, or in the Music folder while the project is unsaved
        const QString proj = m_app.model().filePath();
        QString dir = proj.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + "/Vybrant Edits"
                                     : QFileInfo(proj).absolutePath() + "/Edits";
        QDir().mkpath(dir);
        QString base = QFileInfo(m_path).completeBaseName(), out;
        for (int n = 1; QFileInfo::exists(out = QString("%1/%2 edit %3.wav").arg(dir, base).arg(n)); ++n) {}
        std::vector<float> inter((size_t)a.frames * a.channels);
        for (int64_t i = 0; i < a.frames; ++i)
            for (int c = 0; c < a.channels; ++c) inter[i * a.channels + c] = (c ? a.R() : a.L())[i];
        QString err;
        if (!AudioFile::write(out, inter.data(), a.channels, a.frames, a.sampleRate, 32, "wav", &err)) {
            QMessageBox::warning(this, "Edit audio", "Could not save " + out + ": " + err);
            return;
        }
        m_app.replaceSampleFile(m_source, m_channel, out);
        m_path = out;
        m_asset->path = out;
        m_saved = m_asset;
        setWindowTitle("Edit audio: " + QFileInfo(out).fileName());
        Q_EMIT m_app.statusMessage("Saved the edit as " + out, 4000);
    }

    App& m_app;
    Id m_source, m_channel;
    QString m_path;   // the file shown: the original, then the last applied edit
    std::shared_ptr<AudioAsset> m_asset, m_saved;
    std::vector<std::shared_ptr<AudioAsset>> m_undo;
    std::shared_ptr<PreviewBuffer> m_preview;
    EditWave* m_wave = nullptr;
};

void openAudioEditor(App& app, QWidget* parent, Id sourceId, Id channelId) {
    const AudioSource* s = app.project().audioSource(sourceId);
    const Channel* c = app.project().channel(channelId);
    QString path = s ? s->filePath : c ? c->samplePath : QString(), err;
    auto a = path.isEmpty() ? nullptr : AudioFile::load(path, 0, &err);   // at the file's own rate, so saving doesn't resample
    if (!a || a->frames <= 0) {
        QMessageBox::warning(parent, "Edit audio", "Cannot open " + path + (err.isEmpty() ? QString() : ": " + err));
        return;
    }
    auto* d = new AudioEditor(app, parent ? parent->window() : nullptr, s ? sourceId : 0, c ? channelId : 0, path, a);
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->show();
}

}   // namespace mt
