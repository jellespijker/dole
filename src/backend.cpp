#include "backend.h"

#include <QDir>
#include <QEventLoopLocker>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QStandardPaths>
#include <QtConcurrentRun>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <cmath>

#include "chop.h"
#include "rack.h"
#include "renderer.h"
#include "wav.h"

namespace {

constexpr int kMaxUndo = 500;
constexpr int kPeaks = 600;

bool validPad(int pad) {
    return pad >= 0 && pad < kPads;
}

bool valid(int pad, int step) {
    return validPad(pad) && step >= 0 && step < kMaxSteps;
}

// What a lane is called, for the built-in ones.
QString builtinLaneName(const QString &id) {
    if (id == "pitch")
        return QObject::tr("Pitch");
    if (id == "volume")
        return QObject::tr("Volume");
    if (id == "pan")
        return QObject::tr("Pan");
    if (id == "filter")
        return QObject::tr("Filter");
    if (id == "resonance")
        return QObject::tr("Resonance");
    if (id == "decay")
        return QObject::tr("Decay");
    return id;
}

const QStringList kBuiltinLanes{"pitch", "volume", "pan", "filter", "resonance", "decay"};

QString musicFolder() {
    const QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    return QDir(music).exists() ? music : QDir::homePath();
}

QString withSuffix(QString path, const QString &suffix) {
    if (QFileInfo(path).suffix().compare(suffix, Qt::CaseInsensitive) != 0)
        path += "." + suffix;
    return path;
}

// The pads as the pad list shows them: everything but plugin settings,
// which change with every slider move and would rebuild the whole grid.
std::array<Pad, kPads> outlines(const std::array<Pad, kPads> &pads) {
    std::array<Pad, kPads> result = pads;
    for (Pad &pad : result) {
        pad.synth.params.clear();
        pad.synth.state.clear();
        for (PluginSlot &effect : pad.effects) {
            effect.params.clear();
            effect.state.clear();
        }
    }
    return result;
}

QString sliceKey(const Pad &pad) {
    return QString("%1|%2|%3").arg(pad.sample).arg(pad.start).arg(pad.end);
}

} // namespace

Backend::Backend(Engine *engine, FilePicker *picker, QObject *parent)
    : QObject(parent), m_engine(engine), m_picker(picker), m_song(demoSong()), m_saved(m_song) {
    connect(m_engine, &Engine::positionChanged, this, &Backend::onPosition);
    connect(m_engine, &Engine::repeated, this, [this](int pad, int pattern, int step) {
        emit padHit(pad);
        if (m_recording && m_playing && m_playTick >= 0)
        {
            // The nearest step is the one heard or one ahead of it.
            const int length = m_song.patterns[pattern].length;
            record(pad, pattern, m_playTick + ((step - m_playStep) % length + length) % length, kDefaultLevel, false);
        }
    });
    // The meters fall back gently rather than flicker.
    m_meterTimer.setInterval(50);
    connect(&m_meterTimer, &QTimer::timeout, this, [this] {
        const std::array<float, kPads + 1> peaks = m_engine->takePeaks();
        QVariantList meters;
        for (int i = 0; i <= kPads; ++i)
            meters << std::max(double(peaks[i]), (i < m_meters.size() ? m_meters[i].toDouble() : 0.0) * 0.75);
        m_meters = meters;
        emit metersChanged();
    });
    connect(m_engine, &Engine::failed, this, [this](const QString &kind, const QString &detail) {
        setMessage(kind, {}, detail);
        if (m_playing)
            stop();
    });
    connect(m_picker, &FilePicker::failed, this, [this](const QString &why) {
        setMessage("pickerFailed", {}, why);
        emit saveAborted();
    });
    connect(m_picker, &FilePicker::cancelled, this, [this](FilePicker::Kind kind) {
        if (kind == FilePicker::Song)
            emit saveAborted();
    });
    connect(m_picker, &FilePicker::opened, this, [this](FilePicker::Kind kind, const QUrl &url) {
        if (kind == FilePicker::Song)
            openPath(url.toLocalFile());
        else if (kind == FilePicker::Sample && m_samplePad >= 0)
            setSample(m_samplePad, url.toLocalFile());
        else if (kind == FilePicker::Sample)
            openChop(url.toLocalFile());
    });
    connect(m_picker, &FilePicker::saveChosen, this, [this](FilePicker::Kind kind, const QUrl &url) {
        if (kind == FilePicker::Song)
            saveTo(withSuffix(url.toLocalFile(), "dole"));
        else if (kind == FilePicker::Audio)
            exportTo(withSuffix(url.toLocalFile(), "wav"));
    });
    sync();
}

QVariantList Backend::chain() const {
    QVariantList list;
    for (int p : m_song.chain)
        list << p;
    return list;
}

QVariantList Backend::usedPatterns() const {
    QVariantList list;
    for (const Pattern &pattern : m_song.patterns)
        list << !pattern.isEmpty();
    return list;
}

// The bank's sixteen rows of the pattern being edited: each step's level,
// roll, chance, and nudge, the track's own loop, and its lanes.
QVariantList Backend::grid() const {
    QVariantList rows;
    const Pattern &pattern = m_song.patterns[m_editPattern];
    for (int p = m_bank * kBankPads; p < (m_bank + 1) * kBankPads; ++p) {
        QVariantList levels, rolls, chances, nudges;
        const QVector<Step> &steps = pattern.steps[p];
        for (int i = 0; i < pattern.length; ++i) {
            levels << steps[i].level;
            rolls << steps[i].roll;
            chances << steps[i].chance;
            nudges << steps[i].nudge;
        }
        const int loop = m_song.pads[p].loop;
        QVariantList lanes;
        const Lanes padLanes = pattern.lanes.value(p);
        for (auto it = padLanes.begin(); it != padLanes.end(); ++it)
            lanes << laneOf(p, it.key(), it.value(), pattern.length);
        rows << QVariantMap{{"steps", levels},
                            {"roll", rolls},
                            {"chance", chances},
                            {"nudge", nudges},
                            {"loop", loop > 0 ? std::min(loop, pattern.length) : pattern.length},
                            {"lanes", lanes}};
    }
    return rows;
}

