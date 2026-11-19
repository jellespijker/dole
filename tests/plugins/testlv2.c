/* A tiny LV2 plugin for dole's tests: stereo gain, with one control. */
#include <lv2/core/lv2.h>
#include <stdlib.h>

typedef struct {
    const float *in[2];
    float *out[2];
    const float *gain;
} Gain;

static LV2_Handle instantiate(const LV2_Descriptor *d, double rate, const char *path, const LV2_Feature *const *f) {
    (void)d; (void)rate; (void)path; (void)f;
    return calloc(1, sizeof(Gain));
}

static void connect(LV2_Handle h, uint32_t port, void *data) {
    Gain *g = (Gain *)h;
    if (port < 2)
        g->in[port] = data;
    else if (port < 4)
        g->out[port - 2] = data;
    else
        g->gain = data;
}

static void run(LV2_Handle h, uint32_t frames) {
    Gain *g = (Gain *)h;
    for (int c = 0; c < 2; ++c)
        for (uint32_t i = 0; i < frames; ++i)
            g->out[c][i] = g->in[c][i] * *g->gain;
}

static const LV2_Descriptor descriptor = {"urn:dole:test-gain", instantiate, connect, NULL, run, NULL, free, NULL};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index) {
    return index == 0 ? &descriptor : NULL;
}
