// Minimal LV2 gain used by `musicthingy --selftest`: stereo in/out, one gain control port, a constant latency report.
#include <lv2/core/lv2.h>
#include <stdlib.h>

typedef struct {
    const float* in[2];
    float* out[2];
    const float* gain;
    float* latency;
} Gain;

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate, const char* bundle, const LV2_Feature* const* features) {
    return calloc(1, sizeof(Gain));
}

static void connect_port(LV2_Handle h, uint32_t port, void* data) {
    Gain* g = (Gain*)h;
    if (port < 2) g->in[port] = (const float*)data;
    else if (port < 4) g->out[port - 2] = (float*)data;
    else if (port == 4) g->gain = (const float*)data;
    else if (port == 5) g->latency = (float*)data;
}

static void run(LV2_Handle h, uint32_t n) {
    Gain* g = (Gain*)h;
    for (int c = 0; c < 2; ++c)
        for (uint32_t i = 0; i < n; ++i) g->out[c][i] = g->in[c][i] * *g->gain;
    if (g->latency) *g->latency = 64.f;
}

static void cleanup(LV2_Handle h) { free(h); }

static const LV2_Descriptor descriptor = {"urn:musicthingy:test-gain", instantiate, connect_port, NULL, run, NULL, cleanup, NULL};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) { return index == 0 ? &descriptor : NULL; }
