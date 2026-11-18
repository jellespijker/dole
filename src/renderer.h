#pragma once

#include <array>
#include <functional>

#include "dsp.h"
#include "song.h"
#include "sounds.h"

// Turns a song into sound, a buffer at a time. It knows nothing of audio
// devices or threads: the live engine calls it from its audio thread, and
// export calls it in a loop, so what plays is exactly what is exported.
class Renderer {
public:
    // Called as each step plays: which slot of the song, which pattern, which
    // step, and at which frame of the buffer being rendered.
    using StepCallback = std::function<void(int chainIndex, int pattern, int step, int frame)>;

    void setSong(const Song &song);
    // A sound for each track; an empty one on the bass plays the synthesizer,
    // on a drum track nothing.
    void setSounds(const std::array<SampleBuffer, kTracks> &sounds);

    // Starts at the top of a bar: the song from one of its slots, or one
    // pattern looping.
    void play(bool songMode, int pattern, int chainIndex);
    // Pattern looping moves to another pattern at the next bar, as drum
    // machines do, so the groove never trips.
    void cue(int pattern);
    // Stops sequencing; what is sounding rings out.
    void stop();
    // Stops sequencing after this many bars (for export); 0 never stops.
    void stopAfterBars(int bars) { m_stopAfter = bars; }
    bool playing() const { return m_playing; }
    // Whether anything can still be heard.
    bool sounding() const;

    // Plays one step on one track right away, so an edit can be heard.
    void audition(int track, const Step &step);

    // Renders frames of interleaved stereo into out.
    void render(float *out, int frames, const StepCallback &onStep = {});

    // Frames in one step at the song's tempo.
    double stepFrames() const { return kRate * 60.0 / m_song.bpm / 4.0; }

private:
    struct Voice {
        SampleBuffer sound;
        double position = 0;
        double rate = 1;
        float gain = 0;
        float fade = 1;      // 1 while playing; falls to 0 once choked
        float fadeStep = 0;
        bool active = false;
    };
    struct Bass {
        bool gate = false;
        double amp = 0;
        double filterEnvelope = 0;
        double phase = 0;
        double subPhase = 0;
        double frequency = 55;
        float velocity = 0;
        Svf filter;
        Voice sample; // a loaded sample, pitched by the note
        int auditionFrames = 0;
    };

    void fire(int step);
    void trigger(int track, float gain, double rate = 1.0);
    void choke(Voice &voice);
    void noteOn(int note);
    void noteOff();
    void advanceBar();
    double triggerTime(int step) const;
    void mixVoice(Voice &voice, float gain, float *left, float *right);
    float bassFrame();

    Song m_song;
    std::array<SampleBuffer, kTracks> m_sounds{};
    // Two voices a track: the one playing, and the one fading out under it.
    std::array<std::array<Voice, 2>, kTracks> m_voices{};
    Bass m_bass;

    bool m_playing = false;
    bool m_songMode = false;
    int m_pattern = 0;
    int m_cued = 0;
    int m_chainIndex = 0;
    double m_position = 0; // in steps, from the top of the bar
    int m_nextStep = 0;
    int m_bars = 0;
    int m_stopAfter = 0;
};

// The whole song once (or, with no song, the given pattern once), with the
// last sounds left to ring out, as interleaved stereo.
QVector<float> renderSong(const Song &song, const std::array<SampleBuffer, kTracks> &sounds, int pattern);
