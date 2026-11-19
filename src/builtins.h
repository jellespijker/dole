#pragma once

#include "plugin.h"

// dole's own effects, made for noise: overdrive, bitcrush, ringmod, comb,
// delay, and stutter. Empty for an id that isn't one of them.
std::unique_ptr<Processor> createBuiltin(const QString &id);
QVector<PluginInfo> builtinInfos();
