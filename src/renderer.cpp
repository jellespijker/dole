#include "renderer.h"

#include <algorithm>
#include <cmath>

namespace {

// Which of a track's settings a built-in lane moves.
enum Field { Pitch, Volume, Pan, Filter, Resonance, Decay, NotPad };

Field fieldOf(const QString &param) {
    static const QStringList fields{"pitch", "volume", "pan", "filter", "resonance", "decay"};
    const int index = int(fields.indexOf(param));
    return index < 0 ? NotPad : Field(index);
}

// How quickly volume and pan glide to a new setting: a few milliseconds,
// so a lane that jumps doesn't click.
const double kGlide = 1.0 - std::exp(-1.0 / (0.005 * kRate));

} // namespace

double repeatSteps(int division) {
    static const double steps[kRepeatDivisions] = {0, 2, 1, 0.5, 4.0 / 3, 2.0 / 3};
    return division > 0 && division < kRepeatDivisions ? steps[division] : 0;
}

const char *repeatName(int division) {
    static const char *const names[kRepeatDivisions] = {"", "1/8", "1/16", "1/32", "1/8T", "1/16T"};
    return division > 0 && division < kRepeatDivisions ? names[division] : "";
}

Renderer::Renderer() {
    m_heldKey.fill(-1);
    m_releaseAt.fill(0);
    // Room for every hit a few steps can hold, so the audio thread never
    // has to ask for memory.
    m_hits.reserve(4096);
    m_ticks.reserve(64);
    baseStates();
    for (int p = 0; p < kPads; ++p) {
        m_volumeNow[p] = m_state[p].volume;
        m_panNow[p] = m_state[p].pan;
    }
}

void Renderer::setSong(const Song &song) {
    const std::vector<CompiledLane> old = m_lanes;
    m_song = song;
    // The lanes are found once here, not on every step. They point into the
    // song, which the renderer only reads, so they stay put until the next.
    m_lanes.clear();
    const Song &read = m_song;
    for (int p = 0; p < kPatterns; ++p) {
        const QMap<int, Lanes> &lanes = read.patterns[p].lanes;
        for (auto pad = lanes.constBegin(); pad != lanes.constEnd(); ++pad) {
            if (pad.key() < 0 || pad.key() >= kPads)
                continue;
            for (auto lane = pad.value().constBegin(); lane != pad.value().constEnd(); ++lane) {
                const LaneInfo info = laneInfo(lane.key());
                if (info.valid)
                    m_lanes.push_back({p, pad.key(), info.target, info.effect, info.param, &lane.value(), std::nan("")});
            }
        }
    }
    // A plugin setting a lane no longer moves goes back to the song's.
    for (const CompiledLane &lane : old) {
        if (lane.target == "pad")
            continue;
        const bool kept = std::any_of(m_lanes.begin(), m_lanes.end(), [&](const CompiledLane &now) {
            return now.pattern == lane.pattern && now.pad == lane.pad && now.target == lane.target
                   && now.effect == lane.effect && now.param == lane.param;
        });
        if (!kept)
            restore(lane);
    }
    // Stopped, the settings simply arrive; playing, they glide.
    if (!m_playing) {
        baseStates();
        for (int p = 0; p < kPads; ++p) {
            m_volumeNow[p] = m_song.audible(p) ? m_state[p].volume : 0.0;
            m_panNow[p] = m_state[p].pan;
            m_filterNow[p] = m_state[p].filter;
        }
    }
}

void Renderer::setSounds(const std::array<SampleBuffer, kPads> &sounds) {
    m_sounds = sounds;
}

