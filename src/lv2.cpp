#include "lv2.h"

#include <QDir>
#include <QHash>
#include <QMutex>
#include <cstring>
#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/port-props/port-props.h>
#include <lv2/urid/urid.h>
#include <vector>

#include "sounds.h"

namespace {

// The plugins lilv found, and the URI-to-number map plugins share. Made
// once; lilv itself is only used on the main thread.
// lilv crashes on a relative folder in LV2_PATH, so they're made absolute
// before it reads them.
LilvWorld *newWorld() {
    QStringList folders;
    for (const QString &folder : qEnvironmentVariable("LV2_PATH").split(':', Qt::SkipEmptyParts))
        folders << QDir(folder).absolutePath();
    if (!folders.isEmpty())
        qputenv("LV2_PATH", folders.join(':').toUtf8());
    return lilv_world_new();
}

struct World {
    LilvWorld *world = newWorld();
    QMutex mapMutex;
    QHash<QByteArray, LV2_URID> ids;
    QVector<QByteArray> uris;
    LV2_URID_Map map{this, [](LV2_URID_Map_Handle handle, const char *uri) {
                         auto *w = static_cast<World *>(handle);
                         QMutexLocker lock(&w->mapMutex);
                         const QByteArray key(uri);
                         if (const auto it = w->ids.constFind(key); it != w->ids.constEnd())
                             return *it;
                         w->uris << key;
                         return w->ids[key] = LV2_URID(w->uris.size());
                     }};
    LV2_URID_Unmap unmap{this, [](LV2_URID_Unmap_Handle handle, LV2_URID id) -> const char * {
                             auto *w = static_cast<World *>(handle);
                             QMutexLocker lock(&w->mapMutex);
                             return id > 0 && id <= LV2_URID(w->uris.size()) ? w->uris[id - 1].constData() : nullptr;
                         }};

    LilvNode *node(const char *uri) { return lilv_new_uri(world, uri); }
    LilvNode *audio = node(LV2_CORE__AudioPort);
    LilvNode *control = node(LV2_CORE__ControlPort);
    LilvNode *input = node(LV2_CORE__InputPort);
    LilvNode *output = node(LV2_CORE__OutputPort);
    LilvNode *atom = node(LV2_ATOM__AtomPort);
    LilvNode *midiEvent = node(LV2_MIDI__MidiEvent);
    LilvNode *supports = node(LV2_ATOM__supports);
    LilvNode *logarithmic = node(LV2_PORT_PROPS__logarithmic);
    LilvNode *toggled = node(LV2_CORE__toggled);
    LilvNode *integer = node(LV2_CORE__integer);
    LilvNode *enumeration = node(LV2_CORE__enumeration);

    World() {
        lilv_world_load_all(world);
    }
    LV2_URID urid(const char *uri) { return map.map(map.handle, uri); }

    static World &get() {
        static World world;
        return world;
    }
};

bool isInstrument(World &w, const LilvPlugin *plugin) {
    const LilvPluginClass *pluginClass = lilv_plugin_get_class(plugin);
    if (!strcmp(lilv_node_as_uri(lilv_plugin_class_get_uri(pluginClass)), LV2_CORE__InstrumentPlugin))
        return true;
    // Or: MIDI in, sound out, and no sound in.
    return lilv_plugin_get_num_ports_of_class(plugin, w.audio, w.input, nullptr) == 0
           && lilv_plugin_get_num_ports_of_class(plugin, w.audio, w.output, nullptr) > 0
           && lilv_plugin_get_num_ports_of_class(plugin, w.atom, w.input, nullptr) > 0;
}

class Lv2Processor : public Processor {
public:
    Lv2Processor(const LilvPlugin *plugin, QString *error);
    ~Lv2Processor() override;
    bool ok() const { return m_instance; }

