#pragma once

#include <QVector>

#include "sounds.h"

// Cutting a sample into chops, one a pad. A slicing is the list of the
// chops' start times in seconds; each runs to the next start, the last to
// the end.

// The sample's loudness in count buckets, 0 to 1, for drawing its waveform.
QVector<float> peaks(const SampleBuffer &sound, int count);

// count chops of the same length.
QVector<double> evenSlices(double seconds, int count);

// Chops where the sound hits, at most max of them, the strongest first and
// always one at the start; every chop is at least a tenth of a second.
QVector<double> transientSlices(const SampleBuffer &sound, int max);

double duration(const SampleBuffer &sound);