void Renderer::play(bool songMode, int pattern, int chainIndex) {
    m_songMode = songMode && !m_song.chain.isEmpty();
    if (m_songMode) {
        m_schedChain = qBound(0, chainIndex, int(m_song.chain.size()) - 1);
        m_schedPattern = std::as_const(m_song.chain)[m_schedChain];
    } else {
        m_schedChain = 0;
        m_schedPattern = qBound(0, pattern, kPatterns - 1);
    }
    m_cued = m_schedPattern;
    m_clock = 0;
    m_nextTick = 0;
    m_schedStart = 0;
    m_schedEntered = 0;
    m_passes = 0;
    m_stopAt = -1;
    m_hits.clear();
    m_ticks.clear();
    m_heard = Tick{0, m_schedChain, m_schedPattern, 0, 0};
    m_nextRepeat = 0;
    m_chance = 1; // the same chances every time, so an export matches what was heard
    m_suppressed.clear();
    m_playing = true;
}

void Renderer::cue(int pattern) {
    m_cued = qBound(0, pattern, kPatterns - 1);
}

void Renderer::halt() {
    m_playing = false;
    m_suppressed.clear();
    m_hits.clear();
    m_ticks.clear();
    // Synths let go of their notes, as a drum machine's would ring out.
    for (int p = 0; p < kPads; ++p)
        noteOff(p, m_frame);
}

void Renderer::stop() {
    halt();
    for (const CompiledLane &lane : m_lanes)
        if (lane.target != "pad")
            restore(lane);
    for (CompiledLane &lane : m_lanes)
        lane.lastSet = std::nan("");
    baseStates();
}

bool Renderer::sounding() const {
    for (const auto &voices : m_voices)
        for (const Voice &voice : voices)
            if (voice.active)
                return true;
    for (int key : m_heldKey)
        if (key >= 0)
            return true;
    return m_preview.active;
}

void Renderer::hit(int pad, int level) {
    if (pad >= 0 && pad < kPads)
        trigger(pad, levelGain(level), m_state[pad].pitch, false);
}

void Renderer::release(int pad) {
    if (pad < 0 || pad >= kPads || !m_song.pads[pad].gate)
        return;
    for (Voice &voice : m_voices[pad])
        cut(voice, 0.01f);
    noteOff(pad, m_frame);
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
    if (sound) {
        m_preview = Voice{};
        m_preview.sound = sound;
        m_preview.rate = rate;
        m_preview.gain = 0.9f;
        m_preview.active = true;
    }
}

std::array<float, kPads + 1> Renderer::takePeaks() {
    const std::array<float, kPads + 1> peaks = m_peaks;
    m_peaks.fill(0);
    return peaks;
}

// A track plays its own loop when it has one, counted from when the
// pattern came in, so a three-step loop drifts against the bar.
int Renderer::padStep(int pad, int pattern, int sinceEntered) const {
    const int loop = m_song.pads[pad].loop;
    const int length = loop > 0 ? loop : m_song.patterns[pattern].length;
    return sinceEntered % std::max(1, length);
}

void Renderer::baseStates() {
    for (int p = 0; p < kPads; ++p) {
        const Pad &pad = m_song.pads[p];
        m_state[p] = PadState{0, pad.volume, pad.pan, pad.filter, pad.resonance, pad.decay};
        updateDecay(p);
    }
}

// Decay sets how long a hit takes to die away: from 10 ms at 0 to most of
// two seconds just under 1, where 1 lets it ring.
void Renderer::updateDecay(int pad) {
    const double decay = m_state[pad].decay;
    m_decayStep[pad] = decay >= 0.999 ? 1.f : float(std::exp(-1.0 / (0.01 * std::pow(150.0, decay) * kRate)));
}

