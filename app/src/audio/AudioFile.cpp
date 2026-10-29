#include <QRegularExpression>
#include "audio/AudioFile.h"
#include "core/Log.h"
#include <sndfile.h>
#include <samplerate.h>
#include <signalsmith-stretch/signalsmith-stretch.h>
#include <lame/lame.h>
#include <QFileInfo>
#include <QThreadPool>
#include <QRunnable>
#include <QMetaObject>
#include <QFile>
#include <cmath>
#include <algorithm>

namespace mt {

const WaveformLevel& AudioAsset::levelFor(double samplesPerPixel) const {
    const WaveformLevel* best = &peaks.front();
    for (auto& l : peaks)
        if (l.samplesPerBin <= samplesPerPixel) best = &l;
    return *best;
}

void AudioAsset::buildPeaks() {
    peaks.clear();
    for (int spb : {64, 512, 4096, 32768}) {
        WaveformLevel lv;
        lv.samplesPerBin = spb;
        int64_t bins = (frames + spb - 1) / spb;
        lv.minL.resize(bins);
        lv.maxL.resize(bins);
        lv.minR.resize(bins);
        lv.maxR.resize(bins);
        const float* l = L();
        const float* r = R();
        for (int64_t b = 0; b < bins; ++b) {
            int64_t s = b * spb, e = std::min(frames, s + (int64_t)spb);
            float mnl = 1e9f, mxl = -1e9f, mnr = 1e9f, mxr = -1e9f;
            for (int64_t i = s; i < e; ++i) {
                mnl = std::min(mnl, l[i]);
                mxl = std::max(mxl, l[i]);
                mnr = std::min(mnr, r[i]);
                mxr = std::max(mxr, r[i]);
            }
            lv.minL[b] = mnl;
            lv.maxL[b] = mxl;
            lv.minR[b] = mnr;
            lv.maxR[b] = mxr;
        }
        peaks.push_back(std::move(lv));
    }
    float pk = 0.f;
    for (int64_t i = 0; i < frames; ++i) pk = std::max(pk, std::max(std::fabs(left[i]), std::fabs(R()[i])));
    peakValue = pk;
}

bool AudioFile::isAudioFile(const QString& path) {
    static const QStringList ext = {"wav", "aif", "aiff", "flac", "ogg", "mp3", "aifc", "w64", "caf", "au", "snd", "voc", "opus"};
    return ext.contains(QFileInfo(path).suffix().toLower());
}

bool AudioFile::probe(const QString& path, double* sr, int* channels, int64_t* frames) {
    SF_INFO info{};
    SNDFILE* f = sf_open(path.toLocal8Bit().constData(), SFM_READ, &info);
    if (!f) return false;
    if (sr) *sr = info.samplerate;
    if (channels) *channels = info.channels;
    if (frames) *frames = info.frames;
    sf_close(f);
    return true;
}

std::shared_ptr<AudioAsset> AudioFile::load(const QString& path, double targetSampleRate, QString* error) {
    SF_INFO info{};
    SNDFILE* f = sf_open(path.toLocal8Bit().constData(), SFM_READ, &info);
    if (!f) {
        if (error) *error = QString::fromUtf8(sf_strerror(nullptr));
        return nullptr;
    }
    auto a = std::make_shared<AudioAsset>();
    a->path = path;
    a->originalSampleRate = info.samplerate;
    a->sampleRate = info.samplerate;
    a->channels = info.channels > 1 ? 2 : 1;
    a->frames = info.frames;
    std::vector<float> inter((size_t)info.frames * info.channels);
    sf_count_t got = sf_readf_float(f, inter.data(), info.frames);
    sf_close(f);
    a->frames = got;
    a->left.resize(got);
    if (a->channels > 1) a->right.resize(got);
    for (sf_count_t i = 0; i < got; ++i) {
        a->left[i] = inter[i * info.channels];
        if (a->channels > 1) a->right[i] = inter[i * info.channels + 1];
    }
    if (targetSampleRate > 0 && std::fabs(targetSampleRate - a->sampleRate) > 0.5) a = resample(*a, targetSampleRate);
    a->buildPeaks();
    return a;
}

std::shared_ptr<AudioAsset> AudioFile::resample(const AudioAsset& in, double targetRate) {
    auto out = std::make_shared<AudioAsset>();
    out->path = in.path;
    out->originalSampleRate = in.originalSampleRate;
    out->sampleRate = targetRate;
    out->channels = in.channels;
    double ratio = targetRate / in.sampleRate;
    int64_t outFrames = (int64_t)std::ceil(in.frames * ratio) + 16;
    auto doChannel = [&](const std::vector<float>& src, std::vector<float>& dst) {
        dst.assign(outFrames, 0.f);
        SRC_DATA d{};
        d.data_in = src.data();
        d.input_frames = in.frames;
        d.data_out = dst.data();
        d.output_frames = outFrames;
        d.src_ratio = ratio;
        if (src_simple(&d, SRC_SINC_FASTEST, 1) != 0) {
            dst = src;
            return (int64_t)src.size();
        }
        dst.resize(d.output_frames_gen);
        return (int64_t)d.output_frames_gen;
    };
    out->frames = doChannel(in.left, out->left);
    if (in.channels > 1) {
        int64_t f = doChannel(in.right, out->right);
        out->frames = std::min(out->frames, f);
        out->left.resize(out->frames);
        out->right.resize(out->frames);
    }
    return out;
}

double AudioFile::bpmFromName(const QString& path) {
    static const QRegularExpression re("(\\d{2,3}(?:[.,]\\d+)?)\\s*-?\\s*bpm", QRegularExpression::CaseInsensitiveOption);
    auto m = re.match(QFileInfo(path).completeBaseName());
    double v = m.hasMatch() ? m.captured(1).replace(',', '.').toDouble() : 0.0;
    return v >= 40 && v <= 300 ? v : 0.0;
}
// Signalsmith Stretch (MIT, app/third_party): the whole clip in one pass at an exact output length. The block / interval
// sizes trade time resolution (short: crisp hits) against frequency resolution (long: smooth sustained sounds).
std::shared_ptr<AudioAsset> AudioFile::stretch(const AudioAsset& in, double timeRatio, double pitchSemitones, int mode) {
    auto out = std::make_shared<AudioAsset>();
    out->path = in.path;
    out->sampleRate = in.sampleRate;
    out->originalSampleRate = in.originalSampleRate;
    out->channels = in.channels;
    const int ch = in.channels > 1 ? 2 : 1;
    const float sr = (float)in.sampleRate;
    signalsmith::stretch::SignalsmithStretch<float> st(0x5eed);   // a fixed seed: the same clip always renders the same
    switch (mode) {
    case StretchDrums: st.configure(ch, int(sr * 0.06f), int(sr * 0.015f)); break;   // short blocks: hits stay sharp
    case StretchPads: st.configure(ch, int(sr * 0.2f), int(sr * 0.05f)); break;   // long blocks: smooth, no flamming on held notes
    default: st.presetDefault(ch, sr); break;
    }
    st.setTransposeSemitones((float)pitchSemitones, 8000 / sr);   // above ~8 kHz the timbre is kept rather than transposed
    if (mode == StretchVocals) st.setFormantFactor(1, true);   // formants stay put while the pitch moves
    const int64_t want = std::max<int64_t>(1, std::llround(in.frames * timeRatio));
    // a clip shorter than the stretcher's look-ahead is padded with silence, and the extra output cut off again
    int64_t inLen = std::max<int64_t>(in.frames, st.outputSeekLength(float(in.frames) / want) + 1);
    std::vector<float> padL, padR;
    const float* src[2] = {in.L(), in.R()};
    if (inLen > in.frames) {
        padL.assign(in.L(), in.L() + in.frames), padL.resize(inLen);
        padR.assign(in.R(), in.R() + in.frames), padR.resize(inLen);
        src[0] = padL.data(), src[1] = padR.data();
    }
    const int64_t outLen = inLen == in.frames ? want : std::llround(inLen * timeRatio);
    out->left.resize(outLen);
    if (ch > 1) out->right.resize(outLen);
    float* dst[2] = {out->left.data(), ch > 1 ? out->right.data() : nullptr};
    st.exact(src, (int)inLen, dst, (int)outLen);
    out->left.resize(want);
    if (ch > 1) out->right.resize(want);
    out->frames = want;
    out->buildPeaks();
    return out;
}

bool AudioFile::write(const QString& path, const float* inter, int channels, int64_t frames, double sampleRate, int bitDepth,
    const QString& format, QString* error) {
    QString fmt = format.toLower();
    if (fmt == "mp3") {
        lame_t gf = lame_init();
        lame_set_in_samplerate(gf, (int)sampleRate);
        lame_set_num_channels(gf, channels);
        lame_set_mode(gf, channels == 1 ? MONO : JOINT_STEREO);
        lame_set_quality(gf, 2);
        lame_set_brate(gf, 320);
        if (lame_init_params(gf) < 0) {
            if (error) *error = "LAME init failed";
            lame_close(gf);
            return false;
        }
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) {
            if (error) *error = f.errorString();
            lame_close(gf);
            return false;
        }
        std::vector<unsigned char> mp3(1024 * 1024);
        const int chunk = 8192;
        std::vector<float> l(chunk), r(chunk);
        for (int64_t pos = 0; pos < frames; pos += chunk) {
            int n = (int)std::min<int64_t>(chunk, frames - pos);
            for (int i = 0; i < n; ++i) {
                l[i] = inter[(pos + i) * channels];
                r[i] = channels > 1 ? inter[(pos + i) * channels + 1] : l[i];
            }
            int got = lame_encode_buffer_ieee_float(gf, l.data(), r.data(), n, mp3.data(), (int)mp3.size());
            if (got > 0) f.write((const char*)mp3.data(), got);
        }
        int got = lame_encode_flush(gf, mp3.data(), (int)mp3.size());
        if (got > 0) f.write((const char*)mp3.data(), got);
        lame_close(gf);
        return true;
    }
    SF_INFO info{};
    info.samplerate = (int)sampleRate;
    info.channels = channels;
    int major = SF_FORMAT_WAV;
    if (fmt == "aiff" || fmt == "aif") major = SF_FORMAT_AIFF;
    else if (fmt == "flac") major = SF_FORMAT_FLAC;
    else if (fmt == "ogg") major = SF_FORMAT_OGG;
    int sub = SF_FORMAT_PCM_24;
    if (major == SF_FORMAT_OGG) sub = SF_FORMAT_VORBIS;
    else if (bitDepth == 16) sub = SF_FORMAT_PCM_16;
    else if (bitDepth == 32) sub = (major == SF_FORMAT_FLAC) ? SF_FORMAT_PCM_24 : SF_FORMAT_FLOAT;
    else sub = SF_FORMAT_PCM_24;
    info.format = major | sub;
    SNDFILE* f = sf_open(path.toLocal8Bit().constData(), SFM_WRITE, &info);
    if (!f) {
        if (error) *error = QString::fromUtf8(sf_strerror(nullptr));
        return false;
    }
    sf_command(f, SFC_SET_CLIPPING, nullptr, SF_TRUE);
    sf_writef_float(f, inter, frames);
    sf_close(f);
    return true;
}

