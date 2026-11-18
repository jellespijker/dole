#pragma once

#include <QSharedPointer>
#include <QString>
#include <QVector>

// Every sound the engine plays is interleaved stereo float at kRate, shared
// and never changed once made, so the audio thread can hold one while the
// interface replaces it.
constexpr int kRate = 48000;
using SampleBuffer = QSharedPointer<const QVector<float>>;

// The kit's sound for each pad, synthesized once.
SampleBuffer kitSound(int pad);

// The part of a sound from start to end, in seconds; an end of 0 is the end.
SampleBuffer slice(const SampleBuffer &sound, double start, double end);

// The longest sample loaded, so a whole album dropped on a track by mistake
// doesn't fill memory.
constexpr int kMaxSampleSeconds = 120;

// Decodes any audio file ffmpeg reads, blocking, so it runs on a worker
// thread or in a command. On failure it says why in error; kNoFfmpeg means
// ffmpeg itself is missing.
SampleBuffer decodeSample(const QString &path, QString *error);
inline const char *const kNoFfmpeg = "ffmpeg-missing";
