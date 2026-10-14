// Audio recording: RT thread writes input samples into per-track lock-free rings; a writer thread drains them to
// WAV files, one continuous file per armed track per recording pass. The UI splits a pass into takes afterwards
// (AudioEngine::recordSegment says where each loop pass / punch-in starts in the file).
#pragma once
#include "core/RingBuffer.h"
#include "core/Types.h"
#include <QObject>
#include <QString>
#include <map>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

namespace mt {

struct RecordedFile {
    Id mixerTrackId = 0;
    QString filePath;
    int64_t frames = 0;
    int channels = 1;
    double sampleRate = 44100;
};

class RecordingEngine : public QObject {
    Q_OBJECT
public:
    explicit RecordingEngine(QObject* parent = nullptr);
    ~RecordingEngine();
    // Persistent ring for a mixer track (created on the UI thread; referenced by snapshots)
    FloatRing* ringFor(Id mixerTrackId, int channels);
    void setSampleRate(double sr) { m_sr = sr; }
    void setRecordDirectory(const QString& dir) { m_dir = dir; }
    QString recordDirectory() const { return m_dir; }
    // Begin writing files for the given armed tracks. Call before the engine starts recording: it empties the rings.
    void start(const std::vector<std::pair<Id, int>>& armedTracks);
    std::vector<RecordedFile> stop();   // finalise, returns the files
    bool isActive() const { return m_active.load(); }

private:
    struct Writer {
        Id trackId;
        int channels;
        FloatRing* ring;
        void* sndfile = nullptr;
        QString path;
        int64_t frames = 0;
        double sr = 44100;
    };
    void writerLoop();
    void flushWriter(Writer& w, bool all);
    std::map<Id, std::unique_ptr<FloatRing>> m_rings;
    std::map<Id, int> m_ringChannels;
    std::vector<Writer> m_writers;
    std::mutex m_mutex;
    std::thread m_thread;
    std::atomic<bool> m_active{false}, m_running{false};
    double m_sr = 44100;
    QString m_dir;
    int m_takeCounter = 0;
    std::vector<float> m_tmp;
};

}   // namespace mt
