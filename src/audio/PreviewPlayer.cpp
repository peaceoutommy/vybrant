#include "audio/PreviewPlayer.h"
#include <sndfile.h>
#include <algorithm>
#include <cmath>

namespace mt {

void PreviewLoader::cancel() { if (m_current) m_current->cancel.store(true); m_current.reset(); }   // the thread notices the flag between chunks and exits on its own

std::shared_ptr<PreviewBuffer> PreviewLoader::load(const QString& path, QString* error) {
    cancel(); (void)error;
    auto b = std::make_shared<PreviewBuffer>(); b->path = path; b->minPk.assign(PreviewBuffer::kBins, 0.f); b->maxPk.assign(PreviewBuffer::kBins, 0.f); b->createdUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    m_current = b;
    std::thread([b, path] {
        SF_INFO info{}; SNDFILE* f = sf_open(path.toLocal8Bit().constData(), SFM_READ, &info);
        if (!f || info.frames <= 0) { if (f) sf_close(f); b->failed.store(true); b->done.store(true); return; }
        b->fileRate = info.samplerate; b->channels = info.channels > 1 ? 2 : 1; b->frames = info.frames;
        b->left.assign(info.frames, 0.f); if (b->channels > 1) b->right.assign(info.frames, 0.f);
        const int chunk = 8192; std::vector<float> inter((size_t)chunk * info.channels); int64_t pos = 0; int64_t framesPerBin = std::max<int64_t>(1, (info.frames + PreviewBuffer::kBins - 1) / PreviewBuffer::kBins);
        while (pos < info.frames && !b->cancel.load()) {
            sf_count_t got = sf_readf_float(f, inter.data(), chunk); if (got <= 0) break;
            for (sf_count_t i = 0; i < got; ++i) { b->left[pos + i] = inter[i * info.channels]; if (b->channels > 1) b->right[pos + i] = inter[i * info.channels + 1]; }
            // progressive waveform bins
            int64_t end = pos + got; for (int64_t bin = pos / framesPerBin; bin < (end + framesPerBin - 1) / framesPerBin && bin < PreviewBuffer::kBins; ++bin) { int64_t s = std::max(pos, bin * framesPerBin), e = std::min(end, (bin + 1) * framesPerBin); float mn = b->minPk[bin], mx = b->maxPk[bin]; for (int64_t k = s; k < e; ++k) { float v = std::max(std::fabs(b->left[k]), b->channels > 1 ? std::fabs(b->right[k]) : 0.f); mx = std::max(mx, v); mn = std::min(mn, -v); } b->minPk[bin] = mn; b->maxPk[bin] = mx; b->binsReady.store((int)std::min<int64_t>(PreviewBuffer::kBins, end / framesPerBin)); }
            pos = end; b->available.store(pos, std::memory_order_release);
        }
        if (pos < info.frames && !b->cancel.load()) b->frames = pos;   // short read: play what we have
        b->done.store(true); sf_close(f);
    }).detach();
    return b;
}

} // namespace mt