// A lane as the grid shows it: its name and range, and its value on each
// step, where one is set.
QVariantMap Backend::laneOf(int pad, const QString &id, const Lane &points, int length) const {
    const LaneInfo info = laneInfo(id);
    QString name = builtinLaneName(id);
    double min = info.min, max = info.max;
    bool stepped = info.stepped;
    if (info.target != "pad") {
        const Live live = m_live.value(info.target == "synth" ? slotKey(pad, -1) : slotKey(pad, info.effect));
        name = info.param;
        if (live.processor) {
            for (const Parameter &p : live.processor->parameters())
                if (p.id == info.param) {
                    name = p.name;
                    min = p.min;
                    max = p.max;
                    stepped = p.stepped;
                }
            name = live.processor->name() + ": " + name;
        }
    }
    QVariantList values;
    for (int i = 0; i < length; ++i)
        values << (points.contains(i) ? QVariant(points.value(i)) : QVariant());
    return {{"id", id}, {"name", name}, {"min", min}, {"max", max}, {"stepped", stepped}, {"values", values}};
}

QVariantList Backend::pads() const {
    QVariantList list;
    for (int p = 0; p < kPads; ++p) {
        const Pad &pad = m_song.pads[p];
        const QString state = pad.sample.isEmpty()              ? (p < kBankPads ? "kit" : "empty")
                              : m_samples.contains(pad.sample)  ? "ready"
                              : m_loading.contains(pad.sample)  ? "loading"
                                                                : "failed";
        list << QVariantMap{{"id", p < kBankPads ? kitIds()[p] : QString()},
                            {"name", padName(p)},
                            {"pan", pad.pan},
                            {"filter", pad.filter},
                            {"resonance", pad.resonance},
                            {"attack", pad.attack},
                            {"decay", pad.decay},
                            {"reverse", pad.reverse},
                            {"gate", pad.gate},
                            {"swing", pad.swing},
                            {"loop", pad.loop},
                            {"sample", QFileInfo(pad.sample).completeBaseName()},
                            {"chop", pad.isChop()},
                            {"state", state},
                            {"tune", pad.tune},
                            {"volume", pad.volume},
                            {"group", pad.group},
                            {"mute", pad.mute},
                            {"solo", pad.solo},
                            {"audible", m_song.audible(p)},
                            {"synth", pad.synth.isEmpty() ? QString() : pluginName(pad.synth.id)},
                            {"length", pad.length},
                            {"effects", pad.effects.size()}};
    }
    return list;
}

QString Backend::repeatName() const {
    return QString::fromLatin1(::repeatName(m_repeat ? m_repeat : m_lastRepeat));
}

QString Backend::chopName() const {
    return QFileInfo(m_chopPath).completeBaseName();
}

QString Backend::chopState() const {
    return m_chopPath.isEmpty()                ? "empty"
           : m_samples.contains(m_chopPath)    ? "ready"
           : m_loading.contains(m_chopPath)    ? "loading"
                                               : "failed";
}

QVariantList Backend::chopSlices() const {
    QVariantList list;
    const double length = duration(m_samples.value(m_chopPath));
    if (length > 0)
        for (double start : m_chopStarts)
            list << start / length;
    return list;
}

QString Backend::fileName() const {
    return m_path.isEmpty() ? QString() : QFileInfo(m_path).completeBaseName();
}

void Backend::apply(const Song &next, const QString &coalesce) {
    if (next == m_song)
        return;
    if (coalesce.isEmpty() || coalesce != m_coalesce) {
        m_undo << m_song;
        if (m_undo.size() > kMaxUndo)
            m_undo.removeFirst();
    }
    m_coalesce = coalesce;
    m_redo.clear();
    // Only what changed is announced: a slider on a plugin moves often, and
    // rebuilding the grid and the pads for it would make it stutter.
    const bool grid = next.patterns != m_song.patterns;
    const bool pads = outlines(next.pads) != outlines(m_song.pads) || next.master.size() != m_song.master.size();
    m_song = next;
    m_engine->setSong(m_song);
    syncSounds();
    syncRack();
    emit songChanged();
    if (grid)
        emit gridChanged();
    if (pads) {
        emit padsChanged();
        emit rackChanged();
    }
}

void Backend::sync() {
    m_engine->setSong(m_song);
    syncSounds();
    syncRack();
    emit songChanged();
    emit gridChanged();
    emit padsChanged();
    emit rackChanged();
}

QString Backend::slotKey(int target, int index) {
    return target < 0 ? QString("m%1").arg(index) : index < 0 ? QString("s%1").arg(target) : QString("e%1.%2").arg(target).arg(index);
}

QString Backend::pluginName(const QString &id) const {
    for (const PluginInfo &info : m_plugins)
        if (info.id == id)
            return info.name;
    for (const Live &live : m_live)
        if (live.applied.id == id && live.processor)
            return live.processor->name();
    return id.section(':', 1).section('/', -1).section('#', -1).isEmpty() ? id
                                                                         : id.section(':', 1).section('/', -1).section('#', -1);
}

QVector<PluginSlot> &Backend::chainOf(Song &song, int target) const {
    return target < 0 ? song.master : song.pads[target].effects;
}

PluginSlot *Backend::slotAt(Song &song, int target, int index) const {
    if (target >= kPads || target < -1)
        return nullptr;
    if (index < 0)
        return target >= 0 && !song.pads[target].synth.isEmpty() ? &song.pads[target].synth : nullptr;
    QVector<PluginSlot> &chain = chainOf(song, target);
    return index < chain.size() ? &chain[index] : nullptr;
}

