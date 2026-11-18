#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include "engine.h"
#include "filepicker.h"
#include "song.h"

// The song being made, as the window sees it: every edit goes through here,
// is undoable, and reaches the engine at once, so it is heard on the next step.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(int bpm READ bpm NOTIFY songChanged)
    Q_PROPERTY(int swing READ swing NOTIFY songChanged)
    Q_PROPERTY(QVariantList chain READ chain NOTIFY songChanged)
    Q_PROPERTY(QVariantList usedPatterns READ usedPatterns NOTIFY songChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY songChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY songChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY songChanged)
    Q_PROPERTY(QVariantList grid READ grid NOTIFY gridChanged)
    Q_PROPERTY(QVariantList tracks READ tracks NOTIFY tracksChanged)
    Q_PROPERTY(int editPattern READ editPattern NOTIFY gridChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(bool songMode READ songMode NOTIFY songModeChanged)
    Q_PROPERTY(int playStep READ playStep NOTIFY positionChanged)
    Q_PROPERTY(int playPattern READ playPattern NOTIFY positionChanged)
    Q_PROPERTY(int playChainIndex READ playChainIndex NOTIFY positionChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY fileChanged)
    Q_PROPERTY(bool exporting READ exporting NOTIFY exportingChanged)
    Q_PROPERTY(QVariantMap message READ message NOTIFY messageChanged)

public:
    Backend(Engine *engine, FilePicker *picker, QObject *parent = nullptr);

    int bpm() const { return m_song.bpm; }
    int swing() const { return m_song.swing; }
    QVariantList chain() const;
    QVariantList usedPatterns() const;
    bool dirty() const { return m_song != m_saved; }
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    QVariantList grid() const;
    QVariantList tracks() const;
    int editPattern() const { return m_editPattern; }
    bool playing() const { return m_playing; }
    bool songMode() const { return m_songMode; }
    int playStep() const { return m_playStep; }
    int playPattern() const { return m_playPattern; }
    int playChainIndex() const { return m_playChainIndex; }
    QString fileName() const;
    QString path() const { return m_path; }
    bool exporting() const { return m_exporting; }
    QVariantMap message() const { return m_message; }
    const Song &song() const { return m_song; }

    // Steps.
    Q_INVOKABLE void toggleStep(int track, int step);
    Q_INVOKABLE void holdStep(int track, int step);
    Q_INVOKABLE void clearStep(int track, int step);
    // A drum step's level, or a bass note's pitch in semitones.
    Q_INVOKABLE void changeStep(int track, int step, int delta);

    // Patterns.
    Q_INVOKABLE void selectPattern(int pattern);
    Q_INVOKABLE void copyPattern();
    Q_INVOKABLE void pastePattern();
    Q_INVOKABLE void clearPattern();

    // The song: its chain of patterns, tempo, and swing.
    Q_INVOKABLE void chainInsert(int index, int pattern);
    Q_INVOKABLE void chainSet(int index, int pattern);
    Q_INVOKABLE void chainRemove(int index);
    Q_INVOKABLE void changeBpm(int delta);
    Q_INVOKABLE void changeSwing(int delta);

    // Tracks.
    Q_INVOKABLE void toggleMute(int track);
    Q_INVOKABLE void toggleSolo(int track);
    Q_INVOKABLE void changeVolume(int track, double delta);
    Q_INVOKABLE void chooseSample(int track);
    Q_INVOKABLE void resetSample(int track);
    void setSample(int track, const QString &path);

    // Playing: the song from one of its slots, or the pattern being edited.
    Q_INVOKABLE void togglePlay(int chainStart = 0);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void toggleSongMode();

    // Files.
    Q_INVOKABLE void newSong();
    Q_INVOKABLE void openSong();
    Q_INVOKABLE bool openPath(const QString &path);
    Q_INVOKABLE void save();
    Q_INVOKABLE void saveAs();
    Q_INVOKABLE bool saveTo(const QString &path);
    Q_INVOKABLE void exportAudio();
    void exportTo(const QString &path);

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void dismissMessage();

    // The sound each track plays now: built in, a loaded sample, or nothing
    // while one loads or after it failed.
    std::array<SampleBuffer, kTracks> sounds() const;

signals:
    void songChanged();
    void gridChanged();
    void tracksChanged();
    void playingChanged();
    void songModeChanged();
    void positionChanged();
    void fileChanged();
    void exportingChanged();
    void messageChanged();
    void saved();
    // A save that was asked for didn't happen: the dialog was dismissed, or
    // writing failed.
    void saveAborted();
    void exported();

private:
    // Makes next the song, undoably. Edits with the same coalesce key in a
    // row (nudging the tempo, a volume) undo as one.
    void apply(const Song &next, const QString &coalesce = {});
    void replaceSong(const Song &song, const QString &path);
    void sync();
    void syncSounds();
    void loadSample(const QString &path);
    void setMessage(const QString &kind, const QString &arg = {}, const QString &detail = {});
    void onPosition(int chainIndex, int pattern, int step);
    Step &stepAt(Song &song, int track, int step) { return song.patterns[m_editPattern].steps[track][step]; }
    void audition(int track, const Step &step);

    Engine *m_engine;
    FilePicker *m_picker;
    Song m_song;
    Song m_saved;
    QVector<Song> m_undo;
    QVector<Song> m_redo;
    QString m_coalesce;
    QString m_path;

    int m_editPattern = 0;
    int m_lastNote = kDefaultNote;
    bool m_playing = false;
    bool m_songMode = false;
    int m_playStep = -1;
    int m_playPattern = -1;
    int m_playChainIndex = -1;
    bool m_exporting = false;
    QVariantMap m_message;
    Pattern m_clipboard;
    bool m_hasClipboard = false;
    int m_sampleTrack = -1;

    QHash<QString, SampleBuffer> m_samples; // decoded, by path
    QSet<QString> m_loading;
    QHash<QString, QString> m_failed; // path to why
};
