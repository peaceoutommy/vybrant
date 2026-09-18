// Audio recording: RT thread writes input samples into per-track lock-free rings; a writer thread drains them to
// WAV files. Takes are finalised on stop (or per loop pass when loop-recording) and turned into audio clips.
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

struct RecordedTake {
    Id mixerTrackId = 0;
    QString filePath;
    Tick startTick = 0;
    int64_t frames = 0;
    int channels = 1;
    int takeIndex = 0;
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
    // Begin writing files for the given armed tracks
    void start(const std::vector<std::pair<Id, int>>& armedTracks, Tick startTick);
    void newTake(Tick startTick);   // finalise current files and begin a new pass (loop recording)
    std::vector<RecordedTake> stop();   // finalise, returns takes
    bool isActive() const { return m_active.load(); }
    int takeCounter() const { return m_takeCounter; }
Q_SIGNALS:
    void takesFinished(const std::vector<RecordedTake>& takes);

private:
    struct Writer {
        Id trackId;
        int channels;
        FloatRing* ring;
        void* sndfile = nullptr;
        QString path;
        Tick startTick;
        int64_t frames = 0;
        int take = 0;
    };
    void writerLoop();
    void flushWriter(Writer& w, bool all);
    std::vector<RecordedTake> finalise();
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
    std::vector<RecordedTake> m_finished;
};

}   // namespace mt