// Keeps the plugins playing in step with the song: one made for each slot,
// settings passed on as they change, and the ones no longer used let go.
void Backend::syncRack() {
    Rack rack;
    QSet<QString> used;
    auto use = [&](const QString &key, const PluginSlot &slot) -> std::shared_ptr<Processor> {
        used << key;
        Live &live = m_live[key];
        if (live.applied.id != slot.id) {
            live = Live{slot, nullptr, {}};
            QString error;
            live.processor = createProcessor(slot, &error);
            if (!live.processor) {
                live.error = error;
                setMessage("pluginFailed", pluginName(slot.id), error);
            } else {
                live.processor->onEdited = [this, key] { pullPlugin(key); };
            }
            return live.processor;
        }
        if (live.processor && live.applied != slot) {
            const PluginSlot old = live.applied;
            std::shared_ptr<Processor> processor = live.processor;
            m_engine->locked([&] {
                if (slot.state != old.state && !slot.state.isEmpty())
                    processor->loadState(slot.state);
                // A parameter no longer set (undone) goes back to its default.
                for (const Parameter &p : processor->parameters())
                    if (old.params.contains(p.id) && !slot.params.contains(p.id))
                        processor->setParameter(p.id, p.defaultValue);
                for (auto it = slot.params.begin(); it != slot.params.end(); ++it)
                    if (old.params.value(it.key(), std::nan("")) != it.value())
                        processor->setParameter(it.key(), it.value());
            });
        }
        live.applied = slot;
        return live.processor;
    };
    for (int p = 0; p < kPads; ++p) {
        const Pad &pad = m_song.pads[p];
        if (!pad.synth.isEmpty())
            rack.synths[p] = use(slotKey(p, -1), pad.synth);
        for (int i = 0; i < pad.effects.size(); ++i)
            if (auto processor = use(slotKey(p, i), pad.effects[i]))
                rack.effects[p] << RackSlot{processor, pad.effects[i].bypass, i};
    }
    for (int i = 0; i < m_song.master.size(); ++i)
        if (auto processor = use(slotKey(-1, i), m_song.master[i]))
            rack.master << RackSlot{processor, m_song.master[i].bypass, i};
    m_engine->setRack(rack);
    // Let go of the rest only now that the audio thread has, on this thread.
    for (auto it = m_live.begin(); it != m_live.end();)
        it = used.contains(it.key()) ? std::next(it) : m_live.erase(it);
}

void Backend::pullPlugin(const QString &key) {
    const auto it = m_live.constFind(key);
    if (it == m_live.constEnd() || !it->processor)
        return;
    Song next = m_song;
    PluginSlot *slot = nullptr;
    if (key.startsWith('s'))
        slot = &next.pads[key.mid(1).toInt()].synth;
    else if (key.startsWith('m'))
        slot = &next.master[key.mid(1).toInt()];
    else
        slot = &next.pads[key.mid(1).section('.', 0, 0).toInt()].effects[key.section('.', 1).toInt()];
    slot->state = it->processor->saveState();
    // The sliders' values follow what the window set.
    for (const Parameter &p : it->processor->parameters())
        if (slot->params.contains(p.id))
            slot->params[p.id] = p.value;
    m_live[key].applied = *slot;
    apply(next, "editor " + key);
}

void Backend::pullPlugins() {
    for (const QString &key : m_live.keys())
        pullPlugin(key);
}

QVariantList Backend::plugins() const {
    QVariantList list;
    for (const PluginInfo &info : m_plugins)
        list << QVariantMap{{"id", info.id}, {"name", info.name}, {"vendor", info.vendor},
                            {"format", info.format}, {"instrument", info.instrument}};
    return list;
}

QVariantMap Backend::rack() const {
    auto describe = [this](const QString &key, const PluginSlot &slot) {
        const Live live = m_live.value(key);
        QVariantList params;
        if (live.processor)
            for (const Parameter &p : live.processor->parameters()) {
                const double value = slot.params.value(p.id, p.value);
                params << QVariantMap{{"id", p.id},           {"name", p.name},           {"value", value},
                                      {"min", p.min},         {"max", p.max},             {"default", p.defaultValue},
                                      {"stepped", p.stepped}, {"log", p.logarithmic},
                                      {"text", live.processor->valueText(p.id, value)}};
            }
        return QVariantMap{{"id", slot.id},
                           {"name", pluginName(slot.id)},
                           {"format", slot.id.contains(':') ? slot.id.section(':', 0, 0) : QString("dole")},
                           {"bypass", slot.bypass},
                           {"editor", live.processor && live.processor->hasEditor()},
                           {"error", live.error},
                           {"params", params}};
    };
    QVariantMap map{{"target", m_rackTarget}};
    QVariantList effects;
    const QVector<PluginSlot> &chain = m_rackTarget < 0 ? m_song.master : m_song.pads[m_rackTarget].effects;
    for (int i = 0; i < chain.size(); ++i)
        effects << describe(slotKey(m_rackTarget, i), chain[i]);
    map.insert("effects", effects);
    if (m_rackTarget >= 0) {
        const Pad &pad = m_song.pads[m_rackTarget];
        map.insert("synth", pad.synth.isEmpty() ? QVariant() : QVariant(describe(slotKey(m_rackTarget, -1), pad.synth)));
        map.insert("length", pad.length);
    }
    return map;
}

void Backend::setRackTarget(int target) {
    m_rackTarget = qBound(-1, target, kPads - 1);
    emit rackChanged();
}

void Backend::setScanCommand(const QString &program, const QStringList &arguments) {
    m_scanProgram = program;
    m_scanArguments = arguments;
}

void Backend::scanPlugins() {
    if (m_scanning || m_scanned || m_scanProgram.isEmpty())
        return;
    m_scanning = true;
    emit pluginsChanged();
    // Loading a plugin runs its code; a broken one can crash, so the scan
    // runs in a process of its own and only it goes down.
    auto *scan = new QProcess(this);
    connect(scan, &QProcess::finished, this, [this, scan](int code, QProcess::ExitStatus status) {
        scan->deleteLater();
        m_scanning = false;
        m_scanned = true;
        const QJsonArray found = QJsonDocument::fromJson(scan->readAllStandardOutput()).object().value("plugins").toArray();
        if (status != QProcess::NormalExit || code != 0 || found.isEmpty()) {
            setMessage("scanFailed", {}, QString::fromUtf8(scan->readAllStandardError()).trimmed().section('\n', -1));
        } else {
            m_plugins.clear();
            for (const QJsonValue &value : found) {
                const QJsonObject o = value.toObject();
                m_plugins << PluginInfo{o.value("id").toString(), o.value("name").toString(), o.value("vendor").toString(),
                                        o.value("format").toString(), o.value("instrument").toBool()};
            }
        }
        emit pluginsChanged();
        emit padsChanged();
        emit rackChanged();
    });
    connect(scan, &QProcess::errorOccurred, this, [this, scan](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        scan->deleteLater();
        m_scanning = false;
        setMessage("scanFailed", {}, scan->errorString());
        emit pluginsChanged();
    });
    scan->start(m_scanProgram, m_scanArguments);
}

