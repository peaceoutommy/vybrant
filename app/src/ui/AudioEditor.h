// Sample editor (after FL Studio's Edison): destructive edits on a copy of a sample in memory. Apply writes a new WAV next to the
// project and repoints the sample to it as one undo step, so the original file is never overwritten.
#pragma once
#include "audio/AudioFile.h"
#include "core/Types.h"

class QWidget;

namespace mt {

class App;

// The edit operations on frames [from, to) of an asset's sample data (peaks are not rebuilt)
namespace sampleedit {
void trim(AudioAsset& a, int64_t from, int64_t to);   // keep only [from, to)
void erase(AudioAsset& a, int64_t from, int64_t to);
void silence(AudioAsset& a, int64_t from, int64_t to);
void fade(AudioAsset& a, int64_t from, int64_t to, bool in);   // linear, 0 at the outer end
void gain(AudioAsset& a, int64_t from, int64_t to, float g);
void normalize(AudioAsset& a, int64_t from, int64_t to);   // peak to 0 dB
void reverse(AudioAsset& a, int64_t from, int64_t to);
void removeDc(AudioAsset& a, int64_t from, int64_t to);
void trimSilence(AudioAsset& a, float threshold);   // cut what stays below threshold at the start and the end
}   // namespace sampleedit

// Opens the editor on an audio source's file, or (sourceId 0) on a sampler channel's sample
void openAudioEditor(App& app, QWidget* parent, Id sourceId, Id channelId);

}   // namespace mt
