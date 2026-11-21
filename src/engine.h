#pragma once

#include <QMutex>
#include <QObject>
#include <QThread>
#include <memory>

#include "renderer.h"

// A beat shared by doles playing together: step 0 of it is heard at origin,
// in microseconds of this machine's steady clock, and it runs at bpm. Once
// started, playing began on its step startStep, so every dole plays the
// same step at the same moment.
struct Timeline {
    bool on = false;
    qint64 origin = 0;
    double bpm = 90;
    bool started = false;
    qint64 startStep = 0;

    // The timeline's step heard at a moment of the steady clock.
    double stepAt(qint64 microseconds) const { return double(microseconds - origin) * bpm / 15e6; }
    bool operator==(const Timeline &other) const {
        return on == other.on && origin == other.origin && bpm == other.bpm && started == other.started
               && startStep == other.startStep;
    }
};
// The steady clock, in microseconds, that timelines are kept in.
qint64 steadyMicroseconds();

// What the interface asks of the sound: play, stop, and hear the song as it
// changes. The real one plays through the speakers; tests use a fake that
// records what it was asked.
class Engine : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    virtual void setSong(const Song &song) = 0;
    virtual void setSounds(const std::array<SampleBuffer, kPads> &sounds) = 0;
    virtual void play(bool songMode, int pattern, int chainIndex) = 0;
    virtual void cue(int pattern) = 0;
    virtual void stop() = 0;
    // A pad struck live, and held for note repeat.
    virtual void hit(int pad, int level) = 0;
    virtual void hold(int pad, bool held, int level) = 0;
    // A pad let go: a gated one stops.
    virtual void release(int pad) { Q_UNUSED(pad); }
    virtual void setRepeat(int division) = 0;
    virtual void suppress(int pad, int pattern, int step) = 0;
    virtual void preview(const SampleBuffer &sound, double rate) = 0;
    virtual void setRack(const Rack &rack) = 0;
    // Runs work while the audio is held off, for changing a plugin the
    // audio thread is playing.
    virtual void locked(const std::function<void()> &work) = 0;
    // The loudest each pad, and then the mix, got since the last ask.
    virtual std::array<float, kPads + 1> takePeaks() { return {}; }
    // Plays in time with a shared beat; one that's off plays on its own.
    virtual void setTimeline(const Timeline &timeline) { Q_UNUSED(timeline); }
    // Where the playhead is heard now, in steps since playing started.
    virtual double heardClock() const { return 0; }

signals:
    void positionChanged(int chainIndex, int pattern, int step);
    // Note repeat played a held pad on the step nearest.
    void repeated(int pad, int pattern, int step);
    // "noDevice", or "deviceFailed" with Qt's reason.
    void failed(const QString &kind, const QString &detail);
};

// Plays through the default audio output, rendering on a thread of its own so
// the interface never makes it stutter. The output opens the first time
// anything plays, keeping Qt Multimedia out of the app's start.
class AudioEngine : public Engine {
    Q_OBJECT

public:
    explicit AudioEngine(QObject *parent = nullptr);
    ~AudioEngine() override;

    void setSong(const Song &song) override;
    void setSounds(const std::array<SampleBuffer, kPads> &sounds) override;
    void play(bool songMode, int pattern, int chainIndex) override;
    void cue(int pattern) override;
    void stop() override;
    void hit(int pad, int level) override;
    void hold(int pad, bool held, int level) override;
    void release(int pad) override;
    void setRepeat(int division) override;
    void suppress(int pad, int pattern, int step) override;
    void preview(const SampleBuffer &sound, double rate) override;
    void setRack(const Rack &rack) override;
    void locked(const std::function<void()> &work) override;
    std::array<float, kPads + 1> takePeaks() override;
    void setTimeline(const Timeline &timeline) override;
    double heardClock() const override;
    // Opens the output ahead of the first Space, which would otherwise wait
    // most of a second for it. It opens on the audio thread, so the window
    // never waits.
    void prepare() { open(); }

    // The renderer, shared with the audio thread under its lock.
    struct Shared {
        QMutex mutex;
        Renderer renderer;
        Timeline timeline;
        // How long what is rendered now waits before it's heard.
        qint64 latency = 0;
        // Where the timeline puts the playhead, at the first frame of a
        // buffer heard at a moment.
        double expected(qint64 heardAt) const { return timeline.stepAt(heardAt) - double(timeline.startStep); }
    };

private:
    void open();

    std::shared_ptr<Shared> m_shared = std::make_shared<Shared>();
    QThread m_thread;
    QObject *m_output = nullptr; // lives on m_thread
    bool m_opened = false;
};
