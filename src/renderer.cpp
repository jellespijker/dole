#include "renderer.h"

#include <cmath>

namespace {

constexpr double kTwoPi = 6.283185307179586;

// Rounds off the corner a naive saw or square has at its jump, the cheapest
// way to keep a bass from aliasing into a whine.
double polyBlep(double t, double dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1;
    }
    if (t > 1 - dt) {
        t = (t - 1) / dt;
        return t * t + t + t + 1;
    }
    return 0;
}

double coefficient(double seconds) {
    return std::exp(-1.0 / (seconds * kRate));
}

} // namespace

void Renderer::setSong(const Song &song) {
    m_song = song;
}

void Renderer::setSounds(const std::array<SampleBuffer, kTracks> &sounds) {
    m_sounds = sounds;
}

void Renderer::play(bool songMode, int pattern, int chainIndex) {
    m_songMode = songMode;
    if (songMode && !m_song.chain.isEmpty()) {
        m_chainIndex = qBound(0, chainIndex, int(m_song.chain.size()) - 1);
        m_pattern = m_song.chain[m_chainIndex];
    } else {
        m_chainIndex = 0;
        m_pattern = qBound(0, pattern, kPatterns - 1);
    }
    m_cued = m_pattern;
    m_position = 0;
    m_nextStep = 0;
    m_bars = 0;
    m_playing = true;
}

void Renderer::cue(int pattern) {
    m_cued = qBound(0, pattern, kPatterns - 1);
    if (!m_playing)
        m_pattern = m_cued;
}

void Renderer::stop() {
    m_playing = false;
    noteOff();
}

bool Renderer::sounding() const {
    for (const auto &voices : m_voices)
        for (const Voice &voice : voices)
            if (voice.active)
                return true;
    return m_bass.gate || m_bass.amp > 1e-4;
}

void Renderer::audition(int track, const Step &step) {
    if (step.kind != StepKind::Hit || track < 0 || track >= kTracks)
        return;
    if (track == kBass) {
        noteOn(step.note);
        m_bass.auditionFrames = int(stepFrames() * 2);
        return;
    }
    if (track == kHat)
        for (Voice &voice : m_voices[kOpenHat])
            choke(voice);
    trigger(track, levelGain(step.level));
}

// Swing pushes every second sixteenth late: at 50% not at all, at 66% to
// the triplet, at 75% halfway to the next step.
double Renderer::triggerTime(int step) const {
    return step + ((step & 1) ? (m_song.swing - 50) / 50.0 : 0.0);
}

void Renderer::trigger(int track, float gain, double rate) {
    if (!m_sounds[track])
        return;
    auto &voices = m_voices[track];
    // The voice still playing fades out under the new one, rather than
    // stopping on a click.
    if (voices[0].active) {
        voices[1] = voices[0];
        choke(voices[1]);
    }
    voices[0] = Voice{m_sounds[track], 0, rate, gain, 1, 0, true};
}

void Renderer::choke(Voice &voice) {
    if (voice.active && voice.fadeStep == 0)
        voice.fadeStep = 1.f / (0.004f * kRate);
}

void Renderer::noteOn(int note) {
    m_bass.gate = true;
    m_bass.frequency = 440.0 * std::pow(2.0, (note - 69) / 12.0);
    m_bass.filterEnvelope = 1;
    m_bass.velocity = 1;
    m_bass.auditionFrames = 0;
    if (m_sounds[kBass])
        m_bass.sample = Voice{m_sounds[kBass], 0, std::pow(2.0, (note - kDefaultNote) / 12.0), 1, 1, 0, true};
}

void Renderer::noteOff() {
    m_bass.gate = false;
}

void Renderer::fire(int step) {
    const Pattern &pattern = m_song.patterns[m_pattern];
    for (int t = 0; t < kTracks; ++t) {
        const Step &s = pattern.steps[t][step];
        if (t == kBass) {
            // A hold leaves the note sounding; silence lets it go.
            if (s.kind == StepKind::Hit)
                noteOn(s.note);
            else if (s.kind == StepKind::Off)
                noteOff();
            continue;
        }
        if (s.kind != StepKind::Hit)
            continue;
        // The closed hat cuts the open one off, as on a real kit.
        if (t == kHat)
            for (Voice &voice : m_voices[kOpenHat])
                choke(voice);
        trigger(t, levelGain(s.level));
    }
}

void Renderer::advanceBar() {
    if (m_songMode && !m_song.chain.isEmpty()) {
        m_chainIndex = (m_chainIndex + 1) % m_song.chain.size();
        m_pattern = m_song.chain[m_chainIndex];
    } else {
        m_pattern = m_cued;
    }
}

