// Two tiny CLAP plugins for dole's tests: a gain effect with one parameter
// and saved state, and a sine synth that plays while a note is held.
#include <clap/clap.h>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr clap_id kGainParam = 7;

struct Plugin {
    clap_plugin plugin;
    const clap_host *host;
    bool synth;
    double gain = 1.0;
    double rate = 48000;
    double phase = 0;
    int key = -1;
};

Plugin *self(const clap_plugin *p) {
    return static_cast<Plugin *>(p->plugin_data);
}

const char *gainFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const char *sineFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr};
const clap_plugin_descriptor gainDescriptor{CLAP_VERSION, "dole.test.gain", "Test Gain", "dole tests", "", "", "", "1",
                                            "", gainFeatures};
const clap_plugin_descriptor sineDescriptor{CLAP_VERSION, "dole.test.sine", "Test Sine", "dole tests", "", "", "", "1",
                                            "", sineFeatures};

const clap_plugin_params params{
    [](const clap_plugin *p) -> uint32_t { return self(p)->synth ? 0 : 1; },
    [](const clap_plugin *, uint32_t, clap_param_info *info) {
        *info = {};
        info->id = kGainParam;
        strcpy(info->name, "Gain");
        info->min_value = 0;
        info->max_value = 2;
        info->default_value = 1;
        return true;
    },
    [](const clap_plugin *p, clap_id, double *value) {
        *value = self(p)->gain;
        return true;
    },
    [](const clap_plugin *, clap_id, double value, char *text, uint32_t size) {
        snprintf(text, size, "%.0f%%", value * 100);
        return true;
    },
    [](const clap_plugin *, clap_id, const char *, double *) { return false; },
    [](const clap_plugin *, const clap_input_events *, const clap_output_events *) {}};

const clap_plugin_state state{
    [](const clap_plugin *p, const clap_ostream *stream) {
        return stream->write(stream, &self(p)->gain, sizeof(double)) == sizeof(double);
    },
    [](const clap_plugin *p, const clap_istream *stream) {
        return stream->read(stream, &self(p)->gain, sizeof(double)) == sizeof(double);
    }};

const clap_plugin_audio_ports ports{
    [](const clap_plugin *p, bool input) -> uint32_t { return input && self(p)->synth ? 0 : 1; },
    [](const clap_plugin *, uint32_t, bool, clap_audio_port_info *info) {
        *info = {};
        info->id = 0;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        return true;
    }};

const clap_plugin_note_ports notePorts{
    [](const clap_plugin *p, bool input) -> uint32_t { return input && self(p)->synth ? 1 : 0; },
    [](const clap_plugin *, uint32_t, bool, clap_note_port_info *info) {
        *info = {};
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP;
        info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
        return true;
    }};

clap_process_status process(const clap_plugin *p, const clap_process *process) {
    Plugin *plugin = self(p);
    const uint32_t events = process->in_events->size(process->in_events);
    uint32_t next = 0;
    for (uint32_t i = 0; i < process->frames_count; ++i) {
        while (next < events) {
            const clap_event_header *event = process->in_events->get(process->in_events, next);
            if (event->time > i)
                break;
            if (event->type == CLAP_EVENT_PARAM_VALUE)
                plugin->gain = reinterpret_cast<const clap_event_param_value *>(event)->value;
            else if (event->type == CLAP_EVENT_NOTE_ON)
                plugin->key = reinterpret_cast<const clap_event_note *>(event)->key;
            else if (event->type == CLAP_EVENT_NOTE_OFF)
                plugin->key = -1;
            ++next;
        }
        float *const *out = process->audio_outputs[0].data32;
        if (plugin->synth) {
            float value = 0;
            if (plugin->key >= 0) {
                value = float(0.5 * std::sin(plugin->phase * 6.283185307179586));
                plugin->phase += 440.0 * std::pow(2.0, (plugin->key - 69) / 12.0) / plugin->rate;
            }
            out[0][i] = out[1][i] = value;
        } else {
            float *const *in = process->audio_inputs[0].data32;
            out[0][i] = float(in[0][i] * plugin->gain);
            out[1][i] = float(in[1][i] * plugin->gain);
        }
    }
    return CLAP_PROCESS_CONTINUE;
}

const clap_plugin *create(const clap_host *host, bool synth) {
    auto *plugin = new Plugin{};
    plugin->host = host;
    plugin->synth = synth;
    plugin->plugin = {synth ? &sineDescriptor : &gainDescriptor,
                      plugin,
                      [](const clap_plugin *) { return true; },
                      [](const clap_plugin *p) { delete self(p); },
                      [](const clap_plugin *p, double rate, uint32_t, uint32_t) {
                          self(p)->rate = rate;
                          return true;
                      },
                      [](const clap_plugin *) {},
                      [](const clap_plugin *) { return true; },
                      [](const clap_plugin *) {},
                      [](const clap_plugin *) {},
                      process,
                      [](const clap_plugin *, const char *id) -> const void * {
                          if (!strcmp(id, CLAP_EXT_PARAMS))
                              return &params;
                          if (!strcmp(id, CLAP_EXT_STATE))
                              return &state;
                          if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))
                              return &ports;
                          if (!strcmp(id, CLAP_EXT_NOTE_PORTS))
                              return &notePorts;
                          return nullptr;
                      },
                      [](const clap_plugin *) {}};
    return &plugin->plugin;
}

const clap_plugin_factory factory{
    [](const clap_plugin_factory *) -> uint32_t { return 2; },
    [](const clap_plugin_factory *, uint32_t index) { return index == 0 ? &gainDescriptor : &sineDescriptor; },
    [](const clap_plugin_factory *, const clap_host *host, const char *id) -> const clap_plugin * {
        if (!strcmp(id, gainDescriptor.id))
            return create(host, false);
        if (!strcmp(id, sineDescriptor.id))
            return create(host, true);
        return nullptr;
    }};

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry clap_entry{
    CLAP_VERSION, [](const char *) { return true; }, [] {},
    [](const char *id) -> const void * { return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &factory : nullptr; }};