void Backend::addEffect(int target, const QString &id) {
    if (target < -1 || target >= kPads || id.isEmpty())
        return;
    Song next = m_song;
    QVector<PluginSlot> &chain = chainOf(next, target);
    if (chain.size() >= kMaxEffects) {
        setMessage("chainFull", QString::number(kMaxEffects));
        return;
    }
    chain << PluginSlot{id, {}, {}, false};
    apply(next);
}

void Backend::removeEffect(int target, int index) {
    if (target < -1 || target >= kPads)
        return;
    Song next = m_song;
    if (index < 0) {
        if (target >= 0) {
            next.pads[target].synth = PluginSlot{};
            moveLanes(next, target, [](const QString &id) { return id.startsWith("synth.") ? QString() : id; });
        }
    } else {
        QVector<PluginSlot> &chain = chainOf(next, target);
        if (index >= chain.size())
            return;
        chain.removeAt(index);
        // The effects after it move up, and their lanes with them.
        if (target >= 0)
            moveLanes(next, target, [index](const QString &id) {
                const LaneInfo info = laneInfo(id);
                if (info.target != "fx" || info.effect < index)
                    return id;
                return info.effect == index ? QString() : QString("fx%1.%2").arg(info.effect).arg(info.param);
            });
    }
    apply(next);
}

void Backend::moveEffect(int target, int index, int delta) {
    if (target < -1 || target >= kPads)
        return;
    Song next = m_song;
    QVector<PluginSlot> &chain = chainOf(next, target);
    const int to = index + delta;
    if (index < 0 || index >= chain.size() || to < 0 || to >= chain.size())
        return;
    chain.move(index, to);
    if (target >= 0)
        moveLanes(next, target, [index, to](const QString &id) {
            const LaneInfo info = laneInfo(id);
            if (info.target != "fx" || (info.effect != index && info.effect != to))
                return id;
            return QString("fx%1.%2").arg((info.effect == index ? to : index) + 1).arg(info.param);
        });
    apply(next);
}

void Backend::toggleBypass(int target, int index) {
    Song next = m_song;
    if (PluginSlot *slot = slotAt(next, target, index)) {
        slot->bypass = !slot->bypass;
        apply(next);
    }
}

void Backend::setSynth(int pad, const QString &id) {
    if (!validPad(pad))
        return;
    Song next = m_song;
    next.pads[pad].synth = id.isEmpty() ? PluginSlot{} : PluginSlot{id, {}, {}, false};
    // Another synth has other parameters.
    moveLanes(next, pad, [](const QString &lane) { return lane.startsWith("synth.") ? QString() : lane; });
    apply(next);
}

void Backend::cycleLength(int pad) {
    if (!validPad(pad))
        return;
    Song next = m_song;
    const int *at = std::find(std::begin(kLengths), std::end(kLengths), next.pads[pad].length);
    next.pads[pad].length = at + 1 < std::end(kLengths) ? *(at + 1) : kLengths[0];
    apply(next);
}

QString Backend::setParameter(int target, int index, const QString &id, double value) {
    Song next = m_song;
    PluginSlot *slot = slotAt(next, target, index);
    if (!slot)
        return {};
    slot->params[id] = value;
    apply(next, QString("param %1 %2").arg(slotKey(target, index), id));
    const Live live = m_live.value(slotKey(target, index));
    return live.processor ? live.processor->valueText(id, value) : QString::number(value);
}

void Backend::openEditor(int target, int index) {
    const Live live = m_live.value(slotKey(target, index));
    if (!live.processor)
        return;
    QString error;
    if (!live.processor->showEditor(&error))
        setMessage("editorFailed", live.processor->name(), error);
}

SampleBuffer Backend::padSound(const Pad &pad) const {
    const SampleBuffer whole = m_samples.value(pad.sample);
    if (!whole || !pad.isChop())
        return whole;
    const QString key = sliceKey(pad);
    if (!m_slices.contains(key))
        m_slices.insert(key, slice(whole, pad.start, pad.end));
    return m_slices.value(key);
}

std::array<SampleBuffer, kPads> Backend::sounds() const {
    std::array<SampleBuffer, kPads> sounds;
    for (int p = 0; p < kPads; ++p) {
        const Pad &pad = m_song.pads[p];
        sounds[p] = pad.sample.isEmpty() ? (p < kBankPads ? kitSound(p) : SampleBuffer()) : padSound(pad);
    }
    return sounds;
}

void Backend::syncSounds() {
    for (const Pad &pad : m_song.pads)
        if (!pad.sample.isEmpty() && !m_samples.contains(pad.sample) && !m_loading.contains(pad.sample)
            && !m_failed.contains(pad.sample))
            loadSample(pad.sample);
    m_engine->setSounds(sounds());
}

void Backend::loadSample(const QString &path) {
    m_loading.insert(path);
    auto *watcher = new QFutureWatcher<QPair<SampleBuffer, QString>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, path] {
        watcher->deleteLater();
        const auto [sound, error] = watcher->result();
        m_loading.remove(path);
        if (sound) {
            m_samples.insert(path, sound);
        } else {
            m_failed.insert(path, error);
            if (error == kNoFfmpeg)
                setMessage("noFfmpeg");
            else
                setMessage("sampleFailed", QFileInfo(path).fileName(), error);
        }
        sampleLoaded(path);
    });
    // ffmpeg can take a while on a long file, so it decodes off the GUI thread.
    watcher->setFuture(QtConcurrent::run([path] {
        QString error;
        SampleBuffer sound = decodeSample(path, &error);
        return qMakePair(sound, error);
    }));
}

void Backend::sampleLoaded(const QString &path) {
    m_engine->setSounds(sounds());
    emit padsChanged();
    if (path != m_chopPath)
        return;
    const SampleBuffer sound = m_samples.value(path);
    m_chopPeaks.clear();
    for (float peak : peaks(sound, kPeaks))
        m_chopPeaks << peak;
    // Where it hits is usually where a producer would cut it.
    m_chopStarts = transientSlices(sound, 8);
    if (m_chopStarts.size() < 2)
        m_chopStarts = evenSlices(duration(sound), 8);
    emit chopChanged();
}

void Backend::setMessage(const QString &kind, const QString &arg, const QString &detail) {
    m_message = {{"kind", kind}, {"arg", arg}, {"detail", detail}};
    emit messageChanged();
}

void Backend::dismissMessage() {
    if (m_message.isEmpty())
        return;
    m_message.clear();
    emit messageChanged();
}

