#pragma once

#include <array>
#include <functional>
#include <vector>

#include "dsp.h"
#include "rack.h"
#include "song.h"
#include "sounds.h"

// The note repeat divisions, in steps (sixteenths): off, 1/8, 1/16, 1/32,
// 1/8 triplets, 1/16 triplets.
constexpr int kRepeatDivisions = 6;
double repeatSteps(int division);
const char *repeatName(int division);

// A track's sound settings as they are at a moment: its own, moved by the
// pattern's lanes while it plays.
struct PadState {
    double pitch = 0; // semitones, on top of the pad's tune
    double volume = 0.8;
    double pan = 0;
    double filter = 0;
    double resonance = 0;
    double decay = 1;
};

// Turns a song into sound, a buffer at a time. It knows nothing of audio
// devices or threads: the live engine calls it from its audio thread, and
// export calls it in a loop, so what plays is exactly what is exported.
class Renderer {
public:
    // Called as each step of the pattern plays: which slot of the song,
    // which pattern, which of its steps, and at which frame of the buffer.
    using StepCallback = std::function<void(int chainIndex, int pattern, int step, int frame)>;
    // Called as note repeat plays a held pad while the sequence runs, with
    // the step of the pattern it lands nearest.
    using RepeatCallback = std::function<void(int pad, int pattern, int step, int frame)>;

    Renderer();

    void setSong(const Song &song);
    // A sound for each pad, already sliced; an empty one plays nothing.
    void setSounds(const std::array<SampleBuffer, kPads> &sounds);
    // The plugins: a synth on a pad takes the place of its sound, and the
    // effects follow, on the pad and then on the master.
    void setRack(const Rack &rack) { m_rack = rack; }

    // Starts at the top of a pattern: the song from one of its slots, or
    // one pattern looping.
    void play(bool songMode, int pattern, int chainIndex);
    // Pattern looping moves to another pattern when this one ends, as an
    // MPC does, so the groove never trips.
    void cue(int pattern);
    // Stops sequencing; what is sounding rings out, and the plugin settings
    // the lanes moved go back to the song's.
    void stop();
    // Stops sequencing after this many patterns (for export); 0 never stops.
    void stopAfterBars(int patterns) { m_stopAfter = patterns; }
    bool playing() const { return m_playing; }
    // Whether anything can still be heard.
    bool sounding() const;

    // A pad struck now, live, and let go: a gated pad stops when let go.
    void hit(int pad, int level);
    void release(int pad);
    // Held pads repeat at the note repeat division, in time with the song.
    void hold(int pad, bool held, int level = kDefaultLevel);
    void setRepeat(int division);
    // A live hit was just recorded onto a step the sequence may not have
    // reached yet; that step skips the pad once, so it isn't heard twice.
    void suppress(int pad, int pattern, int step);
    // Plays any sound once, at a rate, as a chop is auditioned.
    void preview(const SampleBuffer &sound, double rate);

    // Renders frames of interleaved stereo into out.
    void render(float *out, int frames, const StepCallback &onStep = {}, const RepeatCallback &onRepeat = {});

    // The loudest each pad, and then the whole mix, got since the last ask,
    // for the mixer's meters.
    std::array<float, kPads + 1> takePeaks();

    // Frames in one step at the song's tempo.
    double stepFrames() const { return kRate * 60.0 / m_song.bpm / 4.0; }

private:
    struct Voice {
        SampleBuffer sound;
        double position = 0;
        double rate = 1; // negative plays it backwards
        float gain = 0;
        float fade = 1; // 1 while playing; falls to 0 once cut off
        float fadeStep = 0;
        float attack = 1;     // rises to 1 as it fades in
        float attackStep = 0; // 0 when it starts at full
        float level = 1;      // falls as it decays
        double releaseIn = -1; // frames until a gated hit lets go; -1 never
        bool active = false;
    };
    // A hit on its way: when, in steps from the start, and how.
    struct Hit {
        double time;
        int pad;
        float gain;
        double pitch;
    };
    // A step of the pattern on its way: the playhead, and the lanes.
    struct Tick {
        double time;
        int chainIndex, pattern, step;
        int sinceEntered; // steps since the pattern came in, which tracks with their own loop count by
    };
    struct Suppressed {
        int pad, pattern, step;
        double framesLeft;
    };
    // A lane of the song, found once when the song arrives.
    struct CompiledLane {
        int pattern, pad;
        QString target; // "pad", "synth", or "fx"
        int effect;
        QString param;
        const Lane *points;
        double lastSet;
    };

