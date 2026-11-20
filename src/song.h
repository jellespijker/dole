#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>
#include <array>

// The whole song as plain values: tempo, the pads and their sounds, eight
// patterns of up to four bars, and the chain of patterns that makes the
// song. Copying one is cheap (the patterns share their steps until one is
// changed), which is what undo and the audio thread rely on.

constexpr int kBanks = 4;     // A to D, as on an MPC
constexpr int kBankPads = 16; // a bank is the sixteen pads under the fingers
constexpr int kPads = kBanks * kBankPads;
constexpr int kSteps = 16;    // steps in a bar
constexpr int kMaxSteps = 64; // a pattern is one to four bars
constexpr int kPatterns = 8;
constexpr int kMinBpm = 40;
constexpr int kMaxBpm = 240;
constexpr int kMinSwing = 50;
constexpr int kMaxSwing = 75;
constexpr int kLevels = 4;
constexpr int kDefaultLevel = 3;
constexpr int kMinTune = -24;
constexpr int kMaxTune = 24;
constexpr int kGroups = 4; // mute (choke) groups; 0 is none
constexpr int kMaxChain = 256;
constexpr int kMaxEffects = 4;
constexpr int kLengths[] = {1, 2, 4, 8, 16}; // a synth's note, or a gated sample, in steps
constexpr int kMaxRoll = 4;
constexpr int kMaxNudge = 50; // percent of a step, early or late

// The master's character: clean, an SP-1200's 12-bit grit, or crushed further.
enum class Grit { Clean, TwelveBit, Crushed };

struct Step {
    int level = 0;    // 0 is off; 1 to 4 soft to accent
    int roll = 1;     // hits within the step: 1 to 4
    int chance = 100; // percent of the times round that it plays
    int nudge = 0;    // percent of a step it plays early (-) or late (+)

    bool on() const { return level > 0; }
    bool operator==(const Step &other) const {
        return level == other.level && roll == other.roll && chance == other.chance && nudge == other.nudge;
    }
    bool operator!=(const Step &other) const { return !(*this == other); }
};

// A lane moves one of a track's settings as the pattern plays: a value set
// on a step holds until the next one. By step, from 0.
using Lane = QMap<int, double>;
// A track's lanes by what they move: "pitch", "volume", "pan", "filter",
// "resonance", "decay", a synth's parameter ("synth.<id>"), or one of its
// effects' ("fx<n>.<id>", n from 1).
using Lanes = QMap<QString, Lane>;

struct Pattern {
    int length = kSteps; // 16, 32, 48, or 64 steps
    // A row of kMaxSteps steps for every pad; the steps past the length are
    // kept, so shortening a pattern and lengthening it again loses nothing.
    QVector<QVector<Step>> steps;
    QMap<int, Lanes> lanes; // by pad

    Pattern();
    bool isEmpty() const;
    bool operator==(const Pattern &other) const {
        return length == other.length && steps == other.steps && lanes == other.lanes;
    }
    bool operator!=(const Pattern &other) const { return !(*this == other); }
};

// A plugin in a chain: one of dole's own effects ("overdrive"), a CLAP
// plugin ("clap:<id>"), or an LV2 plugin ("lv2:<uri>"), with the settings
// dole has given it and, for plugins that keep one, their own saved state.
struct PluginSlot {
    QString id;
    QMap<QString, double> params;
    QByteArray state;
    bool bypass = false;

    bool isEmpty() const { return id.isEmpty(); }
    bool operator==(const PluginSlot &other) const {
        return id == other.id && params == other.params && state == other.state && bypass == other.bypass;
    }
    bool operator!=(const PluginSlot &other) const { return !(*this == other); }
};

struct Pad {
    QString sample; // absolute path, or empty for the pad's kit sound (bank A's)
    double start = 0; // the slice of the sample it plays, in seconds;
    double end = 0;   // an end of 0 plays to the end
    int tune = 0;     // semitones; pitch and speed together, as on an MPC
    double volume = 0.8;
    double pan = 0;       // -1 left to 1 right
    double filter = 0;    // below 0 a low-pass closing, above 0 a high-pass opening
    double resonance = 0; // 0 to 1
    double attack = 0;    // 0 to 1: how slowly each hit fades in, up to half a second
    double decay = 1;     // 0 to 1: how soon each hit dies away; 1 lets it ring
    bool reverse = false;
    bool gate = false; // a sample stops after the length, or when the pad is let go
    int group = 0;     // pads in one choke group cut each other off
    bool mute = false;
    bool solo = false;
    int swing = 0; // its own swing, 50 to 75, or 0 for the song's
    int loop = 0;  // steps before the track starts over, or 0 for the pattern's length
    PluginSlot synth;            // when set, the pad plays this instrument instead of a sample
    int length = 1;              // how many steps a note, or a gated sample, lasts
    QVector<PluginSlot> effects; // in order, after the sound

    bool isChop() const { return !sample.isEmpty() && (start > 0 || end > 0); }
    bool operator==(const Pad &other) const {
        auto same = [](double a, double b) { return qRound(a * 1000) == qRound(b * 1000); };
        return sample == other.sample && same(start, other.start) && same(end, other.end) && tune == other.tune
               && same(volume, other.volume) && same(pan, other.pan) && same(filter, other.filter)
               && same(resonance, other.resonance) && same(attack, other.attack) && same(decay, other.decay)
               && reverse == other.reverse && gate == other.gate && group == other.group && mute == other.mute
               && solo == other.solo && swing == other.swing && loop == other.loop && synth == other.synth
               && length == other.length && effects == other.effects;
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
    QVector<PluginSlot> master; // effects on the whole mix, before the grit

    static std::array<Pad, kPads> defaultPads();
    bool audible(int pad) const;
    bool operator==(const Song &other) const {
        return bpm == other.bpm && swing == other.swing && grit == other.grit && vinyl == other.vinyl
               && pads == other.pads && patterns == other.patterns && chain == other.chain && master == other.master;
    }
    bool operator!=(const Song &other) const { return !(*this == other); }
};

// The kit's sounds, one a pad of bank A: boom, knock, crack, dusty, rim,
// clap, hat, openhat, shaker, tom, crash, clang, stab, bell, bass, horn.
const QStringList &kitIds();
// A pad as people name it: "A1" to "D16".
QString padName(int pad);

// What a lane moves, and over what range.
struct LaneInfo {
    bool valid = false;
    double min = 0, max = 1;
    bool stepped = false;
    QString target; // "pad", "synth", or "fx"
    int effect = -1; // for "fx", the effect's index from 0
    QString param;   // the pad setting, or the plugin's parameter
};
LaneInfo laneInfo(const QString &id);
// The value a pad's built-in lane starts from when no step has set it.
double padValue(const Pad &pad, const QString &param);
// A lane's value on a step: the last one set at or before it, or nothing
// (NaN) before the first.
double laneValue(const Lane &lane, int step);
// How many steps of the song play: each slot's pattern, or the one pattern
// when there is no song.
int songSteps(const Song &song, int pattern);
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