void Backend::toggleStep(int pad, int step) {
    if (!valid(pad, step))
        return;
    Song next = m_song;
    Step &s = next.patterns[m_editPattern].steps[pad][step];
    s.level = s.on() ? 0 : kDefaultLevel;
    if (s.on() && !m_playing)
        m_engine->hit(pad, s.level);
    selectPad(pad);
    apply(next);
}

void Backend::clearStep(int pad, int step) {
    if (!valid(pad, step))
        return;
    Song next = m_song;
    next.patterns[m_editPattern].steps[pad][step] = Step{};
    apply(next);
}

void Backend::changeStep(int pad, int step, int delta) {
    if (!valid(pad, step))
        return;
    Song next = m_song;
    Step &s = next.patterns[m_editPattern].steps[pad][step];
    if (!s.on())
        return;
    s.level = qBound(1, s.level + delta, kLevels);
    if (!m_playing)
        m_engine->hit(pad, s.level);
    apply(next, QString("step %1 %2 %3").arg(m_editPattern).arg(pad).arg(step));
}

void Backend::selectPattern(int pattern) {
    if (pattern < 0 || pattern >= kPatterns || pattern == m_editPattern)
        return;
    m_editPattern = pattern;
    m_coalesce.clear();
    if (m_playing && !m_songMode)
        m_engine->cue(pattern);
    emit gridChanged();
}

void Backend::copyPattern() {
    m_clipboard = m_song.patterns[m_editPattern];
    m_hasClipboard = true;
    setMessage("copied", patternName(m_editPattern));
}

void Backend::pastePattern() {
    if (!m_hasClipboard)
        return;
    Song next = m_song;
    next.patterns[m_editPattern] = m_clipboard;
    apply(next);
    setMessage("pasted", patternName(m_editPattern));
}

void Backend::clearPattern() {
    Song next = m_song;
    next.patterns[m_editPattern] = Pattern{};
    apply(next);
}

void Backend::chainInsert(int index, int pattern) {
    if (pattern < 0 || pattern >= kPatterns || m_song.chain.size() >= kMaxChain)
        return;
    Song next = m_song;
    next.chain.insert(qBound(0, index, int(next.chain.size())), pattern);
    apply(next);
}

void Backend::chainSet(int index, int pattern) {
    if (pattern < 0 || pattern >= kPatterns || index < 0 || index >= m_song.chain.size())
        return;
    Song next = m_song;
    next.chain[index] = pattern;
    apply(next);
}

void Backend::chainRemove(int index) {
    if (index < 0 || index >= m_song.chain.size())
        return;
    Song next = m_song;
    next.chain.removeAt(index);
    apply(next);
}

void Backend::changeBpm(int delta) {
    Song next = m_song;
    next.bpm = qBound(kMinBpm, next.bpm + delta, kMaxBpm);
    apply(next, "bpm");
}

void Backend::changeSwing(int delta) {
    Song next = m_song;
    next.swing = qBound(kMinSwing, next.swing + delta, kMaxSwing);
    apply(next, "swing");
}

void Backend::cycleGrit() {
    Song next = m_song;
    next.grit = Grit((int(next.grit) + 1) % 3);
    apply(next);
}

void Backend::toggleVinyl() {
    Song next = m_song;
    next.vinyl = !next.vinyl;
    apply(next);
}

void Backend::padDown(int pad, int level) {
    if (!validPad(pad))
        return;
    selectPad(pad);
    m_engine->hit(pad, level);
    emit padHit(pad);
    if (m_repeat > 0) {
        m_held.insert(pad);
        m_engine->hold(pad, true, level);
    }
    if (!m_recording || !m_playing || m_playTick < 0)
        return;
    // Recorded on the nearest step: the one heard last, or the next if the
    // hit came more than halfway to it.
    const double stepMs = 60000.0 / m_song.bpm / 4;
    const bool late = m_sinceStep.elapsed() > stepMs / 2;
    record(pad, m_playPattern, m_playTick + (late ? 1 : 0), level, late);
}

void Backend::padUp(int pad) {
    if (!validPad(pad))
        return;
    m_engine->release(pad);
    if (!m_held.remove(pad))
        return;
    m_engine->hold(pad, false, 0);
}

void Backend::releaseAll() {
    for (int pad : std::as_const(m_held))
        m_engine->hold(pad, false, 0);
    m_held.clear();
}

int Backend::rowStep(int pad, int pattern, int tick) const {
    const int loop = m_song.pads[pad].loop;
    return tick % std::max(1, loop > 0 ? loop : m_song.patterns[pattern].length);
}

void Backend::record(int pad, int pattern, int tick, int level, bool suppress) {
    if (pattern < 0 || pattern >= kPatterns || !validPad(pad) || tick < 0)
        return;
    const int step = rowStep(pad, pattern, tick);
    // The step after the one heard hasn't played yet; it was heard just now,
    // live, so the sequence skips it this time round.
    if (suppress)
        m_engine->suppress(pad, pattern, step);
    Song next = m_song;
    Step &s = next.patterns[pattern].steps[pad][step];
    s.level = std::max(s.level, level);
    // A whole take undoes at once.
    apply(next, "record");
}

void Backend::selectPad(int pad) {
    if (!validPad(pad) || pad == m_selectedPad)
        return;
    m_selectedPad = pad;
    // The grid shows the bank the pad is in.
    if (pad / kBankPads != m_bank) {
        m_bank = pad / kBankPads;
        emit gridChanged();
    }
    emit selectedPadChanged();
}

void Backend::setBank(int bank) {
    if (bank < 0 || bank >= kBanks || bank == m_bank)
        return;
    m_bank = bank;
    m_selectedPad = bank * kBankPads + m_selectedPad % kBankPads;
    emit gridChanged();
    emit selectedPadChanged();
}

void Backend::toggleMute(int pad) {
    if (!validPad(pad))
        return;
    Song next = m_song;
    next.pads[pad].mute = !next.pads[pad].mute;
    apply(next);
}

void Backend::toggleSolo(int pad) {
    if (!validPad(pad))
        return;
    Song next = m_song;
    next.pads[pad].solo = !next.pads[pad].solo;
    apply(next);
}

void Backend::changeVolume(int pad, double delta) {
    if (!validPad(pad))
        return;
    Song next = m_song;
    next.pads[pad].volume = qBound(0.0, qRound((next.pads[pad].volume + delta) * 100) / 100.0, 1.0);
    apply(next, QString("volume %1").arg(pad));
}

