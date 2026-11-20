#include "rack.h"

namespace {

RackSlot make(const PluginSlot &slot, const QString &where, QStringList *problems) {
    QString error;
    std::shared_ptr<Processor> processor = createProcessor(slot, &error);
    if (!processor)
        *problems << QString("%1: %2: %3").arg(where, slot.id, error);
    return {processor, slot.bypass, 0};
}

} // namespace

Rack buildRack(const Song &song, QStringList *problems) {
    Rack rack;
    for (int p = 0; p < kPads; ++p) {
        const Pad &pad = song.pads[p];
        const QString where = QString("Pad %1").arg(p + 1);
        if (!pad.synth.isEmpty())
            rack.synths[p] = make(pad.synth, where, problems).processor;
        for (int i = 0; i < pad.effects.size(); ++i)
            if (RackSlot made = make(pad.effects[i], where, problems); made.processor) {
                made.index = i;
                rack.effects[p] << made;
            }
    }
    for (const PluginSlot &slot : song.master)
        if (RackSlot made = make(slot, "Master", problems); made.processor)
            rack.master << made;
    return rack;
}
