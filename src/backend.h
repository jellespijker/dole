#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <functional>

#include "engine.h"
#include "filepicker.h"
#include "plugin.h"
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
    Q_PROPERTY(QVariantList patternLengths READ patternLengths NOTIFY songChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY songChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY songChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY songChanged)
    Q_PROPERTY(QVariantList grid READ grid NOTIFY gridChanged)
    Q_PROPERTY(int editPattern READ editPattern NOTIFY gridChanged)
    Q_PROPERTY(int patternLength READ patternLength NOTIFY gridChanged)
    Q_PROPERTY(int bank READ bank NOTIFY gridChanged)
    Q_PROPERTY(QVariantList meters READ meters NOTIFY metersChanged)
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
    Q_PROPERTY(int playTick READ playTick NOTIFY positionChanged)
    Q_PROPERTY(QString chopName READ chopName NOTIFY chopChanged)
    Q_PROPERTY(QString chopState READ chopState NOTIFY chopChanged)
    Q_PROPERTY(QVariantList chopPeaks READ chopPeaks NOTIFY chopChanged)
    Q_PROPERTY(QVariantList chopSlices READ chopSlices NOTIFY chopChanged)
    Q_PROPERTY(QVariantList plugins READ plugins NOTIFY pluginsChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY pluginsChanged)
    Q_PROPERTY(int rackTarget READ rackTarget NOTIFY rackChanged)
    Q_PROPERTY(QVariantMap rack READ rack NOTIFY rackChanged)
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
    QVariantList patternLengths() const;
    bool dirty() const { return m_song != m_saved; }
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    QVariantList grid() const;
    int editPattern() const { return m_editPattern; }
    int patternLength() const { return m_song.patterns[m_editPattern].length; }
    int bank() const { return m_bank; }
    QVariantList meters() const { return m_meters; }
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
    // Steps since the playing pattern came in: a track with its own loop is
    // on this, counted round its loop.
    int playTick() const { return m_playTick; }
    QString chopName() const;
    QString chopState() const;
    QVariantList chopPeaks() const { return m_chopPeaks; }
    QVariantList chopSlices() const;
    QVariantList plugins() const;
    bool scanning() const { return m_scanning; }
    int rackTarget() const { return m_rackTarget; }
    QVariantMap rack() const;
    QString fileName() const;
    QString path() const { return m_path; }
    bool exporting() const { return m_exporting; }
    QVariantMap message() const { return m_message; }
    const Song &song() const { return m_song; }
    // Makes next the song in one undoable step, as an edit from outside the
    // window (an agent's, over the control socket) is.
    void editSong(const Song &next, const QString &coalesce = {}) {
        if (coalesce.isEmpty())
            m_coalesce.clear();
        apply(next, coalesce);
    }
    // Makes next the song as someone else changed it, in a jam: not an undo
    // of this window's, and undo here still takes back only what was done here.
    void receiveSong(const Song &next);
    // Asked just before playing starts, so a jam can start every dole on one
    // beat.
    void setAboutToPlay(std::function<void(int chainStart)> hook) { m_aboutToPlay = std::move(hook); }
    // Starts afresh on someone else's song, as joining a jam to share it
    // does: nothing to undo, and no file of its own until it's saved.
    void adoptSong(const Song &song) { replaceSong(song, {}); }

    // Steps.
    Q_INVOKABLE void toggleStep(int pad, int step);
    Q_INVOKABLE void clearStep(int pad, int step);
    Q_INVOKABLE void changeStep(int pad, int step, int delta);
    // A step's roll (1 to 4 hits), chance, and nudge off the grid.
    Q_INVOKABLE void cycleRoll(int pad, int step);
    Q_INVOKABLE void cycleChance(int pad, int step);
    Q_INVOKABLE void nudgeStep(int pad, int step, int delta);

    // Lanes: a track's settings, or its plugins', moved step by step.
    // A lane's value on a step can be set, nudged by a part of its range,
    // or cleared, so the one before carries on.
    Q_INVOKABLE QVariantList laneChoices(int pad) const;
    Q_INVOKABLE void addLane(int pad, const QString &id);
    Q_INVOKABLE void removeLane(int pad, const QString &id);
    Q_INVOKABLE void setLanePoint(int pad, const QString &id, int step, double value);
    Q_INVOKABLE void changeLanePoint(int pad, const QString &id, int step, double fraction);
    Q_INVOKABLE void clearLanePoint(int pad, const QString &id, int step);

    // Patterns.
    Q_INVOKABLE void selectPattern(int pattern);
    Q_INVOKABLE void copyPattern();
    Q_INVOKABLE void pastePattern();
    Q_INVOKABLE void clearPattern();
    // One to four bars.
    Q_INVOKABLE void cyclePatternLength();

    // The song: its chain of patterns, tempo, swing, and grit.
    Q_INVOKABLE void chainInsert(int index, int pattern);
    Q_INVOKABLE void chainSet(int index, int pattern);
    Q_INVOKABLE void chainRemove(int index);
    Q_INVOKABLE void changeBpm(int delta);
    // A drag or a tapped tempo sets it outright; in a row, one undo.
    Q_INVOKABLE void setBpm(int bpm);
    Q_INVOKABLE void changeSwing(int delta);
    Q_INVOKABLE void cycleGrit();
    Q_INVOKABLE void toggleVinyl();

    // Pads: playing them, and what they play.
    Q_INVOKABLE void padDown(int pad, int level = kDefaultLevel);
    Q_INVOKABLE void padUp(int pad);
    Q_INVOKABLE void releaseAll();
    Q_INVOKABLE void selectPad(int pad);
    // Banks A to D: the sixteen pads the fingers and the grid are on.
    Q_INVOKABLE void setBank(int bank);
    Q_INVOKABLE void toggleMute(int pad);
    Q_INVOKABLE void toggleSolo(int pad);
    Q_INVOKABLE void changeVolume(int pad, double delta);
    Q_INVOKABLE void changeTune(int pad, int delta);
    Q_INVOKABLE void cycleGroup(int pad);
    // "volume", "pan", "filter", "resonance", "attack", or "decay".
    Q_INVOKABLE void setPadSetting(int pad, const QString &name, double value);
    Q_INVOKABLE void toggleReverse(int pad);
    Q_INVOKABLE void toggleGate(int pad);
    // The track's own swing, from the song's (0) up to 75%.
    Q_INVOKABLE void changePadSwing(int pad, int delta);
    // The track's own loop, from the pattern's (0) to 64 steps.
    Q_INVOKABLE void changeLoop(int pad, int delta);
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

    // Plugins: a synth on a pad, and effects on a pad or (target -1) the
    // master. An effect is named by its index in the chain; -1 is the synth.
    Q_INVOKABLE void setRackTarget(int target);
    Q_INVOKABLE void scanPlugins();
    void setScanCommand(const QString &program, const QStringList &arguments);
    Q_INVOKABLE void addEffect(int target, const QString &id);
    Q_INVOKABLE void removeEffect(int target, int index);
    Q_INVOKABLE void moveEffect(int target, int index, int delta);
    Q_INVOKABLE void toggleBypass(int target, int index);
    Q_INVOKABLE void setSynth(int pad, const QString &id);
    Q_INVOKABLE void cycleLength(int pad);
    // Returns how the new value reads, for the slider's label.
    Q_INVOKABLE QString setParameter(int target, int index, const QString &id, double value);
    Q_INVOKABLE void openEditor(int target, int index);

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

    // The mixer's meters move only while it is open.
    Q_INVOKABLE void setMetering(bool on);

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
    void pluginsChanged();
    void rackChanged();
    void fileChanged();
    void exportingChanged();
    void messageChanged();
    void metersChanged();
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
    // tick counts steps since the pattern came in; each track's own step
    // follows from it.
    void record(int pad, int pattern, int tick, int level, bool suppress);
    int rowStep(int pad, int pattern, int tick) const;
    // The lanes of a pad's effects follow them when they move.
    void moveLanes(Song &song, int pad, const std::function<QString(const QString &)> &rename) const;
    QVariantMap laneOf(int pad, const QString &id, const Lane &points, int length) const;
    SampleBuffer padSound(const Pad &pad) const;
    void syncRack();
    // The chain a target names, in a song.
    QVector<PluginSlot> &chainOf(Song &song, int target) const;
    PluginSlot *slotAt(Song &song, int target, int index) const;
    static QString slotKey(int target, int index);
    QString pluginName(const QString &id) const;
    // Brings what a plugin's own window changed into the song.
    void pullPlugin(const QString &key);
    void pullPlugins();

    struct Live {
        PluginSlot applied;
        std::shared_ptr<Processor> processor;
        QString error;
    };
    QHash<QString, Live> m_live;
    QVector<PluginInfo> m_plugins = builtinPlugins();
    bool m_scanning = false;
    bool m_scanned = false;
    QString m_scanProgram;
    QStringList m_scanArguments;
    int m_rackTarget = 0;

    Engine *m_engine;
    FilePicker *m_picker;
    Song m_song;
    Song m_saved;
    // An edit, as the song was before and after it. Undo puts back before
    // when nothing else has changed since; after someone in a jam changed
    // the song too, it takes back only what the edit changed.
    struct Change {
        Song before, after;
    };
    Song reverted(const Song &from, const Song &to) const;
    QVector<Change> m_undo;
    QVector<Change> m_redo;
    std::function<void(int)> m_aboutToPlay;
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
    int m_playTick = -1; // steps since the playing pattern came in
    int m_bank = 0;
    QTimer m_meterTimer;
    QVariantList m_meters;
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