void Backend::changeTune(int pad, int delta) {
    if (!validPad(pad))
        return;
    Song next = m_song;
    next.pads[pad].tune = qBound(kMinTune, next.pads[pad].tune + delta, kMaxTune);
    apply(next, QString("tune %1").arg(pad));
    if (!m_playing)
        m_engine->hit(pad, kDefaultLevel);
}

void Backend::cycleGroup(int pad) {
    if (!validPad(pad))
        return;
    Song next = m_song;
    next.pads[pad].group = (next.pads[pad].group + 1) % (kGroups + 1);
    apply(next);
}

void Backend::chooseSample(int pad) {
    if (!validPad(pad))
        return;
    m_samplePad = pad;
    m_picker->open(FilePicker::Sample);
}

void Backend::resetPad(int pad) {
    if (!validPad(pad))
        return;
    Song next = m_song;
    next.pads[pad].sample.clear();
    next.pads[pad].start = next.pads[pad].end = 0;
    next.pads[pad].tune = 0;
    next.pads[pad].group = Song::defaultPads()[pad].group;
    apply(next);
}

void Backend::setSample(int pad, const QString &path) {
    if (!validPad(pad))
        return;
    // Picking a sample that failed before tries it again: the file may be fixed.
    m_failed.remove(path);
    Song next = m_song;
    next.pads[pad].sample = path.isEmpty() ? QString() : QFileInfo(path).absoluteFilePath();
    next.pads[pad].start = next.pads[pad].end = 0;
    apply(next);
}

void Backend::chooseChop() {
    m_samplePad = -1;
    m_picker->open(FilePicker::Sample);
}

void Backend::openChop(const QString &path) {
    m_chopPath = QFileInfo(path).absoluteFilePath();
    m_chopPeaks.clear();
    m_chopStarts.clear();
    m_failed.remove(m_chopPath);
    if (m_samples.contains(m_chopPath)) {
        sampleLoaded(m_chopPath);
        return;
    }
    if (!m_loading.contains(m_chopPath))
        loadSample(m_chopPath);
    emit chopChanged();
}

void Backend::sliceEven(int count) {
    const SampleBuffer sound = m_samples.value(m_chopPath);
    if (!sound || count < 1)
        return;
    m_chopStarts = evenSlices(duration(sound), std::min(count, kBankPads));
    emit chopChanged();
}

void Backend::sliceTransients() {
    const SampleBuffer sound = m_samples.value(m_chopPath);
    if (!sound)
        return;
    m_chopStarts = transientSlices(sound, kBankPads);
    emit chopChanged();
}

void Backend::previewSlice(int index) {
    const SampleBuffer sound = m_samples.value(m_chopPath);
    if (!sound || index < 0 || index >= m_chopStarts.size())
        return;
    const double end = index + 1 < m_chopStarts.size() ? m_chopStarts[index + 1] : 0;
    m_engine->preview(slice(sound, m_chopStarts[index], end), 1.0);
}

int Backend::assignChops(int firstPad) {
    if (!m_samples.contains(m_chopPath) || !validPad(firstPad))
        return 0;
    Song next = m_song;
    int count = 0;
    // The chops fill the bank from the pad chosen, and stop at its end.
    const int bankEnd = (firstPad / kBankPads + 1) * kBankPads;
    for (int i = 0; i < m_chopStarts.size() && firstPad + i < bankEnd; ++i, ++count) {
        Pad &pad = next.pads[firstPad + i];
        pad.sample = m_chopPath;
        pad.start = m_chopStarts[i];
        pad.end = i + 1 < m_chopStarts.size() ? m_chopStarts[i + 1] : duration(m_samples.value(m_chopPath));
        pad.tune = 0;
        pad.group = 0;
    }
    apply(next);
    setMessage("chopped", QString::number(count), QString::number(firstPad + 1));
    return count;
}

void Backend::togglePlay(int chainStart) {
    if (m_playing) {
        stop();
        return;
    }
    m_engine->play(m_songMode && !m_song.chain.isEmpty(), m_editPattern, chainStart);
    m_playing = true;
    emit playingChanged();
}

void Backend::stop() {
    if (!m_playing)
        return;
    m_engine->stop();
    m_playing = false;
    m_recording = false;
    m_coalesce.clear();
    m_playStep = m_playPattern = m_playChainIndex = m_playTick = -1;
    emit playingChanged();
    emit positionChanged();
}

void Backend::toggleRecord(int chainStart) {
    m_recording = !m_recording;
    m_coalesce.clear();
    // Recording from a stop starts playing, as REC and PLAY do together.
    if (m_recording && !m_playing)
        togglePlay(chainStart);
    else
        emit playingChanged();
}

void Backend::toggleSongMode() {
    m_songMode = !m_songMode;
    emit songModeChanged();
    if (m_playing)
        m_engine->play(m_songMode && !m_song.chain.isEmpty(), m_editPattern, 0);
}

void Backend::toggleRepeat() {
    m_repeat = m_repeat ? 0 : m_lastRepeat;
    m_engine->setRepeat(m_repeat);
    if (!m_repeat)
        releaseAll();
    emit repeatChanged();
}

void Backend::cycleRepeat() {
    m_lastRepeat = m_lastRepeat % (kRepeatDivisions - 1) + 1;
    if (m_repeat) {
        m_repeat = m_lastRepeat;
        m_engine->setRepeat(m_repeat);
    }
    emit repeatChanged();
}

void Backend::onPosition(int chainIndex, int pattern, int step) {
    // A step already on its way when playing stopped.
    if (!m_playing)
        return;
    m_sinceStep.start();
    // The tracks with a loop of their own count from when the pattern came
    // in, as the renderer does: a pattern that carries on keeps counting.
    if (m_playTick < 0 || pattern != m_playPattern) {
        m_playTick = step;
    } else {
        const int length = m_song.patterns[pattern].length;
        m_playTick += ((step - m_playStep) % length + length) % length;
    }
    m_playChainIndex = chainIndex;
    m_playPattern = pattern;
    m_playStep = step;
    emit positionChanged();
    const Pattern &playing = m_song.patterns[pattern];
    for (int pad = 0; pad < kPads; ++pad)
        if (playing.steps[pad][rowStep(pad, pattern, m_playTick)].on() && m_song.audible(pad))
            emit padHit(pad);
    // Playing the song, the grid follows it.
    if (m_songMode && !m_song.chain.isEmpty() && pattern != m_editPattern) {
        m_editPattern = pattern;
        emit gridChanged();
    }
}

