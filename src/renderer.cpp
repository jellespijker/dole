#include "renderer.h"

#include <algorithm>
#include <cmath>

double repeatSteps(int division) {
    static const double steps[kRepeatDivisions] = {0, 2, 1, 0.5, 4.0 / 3, 2.0 / 3};
    return division > 0 && division < kRepeatDivisions ? steps[division] : 0;
}

const char *repeatName(int division) {
    static const char *const names[kRepeatDivisions] = {"", "1/8", "1/16", "1/32", "1/8T", "1/16T"};
    return division > 0 && division < kRepeatDivisions ? names[division] : "";
}

Renderer::Renderer() = default;

void Renderer::setSong(const Song &song) {
    m_song = song;
}

void Renderer::setSounds(const std::array<SampleBuffer, kPads> &sounds) {
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
    m_nextRepeat = 0;
    m_suppressed.clear();
    m_playing = true;
}

void Renderer::cue(int pattern) {
    m_cued = qBound(0, pattern, kPatterns - 1);
    if (!m_playing)
        m_pattern = m_cued;
}

void Renderer::stop() {
    m_playing = false;
    m_suppressed.clear();
}

bool Renderer::sounding() const {
    for (const auto &voices : m_voices)
        for (const Voice &voice : voices)
            if (voice.active)
                return true;
    return m_preview.active;
}

void Renderer::hit(int pad, int level) {
    if (pad >= 0 && pad < kPads)
        trigger(pad, levelGain(level));
}

void Renderer::hold(int pad, bool held, int level) {
    if (pad < 0 || pad >= kPads)
        return;
    m_held[pad] = held ? level : 0;
    // The first repeat comes on the next beat of the division, not at once:
    // the press itself was the first hit.
    const double division = repeatSteps(m_repeat);
    if (held && division > 0)
        m_nextRepeat = (std::floor(repeatClock() / division) + 1) * division;
}

void Renderer::setRepeat(int division) {
    m_repeat = qBound(0, division, kRepeatDivisions - 1);
    const double steps = repeatSteps(m_repeat);
    if (steps > 0)
        m_nextRepeat = (std::floor(repeatClock() / steps) + 1) * steps;
}

void Renderer::suppress(int pad, int pattern, int step) {
    m_suppressed.append({pad, pattern, step, 2 * stepFrames()});
}

void Renderer::preview(const SampleBuffer &sound, double rate) {
    if (sound)
        m_preview = Voice{sound, 0, rate, 0.9f, 1, 0, true};
}

// Swing pushes every second sixteenth late: at 50% not at all, at 66% to
// the triplet, at 75% halfway to the next step.
double Renderer::triggerTime(int step) const {
    return step + ((step & 1) ? (m_song.swing - 50) / 50.0 : 0.0);
}

void Renderer::trigger(int pad, float gain) {
    if (!m_sounds[pad])
        return;
    // A pad in a mute group silences the others in it.
    const int group = m_song.pads[pad].group;
    if (group > 0)
        for (int other = 0; other < kPads; ++other)
            if (other != pad && m_song.pads[other].group == group)
                for (Voice &voice : m_voices[other])
                    cut(voice);
    auto &voices = m_voices[pad];
    // The voice still playing fades out under the new one, rather than
    // stopping on a click.
    if (voices[0].active) {
        voices[1] = voices[0];
        cut(voices[1]);
    }
    const double rate = std::pow(2.0, m_song.pads[pad].tune / 12.0);
    voices[0] = Voice{m_sounds[pad], 0, rate, gain, 1, 0, true};
}

void Renderer::cut(Voice &voice) {
    if (voice.active && voice.fadeStep == 0)
        voice.fadeStep = 1.f / (0.004f * kRate);
}

