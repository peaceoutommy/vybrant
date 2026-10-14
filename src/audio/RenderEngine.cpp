#include "audio/RenderEngine.h"
#include "audio/AudioEngine.h"
#include "audio/AudioFile.h"
#include "dsp/DspUtils.h"
#include "core/Log.h"
#include <QCoreApplication>
#include <vector>

namespace mt {

bool RenderEngine::render(
    AudioEngine& engine, const Project& project, const RenderOptions& opt, std::function<bool(double)> progress, QString* error) {
    if (opt.path.isEmpty()) {
        if (error) *error = "No output path";
        return false;
    }
    Tick start = opt.start, end = opt.end;
    if (end <= start) {
        end = opt.songMode ? std::max<Tick>(project.songLength(), PPQ * 4) : PPQ * 4;
        if (!opt.songMode) {
            if (auto* pat = project.pattern(project.selectedPatternId)) end = pat->effectiveLength(project.transport.timeSig);
        }
    }
    const bool wasRunning = engine.deviceRunning();
    engine.offlineBegin(opt.sampleRate, opt.blockSize);
    engine.setSongMode(opt.songMode);
    engine.setLoop(false, 0, 0);
    engine.setMetronome(false);
    engine.setCountIn(false, 1);
    engine.setRecording(false);
    engine.seek(start);
    engine.play();
    double sr = opt.sampleRate;
    TempoMap tempo = project.transport.tempoMap();
    int64_t totalFrames = (int64_t)((tempo.secondsAt(end) - tempo.secondsAt(start)) * sr);
    int64_t tailFrames = (int64_t)(opt.tailSeconds * sr);
    std::vector<float> L(opt.blockSize), R(opt.blockSize);
    std::vector<float> outL, outR;
    outL.reserve(totalFrames + tailFrames);
    outR.reserve(totalFrames + tailFrames);
    int64_t done = 0;
    bool cancelled = false;
    int64_t silentRun = 0;
    const int64_t silentNeeded = (int64_t)(sr * 0.1);
    // Priming: process one silent block so parameter/automation state settles at 'start'
    while (done < totalFrames + tailFrames) {
        int n = (int)std::min<int64_t>(opt.blockSize, totalFrames + tailFrames - done);
        std::fill(L.begin(), L.end(), 0.f);
        std::fill(R.begin(), R.end(), 0.f);
        if (done >= totalFrames && engine.isPlaying()) engine.stop();   // tail: let effects ring out
        engine.offlineProcess(L.data(), R.data(), n);
        float pk = 0;
        for (int i = 0; i < n; ++i) {
            outL.push_back(L[i]);
            outR.push_back(R[i]);
            pk = std::max(pk, std::max(std::fabs(L[i]), std::fabs(R[i])));
        }
        done += n;
        if (done >= totalFrames) {
            if (pk < 3e-5f) silentRun += n;
            else silentRun = 0;
            if (silentRun >= silentNeeded) break;
        }
        if ((done / opt.blockSize) % 32 == 0) {
            if (progress && !progress((double)done / (totalFrames + tailFrames))) {
                cancelled = true;
                break;
            }
            QCoreApplication::processEvents();
        }
    }
    engine.stop();
    engine.offlineEnd();
    if (!wasRunning) engine.stopDevice();
    if (cancelled) {
        if (error) *error = "Cancelled";
        return false;
    }
    int64_t frames = (int64_t)outL.size();
    // normalise
    if (opt.normalize) {
        float pk = 0;
        for (int64_t i = 0; i < frames; ++i) pk = std::max(pk, std::max(std::fabs(outL[i]), std::fabs(outR[i])));
        if (pk > 0) {
            float g = dbToGain(opt.normalizeTargetDb) / pk;
            for (int64_t i = 0; i < frames; ++i) {
                outL[i] *= g;
                outR[i] *= g;
            }
        }
    }
    // dither (TPDF at the target bit depth) for fixed-point formats
    int channels = opt.mono ? 1 : 2;
    std::vector<float> inter((size_t)frames * channels);
    dsp::Rng rng;
    bool doDither = opt.dither && (opt.bitDepth == 16 || opt.bitDepth == 24) && opt.format != "mp3" && opt.format != "ogg";
    float lsb = 1.f / (float)(1 << (opt.bitDepth - 1));
    for (int64_t i = 0; i < frames; ++i) {
        float l = outL[i], r = outR[i];
        if (doDither) {
            float d = (rng.uniform() - rng.uniform()) * lsb;
            l += d;
            r += (rng.uniform() - rng.uniform()) * lsb;
        }
        if (channels == 1) inter[i] = clamp((l + r) * 0.5f, -1.f, 1.f);
        else {
            inter[i * 2] = clamp(l, -1.f, 1.f);
            inter[i * 2 + 1] = clamp(r, -1.f, 1.f);
        }
    }
    if (!AudioFile::write(opt.path, inter.data(), channels, frames, sr, opt.bitDepth, opt.format, error)) return false;
    Log::info(QString("Rendered %1 (%2 frames)").arg(opt.path).arg(frames));
    if (progress) progress(1.0);
    return true;
}

}   // namespace mt
