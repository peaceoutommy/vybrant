#include "audio/RecordingEngine.h"
#include "core/Log.h"
#include <sndfile.h>
#include <QDir>
#include <QDateTime>
#include <chrono>

namespace mt {

RecordingEngine::RecordingEngine(QObject* parent) : QObject(parent) { m_tmp.resize(1 << 16); }
RecordingEngine::~RecordingEngine() { stop(); }

FloatRing* RecordingEngine::ringFor(Id id, int channels) {
    auto& r = m_rings[id]; if (!r) { r = std::make_unique<FloatRing>(1 << 21); m_ringChannels[id] = channels; }
    return r.get();
}

void RecordingEngine::start(const std::vector<std::pair<Id, int>>& armed, Tick startTick) {
    if (m_active.load()) return;
    QDir().mkpath(m_dir);
    std::lock_guard<std::mutex> lk(m_mutex);
    m_writers.clear(); m_finished.clear(); m_takeCounter++;
    for (auto& [id, ch] : armed) {
        Writer w; w.trackId = id; w.channels = ch; w.ring = ringFor(id, ch); w.ring->clear(); w.startTick = startTick; w.take = m_takeCounter;
        w.path = QString("%1/rec_%2_track%3_take%4.wav").arg(m_dir).arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")).arg(id).arg(m_takeCounter);
        SF_INFO info{}; info.samplerate = (int)m_sr; info.channels = ch; info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
        w.sndfile = sf_open(w.path.toLocal8Bit().constData(), SFM_WRITE, &info);
        if (!w.sndfile) { Log::error("Cannot open record file " + w.path); continue; }
        m_writers.push_back(w);
    }
    if (m_writers.empty()) return;
    m_active.store(true); m_running.store(true);
    m_thread = std::thread([this] { writerLoop(); });
}

void RecordingEngine::writerLoop() {
    while (m_running.load()) {
        { std::lock_guard<std::mutex> lk(m_mutex); for (auto& w : m_writers) flushWriter(w, false); }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
void RecordingEngine::flushWriter(Writer& w, bool all) {
    if (!w.sndfile) return;
    size_t avail = w.ring->readAvailable(); if (!all && avail < 4096) return;
    while (avail >= (size_t)w.channels) {
        size_t n = std::min(avail, m_tmp.size()); n -= n % w.channels;
        size_t got = w.ring->read(m_tmp.data(), n); if (got == 0) break;
        sf_writef_float((SNDFILE*)w.sndfile, m_tmp.data(), (sf_count_t)(got / w.channels)); w.frames += (int64_t)(got / w.channels);
        avail = w.ring->readAvailable(); if (!all) break;
    }
}
std::vector<RecordedTake> RecordingEngine::finalise() {
    std::vector<RecordedTake> takes;
    for (auto& w : m_writers) { flushWriter(w, true); if (w.sndfile) sf_close((SNDFILE*)w.sndfile); w.sndfile = nullptr; RecordedTake t; t.mixerTrackId = w.trackId; t.filePath = w.path; t.startTick = w.startTick; t.frames = w.frames; t.channels = w.channels; t.takeIndex = w.take; takes.push_back(t); }
    return takes;
}
void RecordingEngine::newTake(Tick startTick) {
    if (!m_active.load()) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    auto takes = finalise(); m_finished.insert(m_finished.end(), takes.begin(), takes.end());
    m_takeCounter++;
    for (auto& w : m_writers) {
        w.startTick = startTick; w.frames = 0; w.take = m_takeCounter; w.ring->clear();
        w.path = QString("%1/rec_%2_track%3_take%4.wav").arg(m_dir).arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")).arg(w.trackId).arg(m_takeCounter);
        SF_INFO info{}; info.samplerate = (int)m_sr; info.channels = w.channels; info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
        w.sndfile = sf_open(w.path.toLocal8Bit().constData(), SFM_WRITE, &info);
    }
}
std::vector<RecordedTake> RecordingEngine::stop() {
    if (!m_active.load()) return {};
    m_running.store(false); if (m_thread.joinable()) m_thread.join();
    std::vector<RecordedTake> takes;
    { std::lock_guard<std::mutex> lk(m_mutex); takes = finalise(); takes.insert(takes.begin(), m_finished.begin(), m_finished.end()); m_writers.clear(); m_finished.clear(); }
    m_active.store(false);
    Q_EMIT takesFinished(takes);
    return takes;
}

} // namespace mt
