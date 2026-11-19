#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>

#include "song.h"

// Where the song is, for effects that keep time (the stutter gate) and
// plugins that ask.
struct Transport {
    double bpm = 90;
    bool playing = false;
    double position = 0; // in steps from the top of the bar, at the block's first frame
};

// A note for an instrument: on or off at a frame of the block.
struct NoteEvent {
    int frame = 0;
    int key = 60;
    float velocity = 0.8f;
    bool on = true;
};

struct Parameter {
    QString id;
    QString name;
    double min = 0;
    double max = 1;
    double defaultValue = 0;
    double value = 0;
    bool stepped = false;
    bool logarithmic = false;
};

// The most frames any processor is asked for at once.
constexpr int kMaxBlock = 1024;

// One instrument or effect, whichever kind of plugin it is. process() runs
// on the audio thread; everything else on the main thread, while the audio
// thread is held off, so the two never meet inside a plugin.
class Processor {
public:
    virtual ~Processor() = default;

    virtual QString name() const = 0;
    virtual bool instrument() const = 0;

    // Stereo, in place: an effect changes what's in left and right; an
    // instrument writes its sound over them.
    virtual void process(float *left, float *right, int frames, const NoteEvent *notes, int noteCount,
                         const Transport &transport) = 0;

    virtual QVector<Parameter> parameters() const = 0;
    virtual void setParameter(const QString &id, double value) = 0;
    // How a value reads ("1.2 kHz"), when the plugin says.
    virtual QString valueText(const QString &id, double value) const;

    // The plugin's own saved state, for plugins that keep more than their
    // parameters.
    virtual QByteArray saveState() { return {}; }
    virtual bool loadState(const QByteArray &) { return false; }

    // The plugin's own window, where it has one.
    virtual bool hasEditor() const { return false; }
    virtual bool showEditor(QString *error);

    // Called on the main thread when the plugin's own window changed it.
    std::function<void()> onEdited;
};

// A plugin that can be put on a pad or the master.
struct PluginInfo {
    QString id; // as a PluginSlot names it
    QString name;
    QString vendor;
    QString format; // "dole", "clap", or "lv2"
    bool instrument = false;
};

// dole's own effects, which need nothing installed.
QVector<PluginInfo> builtinPlugins();
// Every plugin on the machine. Loading a CLAP plugin runs its code, and a
// broken one can crash the process doing it, so the window asks
// `dole plugins` in a process of its own.
QVector<PluginInfo> scanPlugins();

// Makes the plugin a slot names, with its state and parameters applied, on
// the main thread.
std::shared_ptr<Processor> createProcessor(const PluginSlot &slot, QString *error);
