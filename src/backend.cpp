#include "backend.h"

#include <QDir>
#include <QEventLoopLocker>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QStandardPaths>
#include <QtConcurrentRun>

#include "renderer.h"
#include "wav.h"

namespace {

constexpr int kMaxUndo = 500;

bool valid(int track, int step) {
    return track >= 0 && track < kTracks && step >= 0 && step < kSteps;
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

} // namespace

Backend::Backend(Engine *engine, FilePicker *picker, QObject *parent)
    : QObject(parent), m_engine(engine), m_picker(picker), m_song(demoSong()), m_saved(m_song) {
    connect(m_engine, &Engine::positionChanged, this, &Backend::onPosition);
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
        else if (kind == FilePicker::Sample && m_sampleTrack >= 0)
            setSample(m_sampleTrack, url.toLocalFile());
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
    for (int t = 0; t < kTracks; ++t) {
        QVariantList row;
        for (const Step &step : pattern.steps[t])
            row << QVariantMap{{"kind", int(step.kind)}, {"level", step.level}, {"note", noteName(step.note)}};
        rows << QVariant(row);
    }
    return rows;
}

QVariantList Backend::tracks() const {
    QVariantList list;
    for (int t = 0; t < kTracks; ++t) {
        const Track &track = m_song.tracks[t];
        const QString state = track.sample.isEmpty()                ? "builtin"
                              : m_samples.contains(track.sample)    ? "ready"
                              : m_loading.contains(track.sample)    ? "loading"
                                                                    : "failed";
        list << QVariantMap{{"id", trackIds()[t]},
                            {"sample", QFileInfo(track.sample).fileName()},
                            {"state", state},
                            {"volume", track.volume},
                            {"mute", track.mute},
                            {"solo", track.solo},
                            {"audible", m_song.audible(t)}};
    }
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
    emit tracksChanged();
}

std::array<SampleBuffer, kTracks> Backend::sounds() const {
    std::array<SampleBuffer, kTracks> sounds;
    for (int t = 0; t < kTracks; ++t) {
        const QString &sample = m_song.tracks[t].sample;
        sounds[t] = sample.isEmpty() ? builtInSound(t) : m_samples.value(sample);
    }
    return sounds;
}

void Backend::syncSounds() {
    for (const Track &track : m_song.tracks)
        if (!track.sample.isEmpty() && !m_samples.contains(track.sample) && !m_loading.contains(track.sample)
            && !m_failed.contains(track.sample))
            loadSample(track.sample);
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
        m_engine->setSounds(sounds());
        emit tracksChanged();
    });
    // ffmpeg can take a while on a long file, so it decodes off the GUI thread.
    watcher->setFuture(QtConcurrent::run([path] {
        QString error;
        SampleBuffer sound = decodeSample(path, &error);
        return qMakePair(sound, error);
    }));
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

void Backend::audition(int track, const Step &step) {
    if (!m_playing)
        m_engine->audition(track, step);
}

void Backend::toggleStep(int track, int step) {
    if (!valid(track, step))
        return;
    Song next = m_song;
    Step &s = stepAt(next, track, step);
    if (s.kind == StepKind::Hit) {
        s = Step{};
    } else {
        s = Step{};
        s.kind = StepKind::Hit;
        if (track == kBass)
            s.note = m_lastNote;
        audition(track, s);
    }
    apply(next);
}

void Backend::holdStep(int track, int step) {
    if (track != kBass || !valid(track, step))
        return;
    Song next = m_song;
    Step &s = stepAt(next, track, step);
    const bool hold = s.kind != StepKind::Hold;
    s = Step{};
    if (hold)
        s.kind = StepKind::Hold;
    apply(next);
}

void Backend::clearStep(int track, int step) {
    if (!valid(track, step))
        return;
    Song next = m_song;
    stepAt(next, track, step) = Step{};
    apply(next);
}

void Backend::changeStep(int track, int step, int delta) {
    if (!valid(track, step))
        return;
    Song next = m_song;
    Step &s = stepAt(next, track, step);
    if (s.kind != StepKind::Hit)
        return;
    if (track == kBass) {
        s.note = qBound(kMinNote, s.note + delta, kMaxNote);
        m_lastNote = s.note;
    } else {
        s.level = qBound(1, s.level + delta, kLevels);
    }
    audition(track, s);
    apply(next, QString("step %1 %2 %3").arg(m_editPattern).arg(track).arg(step));
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

void Backend::toggleMute(int track) {
    if (track < 0 || track >= kTracks)
        return;
    Song next = m_song;
    next.tracks[track].mute = !next.tracks[track].mute;
    apply(next);
}

void Backend::toggleSolo(int track) {
    if (track < 0 || track >= kTracks)
        return;
    Song next = m_song;
    next.tracks[track].solo = !next.tracks[track].solo;
    apply(next);
}

void Backend::changeVolume(int track, double delta) {
    if (track < 0 || track >= kTracks)
        return;
    Song next = m_song;
    next.tracks[track].volume = qBound(0.0, qRound((next.tracks[track].volume + delta) * 100) / 100.0, 1.0);
    apply(next, QString("volume %1").arg(track));
}

void Backend::chooseSample(int track) {
    if (track < 0 || track >= kTracks)
        return;
    m_sampleTrack = track;
    m_picker->open(FilePicker::Sample);
}

void Backend::resetSample(int track) {
    setSample(track, {});
}

void Backend::setSample(int track, const QString &path) {
    if (track < 0 || track >= kTracks)
        return;
    // Picking a sample that failed before tries it again: the file may be fixed.
    m_failed.remove(path);
    Song next = m_song;
    next.tracks[track].sample = path.isEmpty() ? QString() : QFileInfo(path).absoluteFilePath();
    apply(next);
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
    m_playStep = m_playPattern = m_playChainIndex = -1;
    emit playingChanged();
    emit positionChanged();
}

void Backend::toggleSongMode() {
    m_songMode = !m_songMode;
    emit songModeChanged();
    if (m_playing)
        m_engine->play(m_songMode && !m_song.chain.isEmpty(), m_editPattern, 0);
}

void Backend::onPosition(int chainIndex, int pattern, int step) {
    // A step already on its way when playing stopped.
    if (!m_playing)
        return;
    m_playChainIndex = chainIndex;
    m_playPattern = pattern;
    m_playStep = step;
    emit positionChanged();
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
    for (const Track &track : song.tracks)
        m_failed.remove(track.sample);
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
    const QString suggested =
        m_path.isEmpty() ? musicFolder() + "/" + tr("Untitled") + ".dole" : m_path;
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
