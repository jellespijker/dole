#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <array>

// The whole song as plain values: tempo, eight tracks, eight one-bar
// patterns of sixteen steps, and the chain of patterns that makes the song.
// Copying one is cheap, which is what undo and the audio thread rely on.

constexpr int kTracks = 8;
constexpr int kSteps = 16;
constexpr int kPatterns = 8;
constexpr int kHat = 2;
constexpr int kOpenHat = 3;
constexpr int kBass = 7;
constexpr int kDefaultNote = 36; // C2
constexpr int kMinNote = 24;     // C1
constexpr int kMaxNote = 60;     // C4
constexpr int kMinBpm = 40;
constexpr int kMaxBpm = 240;
constexpr int kMinSwing = 50;
constexpr int kMaxSwing = 75;
constexpr int kLevels = 4;
constexpr int kDefaultLevel = 3;
constexpr int kMaxChain = 256;

enum class StepKind : quint8 { Off, Hit, Hold };

struct Step {
    StepKind kind = StepKind::Off;
    int level = kDefaultLevel; // 1 to 4, for drum hits
    int note = kDefaultNote;   // MIDI note, for the bass

    bool operator==(const Step &other) const {
        return kind == other.kind && level == other.level && note == other.note;
    }
    bool operator!=(const Step &other) const { return !(*this == other); }
};

struct Pattern {
    std::array<std::array<Step, kSteps>, kTracks> steps{};

    bool isEmpty() const;
    bool operator==(const Pattern &other) const { return steps == other.steps; }
    bool operator!=(const Pattern &other) const { return !(*this == other); }
};

struct Track {
    QString sample; // absolute path, or empty for the built-in sound
    double volume = 0.8;
    bool mute = false;
    bool solo = false;

    bool operator==(const Track &other) const {
        return sample == other.sample && qRound(volume * 100) == qRound(other.volume * 100) && mute == other.mute
               && solo == other.solo;
    }
    bool operator!=(const Track &other) const { return !(*this == other); }
};

struct Song {
    int bpm = 96;
    int swing = kMinSwing;
    std::array<Track, kTracks> tracks{};
    std::array<Pattern, kPatterns> patterns{};
    QVector<int> chain;

    bool audible(int track) const;
    bool operator==(const Song &other) const {
        return bpm == other.bpm && swing == other.swing && tracks == other.tracks && patterns == other.patterns
               && chain == other.chain;
    }
    bool operator!=(const Song &other) const { return !(*this == other); }
};

// The tracks' ids, in order: kick, snare, hat, openhat, clap, rim, cowbell, bass.
const QStringList &trackIds();
QString patternName(int pattern); // "A" to "H"
int patternIndex(const QString &name);
QString noteName(int note);        // "C2", "D#1"
int parseNote(const QString &name); // -1 if it isn't a note
// How loud each of the four levels plays.
float levelGain(int level);

QJsonObject songToJson(const Song &song, const QString &baseDirectory);
// Fills song and returns true, or says what's wrong in error.
bool songFromJson(const QJsonObject &json, const QString &baseDirectory, Song *song, QString *error);
bool loadSong(const QString &path, Song *song, QString *error);
bool saveSong(const QString &path, const Song &song, QString *error);

// A short groove, so the first Space makes a noise.
Song demoSong();
