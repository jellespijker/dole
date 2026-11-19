#include "clap.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QHash>
#include <QLibrary>
#include <QMutex>
#include <QSocketNotifier>
#include <QThread>
#include <QTimer>
#include <clap/clap.h>
#include <cstring>
#include <vector>

#include "sounds.h"
#include "x11window.h"

namespace {

struct Library {
    const clap_plugin_entry *entry = nullptr;
    const clap_plugin_factory *factory = nullptr;
};

QStringList clapFiles() {
    QStringList folders = qEnvironmentVariable("CLAP_PATH").split(':', Qt::SkipEmptyParts);
    folders << QDir::homePath() + "/.clap" << "/usr/lib/clap" << "/usr/local/lib/clap";
    QStringList files;
    for (const QString &folder : folders)
        for (QDirIterator it(folder, {"*.clap"}, QDir::Files, QDirIterator::Subdirectories); it.hasNext();)
            files << it.next();
    return files;
}

// A plugin's library stays loaded for the life of the process: plugins
// leave threads and callbacks behind, and unloading under them crashes.
QMutex librariesMutex;
QHash<QString, Library> libraries;
QHash<QString, QString> pathsById;

const Library *library(const QString &path, QString *error) {
    QMutexLocker lock(&librariesMutex);
    if (const auto it = libraries.constFind(path); it != libraries.constEnd())
        return &*it;
    auto *loaded = new QLibrary(path);
    if (!loaded->load()) {
        *error = loaded->errorString();
        return nullptr;
    }
    const auto *entry = reinterpret_cast<const clap_plugin_entry *>(loaded->resolve("clap_entry"));
    if (!entry || !clap_version_is_compatible(entry->clap_version) || !entry->init(path.toUtf8().constData())) {
        *error = "not a CLAP plugin this version of dole can load";
        return nullptr;
    }
    Library library{entry, static_cast<const clap_plugin_factory *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID))};
    return &*libraries.insert(path, library);
}

bool hasFeature(const clap_plugin_descriptor *descriptor, const char *feature) {
    for (const char *const *f = descriptor->features; f && *f; ++f)
        if (strcmp(*f, feature) == 0)
            return true;
    return false;
}

bool isMainThread() {
    return QThread::currentThread() == QCoreApplication::instance()->thread();
}

// An event as the plugin reads it: any of the kinds dole sends.
union Event {
    clap_event_header header;
    clap_event_note note;
    clap_event_midi midi;
    clap_event_param_value param;
};

class ClapProcessor : public Processor {
public:
    ClapProcessor(const Library *library, const QString &id, QString *error);
    ~ClapProcessor() override;
    bool ok() const { return m_plugin; }

    QString name() const override { return m_name; }
    bool instrument() const override { return m_instrument; }
    void process(float *left, float *right, int frames, const NoteEvent *notes, int noteCount,
                 const Transport &transport) override;
    QVector<Parameter> parameters() const override;
    void setParameter(const QString &id, double value) override;
    QString valueText(const QString &id, double value) const override;
    QByteArray saveState() override;
    bool loadState(const QByteArray &state) override;
    bool hasEditor() const override { return m_gui; }
    bool showEditor(QString *error) override;

private:
    static ClapProcessor *self(const clap_host *host) { return static_cast<ClapProcessor *>(host->host_data); }
    static const void *hostExtension(const clap_host *host, const char *id);
    void readParameters();
    void readPorts();
    void onMain(std::function<void()> work) { QMetaObject::invokeMethod(m_main, std::move(work), Qt::QueuedConnection); }
    void closeEditor();

    struct Param {
        clap_id id;
        void *cookie;
        QString name;
        double min, max, defaultValue, value;
        bool stepped;
    };
    struct Pending {
        clap_id id;
        void *cookie;
        double value;
    };

    clap_host m_host{};
    const clap_plugin *m_plugin = nullptr;
    QString m_name;
    bool m_instrument = false;
    bool m_active = false;
    bool m_processing = false;
    QObject *m_main = nullptr; // lives on the main thread: queued work dies with it