void Backend::replaceSong(const Song &song, const QString &path) {
    stop();
    m_song = m_saved = song;
    m_undo.clear();
    m_redo.clear();
    m_coalesce.clear();
    m_editPattern = 0;
    m_bank = 0;
    m_selectedPad = 0;
    m_path = path;
    emit fileChanged();
    sync();
}

void Backend::newSong() {
    replaceSong(Song{}, {});
}

void Backend::openSong() {
    m_picker->open(FilePicker::Song);
}

bool Backend::openPath(const QString &path) {
    Song song;
    QString error;
    if (!loadSong(path, &song, &error)) {
        setMessage("openFailed", QFileInfo(path).fileName(), error);
        return false;
    }
    // A sample that failed before gets another go with the song that uses it.
    for (const Pad &pad : song.pads)
        m_failed.remove(pad.sample);
    replaceSong(song, QFileInfo(path).absoluteFilePath());
    dismissMessage();
    return true;
}

void Backend::save() {
    if (m_path.isEmpty())
        saveAs();
    else
        saveTo(m_path);
}

void Backend::saveAs() {
    const QString suggested = m_path.isEmpty() ? musicFolder() + "/" + tr("Untitled") + ".dole" : m_path;
    m_picker->save(FilePicker::Song, QUrl::fromLocalFile(suggested));
}

bool Backend::saveTo(const QString &path) {
    pullPlugins();
    QString error;
    if (!saveSong(path, m_song, &error)) {
        setMessage("saveFailed", QFileInfo(path).fileName(), error);
        emit saveAborted();
        return false;
    }
    m_saved = m_song;
    if (m_path != QFileInfo(path).absoluteFilePath()) {
        m_path = QFileInfo(path).absoluteFilePath();
        emit fileChanged();
    }
    setMessage("saved", QFileInfo(path).fileName());
    emit songChanged();
    emit saved();
    return true;
}

void Backend::exportAudio() {
    if (m_exporting)
        return;
    const QString base = m_path.isEmpty() ? musicFolder() + "/" + tr("Untitled")
                                          : QFileInfo(m_path).absolutePath() + "/" + fileName();
    m_picker->save(FilePicker::Audio, QUrl::fromLocalFile(base + ".wav"));
}

void Backend::exportTo(const QString &path) {
    if (m_exporting)
        return;
    m_exporting = true;
    emit exportingChanged();
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, path] {
        watcher->deleteLater();
        const QString error = watcher->result();
        m_exporting = false;
        emit exportingChanged();
        if (error.isEmpty()) {
            setMessage("exported", QFileInfo(path).fileName());
            emit exported();
        } else {
            setMessage("exportFailed", QFileInfo(path).fileName(), error);
        }
    });
    // The export plays through plugins of its own, made here, from the
    // song's settings, so the ones playing live carry on undisturbed. They
    // are let go here too, when the export is done.
    pullPlugins();
    QStringList problems;
    auto rack = std::make_shared<Rack>(buildRack(m_song, &problems));
    connect(watcher, &QFutureWatcherBase::finished, this, [rack] {});
    // The export finishes even if the window closes first: the locker keeps
    // the app running, windowless, until the file is written.
    auto locker = std::make_shared<QEventLoopLocker>();
    watcher->setFuture(QtConcurrent::run([song = m_song, sounds = sounds(), rack = rack.get(), pattern = m_editPattern, path, locker] {
        QString error;
        writeWav(path, renderSong(song, sounds, *rack, pattern), &error);
        return error;
    }));
}

void Backend::undo() {
    if (m_undo.isEmpty())
        return;
    m_redo << m_song;
    m_song = m_undo.takeLast();
    m_coalesce.clear();
    sync();
}

void Backend::redo() {
    if (m_redo.isEmpty())
        return;
    m_undo << m_song;
    m_song = m_redo.takeLast();
    m_coalesce.clear();
    sync();
}

void Backend::cycleRoll(int pad, int step) {
    if (!valid(pad, step))
        return;
    Song next = m_song;
    Step &s = next.patterns[m_editPattern].steps[pad][step];
    if (!s.on())
        return;
    s.roll = s.roll % kMaxRoll + 1;
    apply(next);
}

void Backend::cycleChance(int pad, int step) {
    if (!valid(pad, step))
        return;
    Song next = m_song;
    Step &s = next.patterns[m_editPattern].steps[pad][step];
    if (!s.on())
        return;
    // Always, then three times in four, half, a quarter, and back.
    s.chance = s.chance > 75 ? 75 : s.chance > 50 ? 50 : s.chance > 25 ? 25 : 100;
    apply(next);
}

void Backend::nudgeStep(int pad, int step, int delta) {
    if (!valid(pad, step))
        return;
    Song next = m_song;
    Step &s = next.patterns[m_editPattern].steps[pad][step];
    if (!s.on())
        return;
    s.nudge = qBound(-kMaxNudge, s.nudge + delta, kMaxNudge);
    apply(next, QString("nudge %1 %2 %3").arg(m_editPattern).arg(pad).arg(step));
}

void Backend::cyclePatternLength() {
    Song next = m_song;
    Pattern &pattern = next.patterns[m_editPattern];
    pattern.length = pattern.length % kMaxSteps + kSteps;
    apply(next);
}

void Backend::moveLanes(Song &song, int pad, const std::function<QString(const QString &)> &rename) const {
    for (Pattern &pattern : song.patterns) {
        if (!pattern.lanes.contains(pad))
            continue;
        Lanes moved;
        const Lanes lanes = pattern.lanes.value(pad);
        for (auto it = lanes.begin(); it != lanes.end(); ++it) {
            const QString id = rename(it.key());
            if (!id.isEmpty())
                moved.insert(id, it.value());
        }
        if (moved.isEmpty())
            pattern.lanes.remove(pad);
        else
            pattern.lanes[pad] = moved;
    }
}

