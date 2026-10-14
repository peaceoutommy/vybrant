// Minimal CLAP plugins for the self test: a stereo gain effect with one parameter, state and a reported latency
// (64 samples, 128 above 1.5x gain: changing it asks the host for a restart, as CLAP requires while active), and a
// multi-out instrument that writes 0.25 to its main output pair and 0.5 to an extra "Kick" pair. Above 1.95x gain the
// gain plugin crashes on purpose, for the out-of-process test. The gain plugin's editor is a plain 320x200 X11 window named
// "mt-test-gain-editor" (`musicthingy --embed-test` looks for it inside the host's window).
// `musicthingy --selftest` compiles it with the system C++ compiler against src/plugins/ClapAbi.h (not part of the app build).
#include "plugins/ClapAbi.h"
#include <cstdio>
#include <cstring>
#include <X11/Xlib.h>   // after the CLAP declarations: its macros clash with nothing below

using namespace clap;

namespace {
constexpr clap_id kGainId = 7;   // not an index: the host must key parameters by id

struct Gain {
    clap_plugin plugin;
    const clap_host* host;
    double gain = 1.0;
    uint32_t latency = 0;   // reported only, the audio is not delayed
    bool restartAsked = false;
    Display* dpy = nullptr;   // editor
    Window win = 0;
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
const clap_plugin_gui gui = {
    [](const clap_plugin*, const char* api, bool floating) { return !floating && !std::strcmp(api, CLAP_WINDOW_API_X11); },
    [](const clap_plugin*, const char** api, bool* floating) {
        *api = CLAP_WINDOW_API_X11;
        *floating = false;
        return true;
    },
    [](const clap_plugin* p, const char*, bool) { return (self(p)->dpy = XOpenDisplay(nullptr)) != nullptr; },
    [](const clap_plugin* p) {
        auto* g = self(p);
        if (g->win) XDestroyWindow(g->dpy, g->win);
        if (g->dpy) XCloseDisplay(g->dpy);
        g->win = 0;
        g->dpy = nullptr;
    },
    [](const clap_plugin*, double) { return false; },
    [](const clap_plugin*, uint32_t* w, uint32_t* h) {
        *w = 320;
        *h = 200;
        return true;
    },
    [](const clap_plugin*) { return false; },
    [](const clap_plugin*, clap_gui_resize_hints*) { return false; },
    [](const clap_plugin*, uint32_t*, uint32_t*) { return false; },
    [](const clap_plugin*, uint32_t, uint32_t) { return false; },
    [](const clap_plugin* p, const clap_window* w) {   // its own window inside the host's
        auto* g = self(p);
        g->win = XCreateSimpleWindow(g->dpy, w->x11, 0, 0, 320, 200, 0, 0, 0x3366cc);
        XStoreName(g->dpy, g->win, "mt-test-gain-editor");
        XFlush(g->dpy);
        return true;
    },
    [](const clap_plugin*, const clap_window*) { return false; },
    [](const clap_plugin*, const char*) {},
    [](const clap_plugin* p) {
        XMapWindow(self(p)->dpy, self(p)->win);
        XFlush(self(p)->dpy);
        return true;
    },
    [](const clap_plugin* p) {
        XUnmapWindow(self(p)->dpy, self(p)->win);
        XFlush(self(p)->dpy);
        return true;
    },
};
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
            if (g->gain > 1.95) __builtin_trap();
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
            if (!std::strcmp(id, CLAP_EXT_GUI)) return &gui;
            return nullptr;
        },
        [](const clap_plugin*) {}};
    return &g->plugin;
}

const clap_plugin_audio_ports multiPorts = {
    [](const clap_plugin*, bool input) -> uint32_t { return input ? 0 : 2; },
    [](const clap_plugin*, uint32_t i, bool input, clap_audio_port_info* info) {
        if (input || i > 1) return false;
        *info = {};
        info->id = i;
        std::strcpy(info->name, i ? "Kick" : "main");
        info->flags = i ? 0 : CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = "stereo";
        info->in_place_pair = CLAP_INVALID_ID;
        return true;
    },
};
const char* const multiFeatures[] = {"instrument", nullptr};
const clap_plugin_descriptor multiDesc = {CLAP_VERSION, "org.musicthingy.test-multi", "MT Test Multi", "MusicThingy", "", "", "", "1.0",
    "Self-test multi-out instrument", multiFeatures};

const clap_plugin* createMulti() {
    return new clap_plugin{&multiDesc, nullptr, [](const clap_plugin*) { return true; }, [](const clap_plugin* p) { delete p; },
        [](const clap_plugin*, double, uint32_t, uint32_t) { return true; }, [](const clap_plugin*) {},
        [](const clap_plugin*) { return true; }, [](const clap_plugin*) {}, [](const clap_plugin*) {},
        [](const clap_plugin*, const clap_process* pr) -> clap_process_status {
            for (uint32_t port = 0; port < 2; ++port)
                for (uint32_t c = 0; c < 2; ++c)
                    for (uint32_t i = 0; i < pr->frames_count; ++i) pr->audio_outputs[port].data32[c][i] = port ? 0.5f : 0.25f;
            return CLAP_PROCESS_CONTINUE;
        },
        [](const clap_plugin*, const char* id) -> const void* { return std::strcmp(id, CLAP_EXT_AUDIO_PORTS) ? nullptr : &multiPorts; },
        [](const clap_plugin*) {}};
}

const clap_plugin_factory factory = {
    [](const clap_plugin_factory*) -> uint32_t { return 2; },
    [](const clap_plugin_factory*, uint32_t i) { return i == 0 ? &desc
                                                     : i == 1  ? &multiDesc
                                                               : nullptr; },
    [](const clap_plugin_factory*, const clap_host* host, const char* id) {
        return !std::strcmp(id, desc.id) ? create(host) : !std::strcmp(id, multiDesc.id) ? createMulti() : nullptr;
    },
};
}   // namespace

extern "C" const clap_plugin_entry clap_entry = {
    CLAP_VERSION,
    [](const char*) { return true; },
    [] {},
    [](const char* id) -> const void* { return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? nullptr : &factory; },
};
