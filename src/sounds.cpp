#include "sounds.h"

#include "dsp.h"
#include "song.h"

#include <QProcess>
#include <array>
#include <cmath>
#include <functional>

namespace {

// A fixed-seed noise source, so the built-in kit sounds the same every time
// and an export is the same file every time.
struct Noise {
    quint32 state = 0x2545F491u;
    float next() {
        state = state * 1664525u + 1013904223u;
        return float(state >> 8) / float(1 << 23) - 1.f;
    }
};

// Renders seconds of mono sound from a function of time, normalized to
// peak at 0.9 and copied to both channels.
SampleBuffer render(double seconds, const std::function<float(double)> &voice) {
    const int frames = int(seconds * kRate);
    QVector<float> mono(frames);
    float peak = 0.f;
    for (int i = 0; i < frames; ++i) {
        mono[i] = voice(double(i) / kRate);
        peak = std::max(peak, std::abs(mono[i]));
    }
    // A short fade at the end, so a sound never stops on a click.
    const int fade = std::min(frames, kRate / 200);
    for (int i = 0; i < fade; ++i)
        mono[frames - 1 - i] *= float(i) / fade;
    auto *stereo = new QVector<float>(frames * 2);
    const float scale = peak > 0 ? 0.9f / peak : 0.f;
    for (int i = 0; i < frames; ++i)
        (*stereo)[2 * i] = (*stereo)[2 * i + 1] = mono[i] * scale;
    return SampleBuffer(stereo);
}

constexpr double kTwoPi = 6.283185307179586;

double square(double phase) {
    return std::fmod(phase, 1.0) < 0.5 ? 1 : -1;
}

double saw(double phase) {
    return 2 * std::fmod(phase, 1.0) - 1;
}

// A deep kick that hangs, the boom of boom-bap.
SampleBuffer boom() {
    double phase = 0;
    Noise noise;
    Svf lp;
    return render(0.8, [=](double t) mutable {
        phase += kTwoPi * (42 + 95 * std::exp(-t / 0.05)) / kRate;
        const double body = std::sin(phase) * std::exp(-t / 0.35);
        const double click = lp.lowpass(noise.next(), 3000, 0.7) * std::exp(-t / 0.003) * 0.6;
        return float(std::tanh(2.2 * (body + click)));
    });
}

// A short, hard kick that knocks.
SampleBuffer knock() {
    double phase = 0;
    Noise noise;
    Svf bp;
    return render(0.35, [=](double t) mutable {
        phase += kTwoPi * (55 + 150 * std::exp(-t / 0.02)) / kRate;
        const double body = std::sin(phase) * std::exp(-t / 0.12);
        const double click = bp.bandpass(noise.next(), 3000, 1.0) * std::exp(-t / 0.004) * 0.8;
        return float(std::tanh(3.0 * (body + click)));
    });
}

// A snare that cracks: a body under a burst of bright noise, driven.
SampleBuffer crack() {
    Noise noise;
    Svf bp, hp;
    return render(0.4, [=](double t) mutable {
        const double tone = std::sin(kTwoPi * 200 * t) * std::exp(-t / 0.06) * 0.6
                            + std::sin(kTwoPi * 380 * t) * std::exp(-t / 0.03) * 0.3;
        const double n = noise.next();
        const double rattle = bp.bandpass(n, 2500, 0.8) * std::exp(-t / 0.12)
                              + hp.highpass(n, 6000, 0.7) * std::exp(-t / 0.04) * 0.5;
        return float(std::tanh(2.5 * (tone + rattle)));
    });
}

// A darker snare, as if sampled off a worn record.
SampleBuffer dusty() {
    Noise noise;
    Svf lp;
    return render(0.5, [=](double t) mutable {
        const double tone = std::sin(kTwoPi * 160 * t) * std::exp(-t / 0.08) * 0.5;
        const double rattle = lp.lowpass(noise.next(), 3500, 0.7) * std::exp(-t / 0.18);
        return float(std::tanh(1.8 * (tone + rattle)));
    });
}

SampleBuffer rim() {
    Noise noise;
    return render(0.08, [=](double t) mutable {
        return float(std::sin(kTwoPi * 1700 * t) * std::exp(-t / 0.008)
                     + std::sin(kTwoPi * 500 * t) * std::exp(-t / 0.02) * 0.5
                     + noise.next() * std::exp(-t / 0.001) * 0.4);
    });
}

SampleBuffer clap() {
    Noise noise;
    Svf bp;
    return render(0.4, [=](double t) mutable {
        // Three quick bursts and a tail: several hands, not quite together.
        double envelope = std::exp(-(t - 0.03) / 0.12) * (t >= 0.03);
        for (double start : {0.0, 0.011, 0.022})
            if (t >= start && t < 0.03)
                envelope = std::max(envelope, std::exp(-(t - start) / 0.004));
        return float(bp.bandpass(noise.next(), 1200, 1.4) * envelope);
    });
}

// Six detuned square waves, the 808's way to a metallic hat, high-passed,
// with more noise in it than an 808 has.
SampleBuffer hat(double decay, double seconds) {
    Noise noise;
    Svf hp1, hp2;
    return render(seconds, [=](double t) mutable {
        static const double freqs[] = {205.3, 304.4, 369.6, 522.7, 540.0, 800.0};
        double metal = 0;
        for (double f : freqs)
            metal += square(t * f * 2.6);
        const double raw = metal / 6 * 0.5 + noise.next() * 0.7;
        const double filtered = hp2.highpass(hp1.highpass(raw, 7000, 0.7), 7000, 0.7);
        return float(filtered * std::exp(-t / decay));
    });
}

SampleBuffer shaker() {
    Noise noise;
    Svf hp;
    return render(0.2, [=](double t) mutable {
        const double envelope = (1 - std::exp(-t / 0.008)) * std::exp(-t / 0.07);
        return float(hp.highpass(noise.next(), 5000, 0.7) * envelope);
    });
}

SampleBuffer tom() {
    double phase = 0;
    Noise noise;
    return render(0.6, [=](double t) mutable {
        phase += kTwoPi * (95 + 60 * std::exp(-t / 0.06)) / kRate;
        return float(std::tanh(1.5 * (std::sin(phase) * std::exp(-t / 0.25) + noise.next() * std::exp(-t / 0.01) * 0.3)));
    });
}

SampleBuffer crash() {
    Noise noise;
    Svf hp;
    return render(2.0, [=](double t) mutable {
        double metal = 0;
        for (double f : {311.0, 457.0, 691.0, 1033.0})
            metal += square(t * f * 3.1);
        const double raw = noise.next() * 0.8 + metal / 4 * 0.3;
        return float(hp.highpass(raw, 4000, 0.7) * std::exp(-t / 0.6) * (1 - std::exp(-t / 0.002)));
    });
}

// Steel drawn and struck, from a kung-fu film: inharmonic partials, each
// dying at its own pace, beating against a slightly detuned twin.
SampleBuffer clang() {
    Noise noise;
    Svf hp;
    return render(1.6, [=](double t) mutable {
        static const double partials[] = {523, 1187, 1873, 2741, 3511, 4637};
        double ring = 0;
        for (int k = 0; k < 6; ++k) {
            const double decay = 1.0 / (k + 1.2);
            ring += (std::sin(kTwoPi * partials[k] * t) + std::sin(kTwoPi * partials[k] * 1.003 * t)) * std::exp(-t / decay)
                    / (k + 1);
        }
        const double strike = hp.highpass(noise.next(), 2000, 0.7) * std::exp(-t / 0.01);
        return float(ring * 0.5 + strike);
    });
}

// One note of a piano-ish tone: a few harmonics, a hammer, a long fade.
double pianoNote(double t, double f) {
    double v = 0;
    for (int h = 1; h <= 5; ++h)
        v += std::sin(kTwoPi * f * h * t * (1 + 0.0004 * h)) / (h * h * 0.8 + 0.2);
    return v * std::exp(-t / 0.9) * (1 - std::exp(-t / 0.003));
}

double midiHz(double note) {
    return 440.0 * std::pow(2.0, (note - 69) / 12);
}

// An eerie minor ninth on a detuned piano, dulled as if lifted off a record.
SampleBuffer stab() {
    Svf lp;
    return render(2.0, [=](double t) mutable {
        double chord = 0;
        for (double note : {48.0, 51.0, 55.0, 58.0, 62.0}) // C3 Eb3 G3 Bb3 D4
            chord += pianoNote(t, midiHz(note)) + pianoNote(t, midiHz(note + 0.12)) * 0.5;
        return float(lp.lowpass(chord, 2600, 0.7));
    });
}

// A bell struck in an empty temple: FM at an inharmonic ratio.
SampleBuffer bell() {
    return render(2.5, [](double t) {
        const double f = midiHz(63); // Eb4
        const double index = 3 * std::exp(-t / 0.5);
        return float(std::sin(kTwoPi * f * t + index * std::sin(kTwoPi * f * 1.4 * t)) * std::exp(-t / 1.2)
                     * (1 - std::exp(-t / 0.001)));
    });
}

// A sub bass note on C2; tune the pad to play the line.
SampleBuffer bass() {
    return render(1.6, [](double t) {
        const double f = midiHz(36);
        const double tone = std::sin(kTwoPi * f * t) + 0.25 * std::sin(kTwoPi * 2 * f * t);
        return float(std::tanh(1.5 * tone) * std::exp(-t / 0.8) * (1 - std::exp(-t / 0.005)));
    });
}

// A minor brass stab: detuned saws through a filter that opens and shuts.
SampleBuffer horn() {
    Svf lp;
    return render(0.8, [=](double t) mutable {
        double brass = 0;
        for (double note : {51.0, 55.0, 58.0}) // Eb3 G3 Bb3
            for (double detune : {-0.08, 0.0, 0.07})
                brass += saw(t * midiHz(note + detune));
        const double cutoff = 600 + 2400 * std::exp(-t / 0.15) * (1 - std::exp(-t / 0.02));
        const double envelope = (1 - std::exp(-t / 0.02)) * std::exp(-t / 0.35);
        return float(std::tanh(1.2 * lp.lowpass(brass / 9, cutoff, 1.1)) * envelope);
    });
}

} // namespace