// ---------------- SampleCache ----------------
SampleCache::SampleCache(QObject* parent)
    : QObject(parent) {}
SampleCache::~SampleCache() { QThreadPool::globalInstance()->waitForDone(5000); }
void SampleCache::setSampleRate(double sr) {
    if (sr != m_sr) {
        m_sr = sr;
        clear();
    }
}
QString SampleCache::keyFor(const QString& path) const { return path; }

AudioAssetPtr SampleCache::get(const QString& path) {
    if (path.isEmpty()) return nullptr;
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_assets.find(path);
    if (it != m_assets.end()) return it->second;
    if (m_loading[path]) return nullptr;
    m_loading[path] = true;
    m_pending++;
    double sr = m_sr;
    QThreadPool::globalInstance()->start([this, path, sr] {
        QString err;
        auto a = AudioFile::load(path, sr, &err);
        {
            std::lock_guard<std::mutex> lk2(m_mutex);
            if (a) m_assets[path] = a;
            m_loading[path] = false;
        }
        m_pending--;
        if (a) QMetaObject::invokeMethod(this, [this, path] { Q_EMIT assetLoaded(path); }, Qt::QueuedConnection);
    });
    return nullptr;
}

AudioAssetPtr SampleCache::getSync(const QString& path) {
    if (path.isEmpty()) return nullptr;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_assets.find(path);
        if (it != m_assets.end()) return it->second;
    }
    QString err;
    auto a = AudioFile::load(path, m_sr, &err);
    if (!a) {
        Log::warn("Failed to load " + path + ": " + err);
        return nullptr;
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    m_assets[path] = a;
    return a;
}

