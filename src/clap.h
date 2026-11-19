#pragma once

#include "plugin.h"

// CLAP plugins, from $CLAP_PATH, ~/.clap, and /usr/lib/clap.
QVector<PluginInfo> scanClap();
// The plugin with this CLAP id (without the "clap:" prefix).
std::unique_ptr<Processor> createClap(const QString &id, QString *error);