    void renderBlock(float *out, int frames, const StepCallback &onStep, const RepeatCallback &onRepeat);
    void schedule(int tick);
    void applyLanes(const Tick &tick);
    void trigger(int pad, float gain, double pitch, bool sequenced);
    bool isSynth(int pad) const { return !m_song.pads[pad].synth.isEmpty(); }
    int padStep(int pad, int pattern, int sinceEntered) const;
    Processor *laneProcessor(const CompiledLane &lane) const;
    void restore(const CompiledLane &lane);
    void halt();
    void noteOff(int pad, int frame);
    void cut(Voice &voice, float seconds = 0.004f);
    void baseStates();
    void updateDecay(int pad);
    void mixVoice(Voice &voice, int pad, float *left, float *right);
    void master(float *left, float *right);
    double repeatClock() const { return m_playing ? m_clock : m_freeClock; }

    Song m_song;
    std::vector<CompiledLane> m_lanes;
    std::array<SampleBuffer, kPads> m_sounds{};
    Rack m_rack;
    // Each pad's sound for the block, kept apart until its effects have run.
    std::vector<float> m_padBuffers = std::vector<float>(kPads * 2 * kMaxBlock);
    std::vector<float> m_masterBuffer = std::vector<float>(2 * kMaxBlock);
    float *padChannel(int pad, int channel) { return m_padBuffers.data() + (pad * 2 + channel) * kMaxBlock; }
    // Notes for the synth pads in this block, the key each is holding, and
    // when it lets go (in frames from the start of the block).
    std::array<QVector<NoteEvent>, kPads> m_notes;
    std::array<int, kPads> m_heldKey;
    std::array<double, kPads> m_releaseAt;
    int m_frame = 0; // the frame of the block being rendered; 0 between blocks
    // Two voices a pad: the one playing, and the one fading out under it.
    std::array<std::array<Voice, 2>, kPads> m_voices{};
    Voice m_preview;
    // Each pad's settings now, and what its sound passes through.
    std::array<PadState, kPads> m_state{};
    std::array<float, kPads> m_decayStep{}; // how much a voice keeps each frame once it has faded in
    std::array<std::array<Svf, 2>, kPads> m_filters{};
    std::array<double, kPads> m_filterNow{}; // the filter as heard, gliding to the setting
    std::array<double, kPads> m_volumeNow{};
    std::array<double, kPads> m_panNow{};
    std::array<float, kPads + 1> m_peaks{};

    bool m_playing = false;
    bool m_songMode = false;
    int m_cued = 0;
    double m_clock = 0; // in steps since playing started
    // What is scheduled, half a step ahead of what is heard.
    int m_nextTick = 0;
    int m_schedPattern = 0;
    int m_schedChain = 0;
    int m_schedStart = 0;   // the tick its pass began on
    int m_schedEntered = 0; // the tick the pattern came in on
    int m_passes = 0;
    int m_stopAfter = 0;
    int m_stopAt = -1; // the tick playing ends on, once known
    std::vector<Hit> m_hits;
    std::vector<Tick> m_ticks;
    // What is heard: the last step that played, and when.
    Tick m_heard{0, 0, 0, 0, 0};
    quint32 m_chance = 1;

    int m_repeat = 0;
    std::array<int, kPads> m_held{}; // the level a held pad repeats at; 0 if not held
    double m_nextRepeat = 0;
    double m_freeClock = 0; // keeps time for note repeat while stopped
    QVector<Suppressed> m_suppressed;

    // The master's grit: held samples, the filter after them, and vinyl.
    double m_crushPhase = 1;
    float m_crushed[2] = {0, 0};
    Svf m_gritFilter[2];
    Svf m_hissFilter;
    quint32 m_noise = 0x9E3779B9u;
    float m_crackle = 0;
    float m_crackleDecay = 0;
};

// The whole song once (or, with no song, the given pattern once), through
// its plugins, with the last sounds left to ring out, as interleaved stereo.
QVector<float> renderSong(const Song &song, const std::array<SampleBuffer, kPads> &sounds, const Rack &rack,
                          int pattern);
