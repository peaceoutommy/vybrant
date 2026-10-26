#include "audio/JackTransport.h"
#include <jack/jack.h>
#include <jack/transport.h>
#include <cmath>

namespace mt {

JackTransport::JackTransport(Host host, QObject* parent)
    : QObject(parent)
    , m_host(std::move(host)) {
    m_timer.setInterval(30);
    connect(&m_timer, &QTimer::timeout, this, &JackTransport::poll);
}
JackTransport::~JackTransport() {
    if (m_client) jack_client_close((jack_client_t*)m_client);
}
bool JackTransport::open(QString* error) {
    jack_status_t st;
    auto* c = jack_client_open("Vybrant transport", JackNoStartServer, &st);
    if (!c || jack_activate(c) != 0) {
        if (c) jack_client_close(c);
        if (error) *error = "JACK is not running";
        return false;
    }
    m_client = c;
    m_timer.start();
    return true;
}
double JackTransport::frameRate() const { return jack_get_sample_rate((jack_client_t*)m_client); }
void JackTransport::play() {
    if (m_client && !m_applying) jack_transport_start((jack_client_t*)m_client);
}
void JackTransport::stop() {
    if (m_client && !m_applying) jack_transport_stop((jack_client_t*)m_client);
}
void JackTransport::locate(Tick t) {
    if (m_client && !m_applying)
        jack_transport_locate((jack_client_t*)m_client, (jack_nframes_t)std::llround(m_host.secondsAt(t) * frameRate()));
}
void JackTransport::poll() {
    jack_position_t pos;
    jack_transport_state_t st = jack_transport_query((jack_client_t*)m_client, &pos);
    m_applying = true;
    if ((pos.valid & JackPositionBBT) && pos.beats_per_minute > 0) m_host.setBpm(pos.beats_per_minute);   // every poll: transport edits reset it
    bool rolling = st == JackTransportRolling;
    Tick jackTick = (Tick)std::llround(m_host.tickAt(pos.frame / (double)(pos.frame_rate ? pos.frame_rate : frameRate())));
    if (rolling != m_host.playing()) {
        if (rolling && !m_host.looping()) m_host.seek(jackTick);
        m_host.setPlaying(rolling);
    } else if (!m_host.looping() && std::fabs(jackTick - m_host.positionTicks()) > PPQ / 2)
        m_host.seek(jackTick);   // another client located (half a beat: well past poll and block jitter)
    m_applying = false;
}

}   // namespace mt
