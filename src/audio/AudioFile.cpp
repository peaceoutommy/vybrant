#include "audio/AudioFile.h"
#include "core/Log.h"
#include <sndfile.h>
#include <samplerate.h>
#include <rubberband/RubberBandStretcher.h>
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
    for (auto& l : peaks) if (l.samplesPerBin <= samplesPerPixel) best = &l;
    return *best;
}

void AudioAsset::buildPeaks() {
    peaks.clear();
    for (int spb : {64, 512, 4096, 32768}) {
        WaveformLevel lv; lv.samplesPerBin = spb;
        int64_t bins = (frames + spb - 1) / spb;
        lv.minL.resize(bins); lv.maxL.resize(bins); lv.minR.resize(bins); lv.maxR.resize(bins);
        const float* l = L(); const float* r = R();
        for (int64_t b = 0; b < bins; ++b) {
            int64_t s = b * spb, e = std::min(frames, s + (int64_t)spb);
            float mnl = 1e9f, mxl = -1e9f, mnr = 1e9f, mxr = -1e9f;
            for (int64_t i = s; i < e; ++i) { mnl = std::min(mnl, l[i]); mxl = std::max(mxl, l[i]); mnr = std::min(mnr, r[i]); mxr = std::max(mxr, r[i]); }
            lv.minL[b] = mnl; lv.maxL[b] = mxl; lv.minR[b] = mnr; lv.maxR[b] = mxr;
        }
        peaks.push_back(std::move(lv));
    }
    float pk = 0.f; for (int64_t i = 0; i < frames; ++i) pk = std::max(pk, std::max(std::fabs(left[i]), std::fabs(R()[i])));
    peakValue = pk;
}

bool AudioFile::isAudioFile(const QString& path) {
    static const QStringList ext = {"wav", "aif", "aiff", "flac", "ogg", "mp3", "aifc", "w64", "caf", "au", "snd", "voc", "opus"};
    return ext.contains(QFileInfo(path).suffix().toLower());
}

bool AudioFile::probe(const QString& path, double* sr, int* channels, int64_t* frames) {
    SF_INFO info{}; SNDFILE* f = sf_open(path.toLocal8Bit().constData(), SFM_READ, &info);
    if (!f) return false;
    if (sr) *sr = info.samplerate; if (channels) *channels = info.channels; if (frames) *frames = info.frames;
    sf_close(f); return true;
}

std::shared_ptr<AudioAsset> AudioFile::load(const QString& path, double targetSampleRate, QString* error) {
    SF_INFO info{}; SNDFILE* f = sf_open(path.toLocal8Bit().constData(), SFM_READ, &info);
    if (!f) { if (error) *error = QString::fromUtf8(sf_strerror(nullptr)); return nullptr; }
    auto a = std::make_shared<AudioAsset>();
    a->path = path; a->originalSampleRate = info.samplerate; a->sampleRate = info.samplerate; a->channels = info.channels > 1 ? 2 : 1; a->frames = info.frames;
    std::vector<float> inter((size_t)info.frames * info.channels);
    sf_count_t got = sf_readf_float(f, inter.data(), info.frames);
    sf_close(f);
    a->frames = got;
    a->left.resize(got); if (a->channels > 1) a->right.resize(got);
    for (sf_count_t i = 0; i < got; ++i) {
        a->left[i] = inter[i * info.channels];
        if (a->channels > 1) a->right[i] = inter[i * info.channels + 1];
    }
    if (targetSampleRate > 0 && std::fabs(targetSampleRate - a->sampleRate) > 0.5) a = resample(*a, targetSampleRate);
    a->buildPeaks();
    return a;
}

std::shared_ptr<AudioAsset> AudioFile::resample(const AudioAsset& in, double targetRate) {
    auto out = std::make_shared<AudioAsset>(); out->path = in.path; out->originalSampleRate = in.originalSampleRate; out->sampleRate = targetRate; out->channels = in.channels;
    double ratio = targetRate / in.sampleRate;
    int64_t outFrames = (int64_t)std::ceil(in.frames * ratio) + 16;
    auto doChannel = [&](const std::vector<float>& src, std::vector<float>& dst) {
        dst.assign(outFrames, 0.f); SRC_DATA d{}; d.data_in = src.data(); d.input_frames = in.frames; d.data_out = dst.data(); d.output_frames = outFrames; d.src_ratio = ratio;
        if (src_simple(&d, SRC_SINC_MEDIUM_QUALITY, 1) != 0) { dst = src; return (int64_t)src.size(); }
        dst.resize(d.output_frames_gen); return (int64_t)d.output_frames_gen;
    };
    out->frames = doChannel(in.left, out->left);
    if (in.channels > 1) { int64_t f = doChannel(in.right, out->right); out->frames = std::min(out->frames, f); out->left.resize(out->frames); out->right.resize(out->frames); }
    return out;
}