// Schedules a step half a step before it is due, so a step nudged early
// can still play early.
void Renderer::schedule(int tick) {
    const Song &song = m_song;
    if (tick - m_schedStart >= song.patterns[m_schedPattern].length) {
        ++m_passes;
        if (m_stopAfter > 0 && m_passes >= m_stopAfter) {
            m_stopAt = tick;
            return;
        }
        int next = m_cued;
        if (m_songMode && !song.chain.isEmpty()) {
            m_schedChain = (m_schedChain + 1) % int(song.chain.size());
            next = song.chain[m_schedChain];
        }
        // A pattern that carries on keeps its tracks' loops going.
        if (next != m_schedPattern)
            m_schedEntered = tick;
        m_schedPattern = next;
        m_schedStart = tick;
    }
    const Pattern &pattern = song.patterns[m_schedPattern];
    const int step = tick - m_schedStart;
    const int since = tick - m_schedEntered;
    m_ticks.push_back(Tick{double(tick), m_schedChain, m_schedPattern, step, since});

    for (int pad = 0; pad < kPads; ++pad) {
        const int s = padStep(pad, m_schedPattern, since);
        const Step &hit = pattern.steps[pad][s];
        if (!hit.on())
            continue;
        const auto skipped = std::find_if(m_suppressed.begin(), m_suppressed.end(), [&](const Suppressed &x) {
            return x.pad == pad && x.pattern == m_schedPattern && x.step == s;
        });
        if (skipped != m_suppressed.end()) {
            m_suppressed.erase(skipped);
            continue;
        }
        if (hit.chance < 100) {
            m_chance = m_chance * 1664525u + 1013904223u;
            if (int((m_chance >> 8) % 100) >= hit.chance)
                continue;
        }
        // Swing pushes every second sixteenth late: at 50% not at all, at
        // 66% to the triplet, at 75% halfway to the next step.
        const Pad &settings = song.pads[pad];
        const int swing = settings.swing > 0 ? settings.swing : song.swing;
        const double time = tick + ((step & 1) ? (swing - 50) / 50.0 : 0.0) + hit.nudge / 100.0;
        double pitch = 0;
        const auto lanes = pattern.lanes.constFind(pad);
        if (lanes != pattern.lanes.constEnd()) {
            const auto lane = lanes->constFind(QStringLiteral("pitch"));
            if (lane != lanes->constEnd()) {
                const double value = laneValue(*lane, s);
                if (!std::isnan(value))
                    pitch = value;
            }
        }
        const int roll = qBound(1, hit.roll, kMaxRoll);
        for (int r = 0; r < roll; ++r)
            m_hits.push_back(Hit{time + double(r) / roll, pad, levelGain(hit.level), pitch});
    }
}

Processor *Renderer::laneProcessor(const CompiledLane &lane) const {
    if (lane.target == "synth")
        return m_rack.synths[lane.pad].get();
    for (const RackSlot &slot : m_rack.effects[lane.pad])
        if (slot.index == lane.effect)
            return slot.processor.get();
    return nullptr;
}

// Puts a plugin setting a lane moved back to the song's, or the plugin's
// own default when the song doesn't set it.
void Renderer::restore(const CompiledLane &lane) {
    Processor *processor = laneProcessor(lane);
    if (!processor)
        return;
    const Pad &pad = m_song.pads[lane.pad];
    const PluginSlot *slot = lane.target == "synth" ? &pad.synth
                             : lane.effect < pad.effects.size() ? &pad.effects[lane.effect]
                                                                : nullptr;
    if (slot && slot->params.contains(lane.param)) {
        processor->setParameter(lane.param, slot->params.value(lane.param));
        return;
    }
    for (const Parameter &p : processor->parameters())
        if (p.id == lane.param)
            processor->setParameter(p.id, p.defaultValue);
}

