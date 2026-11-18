#pragma once

#include <algorithm>
#include <cmath>

#include "sounds.h"

// A state-variable filter in the topology-preserving form (Zavalishin), which
// stays stable while its cutoff moves every sample, as the bass sweeps it.
struct Svf {
    double ic1 = 0, ic2 = 0;

    struct Out {
        double low, band, high;
    };

    Out process(double input, double cutoff, double q) {
        const double g = std::tan(3.141592653589793 * std::min(cutoff, kRate * 0.45) / kRate);
        const double k = 1.0 / q;
        const double a1 = 1.0 / (1.0 + g * (g + k));
        const double a2 = g * a1;
        const double a3 = g * a2;
        const double v3 = input - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1;
        ic2 = 2 * v2 - ic2;
        return {v2, v1, input - k * v1 - v2};
    }
    double lowpass(double input, double cutoff, double q) { return process(input, cutoff, q).low; }
    double bandpass(double input, double cutoff, double q) { return process(input, cutoff, q).band; }
    double highpass(double input, double cutoff, double q) { return process(input, cutoff, q).high; }
};