SampleBuffer kitSound(int pad) {
    // Made once, the first time any is asked for; static initialization is
    // thread-safe, so a command and the audio thread may both ask.
    static const std::array<SampleBuffer, kBankPads> sounds{
        boom(), knock(), crack(), dusty(), rim(),  clap(), hat(0.03, 0.15), hat(0.25, 0.8),
        shaker(), tom(), crash(), clang(), stab(), bell(), bass(), horn()};
    return pad >= 0 && pad < kBankPads ? sounds[pad] : SampleBuffer();
}

SampleBuffer decodeSample(const QString &path, QString *error) {
    QProcess ffmpeg;
    ffmpeg.start("ffmpeg", {"-v", "error", "-nostdin", "-i", path, "-t", QString::number(kMaxSampleSeconds),
                            "-f", "f32le", "-ac", "2", "-ar", QString::number(kRate), "-"});
    if (!ffmpeg.waitForStarted()) {
        *error = kNoFfmpeg;
        return {};
    }
    ffmpeg.closeWriteChannel();
    ffmpeg.waitForFinished(-1);
    const QByteArray data = ffmpeg.readAllStandardOutput();
    if (ffmpeg.exitStatus() != QProcess::NormalExit || ffmpeg.exitCode() != 0) {
        const QString message = QString::fromUtf8(ffmpeg.readAllStandardError()).trimmed();
        // ffmpeg starts each line with the file name; the reason is after it.
        const QString last = message.section('\n', -1);
        *error = last.isEmpty() ? QString("ffmpeg failed") : last.section(": ", -1);
        return {};
    }
    const qsizetype count = data.size() / qsizetype(sizeof(float)) / 2 * 2;
    if (count == 0) {
        *error = "There is no sound in it.";
        return {};
    }
    auto *samples = new QVector<float>(count);
    memcpy(samples->data(), data.constData(), count * sizeof(float));
    return SampleBuffer(samples);
}

SampleBuffer slice(const SampleBuffer &sound, double start, double end) {
    if (!sound || (start <= 0 && end <= 0))
        return sound;
    const qsizetype frames = sound->size() / 2;
    const qsizetype from = qBound<qsizetype>(0, qsizetype(start * kRate), frames);
    const qsizetype to = end > 0 ? qBound<qsizetype>(from, qsizetype(end * kRate), frames) : frames;
    if (to - from < 2)
        return {};
    auto *part = new QVector<float>(sound->mid(from * 2, (to - from) * 2));
    // A few milliseconds of fade at each cut, so a chop never clicks.
    const qsizetype fade = std::min<qsizetype>((to - from) / 2, kRate / 400);
    for (qsizetype i = 0; i < fade; ++i) {
        const float g = float(i) / fade;
        for (int c = 0; c < 2; ++c) {
            (*part)[2 * i + c] *= g;
            (*part)[part->size() - 2 - 2 * i + c] *= g;
        }
    }
    return SampleBuffer(part);
}