    const clap_plugin_params *m_params = nullptr;
    const clap_plugin_state *m_state = nullptr;
    const clap_plugin_gui *m_gui = nullptr;
    const clap_plugin_timer_support *m_timers = nullptr;
    const clap_plugin_posix_fd_support *m_fds = nullptr;
    QVector<Param> m_paramInfo;
    QVector<Pending> m_pending;
    bool m_midiNotes = false;

    // Audio: a buffer per channel of every port, the first port of each
    // side carrying the pad's stereo.
    std::vector<std::vector<std::vector<float>>> m_inData, m_outData;
    std::vector<std::vector<float *>> m_inPointers, m_outPointers;
    std::vector<clap_audio_buffer> m_inBuffers, m_outBuffers;
    std::vector<Event> m_events;
    clap_input_events m_inEvents{};
    clap_output_events m_outEvents{};
    clap_event_transport m_transport{};
    int64_t m_steadyTime = 0;

    std::unique_ptr<X11Window> m_window;
    bool m_floating = false;
    bool m_guiCreated = false;
    QHash<clap_id, QTimer *> m_timerObjects;
    clap_id m_nextTimer = 1;
    QHash<int, QList<QSocketNotifier *>> m_fdNotifiers;
};

ClapProcessor::ClapProcessor(const Library *library, const QString &id, QString *error) {
    m_main = new QObject;
    m_main->moveToThread(QCoreApplication::instance()->thread());
    m_host = {CLAP_VERSION, this, "dole", "dole", "https://github.com/omacom", DOLE_VERSION,
              hostExtension,
              [](const clap_host *) {},
              [](const clap_host *) {},
              [](const clap_host *host) {
                  ClapProcessor *p = self(host);
                  p->onMain([p] {
                      if (p->m_plugin)
                          p->m_plugin->on_main_thread(p->m_plugin);
                  });
              }};

    m_plugin = library->factory->create_plugin(library->factory, &m_host, id.toUtf8().constData());
    if (!m_plugin || !m_plugin->init(m_plugin)) {
        if (m_plugin)
            m_plugin->destroy(m_plugin);
        m_plugin = nullptr;
        *error = "the plugin wouldn't start";
        return;
    }
    m_name = QString::fromUtf8(m_plugin->desc->name);
    m_instrument = hasFeature(m_plugin->desc, CLAP_PLUGIN_FEATURE_INSTRUMENT);
    m_params = static_cast<const clap_plugin_params *>(m_plugin->get_extension(m_plugin, CLAP_EXT_PARAMS));
    m_state = static_cast<const clap_plugin_state *>(m_plugin->get_extension(m_plugin, CLAP_EXT_STATE));
    m_gui = static_cast<const clap_plugin_gui *>(m_plugin->get_extension(m_plugin, CLAP_EXT_GUI));
    m_timers = static_cast<const clap_plugin_timer_support *>(m_plugin->get_extension(m_plugin, CLAP_EXT_TIMER_SUPPORT));
    m_fds = static_cast<const clap_plugin_posix_fd_support *>(m_plugin->get_extension(m_plugin, CLAP_EXT_POSIX_FD_SUPPORT));
    if (m_gui && !m_gui->is_api_supported(m_plugin, CLAP_WINDOW_API_X11, true)
        && !m_gui->is_api_supported(m_plugin, CLAP_WINDOW_API_X11, false))
        m_gui = nullptr;
    readParameters();
    readPorts();

    m_inEvents.ctx = this;
    m_inEvents.size = [](const clap_input_events *list) {
        return uint32_t(static_cast<ClapProcessor *>(list->ctx)->m_events.size());
    };
    m_inEvents.get = [](const clap_input_events *list, uint32_t index) -> const clap_event_header * {
        return &static_cast<ClapProcessor *>(list->ctx)->m_events[index].header;
    };
    m_outEvents.ctx = this;
    // A change the plugin made itself, from its own window.
    m_outEvents.try_push = [](const clap_output_events *list, const clap_event_header *event) {
        auto *p = static_cast<ClapProcessor *>(list->ctx);
        if (event->space_id == CLAP_CORE_EVENT_SPACE_ID && event->type == CLAP_EVENT_PARAM_VALUE)
            p->onMain([p] {
                if (p->onEdited)
                    p->onEdited();
            });
        return true;
    };

    if (!m_plugin->activate(m_plugin, kRate, 1, kMaxBlock)) {
        *error = "the plugin wouldn't start at 48 kHz";
        m_plugin->destroy(m_plugin);
        m_plugin = nullptr;
        return;
    }
    m_active = true;
}

