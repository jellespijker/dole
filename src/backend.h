#pragma once

#include <QElapsedTimer>
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
    Q_PROPERTY(int grit READ grit NOTIFY songChanged)
    Q_PROPERTY(bool vinyl READ vinyl NOTIFY songChanged)
    Q_PROPERTY(QVariantList chain READ chain NOTIFY songChanged)
    Q_PROPERTY(QVariantList usedPatterns READ usedPatterns NOTIFY songChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY songChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY songChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY songChanged)
    Q_PROPERTY(QVariantList grid READ grid NOTIFY gridChanged)
    Q_PROPERTY(int editPattern READ editPattern NOTIFY gridChanged)
    Q_PROPERTY(QVariantList pads READ pads NOTIFY padsChanged)
    Q_PROPERTY(int selectedPad READ selectedPad NOTIFY selectedPadChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(bool recording READ recording NOTIFY playingChanged)
    Q_PROPERTY(bool songMode READ songMode NOTIFY songModeChanged)
    Q_PROPERTY(int repeat READ repeat NOTIFY repeatChanged)
    Q_PROPERTY(QString repeatName READ repeatName NOTIFY repeatChanged)
    Q_PROPERTY(int playStep READ playStep NOTIFY positionChanged)
    Q_PROPERTY(int playPattern READ playPattern NOTIFY positionChanged)
    Q_PROPERTY(int playChainIndex READ playChainIndex NOTIFY positionChanged)
    Q_PROPERTY(QString chopName READ chopName NOTIFY chopChanged)
    Q_PROPERTY(QString chopState READ chopState NOTIFY chopChanged)
    Q_PROPERTY(QVariantList chopPeaks READ chopPeaks NOTIFY chopChanged)
    Q_PROPERTY(QVariantList chopSlices READ chopSlices NOTIFY chopChanged)
    Q_PROPERTY(QString fileName READ fileName NOTIFY fileChanged)
    Q_PROPERTY(bool exporting READ exporting NOTIFY exportingChanged)
    Q_PROPERTY(QVariantMap message READ message NOTIFY messageChanged)

public:
    Backend(Engine *engine, FilePicker *picker, QObject *parent = nullptr);

    int bpm() const { return m_song.bpm; }
    int swing() const { return m_song.swing; }
    int grit() const { return int(m_song.grit); }
    bool vinyl() const { return m_song.vinyl; }
    QVariantList chain() const;
    QVariantList usedPatterns() const;
    bool dirty() const { return m_song != m_saved; }
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    QVariantList grid() const;
    int editPattern() const { return m_editPattern; }
    QVariantList pads() const;
    int selectedPad() const { return m_selectedPad; }
    bool playing() const { return m_playing; }
    bool recording() const { return m_recording; }
    bool songMode() const { return m_songMode; }
    int repeat() const { return m_repeat; }
    QString repeatName() const;
    int playStep() const { return m_playStep; }
    int playPattern() const { return m_playPattern; }
    int playChainIndex() const { return m_playChainIndex; }
    QString chopName() const;
    QString chopState() const;
    QVariantList chopPeaks() const { return m_chopPeaks; }
    QVariantList chopSlices() const;
    QString fileName() const;
    QString path() const { return m_path; }
    bool exporting() const { return m_exporting; }
    QVariantMap message() const { return m_message; }
    const Song &song() const { return m_song; }

    // Steps.
    Q_INVOKABLE void toggleStep(int pad, int step);
    Q_INVOKABLE void clearStep(int pad, int step);
    Q_INVOKABLE void changeStep(int pad, int step, int delta);

    // Patterns.
    Q_INVOKABLE void selectPattern(int pattern);
    Q_INVOKABLE void copyPattern();
    Q_INVOKABLE void pastePattern();
    Q_INVOKABLE void clearPattern();

    // The song: its chain of patterns, tempo, swing, and grit.
    Q_INVOKABLE void chainInsert(int index, int pattern);
    Q_INVOKABLE void chainSet(int index, int pattern);
    Q_INVOKABLE void chainRemove(int index);
    Q_INVOKABLE void changeBpm(int delta);
    Q_INVOKABLE void changeSwing(int delta);
    Q_INVOKABLE void cycleGrit();
    Q_INVOKABLE void toggleVinyl();

    // Pads: playing them, and what they play.
    Q_INVOKABLE void padDown(int pad, int level = kDefaultLevel);
    Q_INVOKABLE void padUp(int pad);
    Q_INVOKABLE void releaseAll();
    Q_INVOKABLE void selectPad(int pad);
    Q_INVOKABLE void toggleMute(int pad);
    Q_INVOKABLE void toggleSolo(int pad);
    Q_INVOKABLE void changeVolume(int pad, double delta);
    Q_INVOKABLE void changeTune(int pad, int delta);
    Q_INVOKABLE void cycleGroup(int pad);
    Q_INVOKABLE void chooseSample(int pad);
    Q_INVOKABLE void resetPad(int pad);
    void setSample(int pad, const QString &path);

    // Chopping a sample onto pads.
    Q_INVOKABLE void chooseChop();
    Q_INVOKABLE void openChop(const QString &path);
    Q_INVOKABLE void sliceEven(int count);
    Q_INVOKABLE void sliceTransients();
    Q_INVOKABLE void previewSlice(int index);
    // Puts the chops on the pads from the first one on, and says how many fit.
    Q_INVOKABLE int assignChops(int firstPad);

    // Playing and recording: the song from one of its slots, or the pattern
    // being edited.
    Q_INVOKABLE void togglePlay(int chainStart = 0);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void toggleRecord(int chainStart = 0);
    Q_INVOKABLE void toggleSongMode();
    Q_INVOKABLE void toggleRepeat();
    Q_INVOKABLE void cycleRepeat();

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

    // The sound each pad plays now: the kit's, its sample or chop, or nothing
    // while one loads or after it failed.
    std::array<SampleBuffer, kPads> sounds() const;

signals:
    void songChanged();
    void gridChanged();
    void padsChanged();
    void selectedPadChanged();
    void playingChanged();
    void songModeChanged();
    void repeatChanged();
    void positionChanged();
    void chopChanged();
    void fileChanged();
    void exportingChanged();
    void messageChanged();
    // A pad was struck, by a finger or the sequence, so it can light up.
    void padHit(int pad);
    void saved();
    // A save that was asked for didn't happen: the dialog was dismissed, or
    // writing failed.
    void saveAborted();
    void exported();

private:
    // Makes next the song, undoably. Edits with the same coalesce key in a
    // row (nudging the tempo, a volume, one take of recording) undo as one.
    void apply(const Song &next, const QString &coalesce = {});
    void replaceSong(const Song &song, const QString &path);
    void sync();
    void syncSounds();
    void loadSample(const QString &path);
    void sampleLoaded(const QString &path);
    void setMessage(const QString &kind, const QString &arg = {}, const QString &detail = {});
    void onPosition(int chainIndex, int pattern, int step);
    void record(int pad, int pattern, int step, int level, bool suppress);
    SampleBuffer padSound(const Pad &pad) const;

    Engine *m_engine;
    FilePicker *m_picker;
    Song m_song;
    Song m_saved;
    QVector<Song> m_undo;
    QVector<Song> m_redo;
    QString m_coalesce;
    QString m_path;

    int m_editPattern = 0;
    int m_selectedPad = 0;
    bool m_playing = false;
    bool m_recording = false;
    bool m_songMode = false;
    int m_repeat = 0;
    int m_lastRepeat = 2; // 1/16
    QSet<int> m_held;
    int m_playStep = -1;
    int m_playPattern = -1;
    int m_playChainIndex = -1;
    QElapsedTimer m_sinceStep; // since the step playing was heard
    bool m_exporting = false;
    QVariantMap m_message;
    Pattern m_clipboard;
    bool m_hasClipboard = false;
    int m_samplePad = -1; // the pad a sample is being chosen for; -1 for chopping

    QHash<QString, SampleBuffer> m_samples; // decoded, by path
    mutable QHash<QString, SampleBuffer> m_slices; // by path, start, and end
    QSet<QString> m_loading;
    QHash<QString, QString> m_failed; // path to why

    QString m_chopPath;
    QVariantList m_chopPeaks;
    QVector<double> m_chopStarts;
};