// A step is heard: the tracks take their settings, then the lanes move
// them, for this step and until the next.
void Renderer::applyLanes(const Tick &tick) {
    std::array<double, kPads> pitch;
    for (int p = 0; p < kPads; ++p)
        pitch[p] = m_state[p].pitch;
    baseStates();
    for (CompiledLane &lane : m_lanes) {
        if (lane.pattern != tick.pattern)
            continue;
        const double value = laneValue(*lane.points, padStep(lane.pad, tick.pattern, tick.sinceEntered));
        if (std::isnan(value))
            continue;
        if (lane.target == "pad") {
            PadState &state = m_state[lane.pad];
            switch (fieldOf(lane.param)) {
            case Pitch: state.pitch = value; break;
            case Volume: state.volume = value; break;
            case Pan: state.pan = value; break;
            case Filter: state.filter = value; break;
            case Resonance: state.resonance = value; break;
            case Decay: state.decay = value; updateDecay(lane.pad); break;
            case NotPad: break;
            }
        } else if (value != lane.lastSet) {
            if (Processor *processor = laneProcessor(lane)) {
                processor->setParameter(lane.param, value);
                lane.lastSet = value;
            }
        }
    }
    // A track with nothing sounding takes its new volume, place, and filter
    // at once, so a hit on this step is heard with them; one still sounding
    // glides to them.
    for (int p = 0; p < kPads; ++p) {
        if (m_voices[p][0].active || m_voices[p][1].active || isSynth(p))
            continue;
        m_volumeNow[p] = m_song.audible(p) ? m_state[p].volume : 0.0;
        m_panNow[p] = m_state[p].pan;
        m_filterNow[p] = m_state[p].filter;
    }
    // A sample still sounding bends with the pitch lane, so a long one can
    // be played like a tape.
    for (int p = 0; p < kPads; ++p) {
        if (m_state[p].pitch == pitch[p])
            continue;
        const double rate = std::pow(2.0, (m_song.pads[p].tune + m_state[p].pitch) / 12.0);
        for (Voice &voice : m_voices[p])
            if (voice.active)
                voice.rate = voice.rate < 0 ? -rate : rate;
    }
}

void Renderer::noteOff(int pad, int frame) {
    if (m_heldKey[pad] < 0)
        return;
    m_notes[pad] << NoteEvent{frame, m_heldKey[pad], 0.f, false};
    m_heldKey[pad] = -1;
}

void Renderer::trigger(int pad, float gain, double pitch, bool sequenced) {
    const Pad &settings = m_song.pads[pad];
    // A pad in a choke group silences the others in it.
    if (settings.group > 0)
        for (int other = 0; other < kPads; ++other)
            if (other != pad && m_song.pads[other].group == settings.group) {
                for (Voice &voice : m_voices[other])
                    cut(voice);
                noteOff(other, m_frame);
            }
    // A synth pad plays a note: middle C, tuned, held for the pad's length,
    // or, gated and played live, for as long as the pad is held.
    if (isSynth(pad)) {
        noteOff(pad, m_frame);
        const int key = qBound(0, 60 + settings.tune + int(std::lround(pitch)), 127);
        m_notes[pad] << NoteEvent{m_frame, key, gain, true};
        m_heldKey[pad] = key;
        m_releaseAt[pad] = !sequenced && settings.gate ? 1e18 : m_frame + settings.length * stepFrames();
        return;
    }
    const SampleBuffer &sound = m_sounds[pad];
    if (!sound || sound->size() < 4)
        return;
    auto &voices = m_voices[pad];
    // The voice still playing fades out under the new one, rather than
    // stopping on a click.
    if (voices[0].active) {
        voices[1] = voices[0];
        cut(voices[1]);
    }
    Voice voice;
    voice.sound = sound;
    const double rate = std::pow(2.0, (settings.tune + pitch) / 12.0);
    voice.rate = settings.reverse ? -rate : rate;
    voice.position = settings.reverse ? double(sound->size() / 2 - 2) : 0;
    voice.gain = gain;
    const double attack = 0.5 * settings.attack * settings.attack;
    if (attack > 0.0005) {
        voice.attack = 0;
        voice.attackStep = float(1.0 / (attack * kRate));
    }
    if (settings.gate && sequenced)
        voice.releaseIn = settings.length * stepFrames();
    voice.active = true;
    voices[0] = voice;
}

void Renderer::cut(Voice &voice, float seconds) {
    if (voice.active && voice.fadeStep == 0)
        voice.fadeStep = 1.f / (seconds * kRate);
}

