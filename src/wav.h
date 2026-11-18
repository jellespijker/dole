#pragma once

#include <QString>
#include <QVector>

// Writes interleaved stereo float at kRate as a 16-bit WAV file, atomically.
bool writeWav(const QString &path, const QVector<float> &samples, QString *error);
