#include "engine.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QTimer>
#include <cmath>

namespace {

// The device Qt's audio sink pulls sound from, a buffer at a time, on the
// audio thread.
class Generator : public QIODevice {
public:
    Generator(std::shared_ptr<AudioEngine::Shared> shared, const QAudioFormat &format, Engine *engine,
              QAudioSink *sink, QObject *parent)
        : QIODevice(parent), m_shared(std::move(shared)), m_format(format), m_engine(engine), m_sink(sink) {}

    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return m_format.bytesForFrames(4096) + QIODevice::bytesAvailable(); }

protected:
    qint64 writeData(const char *, qint64) override { return -1; }

    qint64 readData(char *data, qint64 maxSize) override {
        const int frames = int(std::min<qint64>(maxSize / m_format.bytesPerFrame(), 4096));
        if (frames <= 0)
            return 0;
        m_buffer.resize(frames * 2);

        struct Event {
            int a, b, c, frame;
            bool repeat;
        };
        QVarLengthArray<Event, 8> events;
        {
            QMutexLocker lock(&m_shared->mutex);
            m_shared->renderer.render(
                m_buffer.data(), frames,
                [&](int chainIndex, int pattern, int step, int frame) {
                    events.append({chainIndex, pattern, step, frame, false});
                },
                [&](int pad, int pattern, int step, int frame) { events.append({pad, pattern, step, frame, true}); });
        }

        if (m_format.sampleFormat() == QAudioFormat::Float) {
            memcpy(data, m_buffer.constData(), frames * 2 * sizeof(float));
        } else {
            auto *out = reinterpret_cast<qint16 *>(data);
            for (int i = 0; i < frames * 2; ++i)
                out[i] = qint16(std::lround(qBound(-1.f, m_buffer[i], 1.f) * 32767));
        }

        // A step is heard once what is already queued has played, so the
        // playhead moves then, not as the step is rendered.
        const int queued = m_format.framesForBytes(m_sink->bufferSize() - m_sink->bytesFree());
        for (const Event &event : events) {
            const int delay = int((queued + event.frame) * 1000LL / kRate);
            QTimer::singleShot(delay, Qt::PreciseTimer, m_engine, [engine = m_engine, event] {
                if (event.repeat)
                    emit engine->repeated(event.a, event.b, event.c);
                else
                    emit engine->positionChanged(event.a, event.b, event.c);
            });
        }
        return qint64(frames) * m_format.bytesPerFrame();
    }

private:
    std::shared_ptr<AudioEngine::Shared> m_shared;
    QAudioFormat m_format;
    Engine *m_engine;
    QAudioSink *m_sink;
    QVector<float> m_buffer;
};

} // namespace

AudioEngine::AudioEngine(QObject *parent) : Engine(parent) {
    m_output = new QObject;
    m_output->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_output, &QObject::deleteLater);
    m_thread.setObjectName("dole audio");
    m_thread.start(QThread::TimeCriticalPriority);
}

AudioEngine::~AudioEngine() {
    m_thread.quit();
    m_thread.wait();
}

void AudioEngine::open() {
    if (m_opened)
        return;
    m_opened = true;
    QMetaObject::invokeMethod(m_output, [this, shared = m_shared, output = m_output] {
        const QAudioDevice device = QMediaDevices::defaultAudioOutput();
        if (device.isNull()) {
            emit failed("noDevice", {});
            return;
        }
        QAudioFormat format;
        format.setSampleRate(kRate);
        format.setChannelCount(2);
        format.setSampleFormat(QAudioFormat::Float);
        if (!device.isFormatSupported(format))
            format.setSampleFormat(QAudioFormat::Int16);

        auto *sink = new QAudioSink(device, format, output);
        // About 40 ms ahead: short enough that an edit is heard at once.
        sink->setBufferSize(format.bytesForFrames(2048));
        auto *generator = new Generator(shared, format, this, sink, output);
        generator->open(QIODevice::ReadOnly);
        connect(sink, &QAudioSink::stateChanged, output, [this, sink] {
            const QtAudio::Error error = sink->error();
            if (error == QtAudio::OpenError || error == QtAudio::IOError || error == QtAudio::FatalError)
                emit failed("deviceFailed", error == QtAudio::OpenError ? "open" : "io");
        });
        sink->start(generator);
    });
}

void AudioEngine::setSong(const Song &song) {
    QMutexLocker lock(&m_shared->mutex);
    m_shared->renderer.setSong(song);
}

void AudioEngine::setSounds(const std::array<SampleBuffer, kPads> &sounds) {
    QMutexLocker lock(&m_shared->mutex);
    m_shared->renderer.setSounds(sounds);
}

void AudioEngine::play(bool songMode, int pattern, int chainIndex) {
    open();
    QMutexLocker lock(&m_shared->mutex);
    m_shared->renderer.play(songMode, pattern, chainIndex);
}

void AudioEngine::cue(int pattern) {
    QMutexLocker lock(&m_shared->mutex);
    m_shared->renderer.cue(pattern);
}

void AudioEngine::stop() {
    QMutexLocker lock(&m_shared->mutex);
    m_shared->renderer.stop();
}

void AudioEngine::hit(int pad, int level) {
    open();
    QMutexLocker lock(&m_shared->mutex);
    m_shared->renderer.hit(pad, level);
}

void AudioEngine::hold(int pad, bool held, int level) {
    QMutexLocker lock(&m_shared->mutex);
    m_shared->renderer.hold(pad, held, level);
}

void AudioEngine::setRepeat(int division) {
    QMutexLocker lock(&m_shared->mutex);
    m_shared->renderer.setRepeat(division);
}

void AudioEngine::suppress(int pad, int pattern, int step) {
    QMutexLocker lock(&m_shared->mutex);
    m_shared->renderer.suppress(pad, pattern, step);
}

void AudioEngine::preview(const SampleBuffer &sound, double rate) {
    open();
    QMutexLocker lock(&m_shared->mutex);
    m_shared->renderer.preview(sound, rate);
}
