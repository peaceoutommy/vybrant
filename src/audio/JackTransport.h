// JACK transport sync: play / stop / position follow the other JACK clients (and the tempo, when a timebase master
// publishes one), and MusicThingy's own play / stop / seek drive the JACK transport for them in turn.
#pragma once
#include "core/Types.h"
#include <QObject>
#include <QTimer>
#include <functional>

namespace mt {

class JackTransport : public QObject {
    Q_OBJECT
public:
    struct Host {   // the app side, so this file knows nothing about App
        std::function<bool()> playing;
        std::function<double()> positionTicks, bpm;
        std::function<bool()> looping;   // our transport wraps by itself (pattern mode, song loop): JACK positions can't be followed
        std::function<void(bool)> setPlaying;
        std::function<void(Tick)> seek;
        std::function<void(double)> setBpm;
    };
    explicit JackTransport(Host host, QObject* parent = nullptr);
    ~JackTransport() override;
    bool open(QString* error = nullptr);   // false when no JACK server runs
    // our transport -> JACK (ignored while applying a JACK change, so nothing echoes back)
    void play();
    void stop();
    void locate(Tick t);

private:
    void poll();
    double frameRate() const;
    Host m_host;
    void* m_client = nullptr;
    QTimer m_timer;
    bool m_applying = false;
    double m_lastBpm = 0;
};

}   // namespace mt