std::shared_ptr<AudioAsset> AudioFile::stretch(const AudioAsset& in, double timeRatio, double pitchSemitones) {
    using namespace RubberBand;
    auto out = std::make_shared<AudioAsset>(); out->path = in.path; out->sampleRate = in.sampleRate; out->originalSampleRate = in.originalSampleRate; out->channels = in.channels;
    RubberBandStretcher rb((size_t)in.sampleRate, in.channels, RubberBandStretcher::OptionProcessOffline | RubberBandStretcher::OptionEngineFiner | RubberBandStretcher::OptionPitchHighQuality, timeRatio, std::pow(2.0, pitchSemitones / 12.0));
    rb.setExpectedInputDuration((size_t)in.frames);
    const float* chans[2] = {in.L(), in.R()};
    rb.study(chans, (size_t)in.frames, true);
    size_t pos = 0; const size_t block = 8192;
    std::vector<float> ol, orr;
    std::vector<float> tmpL(block), tmpR(block);
    while (pos < (size_t)in.frames) {
        size_t n = std::min(block, (size_t)in.frames - pos);
        const float* c[2] = {chans[0] + pos, chans[1] + pos};
        rb.process(c, n, pos + n >= (size_t)in.frames);
        pos += n;
        int avail;
        while ((avail = rb.available()) > 0) {
            size_t take = std::min((size_t)avail, block); float* o[2] = {tmpL.data(), tmpR.data()};
            size_t got = rb.retrieve(o, take);
            ol.insert(ol.end(), tmpL.begin(), tmpL.begin() + got);
            if (in.channels > 1) orr.insert(orr.end(), tmpR.begin(), tmpR.begin() + got);
        }
    }
    int avail;
    while ((avail = rb.available()) > 0) {
        size_t take = std::min((size_t)avail, block); float* o[2] = {tmpL.data(), tmpR.data()};
        size_t got = rb.retrieve(o, take);
        ol.insert(ol.end(), tmpL.begin(), tmpL.begin() + got);
        if (in.channels > 1) orr.insert(orr.end(), tmpR.begin(), tmpR.begin() + got);
    }
    out->left = std::move(ol); out->right = std::move(orr); out->frames = (int64_t)out->left.size();
    out->buildPeaks();
    return out;
}

bool AudioFile::write(const QString& path, const float* inter, int channels, int64_t frames, double sampleRate, int bitDepth, const QString& format, QString* error) {
    QString fmt = format.toLower();
    if (fmt == "mp3") {
        lame_t gf = lame_init();
        lame_set_in_samplerate(gf, (int)sampleRate); lame_set_num_channels(gf, channels); lame_set_mode(gf, channels == 1 ? MONO : JOINT_STEREO); lame_set_quality(gf, 2); lame_set_brate(gf, 320);
        if (lame_init_params(gf) < 0) { if (error) *error = "LAME init failed"; lame_close(gf); return false; }
        QFile f(path); if (!f.open(QIODevice::WriteOnly)) { if (error) *error = f.errorString(); lame_close(gf); return false; }
        std::vector<unsigned char> mp3(1024 * 1024);
        const int chunk = 8192; std::vector<float> l(chunk), r(chunk);
        for (int64_t pos = 0; pos < frames; pos += chunk) {
            int n = (int)std::min<int64_t>(chunk, frames - pos);
            for (int i = 0; i < n; ++i) { l[i] = inter[(pos + i) * channels]; r[i] = channels > 1 ? inter[(pos + i) * channels + 1] : l[i]; }
            int got = lame_encode_buffer_ieee_float(gf, l.data(), r.data(), n, mp3.data(), (int)mp3.size());
            if (got > 0) f.write((const char*)mp3.data(), got);
        }
        int got = lame_encode_flush(gf, mp3.data(), (int)mp3.size()); if (got > 0) f.write((const char*)mp3.data(), got);
        lame_close(gf); return true;
    }
    SF_INFO info{}; info.samplerate = (int)sampleRate; info.channels = channels;
    int major = SF_FORMAT_WAV; if (fmt == "aiff" || fmt == "aif") major = SF_FORMAT_AIFF; else if (fmt == "flac") major = SF_FORMAT_FLAC; else if (fmt == "ogg") major = SF_FORMAT_OGG;
    int sub = SF_FORMAT_PCM_24;
    if (major == SF_FORMAT_OGG) sub = SF_FORMAT_VORBIS; else if (bitDepth == 16) sub = SF_FORMAT_PCM_16; else if (bitDepth == 32) sub = (major == SF_FORMAT_FLAC) ? SF_FORMAT_PCM_24 : SF_FORMAT_FLOAT; else sub = SF_FORMAT_PCM_24;
    info.format = major | sub;
    SNDFILE* f = sf_open(path.toLocal8Bit().constData(), SFM_WRITE, &info);
    if (!f) { if (error) *error = QString::fromUtf8(sf_strerror(nullptr)); return false; }
    sf_command(f, SFC_SET_CLIPPING, nullptr, SF_TRUE);
    sf_writef_float(f, inter, frames);
    sf_close(f);
    return true;
}