void Renderer::mixVoice(Voice &voice, int pad, float *left, float *right) {
    if (!voice.active)
        return;
    const QVector<float> &data = *voice.sound;
    const double position = voice.position;
    const qsizetype index = qsizetype(std::floor(position));
    if (index < 0 || 2 * index + 3 >= data.size()) {
        voice.active = false;
        return;
    }
    const float frac = float(position - index);
    const float *frame = data.constData() + 2 * index;
    const float g = voice.gain * voice.fade * voice.attack * voice.level;
    *left += (frame[0] + (frame[2] - frame[0]) * frac) * g;
    *right += (frame[1] + (frame[3] - frame[1]) * frac) * g;
    voice.position += voice.rate;
    if (voice.attack < 1) {
        voice.attack = std::min(1.f, voice.attack + voice.attackStep);
    } else if (pad >= 0 && m_decayStep[pad] < 1) {
        voice.level *= m_decayStep[pad];
        if (voice.level < 1e-4f)
            voice.active = false;
    }
    if (voice.releaseIn >= 0) {
        voice.releaseIn -= 1;
        if (voice.releaseIn <= 0)
            cut(voice, 0.01f);
    }
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
    for (int done = 0; done < frames;) {
        int block = std::min(kMaxBlock, frames - done);
        // A block ends on a step, so what the lanes set for a plugin is heard
        // from that step on, not from wherever the block began.
        if (m_playing) {
            const double untilStep = (std::floor(m_clock + 1e-9) + 1 - m_clock) * stepFrames();
            block = std::min(block, std::max(1, int(std::ceil(untilStep - 1e-6))));
        }
        renderBlock(out + 2 * done, block,
                    onStep ? StepCallback([&, done](int c, int p, int s, int f) { onStep(c, p, s, f + done); })
                           : StepCallback(),
                    onRepeat ? RepeatCallback([&, done](int pad, int p, int s, int f) { onRepeat(pad, p, s, f + done); })
                             : RepeatCallback());
        done += block;
    }
    for (Suppressed &s : m_suppressed)
        s.framesLeft -= frames;
    m_suppressed.removeIf([](const Suppressed &s) { return s.framesLeft <= 0; });
}

