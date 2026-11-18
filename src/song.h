#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <array>

// The whole song as plain values: tempo, the sixteen pads and their sounds,
// eight one-bar patterns of sixteen steps, and the chain of patterns that
// makes the song. Copying one is cheap, which is what undo and the audio
// thread rely on.

constexpr int kPads = 16;
constexpr int kSteps = 16;
constexpr int kPatterns = 8;
constexpr int kMinBpm = 40;
constexpr int kMaxBpm = 240;
constexpr int kMinSwing = 50;
constexpr int kMaxSwing = 75;
constexpr int kLevels = 4;
constexpr int kDefaultLevel = 3;
constexpr int kMinTune = -24;
constexpr int kMaxTune = 24;
constexpr int kGroups = 4; // mute groups; 0 is none
constexpr int kMaxChain = 256;

// The master's character: clean, an SP-1200's 12-bit grit, or crushed further.
enum class Grit { Clean, TwelveBit, Crushed };

struct Step {
    int level = 0; // 0 is off; 1 to 4 soft to accent

    bool on() const { return level > 0; }
    bool operator==(const Step &other) const { return level == other.level; }
    bool operator!=(const Step &other) const { return !(*this == other); }
};

struct Pattern {
    std::array<std::array<Step, kSteps>, kPads> steps{};

    bool isEmpty() const;
    bool operator==(const Pattern &other) const { return steps == other.steps; }
    bool operator!=(const Pattern &other) const { return !(*this == other); }
};

struct Pad {
    QString sample; // absolute path, or empty for the pad's kit sound
    double start = 0; // the slice of the sample it plays, in seconds;
    double end = 0;   // an end of 0 plays to the end
    int tune = 0;     // semitones; pitch and speed together, as on an MPC
    double volume = 0.8;
    int group = 0; // pads in one mute group cut each other off
    bool mute = false;
    bool solo = false;

    bool isChop() const { return !sample.isEmpty() && (start > 0 || end > 0); }
    bool operator==(const Pad &other) const {
        return sample == other.sample && qRound(start * 1000) == qRound(other.start * 1000)
               && qRound(end * 1000) == qRound(other.end * 1000) && tune == other.tune
               && qRound(volume * 100) == qRound(other.volume * 100) && group == other.group && mute == other.mute
               && solo == other.solo;
    }
    bool operator!=(const Pad &other) const { return !(*this == other); }
};

struct Song {
    int bpm = 90;
    int swing = kMinSwing;
    Grit grit = Grit::Clean;
    bool vinyl = false;
    std::array<Pad, kPads> pads = defaultPads();
    std::array<Pattern, kPatterns> patterns{};
    QVector<int> chain;

    static std::array<Pad, kPads> defaultPads();
    bool audible(int pad) const;
    bool operator==(const Song &other) const {
        return bpm == other.bpm && swing == other.swing && grit == other.grit && vinyl == other.vinyl
               && pads == other.pads && patterns == other.patterns && chain == other.chain;
    }
    bool operator!=(const Song &other) const { return !(*this == other); }
};

// The kit's sounds, one a pad: boom, knock, crack, dusty, rim, clap, hat,
// openhat, shaker, tom, crash, clang, stab, bell, bass, horn.
const QStringList &kitIds();
QString patternName(int pattern); // "A" to "H"
int patternIndex(const QString &name);
QString gritName(Grit grit); // "clean", "12-bit", "crushed"
// How loud each of the four levels plays.
float levelGain(int level);

QJsonObject songToJson(const Song &song, const QString &baseDirectory);
// Fills song and returns true, or says what's wrong in error.
bool songFromJson(const QJsonObject &json, const QString &baseDirectory, Song *song, QString *error);
bool loadSong(const QString &path, Song *song, QString *error);
bool saveSong(const QString &path, const Song &song, QString *error);

// A dark, dusty loop, so the first Space makes a noise.
Song demoSong();
