#pragma once

#include "plugin.h"

// LV2 plugins, wherever lilv finds them ($LV2_PATH, ~/.lv2, /usr/lib/lv2).
// Reading their descriptions runs no plugin code.
QVector<PluginInfo> scanLv2();
// The plugin with this URI (without the "lv2:" prefix).
std::unique_ptr<Processor> createLv2(const QString &uri, QString *error);