QVariantList Backend::laneChoices(int pad) const {
    QVariantList list;
    if (!validPad(pad))
        return list;
    const Lanes taken = m_song.patterns[m_editPattern].lanes.value(pad);
    auto add = [&](const QString &id, const QString &name, const QString &group) {
        if (!taken.contains(id))
            list << QVariantMap{{"id", id}, {"name", name}, {"group", group}};
    };
    for (const QString &id : kBuiltinLanes)
        add(id, builtinLaneName(id), tr("Track"));
    const Pad &settings = m_song.pads[pad];
    auto plugin = [&](const QString &key, const QString &prefix) {
        const Live live = m_live.value(key);
        if (!live.processor)
            return;
        for (const Parameter &p : live.processor->parameters())
            add(prefix + p.id, p.name, live.processor->name());
    };
    if (!settings.synth.isEmpty())
        plugin(slotKey(pad, -1), "synth.");
    for (int i = 0; i < settings.effects.size(); ++i)
        plugin(slotKey(pad, i), QString("fx%1.").arg(i + 1));
    return list;
}

void Backend::addLane(int pad, const QString &id) {
    const LaneInfo info = laneInfo(id);
    if (!validPad(pad) || !info.valid)
        return;
    Song next = m_song;
    Lanes &lanes = next.patterns[m_editPattern].lanes[pad];
    if (lanes.contains(id))
        return;
    // A lane starts where the setting is, so adding it changes nothing yet.
    double value = info.target == "pad" ? padValue(next.pads[pad], info.param) : 0;
    if (info.target != "pad") {
        const Live live = m_live.value(info.target == "synth" ? slotKey(pad, -1) : slotKey(pad, info.effect));
        if (live.processor)
            for (const Parameter &p : live.processor->parameters())
                if (p.id == info.param)
                    value = p.value;
    }
    lanes[id].insert(0, value);
    apply(next);
}

void Backend::removeLane(int pad, const QString &id) {
    if (!validPad(pad))
        return;
    Song next = m_song;
    Pattern &pattern = next.patterns[m_editPattern];
    if (!pattern.lanes.contains(pad))
        return;
    pattern.lanes[pad].remove(id);
    if (pattern.lanes[pad].isEmpty())
        pattern.lanes.remove(pad);
    apply(next);
}

void Backend::setLanePoint(int pad, const QString &id, int step, double value) {
    if (!valid(pad, step) || step >= m_song.patterns[m_editPattern].length)
        return;
    const QVariantMap lane = laneOf(pad, id, {}, 0);
    Song next = m_song;
    const double min = lane.value("min").toDouble(), max = lane.value("max").toDouble();
    value = qBound(min, value, max);
    if (lane.value("stepped").toBool())
        value = std::round(value);
    next.patterns[m_editPattern].lanes[pad][id][step] = value;
    // A drag across a lane undoes as one.
    apply(next, QString("lane %1 %2 %3").arg(m_editPattern).arg(pad).arg(id));
}

void Backend::changeLanePoint(int pad, const QString &id, int step, double fraction) {
    if (!valid(pad, step))
        return;
    const QVariantMap lane = laneOf(pad, id, {}, 0);
    const double min = lane.value("min").toDouble(), max = lane.value("max").toDouble();
    const Lane points = m_song.patterns[m_editPattern].lanes.value(pad).value(id);
    double now = laneValue(points, step);
    if (std::isnan(now)) {
        const LaneInfo info = laneInfo(id);
        now = info.target == "pad" ? padValue(m_song.pads[pad], info.param) : min;
    }
    // A stepped lane, as pitch is, moves a whole step at a time.
    const double delta = lane.value("stepped").toBool() ? (fraction > 0 ? 1 : -1) : fraction * (max - min);
    setLanePoint(pad, id, step, now + delta);
}

void Backend::clearLanePoint(int pad, const QString &id, int step) {
    if (!validPad(pad))
        return;
    Song next = m_song;
    Pattern &pattern = next.patterns[m_editPattern];
    if (!pattern.lanes.value(pad).value(id).contains(step))
        return;
    pattern.lanes[pad][id].remove(step);
    apply(next);
}

void Backend::setPadSetting(int pad, const QString &name, double value) {
    if (!validPad(pad))
        return;
    Song next = m_song;
    Pad &settings = next.pads[pad];
    double *field = name == "volume"      ? &settings.volume
                    : name == "pan"       ? &settings.pan
                    : name == "filter"    ? &settings.filter
                    : name == "resonance" ? &settings.resonance
                    : name == "attack"    ? &settings.attack
                    : name == "decay"     ? &settings.decay
                                          : nullptr;
    if (!field)
        return;
    const double low = name == "pan" || name == "filter" ? -1 : 0;
    *field = qBound(low, std::round(value * 1000) / 1000, 1.0);
    apply(next, QString("setting %1 %2").arg(pad).arg(name));
}

void Backend::toggleReverse(int pad) {
    if (!validPad(pad))
        return;
    Song next = m_song;
    next.pads[pad].reverse = !next.pads[pad].reverse;
    apply(next);
    if (!m_playing)
        m_engine->hit(pad, kDefaultLevel);
}

void Backend::toggleGate(int pad) {
    if (!validPad(pad))
        return;
    Song next = m_song;
    next.pads[pad].gate = !next.pads[pad].gate;
    apply(next);
}

void Backend::changePadSwing(int pad, int delta) {
    if (!validPad(pad))
        return;
    Song next = m_song;
    int &swing = next.pads[pad].swing;
    // A track's own swing starts from the song's; below the straightest
    // it goes back to following the song.
    if (swing == 0 && delta < 0)
        return;
    const int to = (swing == 0 ? m_song.swing : swing) + delta;
    swing = to < kMinSwing ? 0 : std::min(to, kMaxSwing);
    apply(next, QString("swing %1").arg(pad));
}

void Backend::changeLoop(int pad, int delta) {
    if (!validPad(pad))
        return;
    Song next = m_song;
    int &loop = next.pads[pad].loop;
    // From the pattern's length, a loop of its own shortens or grows a step
    // at a time; at the pattern's length again it follows the pattern.
    const int length = next.patterns[m_editPattern].length;
    const int from = loop == 0 ? length : loop;
    const int to = qBound(1, from + delta, kMaxSteps);
    loop = to == length ? 0 : to;
    apply(next, QString("loop %1").arg(pad));
}

void Backend::setMetering(bool on) {
    if (on) {
        m_meterTimer.start();
    } else {
        m_meterTimer.stop();
        m_meters.clear();
        emit metersChanged();
    }
}
