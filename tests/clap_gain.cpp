// Minimal CLAP plugin for the self test: a stereo gain effect with one parameter, state and a reported latency
// (64 samples, 128 above 1.5x gain: changing it asks the host for a restart, as CLAP requires while active).
// `musicthingy --selftest` compiles it with the system C++ compiler against src/plugins/ClapAbi.h (not part of the app build).
#include "plugins/ClapAbi.h"
#include <cstdio>
#include <cstring>

using namespace clap;

namespace {
constexpr clap_id kGainId = 7;   // not an index: the host must key parameters by id

struct Gain {
    clap_plugin plugin;
    const clap_host* host;
    double gain = 1.0;
    uint32_t latency = 0;   // reported only, the audio is not delayed
    bool restartAsked = false;
};
Gain* self(const clap_plugin* p) { return (Gain*)p->plugin_data; }

void applyEvents(Gain* g, const clap_input_events* in) {
    for (uint32_t i = 0, n = in->size(in); i < n; ++i) {
        auto* e = (const clap_event_param_value*)in->get(in, i);
        if (e->header.space_id == CLAP_CORE_EVENT_SPACE_ID && e->header.type == CLAP_EVENT_PARAM_VALUE && e->param_id == kGainId)
            g->gain = e->value;
    }
}

const clap_plugin_params params = {
    [](const clap_plugin*) -> uint32_t { return 1; },
    [](const clap_plugin*, uint32_t i, clap_param_info* info) {
        if (i != 0) return false;
        *info = {};
        info->id = kGainId;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE;
        std::strcpy(info->name, "Gain");
        info->max_value = 2;
        info->default_value = 1;
        return true;
    },
    [](const clap_plugin* p, clap_id id, double* v) {
        *v = self(p)->gain;
        return id == kGainId;
    },
    [](const clap_plugin*, clap_id, double v, char* buf, uint32_t cap) { return std::snprintf(buf, cap, "%.2f x", v) > 0; },
    [](const clap_plugin*, clap_id, const char*, double*) { return false; },
    [](const clap_plugin* p, const clap_input_events* in, const clap_output_events*) { applyEvents(self(p), in); },
};
const clap_plugin_state state = {
    [](const clap_plugin* p, const clap_ostream* s) { return s->write(s, &self(p)->gain, sizeof(double)) == sizeof(double); },
    [](const clap_plugin* p, const clap_istream* s) { return s->read(s, &self(p)->gain, sizeof(double)) == sizeof(double); },
};
const clap_plugin_latency latency = {[](const clap_plugin* p) { return self(p)->latency; }};
const clap_plugin_audio_ports ports = {
    [](const clap_plugin*, bool) -> uint32_t { return 1; },
    [](const clap_plugin*, uint32_t i, bool input, clap_audio_port_info* info) {
        if (i != 0) return false;
        *info = {};
        info->id = input ? 0 : 1;
        std::strcpy(info->name, "main");
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = "stereo";
        info->in_place_pair = CLAP_INVALID_ID;
        return true;
    },
};

const char* const features[] = {"audio-effect", "utility", nullptr};
const clap_plugin_descriptor desc = {
    CLAP_VERSION, "org.musicthingy.test-gain", "MT Test Gain", "MusicThingy", "", "", "", "1.0", "Self-test gain", features};

const clap_plugin* create(const clap_host* host) {
    auto* g = new Gain();
    g->host = host;
    g->plugin = {&desc, g, [](const clap_plugin*) { return true; }, [](const clap_plugin* p) { delete self(p); },
        [](const clap_plugin* p, double, uint32_t, uint32_t) {
            self(p)->latency = self(p)->gain > 1.5 ? 128 : 64;
            self(p)->restartAsked = false;
            return true;
        },
        [](const clap_plugin*) {}, [](const clap_plugin*) { return true; }, [](const clap_plugin*) {}, [](const clap_plugin*) {},
        [](const clap_plugin* p, const clap_process* pr) -> clap_process_status {
            auto* g = self(p);
            applyEvents(g, pr->in_events);   // whole-block parameter changes are enough for a test
            if ((g->gain > 1.5 ? 128u : 64u) != g->latency && !g->restartAsked) {
                g->restartAsked = true;
                g->host->request_restart(g->host);
            }
            for (uint32_t c = 0; c < 2; ++c)
                for (uint32_t i = 0; i < pr->frames_count; ++i)
                    pr->audio_outputs[0].data32[c][i] = pr->audio_inputs[0].data32[c][i] * (float)g->gain;
            return CLAP_PROCESS_CONTINUE;
        },
        [](const clap_plugin*, const char* id) -> const void* {
            if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &params;
            if (!std::strcmp(id, CLAP_EXT_STATE)) return &state;
            if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &latency;
            if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &ports;
            return nullptr;
        },
        [](const clap_plugin*) {}};
    return &g->plugin;
}

const clap_plugin_factory factory = {
    [](const clap_plugin_factory*) -> uint32_t { return 1; },
    [](const clap_plugin_factory*, uint32_t i) { return i == 0 ? &desc : nullptr; },
    [](const clap_plugin_factory*, const clap_host* host, const char* id) { return std::strcmp(id, desc.id) ? nullptr : create(host); },
};
}   // namespace

extern "C" const clap_plugin_entry clap_entry = {
    CLAP_VERSION,
    [](const char*) { return true; },
    [] {},
    [](const char* id) -> const void* { return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? nullptr : &factory; },
};