void Renderer::renderBlock(float *out, int frames, const StepCallback &onStep, const RepeatCallback &onRepeat) {
    // Stopped, a setting is heard on the next hit.
    if (!m_playing)
        baseStates();
    const double stepsPerFrame = 1.0 / stepFrames();
    const double division = repeatSteps(m_repeat);
    bool anyHeld = false;
    for (int level : m_held)
        anyHeld = anyHeld || level > 0;
    const Transport transport{double(m_song.bpm), m_playing, m_clock};
    std::fill(m_padBuffers.begin(), m_padBuffers.end(), 0.f);
    std::fill(m_masterBuffer.begin(), m_masterBuffer.end(), 0.f);
    std::array<bool, kPads> sounded{};

    // The sequence, a frame at a time: steps and repeats strike pads, and
    // the sample pads' voices play into their own buffers.
    for (int i = 0; i < frames; ++i) {
        m_frame = i;
        if (m_playing) {
            while (m_nextTick - 0.5 <= m_clock && !(m_stopAt >= 0 && m_nextTick >= m_stopAt))
                schedule(m_nextTick++);
            int heard = 0;
            for (; heard < int(m_ticks.size()) && m_ticks[heard].time <= m_clock + 1e-9; ++heard) {
                m_heard = m_ticks[heard];
                applyLanes(m_heard);
                if (onStep)
                    onStep(m_heard.chainIndex, m_heard.pattern, m_heard.step, i);
            }
            if (heard > 0)
                m_ticks.erase(m_ticks.begin(), m_ticks.begin() + heard);
            for (size_t h = 0; h < m_hits.size();) {
                if (m_hits[h].time <= m_clock + 1e-9) {
                    const Hit hit = m_hits[h];
                    m_hits[h] = m_hits.back();
                    m_hits.pop_back();
                    trigger(hit.pad, hit.gain, hit.pitch, true);
                } else {
                    ++h;
                }
            }
        }
        if (anyHeld && division > 0 && repeatClock() >= m_nextRepeat) {
            const int length = m_song.patterns[m_heard.pattern].length;
            const int nearest = m_clock - m_heard.time > 0.5 ? (m_heard.step + 1) % length : m_heard.step;
            for (int pad = 0; pad < kPads; ++pad) {
                if (!m_held[pad])
                    continue;
                trigger(pad, levelGain(m_held[pad]), m_state[pad].pitch, false);
                if (m_playing && onRepeat)
                    onRepeat(pad, m_heard.pattern, nearest, i);
            }
            m_nextRepeat = (std::floor(repeatClock() / division) + 1) * division;
        }

        for (int p = 0; p < kPads; ++p) {
            auto &voices = m_voices[p];
            if (!voices[0].active && !voices[1].active)
                continue;
            sounded[p] = true;
            float left = 0, right = 0;
            for (Voice &voice : voices)
                mixVoice(voice, p, &left, &right);
            padChannel(p, 0)[i] = left;
            padChannel(p, 1)[i] = right;
        }
        float left = 0, right = 0;
        mixVoice(m_preview, -1, &left, &right);
        m_masterBuffer[i] = left;
        m_masterBuffer[kMaxBlock + i] = right;

        m_freeClock += stepsPerFrame;
        if (m_freeClock > 1e6) {
            m_freeClock -= 1e6;
            if (!m_playing)
                m_nextRepeat -= 1e6;
        }
        if (m_playing) {
            m_clock += stepsPerFrame;
            // The last step played: stop once its late hits are out too.
            if (m_stopAt >= 0 && m_clock >= m_stopAt && m_hits.empty())
                halt();
        }
    }
    m_frame = 0;

    // Then each pad on its own: its synth, its filter, its effects, and into
    // the mix at its volume and place. A muted pad still runs, so its echoes
    // are where they should be when it comes back.
    float *masterLeft = m_masterBuffer.data(), *masterRight = m_masterBuffer.data() + kMaxBlock;
    for (int p = 0; p < kPads; ++p) {
        const PadState &state = m_state[p];
        const double volume = m_song.audible(p) ? state.volume : 0.0;
        const bool busy = sounded[p] || isSynth(p) || !m_rack.effects[p].isEmpty() || !m_notes[p].isEmpty();
        if (!busy) {
            // Nothing to hear: the settings simply arrive.
            m_volumeNow[p] = volume;
            m_panNow[p] = state.pan;
            m_filterNow[p] = state.filter;
            m_filters[p] = {};
            continue;
        }
        float *left = padChannel(p, 0), *right = padChannel(p, 1);
        if (isSynth(p)) {
            if (m_heldKey[p] >= 0 && m_releaseAt[p] < frames)
                noteOff(p, int(m_releaseAt[p]));
            m_releaseAt[p] -= frames;
            auto &notes = m_notes[p];
            std::stable_sort(notes.begin(), notes.end(), [](const NoteEvent &a, const NoteEvent &b) { return a.frame < b.frame; });
            if (m_rack.synths[p])
                m_rack.synths[p]->process(left, right, frames, notes.constData(), int(notes.size()), transport);
            notes.clear();
        } else {
            m_notes[p].clear();
        }

        // The filter: below the middle a low-pass closing from 20 kHz to
        // 60 Hz, above it a high-pass opening from 20 Hz to 8 kHz. Its
        // coefficients move every 32 frames, gliding to the setting.
        if (std::abs(state.filter) > 1e-4 || std::abs(m_filterNow[p]) > 1e-4) {
            const double k = 1.0 / (0.707 + state.resonance * 7.0);
            double g = 0;
            for (int i = 0; i < frames; ++i) {
                if (i % 32 == 0) {
                    m_filterNow[p] += (state.filter - m_filterNow[p]) * 0.08;
                    if (std::abs(m_filterNow[p] - state.filter) < 1e-4)
                        m_filterNow[p] = state.filter;
                    const double f = m_filterNow[p];
                    g = Svf::coefficient(f < 0 ? 20000.0 * std::pow(60.0 / 20000.0, -f) : 20.0 * std::pow(400.0, f));
                }
                const bool high = m_filterNow[p] > 0;
                const Svf::Out l = m_filters[p][0].processWith(left[i], g, k);
                const Svf::Out r = m_filters[p][1].processWith(right[i], g, k);
                left[i] = float(high ? l.high : l.low);
                right[i] = float(high ? r.high : r.low);
            }
        } else {
            m_filters[p] = {};
        }

        for (const RackSlot &effect : std::as_const(m_rack.effects[p]))
            if (!effect.bypass)
                effect.processor->process(left, right, frames, nullptr, 0, transport);

        float peak = m_peaks[p];
        for (int i = 0; i < frames; ++i) {
            m_volumeNow[p] += (volume - m_volumeNow[p]) * kGlide;
            m_panNow[p] += (state.pan - m_panNow[p]) * kGlide;
            const float toLeft = float(m_volumeNow[p] * std::min(1.0, 1.0 - m_panNow[p]));
            const float toRight = float(m_volumeNow[p] * std::min(1.0, 1.0 + m_panNow[p]));
            const float l = left[i] * toLeft, r = right[i] * toRight;
            masterLeft[i] += l;
            masterRight[i] += r;
            peak = std::max({peak, std::abs(l), std::abs(r)});
        }
        m_peaks[p] = peak;
    }
    for (const RackSlot &effect : std::as_const(m_rack.master))
        if (!effect.bypass)
            effect.processor->process(masterLeft, masterRight, frames, nullptr, 0, transport);

    float peak = m_peaks[kPads];
    for (int i = 0; i < frames; ++i) {
        // A soft limit, so a busy bar thickens rather than clips, with room
        // left for the grit's drive.
        float left = std::tanh(masterLeft[i] * 0.7f);
        float right = std::tanh(masterRight[i] * 0.7f);
        master(&left, &right);
        out[2 * i] = left;
        out[2 * i + 1] = right;
        peak = std::max({peak, std::abs(left), std::abs(right)});
    }
    m_peaks[kPads] = peak;
}