// ---------------- SampleCache ----------------
SampleCache::SampleCache(QObject* parent) : QObject(parent) {}
SampleCache::~SampleCache() { QThreadPool::globalInstance()->waitForDone(5000); }
void SampleCache::setSampleRate(double sr) { if (sr != m_sr) { m_sr = sr; clear(); } }
QString SampleCache::keyFor(const QString& path) const { return path; }

AudioAssetPtr SampleCache::get(const QString& path) {
    if (path.isEmpty()) return nullptr;
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_assets.find(path); if (it != m_assets.end()) return it->second;
    if (m_loading[path]) return nullptr;
    m_loading[path] = true; m_pending++;
    double sr = m_sr;
    QThreadPool::globalInstance()->start([this, path, sr] {
        QString err; auto a = AudioFile::load(path, sr, &err);
        {
            std::lock_guard<std::mutex> lk2(m_mutex);
            if (a) m_assets[path] = a;
            m_loading[path] = false;
        }
        m_pending--;
        if (a) QMetaObject::invokeMethod(this, [this, path] { Q_EMIT assetLoaded(path); }, Qt::QueuedConnection);
        else QMetaObject::invokeMethod(this, [this, path, err] { Q_EMIT loadFailed(path, err); }, Qt::QueuedConnection);
    });
    return nullptr;
}

AudioAssetPtr SampleCache::getSync(const QString& path) {
    if (path.isEmpty()) return nullptr;
    { std::lock_guard<std::mutex> lk(m_mutex); auto it = m_assets.find(path); if (it != m_assets.end()) return it->second; }
    QString err; auto a = AudioFile::load(path, m_sr, &err);
    if (!a) { Log::warn("Failed to load " + path + ": " + err); return nullptr; }
    std::lock_guard<std::mutex> lk(m_mutex); m_assets[path] = a; return a;
}

AudioAssetPtr SampleCache::getStretched(const QString& path, double ratio, double pitch) {
    QString key = QString("%1|stretch|%2|%3").arg(path).arg(ratio, 0, 'f', 5).arg(pitch, 0, 'f', 3);
    AudioAssetPtr base;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_assets.find(key); if (it != m_assets.end()) return it->second;
        auto b = m_assets.find(path); if (b == m_assets.end()) { base = nullptr; } else base = b->second;
        if (!base) return nullptr;
        if (m_loading[key]) return nullptr;
        m_loading[key] = true; m_pending++;
    }
    QThreadPool::globalInstance()->start([this, key, base, ratio, pitch, path] {
        auto a = AudioFile::stretch(*base, ratio, pitch);
        { std::lock_guard<std::mutex> lk2(m_mutex); m_assets[key] = a; m_loading[key] = false; }
        m_pending--;
        QMetaObject::invokeMethod(this, [this, path] { Q_EMIT assetLoaded(path); }, Qt::QueuedConnection);
    });
    return nullptr;
}

bool SampleCache::isLoading(const QString& path) const { std::lock_guard<std::mutex> lk(m_mutex); auto it = m_loading.find(path); return it != m_loading.end() && it->second; }
void SampleCache::clear() { std::lock_guard<std::mutex> lk(m_mutex); m_assets.clear(); }

} // namespace mt
