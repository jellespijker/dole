#include "builtins.h"

#include <QHash>
#include <QObject>
#include <cmath>

#include "dsp.h"
#include "sounds.h"

namespace {

constexpr double kTwoPi = 6.283185307179586;

Parameter amount(const char *id, const char *name, double value) {
    return {id, name, 0, 1, value, value, false, false};
}

// Names are looked up as they're shown, so a language switch reaches them.
QString label(const QString &english) {
    static const QHash<QString, std::function<QString()>> labels{
        {"Overdrive", [] { return QObject::tr("Overdrive"); }},   {"Bitcrush", [] { return QObject::tr("Bitcrush"); }},
        {"Ring mod", [] { return QObject::tr("Ring mod"); }},     {"Metal comb", [] { return QObject::tr("Metal comb"); }},
        {"Dirty delay", [] { return QObject::tr("Dirty delay"); }}, {"Stutter", [] { return QObject::tr("Stutter"); }},
        {"Drive", [] { return QObject::tr("Drive"); }},           {"Fuzz", [] { return QObject::tr("Fuzz"); }},
        {"Tone", [] { return QObject::tr("Tone"); }},             {"Mix", [] { return QObject::tr("Mix"); }},
        {"Bits", [] { return QObject::tr("Bits"); }},             {"Rate", [] { return QObject::tr("Rate"); }},
        {"Frequency", [] { return QObject::tr("Frequency"); }},   {"Shape", [] { return QObject::tr("Shape"); }},
        {"Feedback", [] { return QObject::tr("Feedback"); }},     {"Damping", [] { return QObject::tr("Damping"); }},
        {"Time", [] { return QObject::tr("Time"); }},             {"Length", [] { return QObject::tr("Length"); }},
        {"Depth", [] { return QObject::tr("Depth"); }},
    };
    const auto it = labels.constFind(english);
    return it != labels.constEnd() ? (*it)() : english;
}

// A built-in effect: its parameters, and their values by position.
class Builtin : public Processor {
public:
    Builtin(QString name, QVector<Parameter> parameters) : m_name(std::move(name)), m_parameters(std::move(parameters)) {
        for (Parameter &p : m_parameters)
            p.value = p.defaultValue;
    }

    QString name() const override { return label(m_name); }
    bool instrument() const override { return false; }
    QVector<Parameter> parameters() const override {
        QVector<Parameter> parameters = m_parameters;
        for (Parameter &p : parameters)
            p.name = label(p.name);
        return parameters;
    }

    void setParameter(const QString &id, double value) override {
        for (Parameter &p : m_parameters)
            if (p.id == id)
                p.value = qBound(p.min, p.stepped ? std::round(value) : value, p.max);
    }

    QString valueText(const QString &id, double value) const override {
        const QString unit = unitOf(id);
        if (unit == "Hz")
            return value >= 1000 ? QString("%1 kHz").arg(value / 1000, 0, 'f', 1) : QString("%1 Hz").arg(value, 0, 'f', 0);
        if (unit == "s")
            return value >= 1 ? QString("%1 s").arg(value, 0, 'f', 2) : QString("%1 ms").arg(value * 1000, 0, 'f', 0);
        if (unit == "%")
            return QString("%1%").arg(value * 100, 0, 'f', 0);
        return QString::number(value, 'f', 0);
    }

protected:
    double value(int index) const { return m_parameters[index].value; }
    virtual QString unitOf(const QString &) const { return "%"; }

private:
    QString m_name;
    QVector<Parameter> m_parameters;
};

// Gain into a clipper. Fuzz moves it from a smooth tube-ish curve to a hard,
// lopsided clip that rectifies, the sound of an amp being abused.
class Overdrive : public Builtin {
public:
    Overdrive()
        : Builtin("Overdrive",
                  {amount("drive", "Drive", 0.5), amount("fuzz", "Fuzz", 0.0),
                   {"tone", "Tone", 300, 16000, 6000, 6000, false, true},
                   amount("mix", "Mix", 1.0)}) {}

