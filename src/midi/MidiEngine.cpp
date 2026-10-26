// The platform-independent part of MidiEngine; the device side is in MidiEngineAlsa.cpp (Linux) and MidiEngineWin.cpp (Windows)
#include "midi/MidiEngine.h"
#include "audio/AudioEngine.h"
#include <QSettings>
#include <QMetaObject>
#include <chrono>
#include <cmath>

namespace mt {

MidiEngine::MidiEngine(AudioEngine& engine, QObject* parent)
    : QObject(parent)
    , m_engine(engine) {}
MidiEngine::~MidiEngine() { stop(); }

void MidiEngine::setEnabledInputs(const QStringList& ids) {
    for (auto& p : ports())
        if (p.input) {
            bool was = m_enabledInputs.contains(p.id()), now = ids.contains(p.id());
            if (was != now) connectInput(p, now);
        }
    m_enabledInputs = ids;
    saveSettings();
}
void MidiEngine::setEnabledOutputs(const QStringList& ids) {
    for (auto& p : ports())
        if (p.output) {
            bool was = m_enabledOutputs.contains(p.id()), now = ids.contains(p.id());
            if (was != now) connectOutput(p, now);
        }
    m_enabledOutputs = ids;
    saveSettings();
}
void MidiEngine::loadSettings() {
    QSettings s;
    m_enabledInputs = s.value("midi/inputs").toStringList();
    m_enabledOutputs = s.value("midi/outputs").toStringList();
}
void MidiEngine::saveSettings() const {
    QSettings s;
    s.setValue("midi/inputs", m_enabledInputs);
    s.setValue("midi/outputs", m_enabledOutputs);
}

void MidiEngine::handleClock(int status, int songPos) {
    auto transport = [this](int what, double tick) {
        QMetaObject::invokeMethod(this, [this, what, tick] { Q_EMIT clockTransport(what, tick); }, Qt::QueuedConnection);
    };
    switch (status) {
    case 0xF8: {
        double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        m_clockTimes[m_clockN++ % kClockWindow] = now;
        int64_t n = std::min<int64_t>(m_clockN, kClockWindow);
        if (n < 2) return;
        double bpm = 60.0 / ((now - m_clockTimes[(m_clockN - n) % kClockWindow]) / (n - 1) * 24.0);
        if (m_clockRunning) {
            // phase lock: nudge the tempo until the playhead's place in the beat matches the clock's (loops are beat-aligned)
            double expect = std::fmod(m_clockBase + m_clockCount++ * (PPQ / 24.0), PPQ);
            double err = std::remainder(expect - std::fmod(m_engine.positionTicks(), PPQ), PPQ);
            bpm *= 1.0 + std::clamp(err / PPQ, -0.03, 0.03);
        }
        if (bpm >= 20 && bpm <= 999) m_engine.setBpm(bpm);
        break;
    }
    case 0xFA:
        m_clockRunning = true;
        m_clockBase = 0;
        m_clockCount = 0;
        m_clockSpp = false;
        transport(0, 0);
        break;
    case 0xFB:
        m_clockRunning = true;
        if (!m_clockSpp) m_clockBase = m_engine.positionTicks();   // after a pointer the seek it queued may not have run yet
        m_clockSpp = false;
        m_clockCount = 0;
        transport(1, 0);
        break;
    case 0xFC:
        m_clockRunning = false;
        transport(2, 0);
        break;
    case 0xF2:   // in MIDI beats (sixteenths)
        m_clockBase = songPos * (PPQ / 4.0);
        m_clockCount = 0;
        m_clockSpp = true;
        transport(3, m_clockBase);
        break;
    }
}
void MidiEngine::received(const MidiEvent& e) {
    m_activity.fetch_add(1);
    m_engine.midiInQueue().push(e);
    if (m_learning.load() && e.type == MidiEvent::CC) {
        int cc = e.data1, ch = e.channel, val = e.data2;
        QMetaObject::invokeMethod(this, [this, cc, ch, val] { Q_EMIT ccReceived(cc, ch, val); }, Qt::QueuedConnection);
    }
}

}   // namespace mt