ClapProcessor::~ClapProcessor() {
    if (m_plugin) {
        closeEditor();
        if (m_processing && m_plugin->stop_processing)
            m_plugin->stop_processing(m_plugin);
        if (m_active)
            m_plugin->deactivate(m_plugin);
        m_plugin->destroy(m_plugin);
    }
    qDeleteAll(m_timerObjects);
    for (const auto &notifiers : std::as_const(m_fdNotifiers))
        qDeleteAll(notifiers);
    delete m_main;
}

void ClapProcessor::readParameters() {
    m_paramInfo.clear();
    if (!m_params)
        return;
    for (uint32_t i = 0; i < m_params->count(m_plugin); ++i) {
        clap_param_info info{};
        if (!m_params->get_info(m_plugin, i, &info) || (info.flags & (CLAP_PARAM_IS_HIDDEN | CLAP_PARAM_IS_READONLY)))
            continue;
        double value = info.default_value;
        m_params->get_value(m_plugin, info.id, &value);
        m_paramInfo << Param{info.id, info.cookie, QString::fromUtf8(info.name), info.min_value, info.max_value,
                             info.default_value, value, bool(info.flags & CLAP_PARAM_IS_STEPPED)};
    }
}

void ClapProcessor::readPorts() {
    // Without the audio ports extension, an effect takes stereo in and out,
    // and an instrument only gives stereo out.
    std::vector<int> ins{m_instrument ? 0 : 2}, outs{2};
    if (m_instrument)
        ins.clear();
    const auto *ports = static_cast<const clap_plugin_audio_ports *>(m_plugin->get_extension(m_plugin, CLAP_EXT_AUDIO_PORTS));
    if (ports) {
        ins.clear();
        outs.clear();
        for (bool input : {true, false})
            for (uint32_t i = 0; i < ports->count(m_plugin, input); ++i) {
                clap_audio_port_info info{};
                ports->get(m_plugin, i, input, &info);
                (input ? ins : outs).push_back(int(std::max<uint32_t>(1, info.channel_count)));
            }
    }
    auto build = [](const std::vector<int> &channels, auto &data, auto &pointers, auto &buffers) {
        data.assign(channels.size(), {});
        pointers.assign(channels.size(), {});
        buffers.assign(channels.size(), clap_audio_buffer{});
        for (size_t p = 0; p < channels.size(); ++p) {
            data[p].assign(channels[p], std::vector<float>(kMaxBlock, 0.f));
            for (auto &channel : data[p])
                pointers[p].push_back(channel.data());
            buffers[p].data32 = pointers[p].data();
            buffers[p].channel_count = uint32_t(channels[p]);
        }
    };
    build(ins, m_inData, m_inPointers, m_inBuffers);
    build(outs, m_outData, m_outPointers, m_outBuffers);

    const auto *notes = static_cast<const clap_plugin_note_ports *>(m_plugin->get_extension(m_plugin, CLAP_EXT_NOTE_PORTS));
    if (notes && notes->count(m_plugin, true) > 0) {
        clap_note_port_info info{};
        notes->get(m_plugin, 0, true, &info);
        m_midiNotes = !(info.supported_dialects & CLAP_NOTE_DIALECT_CLAP) && (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI);
    }
}

