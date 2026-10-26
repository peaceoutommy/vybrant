// Offline render/export. Drives the same engine graph used for playback with the audio device closed.
#pragma once
#include "core/Types.h"
#include "project/Project.h"
#include <QString>
#include <functional>

namespace mt {
class AudioEngine;

struct RenderOptions {
    QString path;
    QString format = "wav";
    double sampleRate = 44100;
    int bitDepth = 24;
    bool mono = false;
    bool normalize = false;
    bool dither = true;
    double tailSeconds = 2.0;
    bool playTail = false;   // the transport runs on through the tail instead of stopping (consolidate: notes end as in the song)
    Tick start = 0;
    Tick end = 0;
    bool songMode = true;
    int blockSize = 512;
    float normalizeTargetDb = -0.3f;
    Id tapTrack = 0;   // render only this mixer track's output (post effects and fader) instead of the master mix
    bool tapPreFader = false;   // the tap before the track's fader and pan (freeze)
    bool tapInput = false;   // the tap at the track's input, before its effects (consolidate: what reaches the master)
    bool stems = false;   // App::exportAudio: one file per used mixer track (master included), named after 'path'
};

class RenderEngine {
public:
    // Blocks the calling (UI) thread; 'progress' receives 0..1 and may return false to cancel.
    static bool render(AudioEngine& engine, const Project& project, const RenderOptions& opt, std::function<bool(double)> progress,
        QString* error = nullptr);
};

}   // namespace mt
