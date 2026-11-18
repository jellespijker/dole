#include "chop.h"

#include <algorithm>
#include <cmath>

double duration(const SampleBuffer &sound) {
    return sound ? sound->size() / 2.0 / kRate : 0;
}

QVector<float> peaks(const SampleBuffer &sound, int count) {
    QVector<float> result(count, 0.f);
    if (!sound || count <= 0)
        return result;
    const qsizetype frames = sound->size() / 2;
    float loudest = 0;
    for (int b = 0; b < count; ++b) {
        const qsizetype from = frames * b / count, to = std::max(from + 1, frames * (b + 1) / count);
        float peak = 0;
        for (qsizetype i = from; i < to && i < frames; ++i)
            peak = std::max({peak, std::abs((*sound)[2 * i]), std::abs((*sound)[2 * i + 1])});
        result[b] = peak;
        loudest = std::max(loudest, peak);
    }
    if (loudest > 0)
        for (float &p : result)
            p /= loudest;
    return result;
}

QVector<double> evenSlices(double seconds, int count) {
    QVector<double> starts;
    for (int i = 0; i < count; ++i)
        starts << seconds * i / count;
    return starts;
}

QVector<double> transientSlices(const SampleBuffer &sound, int max) {
    if (!sound || max <= 0)
        return {};
    // The energy of each 5 ms window; a hit is a window much louder than the
    // ones just before it.
    constexpr int window = kRate / 200;
    const qsizetype frames = sound->size() / 2;
    QVector<double> energy;
    for (qsizetype from = 0; from + window <= frames; from += window) {
        double sum = 0;
        for (qsizetype i = from; i < from + window; ++i) {
            const double v = ((*sound)[2 * i] + (*sound)[2 * i + 1]) / 2;
            sum += v * v;
        }
        energy << std::sqrt(sum / window);
    }
    struct Onset {
        double time, strength;
    };
    QVector<Onset> onsets;
    for (int w = 4; w < energy.size(); ++w) {
        const double before = (energy[w - 1] + energy[w - 2] + energy[w - 3] + energy[w - 4]) / 4;
        const double rise = energy[w] - before;
        if (rise > 0.02 && energy[w] > 1.8 * before && energy[w] >= energy[w - 1])
            onsets.append({double(w) * window / kRate, rise});
    }
    // Strongest first, none closer than a tenth of a second to another.
    std::sort(onsets.begin(), onsets.end(), [](const Onset &a, const Onset &b) { return a.strength > b.strength; });
    QVector<double> starts{0.0};
    for (const Onset &onset : onsets) {
        if (starts.size() >= max)
            break;
        const bool apart = std::all_of(starts.begin(), starts.end(),
                                       [&](double s) { return std::abs(s - onset.time) >= 0.1; });
        if (apart)
            starts << onset.time;
    }
    std::sort(starts.begin(), starts.end());
    return starts;
}