const void *ClapProcessor::hostExtension(const clap_host *, const char *id) {
    static const clap_host_log log{[](const clap_host *, clap_log_severity, const char *) {}};
    static const clap_host_thread_check threads{[](const clap_host *) { return isMainThread(); },
                                                [](const clap_host *) { return !isMainThread(); }};
    static const clap_host_params params{
        [](const clap_host *host, clap_param_rescan_flags) {
            ClapProcessor *p = self(host);
            p->onMain([p] { p->readParameters(); });
        },
        [](const clap_host *, clap_id, clap_param_clear_flags) {}, [](const clap_host *) {}};
    static const clap_host_state state{[](const clap_host *host) {
        ClapProcessor *p = self(host);
        p->onMain([p] {
            if (p->onEdited)
                p->onEdited();
        });
    }};
    static const clap_host_latency latency{[](const clap_host *) {}};
    static const clap_host_gui gui{
        [](const clap_host *) {},
        [](const clap_host *host, uint32_t width, uint32_t height) {
            ClapProcessor *p = self(host);
            if (!p->m_window)
                return false;
            p->m_window->resize(int(width), int(height));
            return true;
        },
        [](const clap_host *host) {
            QString error;
            return self(host)->showEditor(&error);
        },
        [](const clap_host *host) {
            ClapProcessor *p = self(host);
            if (p->m_window)
                p->m_window->hide();
            return true;
        },
        [](const clap_host *host, bool destroyed) {
            if (destroyed)
                self(host)->m_guiCreated = false;
        }};
    // Plugin windows run on dole's event loop: timers and file descriptors
    // the plugin hands over, answered when they fire, never polled.
    static const clap_host_timer_support timers{
        [](const clap_host *host, uint32_t period, clap_id *id) {
            ClapProcessor *p = self(host);
            const clap_id timer = p->m_nextTimer++;
            auto *object = new QTimer;
            object->moveToThread(p->m_main->thread());
            object->setInterval(int(std::max<uint32_t>(period, 10)));
            QObject::connect(object, &QTimer::timeout, p->m_main, [p, timer] {
                if (p->m_timers)
                    p->m_timers->on_timer(p->m_plugin, timer);
            });
            QMetaObject::invokeMethod(object, qOverload<>(&QTimer::start));
            p->m_timerObjects.insert(timer, object);
            *id = timer;
            return true;
        },
        [](const clap_host *host, clap_id id) {
            delete self(host)->m_timerObjects.take(id);
            return true;
        }};
    static const clap_host_posix_fd_support fds{
        [](const clap_host *host, int fd, clap_posix_fd_flags_t flags) {
            ClapProcessor *p = self(host);
            QList<QSocketNotifier *> notifiers;
            const std::pair<clap_posix_fd_flags_t, QSocketNotifier::Type> kinds[] = {
                {CLAP_POSIX_FD_READ, QSocketNotifier::Read},
                {CLAP_POSIX_FD_WRITE, QSocketNotifier::Write},
                {CLAP_POSIX_FD_ERROR, QSocketNotifier::Exception}};
            for (const auto &[flag, type] : kinds) {
                if (!(flags & flag))
                    continue;
                auto *notifier = new QSocketNotifier(fd, type);
                QObject::connect(notifier, &QSocketNotifier::activated, p->m_main, [p, fd, flag] {
                    if (p->m_fds)
                        p->m_fds->on_fd(p->m_plugin, fd, flag);
                });
                notifiers << notifier;
            }
            qDeleteAll(p->m_fdNotifiers.take(fd));
            p->m_fdNotifiers.insert(fd, notifiers);
            return true;
        },
        [](const clap_host *host, int fd, clap_posix_fd_flags_t flags) {
            return fds.register_fd(host, fd, flags);
        },
        [](const clap_host *host, int fd) {
            qDeleteAll(self(host)->m_fdNotifiers.take(fd));
            return true;
        }};

    if (!strcmp(id, CLAP_EXT_LOG))
        return &log;
    if (!strcmp(id, CLAP_EXT_THREAD_CHECK))
        return &threads;
    if (!strcmp(id, CLAP_EXT_PARAMS))
        return &params;
    if (!strcmp(id, CLAP_EXT_STATE))
        return &state;
    if (!strcmp(id, CLAP_EXT_LATENCY))
        return &latency;
    if (!strcmp(id, CLAP_EXT_GUI))
        return &gui;
    if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT))
        return &timers;
    if (!strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT))
        return &fds;
    return nullptr;
}