QVector<float> renderSong(const Song &song, const std::array<SampleBuffer, kPads> &sounds, const Rack &rack, int pattern) {
    Renderer renderer;
    renderer.setSong(song);
    renderer.setSounds(sounds);
    renderer.setRack(rack);
    const int passes = song.chain.isEmpty() ? 1 : int(song.chain.size());
    renderer.play(!song.chain.isEmpty(), pattern, 0);
    renderer.stopAfterBars(passes);

    constexpr int block = 1024;
    QVector<float> out;
    float buffer[block * 2];
    while (renderer.playing()) {
        renderer.render(buffer, block);
        out.append(QList<float>(buffer, buffer + block * 2));
    }
    // Let the last sounds ring out, echoes and all, up to eight seconds.
    for (int tail = 0; tail < 8 * kRate; tail += block) {
        renderer.render(buffer, block);
        out.append(QList<float>(buffer, buffer + block * 2));
        float loudest = 0;
        for (float sample : buffer)
            loudest = std::max(loudest, std::abs(sample));
        if (!renderer.sounding() && loudest < 1e-4f)
            break;
    }
    // Then trim the silence, but never into the music.
    const qsizetype musical = qsizetype(std::ceil(songSteps(song, pattern) * renderer.stepFrames())) * 2;
    qsizetype end = out.size();
    while (end > musical && std::abs(out[end - 1]) < 1e-4f && std::abs(out[end - 2]) < 1e-4f)
        end -= 2;
    out.resize(std::max(end, std::min(musical, out.size())));
    return out;
}
