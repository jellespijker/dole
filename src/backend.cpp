#include "backend.h"

#include <QDir>
#include <QEventLoopLocker>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QStandardPaths>
#include <QtConcurrentRun>

#include "chop.h"
#include "renderer.h"
#include "wav.h"

namespace {

constexpr int kMaxUndo = 500;
constexpr int kPeaks = 600;

bool validPad(int pad) {
    return pad >= 0 && pad < kPads;
}

bool valid(int pad, int step) {
    return validPad(pad) && step >= 0 && step < kSteps;
}

QString musicFolder() {
    const QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    return QDir(music).exists() ? music : QDir::homePath();
}

QString withSuffix(QString path, const QString &suffix) {
    if (QFileInfo(path).suffix().compare(suffix, Qt::CaseInsensitive) != 0)
        path += "." + suffix;
    return path;
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
        if (m_recording && m_playing)
            record(pad, pattern, step, kDefaultLevel, false);
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

QVariantList Backend::grid() const {
    QVariantList rows;
    const Pattern &pattern = m_song.patterns[m_editPattern];
    for (int p = 0; p < kPads; ++p) {
        QVariantList row;
        for (const Step &step : pattern.steps[p])
            row << step.level;
        rows << QVariant(row);
    }
    return rows;
}

QVariantList Backend::pads() const {
    QVariantList list;
    for (int p = 0; p < kPads; ++p) {
        const Pad &pad = m_song.pads[p];
        const QString state = pad.sample.isEmpty()              ? "kit"
                              : m_samples.contains(pad.sample)  ? "ready"
                              : m_loading.contains(pad.sample)  ? "loading"
                                                                : "failed";
        list << QVariantMap{{"id", kitIds()[p]},
                            {"sample", QFileInfo(pad.sample).completeBaseName()},
                            {"chop", pad.isChop()},
                            {"state", state},
                            {"tune", pad.tune},
                            {"volume", pad.volume},
                            {"group", pad.group},
                            {"mute", pad.mute},
                            {"solo", pad.solo},
                            {"audible", m_song.audible(p)}};
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
    m_song = next;
    sync();
}

void Backend::sync() {
    m_engine->setSong(m_song);
    syncSounds();
    emit songChanged();
    emit gridChanged();
    emit padsChanged();
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
        sounds[p] = pad.sample.isEmpty() ? kitSound(p) : padSound(pad);
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
    if (!m_recording || !m_playing || m_playStep < 0)
        return;
    // Recorded on the nearest step: the one heard last, or the next if the
    // hit came more than halfway to it.
    const double stepMs = 60000.0 / m_song.bpm / 4;
    const bool late = m_sinceStep.elapsed() > stepMs / 2;
    const int step = (m_playStep + (late ? 1 : 0)) % kSteps;
    record(pad, m_playPattern, step, level, late);
}

void Backend::padUp(int pad) {
    if (!m_held.remove(pad))
        return;
    m_engine->hold(pad, false, 0);
}

void Backend::releaseAll() {
    for (int pad : std::as_const(m_held))
        m_engine->hold(pad, false, 0);
    m_held.clear();
}

void Backend::record(int pad, int pattern, int step, int level, bool suppress) {
    if (pattern < 0 || pattern >= kPatterns || !valid(pad, step))
        return;
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
    m_chopStarts = evenSlices(duration(sound), std::min(count, kPads));
    emit chopChanged();
}

void Backend::sliceTransients() {
    const SampleBuffer sound = m_samples.value(m_chopPath);
    if (!sound)
        return;
    m_chopStarts = transientSlices(sound, kPads);
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
    for (int i = 0; i < m_chopStarts.size() && firstPad + i < kPads; ++i, ++count) {
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
    m_playStep = m_playPattern = m_playChainIndex = -1;
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
    m_playChainIndex = chainIndex;
    m_playPattern = pattern;
    m_playStep = step;
    emit positionChanged();
    const Pattern &playing = m_song.patterns[pattern];
    for (int pad = 0; pad < kPads; ++pad)
        if (playing.steps[pad][step].on() && m_song.audible(pad))
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
    // The export finishes even if the window closes first: the locker keeps
    // the app running, windowless, until the file is written.
    auto locker = std::make_shared<QEventLoopLocker>();
    watcher->setFuture(QtConcurrent::run([song = m_song, sounds = sounds(), pattern = m_editPattern, path, locker] {
        QString error;
        writeWav(path, renderSong(song, sounds, pattern), &error);
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