void Renderer::fire(int step) {
    const Pattern &pattern = m_song.patterns[m_pattern];
    for (int pad = 0; pad < kPads; ++pad) {
        const Step &s = pattern.steps[pad][step];
        if (!s.on())
            continue;
        const auto skipped = std::find_if(m_suppressed.begin(), m_suppressed.end(), [&](const Suppressed &x) {
            return x.pad == pad && x.pattern == m_pattern && x.step == step;
        });
        if (skipped != m_suppressed.end()) {
            m_suppressed.erase(skipped);
            continue;
        }
        trigger(pad, levelGain(s.level));
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

// After the soft limit: the grit of an old sampler, then the record it
// was sampled from.
void Renderer::master(float *left, float *right) {
    float *channels[2] = {left, right};
    if (m_song.grit != Grit::Clean) {
        // An SP-1200 samples at 26 kHz and 12 bits; crushed goes further.
        const bool crushed = m_song.grit == Grit::Crushed;
        const double rate = crushed ? 16000 : 26040;
        const float steps = crushed ? 128 : 2048;
        const double cutoff = crushed ? 6500 : 12000;
        const float drive = crushed ? 2.2f : 1.4f;
        m_crushPhase += rate / kRate;
        const bool sample = m_crushPhase >= 1;
        if (sample)
            m_crushPhase -= 1;
        for (int c = 0; c < 2; ++c) {
            if (sample)
                m_crushed[c] = std::round(*channels[c] * steps) / steps;
            const float filtered = float(m_gritFilter[c].lowpass(m_crushed[c], cutoff, 0.7));
            *channels[c] = std::tanh(drive * filtered) / drive * 1.2f;
        }
    }
    if (m_song.vinyl && m_playing) {
        auto noise = [this] {
            m_noise = m_noise * 1664525u + 1013904223u;
            return float(m_noise >> 8) / float(1 << 23) - 1.f;
        };
        // Now and then a speck of dust: a click that dies in a millisecond.
        if (std::abs(noise()) < 7.0f / kRate) {
            m_crackle = noise() * 0.25f;
            m_crackleDecay = std::exp(-1.f / (0.0006f * kRate));
        }
        const float hiss = float(m_hissFilter.lowpass(noise(), 5000, 0.7)) * 0.012f;
        const float dust = m_crackle;
        m_crackle *= m_crackleDecay;
        *left += hiss + dust;
        *right += hiss + dust * 0.8f;
    }
}

void Renderer::render(float *out, int frames, const StepCallback &onStep, const RepeatCallback &onRepeat) {
    std::array<float, kPads> gains;
    for (int p = 0; p < kPads; ++p)
        gains[p] = m_song.audible(p) ? float(m_song.pads[p].volume) : 0.f;
    const double stepsPerFrame = 1.0 / stepFrames();
    const double division = repeatSteps(m_repeat);
    bool anyHeld = false;
    for (int level : m_held)
        anyHeld = anyHeld || level > 0;

    for (int i = 0; i < frames; ++i) {
        if (m_playing) {
            while (m_nextStep < kSteps && m_position >= triggerTime(m_nextStep)) {
                fire(m_nextStep);
                if (onStep)
                    onStep(m_chainIndex, m_pattern, m_nextStep, i);
                ++m_nextStep;
            }
        }
        if (anyHeld && division > 0 && repeatClock() >= m_nextRepeat) {
            for (int pad = 0; pad < kPads; ++pad) {
                if (!m_held[pad])
                    continue;
                trigger(pad, levelGain(m_held[pad]));
                if (m_playing && onRepeat)
                    onRepeat(pad, m_pattern, int(std::lround(m_position)) % kSteps, i);
            }
            m_nextRepeat = (std::floor(repeatClock() / division) + 1) * division;
        }

        float left = 0, right = 0;
        for (int p = 0; p < kPads; ++p)
            for (Voice &voice : m_voices[p])
                mixVoice(voice, gains[p], &left, &right);
        mixVoice(m_preview, 1, &left, &right);

        // A soft limit, so a busy bar thickens rather than clips, with room
        // left for the grit's drive.
        left = std::tanh(left * 0.7f);
        right = std::tanh(right * 0.7f);
        master(&left, &right);
        out[2 * i] = left;
        out[2 * i + 1] = right;

        m_freeClock += stepsPerFrame;
        if (m_freeClock > 1e6) {
            m_freeClock -= 1e6;
            m_nextRepeat -= 1e6;
        }
        if (m_playing) {
            m_position += stepsPerFrame;
            if (m_position >= kSteps) {
                m_position -= kSteps;
                m_nextRepeat -= kSteps;
                m_nextStep = 0;
                ++m_bars;
                if (m_stopAfter > 0 && m_bars >= m_stopAfter)
                    stop();
                else
                    advanceBar();
            }
        }
    }

    for (Suppressed &s : m_suppressed)
        s.framesLeft -= frames;
    m_suppressed.removeIf([](const Suppressed &s) { return s.framesLeft <= 0; });
}

QVector<float> renderSong(const Song &song, const std::array<SampleBuffer, kPads> &sounds, int pattern) {
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