void Renderer::mixVoice(Voice &voice, float gain, float *left, float *right) {
    if (!voice.active)
        return;
    const QVector<float> &data = *voice.sound;
    const qsizetype index = qsizetype(voice.position);
    if (2 * index + 3 >= data.size()) {
        voice.active = false;
        return;
    }
    const float frac = float(voice.position - index);
    const float *frame = data.constData() + 2 * index;
    const float g = gain * voice.gain * voice.fade;
    *left += (frame[0] + (frame[2] - frame[0]) * frac) * g;
    *right += (frame[1] + (frame[3] - frame[1]) * frac) * g;
    voice.position += voice.rate;
    if (voice.fadeStep > 0) {
        voice.fade -= voice.fadeStep;
        if (voice.fade <= 0)
            voice.active = false;
    }
}

void Renderer::render(float *out, int frames, const StepCallback &onStep) {
    static const double attack = 1 - coefficient(0.003);
    static const double release = coefficient(0.06);
    static const double filterDecay = coefficient(0.15);

    std::array<float, kTracks> gains;
    for (int t = 0; t < kTracks; ++t)
        gains[t] = m_song.audible(t) ? float(m_song.tracks[t].volume) : 0.f;
    const double stepsPerFrame = 1.0 / stepFrames();

    for (int i = 0; i < frames; ++i) {
        if (m_playing) {
            while (m_nextStep < kSteps && m_position >= triggerTime(m_nextStep)) {
                fire(m_nextStep);
                if (onStep)
                    onStep(m_chainIndex, m_pattern, m_nextStep, i);
                ++m_nextStep;
            }
        }

        float left = 0, right = 0;
        for (int t = 0; t < kBass; ++t)
            for (Voice &voice : m_voices[t])
                mixVoice(voice, gains[t], &left, &right);

        // The bass: one note at a time, from the synthesizer or a sample.
        Bass &bass = m_bass;
        if (bass.auditionFrames > 0 && --bass.auditionFrames == 0 && !m_playing)
            noteOff();
        if (bass.gate)
            bass.amp += (1 - bass.amp) * attack;
        else
            bass.amp *= release;
        if (bass.amp > 1e-5) {
            const float gain = gains[kBass] * float(bass.amp) * bass.velocity;
            if (m_sounds[kBass]) {
                mixVoice(bass.sample, gain, &left, &right);
            } else {
                const double dt = bass.frequency / kRate;
                const double saw = 2 * bass.phase - 1 - polyBlep(bass.phase, dt);
                const double square = (bass.phase < 0.5 ? 1 : -1) + polyBlep(bass.phase, dt)
                                      - polyBlep(std::fmod(bass.phase + 0.5, 1.0), dt);
                const double sub = std::sin(kTwoPi * bass.subPhase);
                bass.phase = std::fmod(bass.phase + dt, 1.0);
                bass.subPhase = std::fmod(bass.subPhase + dt / 2, 1.0);
                const double cutoff = 160 + 1800 * bass.filterEnvelope;
                bass.filterEnvelope *= filterDecay;
                const float value =
                    float(bass.filter.lowpass(0.5 * saw + 0.3 * square + 0.35 * sub, cutoff, 0.9)) * 0.8f * gain;
                left += value;
                right += value;
            }
        } else {
            bass.amp = 0;
        }

        // A soft limit on the master, so a busy bar thickens rather than clips.
        out[2 * i] = std::tanh(left * 0.9f);
        out[2 * i + 1] = std::tanh(right * 0.9f);

        if (m_playing) {
            m_position += stepsPerFrame;
            if (m_position >= kSteps) {
                m_position -= kSteps;
                m_nextStep = 0;
                ++m_bars;
                if (m_stopAfter > 0 && m_bars >= m_stopAfter)
                    stop();
                else
                    advanceBar();
            }
        }
    }
}

QVector<float> renderSong(const Song &song, const std::array<SampleBuffer, kTracks> &sounds, int pattern) {
    Renderer renderer;
    renderer.setSong(song);
    renderer.setSounds(sounds);
    const int bars = song.chain.isEmpty() ? 1 : int(song.chain.size());
    renderer.play(!song.chain.isEmpty(), pattern, 0);
    renderer.stopAfterBars(bars);

    constexpr int block = 1024;
    QVector<float> out;
    float buffer[block * 2];
    while (renderer.playing()) {
        renderer.render(buffer, block);
        out.append(QList<float>(buffer, buffer + block * 2));
    }
    // Let the last sounds ring out, up to four seconds.
    for (int tail = 0; tail < 4 * kRate && renderer.sounding(); tail += block) {
        renderer.render(buffer, block);
        out.append(QList<float>(buffer, buffer + block * 2));
    }
    // Then trim the silence, but never into the music.
    const qsizetype musical = qsizetype(std::ceil(bars * kSteps * renderer.stepFrames())) * 2;
    qsizetype end = out.size();
    while (end > musical && std::abs(out[end - 1]) < 1e-4f && std::abs(out[end - 2]) < 1e-4f)
        end -= 2;
    out.resize(std::max(end, std::min(musical, out.size())));
    return out;
}