    void process(float *left, float *right, int frames, const NoteEvent *, int, const Transport &) override {
        const double gain = 1 + std::pow(value(0), 2) * 80, fuzz = value(1), tone = value(2), mix = value(3);
        float *channels[2] = {left, right};
        for (int c = 0; c < 2; ++c) {
            for (int i = 0; i < frames; ++i) {
                const double x = channels[c][i];
                const double driven = x * gain;
                const double smooth = std::tanh(driven);
                const double hard = qBound(-1.0, driven + 0.3 * fuzz, 1.0);
                const double clipped = (1 - fuzz) * smooth + fuzz * (hard > 0 ? hard : hard * 0.35);
                // The lopsided clip leaves an offset, which a gentle high-pass removes.
                const double centred = m_dc[c].highpass(clipped, 20, 0.7);
                const double wet = m_tone[c].lowpass(centred, tone, 0.7) * 0.7;
                channels[c][i] = float(mix * wet + (1 - mix) * x);
            }
        }
    }

protected:
    QString unitOf(const QString &id) const override { return id == "tone" ? "Hz" : "%"; }

private:
    Svf m_dc[2], m_tone[2];
};

// Fewer bits and a lower sample rate, down to the ugly end.
class Bitcrush : public Builtin {
public:
    Bitcrush()
        : Builtin("Bitcrush",
                  {{"bits", "Bits", 1, 16, 6, 6, true, false},
                   {"rate", "Rate", 300, 48000, 8000, 8000, false, true},
                   amount("mix", "Mix", 1.0)}) {}

    void process(float *left, float *right, int frames, const NoteEvent *, int, const Transport &) override {
        const double steps = std::pow(2.0, value(0) - 1), step = value(1) / kRate, mix = value(2);
        for (int i = 0; i < frames; ++i) {
            m_phase += step;
            if (m_phase >= 1) {
                m_phase -= std::floor(m_phase);
                m_held[0] = float(std::round(left[i] * steps) / steps);
                m_held[1] = float(std::round(right[i] * steps) / steps);
            }
            left[i] = float(mix * m_held[0] + (1 - mix) * left[i]);
            right[i] = float(mix * m_held[1] + (1 - mix) * right[i]);
        }
    }

protected:
    QString unitOf(const QString &id) const override { return id == "rate" ? "Hz" : id == "bits" ? "" : "%"; }

private:
    double m_phase = 1;
    float m_held[2] = {0, 0};
};

// The sound times a tone: metallic sidebands, a bell from a drum, a robot
// from a voice. Shape turns the sine into a square.
class Ringmod : public Builtin {
public:
    Ringmod()
        : Builtin("Ring mod",
                  {{"freq", "Frequency", 20, 4000, 220, 220, false, true},
                   amount("shape", "Shape", 0.0), amount("mix", "Mix", 1.0)}) {}

    void process(float *left, float *right, int frames, const NoteEvent *, int, const Transport &) override {
        const double step = value(0) / kRate, shape = value(1), mix = value(2);
        for (int i = 0; i < frames; ++i) {
            const double sine = std::sin(kTwoPi * m_phase);
            const double carrier = (1 - shape) * sine + shape * (sine >= 0 ? 1 : -1);
            m_phase = std::fmod(m_phase + step, 1.0);
            left[i] = float(mix * left[i] * carrier + (1 - mix) * left[i]);
            right[i] = float(mix * right[i] * carrier + (1 - mix) * right[i]);
        }
    }

protected:
    QString unitOf(const QString &id) const override { return id == "freq" ? "Hz" : "%"; }

private:
    double m_phase = 0;
};

// A short delay fed back into itself: anything through it rings at the
// frequency, like a struck pipe. The loop saturates, so even full feedback
// screams rather than explodes.
class Comb : public Builtin {
public:
    Comb()
        : Builtin("Metal comb",
                  {{"freq", "Frequency", 30, 3000, 110, 110, false, true},
                   amount("feedback", "Feedback", 0.85), amount("damp", "Damping", 0.3),
                   amount("mix", "Mix", 0.6)}),
          m_line{QVector<float>(kRate / 20, 0.f), QVector<float>(kRate / 20, 0.f)} {}

    void process(float *left, float *right, int frames, const NoteEvent *, int, const Transport &) override {
        const double delay = std::min(double(kRate) / value(0), double(m_line[0].size() - 2));
        const double feedback = value(1) * 0.995, damp = 500 + (1 - value(2)) * 15000, mix = value(3);
        float *channels[2] = {left, right};
        const int size = int(m_line[0].size());
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < 2; ++c) {
                double read = m_write - delay;
                if (read < 0)
                    read += size;
                const int a = int(read), b = (a + 1) % size;
                const double frac = read - a;
                const double delayed = m_line[c][a] + (m_line[c][b] - m_line[c][a]) * frac;
                const double x = channels[c][i];
                const double y = std::tanh(x + feedback * m_damp[c].lowpass(delayed, damp, 0.6));
                m_line[c][m_write] = float(y);
                channels[c][i] = float(mix * y + (1 - mix) * x);
            }
            m_write = (m_write + 1) % size;
        }
    }