    QString name() const override { return m_name; }
    bool instrument() const override { return m_instrument; }
    void process(float *left, float *right, int frames, const NoteEvent *notes, int noteCount,
                 const Transport &transport) override;
    QVector<Parameter> parameters() const override;
    void setParameter(const QString &id, double value) override;

private:
    enum Kind { AudioIn, AudioOut, ControlIn, ControlOut, AtomIn, AtomOut, Other };
    struct Port {
        uint32_t index;
        Kind kind;
        QString symbol, name;
        float min, max, defaultValue;
        bool logarithmic, stepped;
    };

    const LilvPlugin *m_plugin;
    LilvInstance *m_instance = nullptr;
    QString m_name;
    bool m_instrument = false;
    std::vector<Port> m_ports;
    std::vector<float> m_values; // a control value per port, connected in place
    std::vector<std::vector<float>> m_audio; // a buffer per port
    std::vector<uint32_t> m_audioIn, m_audioOut;
    std::vector<std::vector<uint64_t>> m_atoms; // per port, 8-byte aligned as LV2 wants
    std::vector<uint32_t> m_atomIn, m_atomOut;
    LV2_URID m_midiType = 0, m_sequenceType = 0, m_chunkType = 0;

    int32_t m_blockLength = kMaxBlock, m_minBlock = 1;
    float m_sampleRate = kRate;
    LV2_Options_Option m_options[4];
    LV2_Feature m_mapFeature, m_unmapFeature, m_boundedFeature, m_optionsFeature;
    const LV2_Feature *m_features[5];
};

Lv2Processor::Lv2Processor(const LilvPlugin *plugin, QString *error) : m_plugin(plugin) {
    World &w = World::get();
    LilvNode *name = lilv_plugin_get_name(plugin);
    m_name = QString::fromUtf8(lilv_node_as_string(name));
    lilv_node_free(name);
    m_instrument = isInstrument(w, plugin);
    m_midiType = w.urid(LV2_MIDI__MidiEvent);
    m_sequenceType = w.urid(LV2_ATOM__Sequence);
    m_chunkType = w.urid(LV2_ATOM__Chunk);

    const LV2_URID intType = w.urid(LV2_ATOM__Int), floatType = w.urid(LV2_ATOM__Float);
    m_options[0] = {LV2_OPTIONS_INSTANCE, 0, w.urid(LV2_BUF_SIZE__maxBlockLength), sizeof(int32_t), intType, &m_blockLength};
    m_options[1] = {LV2_OPTIONS_INSTANCE, 0, w.urid(LV2_BUF_SIZE__minBlockLength), sizeof(int32_t), intType, &m_minBlock};
    m_options[2] = {LV2_OPTIONS_INSTANCE, 0, w.urid(LV2_PARAMETERS__sampleRate), sizeof(float), floatType, &m_sampleRate};
    m_options[3] = {LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr};
    m_mapFeature = {LV2_URID__map, &w.map};
    m_unmapFeature = {LV2_URID__unmap, &w.unmap};
    m_boundedFeature = {LV2_BUF_SIZE__boundedBlockLength, nullptr};
    m_optionsFeature = {LV2_OPTIONS__options, m_options};
    m_features[0] = &m_mapFeature;
    m_features[1] = &m_unmapFeature;
    m_features[2] = &m_boundedFeature;
    m_features[3] = &m_optionsFeature;
    m_features[4] = nullptr;

    m_instance = lilv_plugin_instantiate(plugin, kRate, m_features);
    if (!m_instance) {
        QStringList missing;
        LilvNodes *required = lilv_plugin_get_required_features(plugin);
        LILV_FOREACH(nodes, it, required) {
            const char *uri = lilv_node_as_uri(lilv_nodes_get(required, it));
            if (strcmp(uri, LV2_URID__map) && strcmp(uri, LV2_URID__unmap) && strcmp(uri, LV2_BUF_SIZE__boundedBlockLength)
                && strcmp(uri, LV2_OPTIONS__options))
                missing << QString::fromUtf8(uri).section('#', -1).section('/', -1);
        }
        lilv_nodes_free(required);
        *error = missing.isEmpty() ? QString("the plugin wouldn't start")
                                   : QString("it needs %1, which dole doesn't offer").arg(missing.join(", "));
        return;
    }

    const uint32_t count = lilv_plugin_get_num_ports(plugin);
    std::vector<float> mins(count), maxes(count), defaults(count);
    lilv_plugin_get_port_ranges_float(plugin, mins.data(), maxes.data(), defaults.data());
    m_values.assign(count, 0.f);
    m_audio.assign(count, {});
    m_atoms.assign(count, {});
    for (uint32_t i = 0; i < count; ++i) {
        const LilvPort *port = lilv_plugin_get_port_by_index(plugin, i);
        const bool in = lilv_port_is_a(plugin, port, w.input);
        Kind kind = Other;
        if (lilv_port_is_a(plugin, port, w.audio))
            kind = in ? AudioIn : AudioOut;
        else if (lilv_port_is_a(plugin, port, w.control))
            kind = in ? ControlIn : ControlOut;
        else if (lilv_port_is_a(plugin, port, w.atom))
            kind = in ? AtomIn : AtomOut;
        LilvNode *portName = lilv_port_get_name(plugin, port);
        const float min = std::isnan(mins[i]) ? 0.f : mins[i], max = std::isnan(maxes[i]) ? 1.f : maxes[i];
        const float def = std::isnan(defaults[i]) ? min : defaults[i];
        m_ports.push_back({i, kind, QString::fromUtf8(lilv_node_as_string(lilv_port_get_symbol(plugin, port))),
                           QString::fromUtf8(portName ? lilv_node_as_string(portName) : ""), min, max, def,
                           lilv_port_has_property(plugin, port, w.logarithmic) && min > 0,
                           lilv_port_has_property(plugin, port, w.toggled)
                               || lilv_port_has_property(plugin, port, w.integer)
                               || lilv_port_has_property(plugin, port, w.enumeration)});
        lilv_node_free(portName);

        switch (kind) {
        case AudioIn:
        case AudioOut:
        case Other:
            // Unknown ports (CV and the like) get a silent buffer.
            m_audio[i].assign(kMaxBlock, 0.f);
            lilv_instance_connect_port(m_instance, i, m_audio[i].data());
            if (kind == AudioIn)
                m_audioIn.push_back(i);
            else if (kind == AudioOut)
                m_audioOut.push_back(i);
            break;
        case ControlIn:
        case ControlOut:
            m_values[i] = def;
            lilv_instance_connect_port(m_instance, i, &m_values[i]);
            break;
        case AtomIn:
        case AtomOut:
            m_atoms[i].assign(1024, 0);
            lilv_instance_connect_port(m_instance, i, m_atoms[i].data());
            (kind == AtomIn ? m_atomIn : m_atomOut).push_back(i);
            break;
        }
    }
    lilv_instance_activate(m_instance);
}

Lv2Processor::~Lv2Processor() {
    if (m_instance) {
        lilv_instance_deactivate(m_instance);
        lilv_instance_free(m_instance);
    }
}

void Lv2Processor::process(float *left, float *right, int frames, const NoteEvent *notes, int noteCount,
                           const Transport &) {
    // The pad's stereo in: both channels to a stereo plugin, their mix to a mono one.
    if (m_audioIn.size() == 1) {
        float *in = m_audio[m_audioIn[0]].data();
        for (int i = 0; i < frames; ++i)
            in[i] = (left[i] + right[i]) / 2;
    } else {
        for (size_t p = 0; p < m_audioIn.size(); ++p) {
            float *in = m_audio[m_audioIn[p]].data();
            if (p < 2)
                memcpy(in, p == 0 ? left : right, frames * sizeof(float));
            else
                memset(in, 0, frames * sizeof(float));
        }
    }

    // Notes go in as MIDI on the first event input; every other event
    // port gets an empty sequence, and outputs room to write.
    for (size_t p = 0; p < m_atomIn.size(); ++p) {
        auto *sequence = reinterpret_cast<LV2_Atom_Sequence *>(m_atoms[m_atomIn[p]].data());
        sequence->atom.type = m_sequenceType;
        sequence->atom.size = sizeof(LV2_Atom_Sequence_Body);
        sequence->body.unit = 0;
        sequence->body.pad = 0;
        if (p != 0)
            continue;
        const uint32_t capacity = uint32_t(m_atoms[m_atomIn[p]].size() * sizeof(uint64_t));
        for (int n = 0; n < noteCount; ++n) {
            struct {
                LV2_Atom_Event event;
                uint8_t midi[3];
            } event{};
            event.event.time.frames = notes[n].frame;
            event.event.body = {3, m_midiType};
            event.midi[0] = notes[n].on ? 0x90 : 0x80;
            event.midi[1] = uint8_t(notes[n].key);
            event.midi[2] = notes[n].on ? uint8_t(std::max(1, int(notes[n].velocity * 127))) : 0;
            lv2_atom_sequence_append_event(sequence, capacity, &event.event);
        }
    }
    for (uint32_t index : m_atomOut) {
        auto *atom = reinterpret_cast<LV2_Atom *>(m_atoms[index].data());
        atom->type = m_chunkType;
        atom->size = uint32_t(m_atoms[index].size() * sizeof(uint64_t) - sizeof(LV2_Atom));
    }

    lilv_instance_run(m_instance, uint32_t(frames));

    if (m_audioOut.empty())
        return;
    memcpy(left, m_audio[m_audioOut[0]].data(), frames * sizeof(float));
    memcpy(right, m_audio[m_audioOut[m_audioOut.size() > 1 ? 1 : 0]].data(), frames * sizeof(float));
}

QVector<Parameter> Lv2Processor::parameters() const {
    QVector<Parameter> parameters;
    for (const Port &port : m_ports)
        if (port.kind == ControlIn)
            parameters << Parameter{port.symbol, port.name, port.min, port.max, port.defaultValue,
                                    m_values[port.index], port.stepped, port.logarithmic};
    return parameters;
}

void Lv2Processor::setParameter(const QString &id, double value) {
    for (const Port &port : m_ports)
        if (port.kind == ControlIn && port.symbol == id)
            m_values[port.index] = float(qBound(double(port.min), port.stepped ? std::round(value) : value, double(port.max)));
}

} // namespace