AudioAssetPtr SampleCache::getStretched(const QString& path, double ratio, double pitch, int mode) {
    QString key = QString("%1|stretch|%2|%3|%4").arg(path).arg(ratio, 0, 'f', 5).arg(pitch, 0, 'f', 3).arg(mode);
    AudioAssetPtr base;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_assets.find(key);
        if (it != m_assets.end()) return it->second;
        auto b = m_assets.find(path);
        if (b == m_assets.end()) {
            base = nullptr;
        } else base = b->second;
        if (!base) return nullptr;
        const QString prefix = path + "|stretch|";
        for (auto& [k, busy] : m_loading)
            if (busy && k.startsWith(prefix)) return nullptr;   // one render per file at a time (see header)
        m_loading[key] = true;
        m_pending++;
    }
    QThreadPool::globalInstance()->start([this, key, base, ratio, pitch, path, mode] {
        auto a = AudioFile::stretch(*base, ratio, pitch, mode);
        {
            std::lock_guard<std::mutex> lk2(m_mutex);
            m_assets[key] = a;
            m_loading[key] = false;
        }
        m_pending--;
        QMetaObject::invokeMethod(this, [this, path] { Q_EMIT assetLoaded(path); }, Qt::QueuedConnection);
    });
    return nullptr;
}

void SampleCache::clear() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_assets.clear();
}

}   // namespace mt
