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

SampleBuffer kick() {
    double phase = 0;
    Noise noise;
    return render(0.5, [=](double t) mutable {
        // A sine dropping from about 155 Hz to 45 Hz, with a click on top.
        phase += kTwoPi * (45 + 110 * std::exp(-t / 0.035)) / kRate;
        const double body = std::sin(phase) * std::exp(-t / 0.16);
        const double click = noise.next() * std::exp(-t / 0.002) * 0.3;
        return float(std::tanh(1.6 * (body + click)));
    });
}

SampleBuffer snare() {
    Noise noise;
    Svf hp;
    return render(0.35, [=](double t) mutable {
        const double tone = std::sin(kTwoPi * 185 * t) * std::exp(-t / 0.05) * 0.5
                            + std::sin(kTwoPi * 330 * t) * std::exp(-t / 0.03) * 0.3;
        const double rattle = hp.highpass(noise.next(), 1500, 0.7) * std::exp(-t / 0.09) * 0.8;
        return float(tone + rattle);
    });
}

// Six detuned square waves, the 808's way to a metallic hat, high-passed.
SampleBuffer hat(double decay, double seconds) {
    Noise noise;
    Svf hp1, hp2;
    return render(seconds, [=](double t) mutable {
        static const double freqs[] = {205.3, 304.4, 369.6, 522.7, 540.0, 800.0};
        double metal = 0;
        for (double f : freqs)
            metal += std::fmod(t * f * 2.6, 1.0) < 0.5 ? 1 : -1;
        const double raw = metal / 6 * 0.6 + noise.next() * 0.5;
        const double filtered = hp2.highpass(hp1.highpass(raw, 7000, 0.7), 7000, 0.7);
        return float(filtered * std::exp(-t / decay));
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

SampleBuffer rim() {
    Noise noise;
    return render(0.08, [=](double t) mutable {
        return float(std::sin(kTwoPi * 1700 * t) * std::exp(-t / 0.008)
                     + std::sin(kTwoPi * 500 * t) * std::exp(-t / 0.02) * 0.5
                     + noise.next() * std::exp(-t / 0.001) * 0.4);
    });
}

SampleBuffer cowbell() {
    Svf bp;
    return render(0.6, [=](double t) mutable {
        const double squares = (std::fmod(t * 540, 1.0) < 0.5 ? 1 : -1) + (std::fmod(t * 800, 1.0) < 0.5 ? 1 : -1);
        const double envelope = std::exp(-t / 0.05) * 0.7 + std::exp(-t / 0.3) * 0.3;
        return float(bp.bandpass(squares, 900, 2.5) * envelope);
    });
}

} // namespace

SampleBuffer builtInSound(int track) {
    // Made once, the first time any is asked for; static initialization is
    // thread-safe, so a command and the audio thread may both ask.
    static const std::array<SampleBuffer, kTracks> sounds{
        kick(), snare(), hat(0.025, 0.12), hat(0.18, 0.6), clap(), rim(), cowbell(), SampleBuffer()};
    return track >= 0 && track < kTracks ? sounds[track] : SampleBuffer();
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