QVector<PluginInfo> scanLv2() {
    World &w = World::get();
    QVector<PluginInfo> infos;
    const LilvPlugins *plugins = lilv_world_get_all_plugins(w.world);
    LILV_FOREACH(plugins, it, plugins) {
        const LilvPlugin *plugin = lilv_plugins_get(plugins, it);
        LilvNode *name = lilv_plugin_get_name(plugin);
        LilvNode *author = lilv_plugin_get_author_name(plugin);
        infos << PluginInfo{"lv2:" + QString::fromUtf8(lilv_node_as_uri(lilv_plugin_get_uri(plugin))),
                            QString::fromUtf8(name ? lilv_node_as_string(name) : "?"),
                            QString::fromUtf8(author ? lilv_node_as_string(author) : ""), "lv2",
                            isInstrument(w, plugin)};
        lilv_node_free(name);
        lilv_node_free(author);
    }
    return infos;
}

std::unique_ptr<Processor> createLv2(const QString &uri, QString *error) {
    World &w = World::get();
    LilvNode *node = lilv_new_uri(w.world, uri.toUtf8().constData());
    const LilvPlugin *plugin = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(w.world), node);
    lilv_node_free(node);
    if (!plugin) {
        *error = "it isn't installed";
        return {};
    }
    auto processor = std::make_unique<Lv2Processor>(plugin, error);
    if (!processor->ok())
        return {};
    return processor;
}
