#include "plugin.h"

#include "builtins.h"
#include "clap.h"
#include "lv2.h"

QString Processor::valueText(const QString &, double value) const {
    return std::abs(value) >= 100 ? QString::number(value, 'f', 0) : QString::number(value, 'g', 3);
}

bool Processor::showEditor(QString *error) {
    *error = "it has no window of its own";
    return false;
}

QVector<PluginInfo> builtinPlugins() {
    return builtinInfos();
}

QVector<PluginInfo> scanPlugins() {
    return builtinInfos() + scanClap() + scanLv2();
}

std::shared_ptr<Processor> createProcessor(const PluginSlot &slot, QString *error) {
    std::unique_ptr<Processor> processor;
    if (slot.id.startsWith("clap:"))
        processor = createClap(slot.id.mid(5), error);
    else if (slot.id.startsWith("lv2:"))
        processor = createLv2(slot.id.mid(4), error);
    else if (!(processor = createBuiltin(slot.id)))
        *error = "dole has no effect by that name";
    if (!processor)
        return {};
    if (!slot.state.isEmpty())
        processor->loadState(slot.state);
    for (auto it = slot.params.begin(); it != slot.params.end(); ++it)
        processor->setParameter(it.key(), it.value());
    return std::shared_ptr<Processor>(std::move(processor));
}
