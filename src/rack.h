#pragma once

#include <QStringList>
#include <array>
#include <memory>

#include "plugin.h"

struct RackSlot {
    std::shared_ptr<Processor> processor;
    bool bypass = false;
    int index = 0; // its place in the song's chain, which a lane names it by
};

// The plugins a song plays through, made: each pad's synth and effects, and
// the master's effects. The renderer holds one; whoever made it destroys
// the plugins, on the main thread.
struct Rack {
    std::array<std::shared_ptr<Processor>, kPads> synths;
    std::array<QVector<RackSlot>, kPads> effects;
    QVector<RackSlot> master;
};

// A fresh set of plugins for a song, as an export or a command needs; what
// couldn't be made is left out, and said in problems.
Rack buildRack(const Song &song, QStringList *problems);
