#include "wav.h"

#include "sounds.h"

#include <QSaveFile>
#include <QtEndian>
#include <cmath>

namespace {

void put32(QByteArray &data, quint32 value) {
    char bytes[4];
    qToLittleEndian(value, bytes);
    data.append(bytes, 4);
}

void put16(QByteArray &data, quint16 value) {
    char bytes[2];
    qToLittleEndian(value, bytes);
    data.append(bytes, 2);
}

} // namespace

bool writeWav(const QString &path, const QVector<float> &samples, QString *error) {
    constexpr int channels = 2, bits = 16;
    const quint32 bytes = quint32(samples.size()) * (bits / 8);

    QByteArray data;
    data.reserve(44 + bytes);
    data.append("RIFF");
    put32(data, 36 + bytes);
    data.append("WAVEfmt ");
    put32(data, 16);
    put16(data, 1); // PCM
    put16(data, channels);
    put32(data, kRate);
    put32(data, kRate * channels * bits / 8);
    put16(data, channels * bits / 8);
    put16(data, bits);
    data.append("data");
    put32(data, bytes);
    for (float sample : samples)
        put16(data, quint16(qint16(std::lround(qBound(-1.f, sample, 1.f) * 32767))));

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}