protected:
    QString unitOf(const QString &id) const override { return id == "freq" ? "Hz" : "%"; }

private:
    QVector<float> m_line[2];
    Svf m_damp[2];
    int m_write = 0;
};

// Echoes, darker and dirtier each time round. Feedback past 100% runs away
// into a wall of noise, held in by the saturation.
class Delay : public Builtin {
public:
    Delay()
        : Builtin("Dirty delay",
                  {{"time", "Time", 0.01, 1.5, 0.25, 0.25, false, true},
                   {"feedback", "Feedback", 0, 1.2, 0.5, 0.5, false, false},
                   amount("drive", "Drive", 0.3),
                   {"tone", "Tone", 300, 16000, 4000, 4000, false, true},
                   amount("mix", "Mix", 0.4)}),
          m_line{QVector<float>(int(1.6 * kRate), 0.f), QVector<float>(int(1.6 * kRate), 0.f)} {}

    void process(float *left, float *right, int frames, const NoteEvent *, int, const Transport &) override {
        const int size = int(m_line[0].size());
        const int delay = qBound(1, int(value(0) * kRate), size - 1);
        const double feedback = value(1), drive = 1 + value(2) * 6, tone = value(3), mix = value(4);
        float *channels[2] = {left, right};
        for (int i = 0; i < frames; ++i) {
            const int read = (m_write - delay + size) % size;
            for (int c = 0; c < 2; ++c) {
                const double x = channels[c][i];
                const double echo = m_line[c][read];
                const double back = std::tanh(drive * m_tone[c].lowpass(x + feedback * echo, tone, 0.7)) / drive;
                m_line[c][m_write] = float(back);
                channels[c][i] = float(x + mix * echo);
            }
            m_write = (m_write + 1) % size;
        }
    }

protected:
    QString unitOf(const QString &id) const override {
        return id == "time" ? "s" : id == "tone" ? "Hz" : "%";
    }

private:
    QVector<float> m_line[2];
    Svf m_tone[2];
    int m_write = 0;
};

// Chops the sound on and off in time with the song.
class Stutter : public Builtin {
public:
    Stutter()
        : Builtin("Stutter",
                  {{"rate", "Rate", 0, 3, 2, 2, true, false},
                   amount("duty", "Length", 0.5), amount("depth", "Depth", 1.0)}) {}

    QString valueText(const QString &id, double value) const override {
        static const char *const rates[] = {"1/4", "1/8", "1/16", "1/32"};
        return id == "rate" ? QString(rates[qBound(0, int(value), 3)]) : Builtin::valueText(id, value);
    }

    void process(float *left, float *right, int frames, const NoteEvent *, int, const Transport &transport) override {
        // A quarter is four steps; each rate halves it.
        const double length = 4.0 / std::pow(2.0, value(0));
        const double duty = std::max(0.02, value(1)), depth = value(2);
        const double stepsPerFrame = transport.bpm * 4 / 60.0 / kRate;
        // Smoothed over a millisecond, so the cuts are hard but don't crackle.
        const double smoothing = 1 - std::exp(-1.0 / (0.001 * kRate));
        for (int i = 0; i < frames; ++i) {
            const double position = transport.playing ? transport.position + i * stepsPerFrame : m_free;
            const bool open = std::fmod(position / length, 1.0) < duty;
            m_gain += ((open ? 1.0 : 1 - depth) - m_gain) * smoothing;
            left[i] = float(left[i] * m_gain);
            right[i] = float(right[i] * m_gain);
            m_free += stepsPerFrame;
        }
    }

private:
    double m_gain = 1;
    double m_free = 0;
};

} // namespace

std::unique_ptr<Processor> createBuiltin(const QString &id) {
    if (id == "overdrive")
        return std::make_unique<Overdrive>();
    if (id == "bitcrush")
        return std::make_unique<Bitcrush>();
    if (id == "ringmod")
        return std::make_unique<Ringmod>();
    if (id == "comb")
        return std::make_unique<Comb>();
    if (id == "delay")
        return std::make_unique<Delay>();
    if (id == "stutter")
        return std::make_unique<Stutter>();
    return {};
}

QVector<PluginInfo> builtinInfos() {
    QVector<PluginInfo> infos;
    for (const char *id : {"overdrive", "bitcrush", "ringmod", "comb", "delay", "stutter"})
        infos << PluginInfo{id, createBuiltin(id)->name(), "dole", "dole", false};
    return infos;
}