void ClapProcessor::process(float *left, float *right, int frames, const NoteEvent *notes, int noteCount,
                            const Transport &transport) {
    if (!m_active)
        return;
    if (!m_processing) {
        if (m_plugin->start_processing && !m_plugin->start_processing(m_plugin))
            return;
        m_processing = true;
    }

    m_events.clear();
    for (const Pending &pending : std::as_const(m_pending)) {
        Event event{};
        event.param = {{sizeof(clap_event_param_value), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0},
                       pending.id, pending.cookie, -1, -1, -1, -1, pending.value};
        m_events.push_back(event);
    }
    m_pending.clear();
    for (int n = 0; n < noteCount; ++n) {
        const NoteEvent &note = notes[n];
        Event event{};
        if (m_midiNotes) {
            event.midi = {{sizeof(clap_event_midi), uint32_t(note.frame), CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0},
                          0,
                          {uint8_t(note.on ? 0x90 : 0x80), uint8_t(note.key), uint8_t(note.on ? std::max(1, int(note.velocity * 127)) : 0)}};
        } else {
            event.note = {{sizeof(clap_event_note), uint32_t(note.frame), CLAP_CORE_EVENT_SPACE_ID,
                           uint16_t(note.on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0},
                          -1, 0, 0, int16_t(note.key), note.velocity};
        }
        m_events.push_back(event);
    }

    // The pad's stereo into the first input port; the rest stay silent.
    for (size_t p = 0; p < m_inData.size(); ++p)
        for (size_t c = 0; c < m_inData[p].size(); ++c) {
            float *data = m_inData[p][c].data();
            if (p == 0 && m_inData[p].size() == 1)
                for (int i = 0; i < frames; ++i)
                    data[i] = (left[i] + right[i]) / 2;
            else if (p == 0 && c < 2)
                memcpy(data, c == 0 ? left : right, frames * sizeof(float));
            else
                memset(data, 0, frames * sizeof(float));
        }

    m_transport = {};
    m_transport.header = {sizeof(clap_event_transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
    m_transport.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_TIME_SIGNATURE
                        | (transport.playing ? uint32_t(CLAP_TRANSPORT_IS_PLAYING) : 0u);
    m_transport.tempo = transport.bpm;
    m_transport.tsig_num = 4;
    m_transport.tsig_denom = 4;
    clap_process process{};
    process.steady_time = m_steadyTime;
    process.frames_count = uint32_t(frames);
    process.transport = &m_transport;
    process.audio_inputs = m_inBuffers.data();
    process.audio_inputs_count = uint32_t(m_inBuffers.size());
    process.audio_outputs = m_outBuffers.data();
    process.audio_outputs_count = uint32_t(m_outBuffers.size());
    process.in_events = &m_inEvents;
    process.out_events = &m_outEvents;
    const clap_process_status status = m_plugin->process(m_plugin, &process);
    m_steadyTime += frames;

    if (status == CLAP_PROCESS_ERROR || m_outData.empty())
        return;
    const auto &out = m_outData[0];
    memcpy(left, out[0].data(), frames * sizeof(float));
    memcpy(right, out[out.size() > 1 ? 1 : 0].data(), frames * sizeof(float));
}

QVector<Parameter> ClapProcessor::parameters() const {
    QVector<Parameter> parameters;
    for (const Param &p : m_paramInfo) {
        double value = p.value;
        if (m_params)
            m_params->get_value(m_plugin, p.id, &value);
        parameters << Parameter{QString::number(p.id), p.name, p.min, p.max, p.defaultValue, value, p.stepped, false};
    }
    return parameters;
}

void ClapProcessor::setParameter(const QString &id, double value) {
    for (Param &p : m_paramInfo)
        if (QString::number(p.id) == id) {
            p.value = qBound(p.min, value, p.max);
            m_pending << Pending{p.id, p.cookie, p.value};
        }
}

QString ClapProcessor::valueText(const QString &id, double value) const {
    char text[64] = {};
    if (m_params && m_params->value_to_text && m_params->value_to_text(m_plugin, clap_id(id.toUInt()), value, text, sizeof text))
        return QString::fromUtf8(text);
    return Processor::valueText(id, value);
}

QByteArray ClapProcessor::saveState() {
    QByteArray data;
    if (!m_state)
        return data;
    clap_ostream stream{&data, [](const clap_ostream *s, const void *buffer, uint64_t size) -> int64_t {
                            static_cast<QByteArray *>(s->ctx)->append(static_cast<const char *>(buffer), qsizetype(size));
                            return int64_t(size);
                        }};
    return m_state->save(m_plugin, &stream) ? data : QByteArray();
}

bool ClapProcessor::loadState(const QByteArray &state) {
    if (!m_state)
        return false;
    struct Reader {
        const QByteArray &data;
        qsizetype position;
    } reader{state, 0};
    clap_istream stream{&reader, [](const clap_istream *s, void *buffer, uint64_t size) -> int64_t {
                            auto *r = static_cast<Reader *>(s->ctx);
                            const qsizetype count = std::min<qsizetype>(qsizetype(size), r->data.size() - r->position);
                            memcpy(buffer, r->data.constData() + r->position, size_t(count));
                            r->position += count;
                            return count;
                        }};
    const bool loaded = m_state->load(m_plugin, &stream);
    readParameters();
    return loaded;
}

bool ClapProcessor::showEditor(QString *error) {
    if (!m_gui) {
        *error = "it has no window of its own";
        return false;
    }
    if (!m_guiCreated) {
        m_floating = m_gui->is_api_supported(m_plugin, CLAP_WINDOW_API_X11, true);
        if (!m_floating && qEnvironmentVariableIsEmpty("DISPLAY")) {
            *error = "plugin windows need XWayland, and DISPLAY isn't set";
            return false;
        }
        if (!m_gui->create(m_plugin, CLAP_WINDOW_API_X11, m_floating)) {
            *error = "it couldn't open its window";
            return false;
        }
        m_guiCreated = true;
        m_gui->set_scale(m_plugin, 1.0);
        if (m_floating) {
            m_gui->suggest_title(m_plugin, m_name.toUtf8().constData());
        } else {
            uint32_t width = 600, height = 400;
            m_gui->get_size(m_plugin, &width, &height);
            m_window = X11Window::create(m_name, int(width), int(height), error);
            if (!m_window) {
                closeEditor();
                return false;
            }
            QObject::connect(m_window.get(), &X11Window::closed, m_main, [this] {
                m_gui->hide(m_plugin);
                if (onEdited)
                    onEdited();
            });
            clap_window window{CLAP_WINDOW_API_X11, {}};
            window.x11 = m_window->id();
            if (!m_gui->set_parent(m_plugin, &window)) {
                *error = "it couldn't open its window";
                closeEditor();
                return false;
            }
        }
    }
    if (m_window)
        m_window->show();
    m_gui->show(m_plugin);
    return true;
}

void ClapProcessor::closeEditor() {
    if (m_guiCreated)
        m_gui->destroy(m_plugin);
    m_guiCreated = false;
    m_window.reset();
}

} // namespace

QVector<PluginInfo> scanClap() {
    QVector<PluginInfo> infos;
    for (const QString &path : clapFiles()) {
        QString error;
        const Library *lib = library(path, &error);
        if (!lib || !lib->factory)
            continue;
        for (uint32_t i = 0; i < lib->factory->get_plugin_count(lib->factory); ++i) {
            const clap_plugin_descriptor *d = lib->factory->get_plugin_descriptor(lib->factory, i);
            if (!d)
                continue;
            infos << PluginInfo{"clap:" + QString::fromUtf8(d->id), QString::fromUtf8(d->name),
                                QString::fromUtf8(d->vendor ? d->vendor : ""), "clap",
                                hasFeature(d, CLAP_PLUGIN_FEATURE_INSTRUMENT)};
            QMutexLocker lock(&librariesMutex);
            pathsById.insert(QString::fromUtf8(d->id), path);
        }
    }
    return infos;
}

std::unique_ptr<Processor> createClap(const QString &id, QString *error) {
    QString path;
    {
        QMutexLocker lock(&librariesMutex);
        path = pathsById.value(id);
    }
    if (path.isEmpty()) {
        scanClap();
        QMutexLocker lock(&librariesMutex);
        path = pathsById.value(id);
    }
    if (path.isEmpty()) {
        *error = "it isn't installed";
        return {};
    }
    const Library *lib = library(path, error);
    if (!lib || !lib->factory)
        return {};
    auto processor = std::make_unique<ClapProcessor>(lib, id, error);
    if (!processor->ok())
        return {};
    return processor;
}
