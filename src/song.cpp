#include "song.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHash>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>
#include <cmath>

namespace {

// A row is one character per step: '.' is off, '1' to '4' a level, with 'o'
// for 2, 'x' for 3, and 'X' for 4 as easier names.
QChar levelChar(int level) {
    return level == 4 ? 'X' : level == 3 ? 'x' : level == 0 ? '.' : QChar('0' + level);
}

int charLevel(QChar c) {
    if (c == 'x')
        return 3;
    if (c == 'X')
        return 4;
    if (c == 'o')
        return 2;
    if (c >= '1' && c <= '4')
        return c.unicode() - '0';
    return c == '.' || c == '-' ? 0 : -1;
}

QString relativeTo(const QString &baseDirectory, const QString &path) {
    if (baseDirectory.isEmpty())
        return path;
    const QString relative = QDir(baseDirectory).relativeFilePath(path);
    return relative.startsWith("..") ? path : relative;
}

// A pad by its number ("1" to "64"), its bank and number ("B3"), or its
// kit sound's name ("boom").
int padIndex(const QString &key) {
    bool number = false;
    const int n = key.toInt(&number);
    if (number)
        return n >= 1 && n <= kPads ? n - 1 : -1;
    if (key.size() >= 2 && key.at(0).toUpper() >= 'A' && key.at(0).toUpper() <= QChar('A' + kBanks - 1)) {
        const int inBank = key.mid(1).toInt(&number);
        if (number && inBank >= 1 && inBank <= kBankPads)
            return (key.at(0).toUpper().unicode() - 'A') * kBankPads + inBank - 1;
    }
    return kitIds().indexOf(key);
}

// Steps in JSON count from 1, as a musician counts them.
QJsonObject stepNumbers(const QMap<int, double> &values) {
    QJsonObject object;
    for (auto it = values.begin(); it != values.end(); ++it)
        object.insert(QString::number(it.key() + 1), std::round(it.value() * 10000) / 10000);
    return object;
}

bool fromStepNumbers(const QJsonValue &value, int length, const QString &where, QMap<int, double> *out,
                     QString *error) {
    const QJsonObject object = value.toObject();
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool number = false;
        const int step = it.key().toInt(&number) - 1;
        if (!number || step < 0 || step >= length) {
            *error = where + QString("\"%1\" isn't a step from 1 to %2.").arg(it.key()).arg(length);
            return false;
        }
        if (!it.value().isDouble()) {
            *error = where + QString("step %1 should be a number.").arg(it.key());
            return false;
        }
        out->insert(step, it.value().toDouble());
    }
    return true;
}

QJsonObject slotToJson(const PluginSlot &slot) {
    QJsonObject object{{"plugin", slot.id}};
    if (!slot.params.isEmpty()) {
        QJsonObject params;
        for (auto it = slot.params.begin(); it != slot.params.end(); ++it)
            params.insert(it.key(), std::round(it.value() * 10000) / 10000);
        object.insert("params", params);
    }
    if (!slot.state.isEmpty())
        object.insert("state", QString::fromLatin1(slot.state.toBase64()));
    if (slot.bypass)
        object.insert("bypass", true);
    return object;
}

bool slotFromJson(const QJsonValue &value, const QString &where, PluginSlot *slot, QString *error) {
    const QJsonObject object = value.toObject();
    slot->id = object.value("plugin").toString();
    if (slot->id.isEmpty()) {
        *error = where + "needs a \"plugin\".";
        return false;
    }
    const QJsonObject params = object.value("params").toObject();
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (!it.value().isDouble()) {
            *error = where + QString("parameter \"%1\" should be a number.").arg(it.key());
            return false;
        }
        slot->params.insert(it.key(), it.value().toDouble());
    }
    slot->state = QByteArray::fromBase64(object.value("state").toString().toLatin1());
    slot->bypass = object.value("bypass").toBool();
    return true;
}

QJsonArray chainToJson(const QVector<PluginSlot> &chain) {
    QJsonArray array;
    for (const PluginSlot &slot : chain)
        array.append(slotToJson(slot));
    return array;
}

bool chainFromJson(const QJsonValue &value, const QString &where, QVector<PluginSlot> *chain, QString *error) {
    const QJsonArray array = value.toArray();
    if (array.size() > kMaxEffects) {
        *error = where + QString("has more than %1 effects.").arg(kMaxEffects);
        return false;
    }
    for (int i = 0; i < array.size(); ++i) {
        PluginSlot slot;
        if (!slotFromJson(array[i], where + QString("effect %1: ").arg(i + 1), &slot, error))
            return false;
        *chain << slot;
    }
    return true;
}

double seconds(double value) {
    return std::round(value * 1000) / 1000;
}

// The first format had eight named tracks and a bass synthesizer; each
// track moves to the pad with the nearest sound, and the bass to the bass pad.
QJsonObject upgradeFromTracks(QJsonObject json) {
    static const QHash<QString, QString> pads{{"kick", "1"},  {"snare", "3"},   {"hat", "7"},     {"openhat", "8"},
                                             {"clap", "6"},  {"rim", "5"},     {"cowbell", "12"}, {"bass", "15"}};
    QJsonObject padSettings;
    const QJsonObject tracks = json.take("tracks").toObject();
    for (auto it = tracks.begin(); it != tracks.end(); ++it)
        if (pads.contains(it.key()))
            padSettings.insert(pads.value(it.key()), it.value());
    QJsonObject patterns = json.value("patterns").toObject();
    for (auto it = patterns.begin(); it != patterns.end(); ++it) {
        QJsonObject rows;
        const QJsonObject old = it.value().toObject();
        for (auto row = old.begin(); row != old.end(); ++row) {
            if (!pads.contains(row.key()))
                continue;
            QString text = row.value().toString();
            if (row.key() == "bass") {
                QString steps;
                for (const QString &word : text.split(' ', Qt::SkipEmptyParts))
                    steps += word == "." || word == "-" ? '.' : 'x';
                text = steps;
            }
            rows.insert(pads.value(row.key()), text);
        }
        it.value() = rows;
    }
    json.insert("pads", padSettings);
    json.insert("patterns", patterns);
    json.insert("dole", 2);
    return json;
}

} // namespace

Pattern::Pattern() {
    // Every new pattern shares one set of empty rows until it is written to.
    static const QVector<QVector<Step>> empty(kPads, QVector<Step>(kMaxSteps));
    steps = empty;
}

bool Pattern::isEmpty() const {
    if (length != kSteps || !lanes.isEmpty())
        return false;
    for (const auto &row : steps)
        for (const Step &step : row)
            if (step.on())
                return false;
    return true;
}

std::array<Pad, kPads> Song::defaultPads() {
    std::array<Pad, kPads> pads{};
    // The closed hat cuts the open one off, as on a real kit.
    pads[6].group = 1;
    pads[7].group = 1;
    return pads;
}

int padFromKey(const QString &key) {
    return padIndex(key);
}

QString padName(int pad) {
    return QString(QChar('A' + pad / kBankPads)) + QString::number(pad % kBankPads + 1);
}

LaneInfo laneInfo(const QString &id) {
    LaneInfo info;
    static const QHash<QString, std::array<double, 2>> ranges{
        {"pitch", {double(kMinTune), double(kMaxTune)}}, {"volume", {0, 1}}, {"pan", {-1, 1}},
        {"filter", {-1, 1}}, {"resonance", {0, 1}}, {"decay", {0, 1}}};
    if (ranges.contains(id)) {
        info.valid = true;
        info.target = "pad";
        info.param = id;
        info.min = ranges[id][0];
        info.max = ranges[id][1];
        info.stepped = id == "pitch";
        return info;
    }
    // A plugin's range is the plugin's to say; the lane only names it.
    if (id.startsWith("synth.") && id.size() > 6) {
        info.valid = true;
        info.target = "synth";
        info.param = id.mid(6);
        return info;
    }
    static const QRegularExpression effect("^fx([1-9])\\.(.+)$");
    const QRegularExpressionMatch match = effect.match(id);
    if (match.hasMatch() && match.captured(1).toInt() <= kMaxEffects) {
        info.valid = true;
        info.target = "fx";
        info.effect = match.captured(1).toInt() - 1;
        info.param = match.captured(2);
    }
    return info;
}

double padValue(const Pad &pad, const QString &param) {
    if (param == "volume")
        return pad.volume;
    if (param == "pan")
        return pad.pan;
    if (param == "filter")
        return pad.filter;
    if (param == "resonance")
        return pad.resonance;
    if (param == "decay")
        return pad.decay;
    return 0; // pitch, which adds to the pad's tune
}

double laneValue(const Lane &lane, int step) {
    auto it = lane.upperBound(step);
    if (it == lane.begin())
        return std::nan("");
    return std::prev(it).value();
}

int songSteps(const Song &song, int pattern) {
    if (song.chain.isEmpty())
        return song.patterns[qBound(0, pattern, kPatterns - 1)].length;
    int steps = 0;
    for (int p : song.chain)
        steps += song.patterns[p].length;
    return steps;
}

bool Song::audible(int pad) const {
    bool anySolo = false;
    for (const Pad &p : pads)
        anySolo = anySolo || p.solo;
    return !pads[pad].mute && (!anySolo || pads[pad].solo);
}

const QStringList &kitIds() {
    static const QStringList ids{"boom",   "knock", "crack", "dusty", "rim",  "clap", "hat",  "openhat",
                                 "shaker", "tom",   "crash", "clang", "stab", "bell", "bass", "horn"};
    return ids;
}

QString patternName(int pattern) {
    return QString(QChar('A' + pattern));
}

int patternIndex(const QString &name) {
    if (name.size() != 1)
        return -1;
    const int index = name.at(0).toUpper().unicode() - 'A';
    return index >= 0 && index < kPatterns ? index : -1;
}

QString gritName(Grit grit) {
    return grit == Grit::TwelveBit ? "12-bit" : grit == Grit::Crushed ? "crushed" : "clean";
}

float levelGain(int level) {
    static const float gains[] = {0.f, 0.3f, 0.55f, 0.8f, 1.f};
    return gains[qBound(0, level, kLevels)];
}

QJsonObject songToJson(const Song &song, const QString &baseDirectory) {
    const auto defaults = Song::defaultPads();
    QJsonObject pads;
    for (int p = 0; p < kPads; ++p) {
        const Pad &pad = song.pads[p];
        QJsonObject object{{"volume", std::round(pad.volume * 100) / 100}};
        if (!pad.sample.isEmpty())
            object.insert("sample", relativeTo(baseDirectory, pad.sample));
        if (pad.start > 0)
            object.insert("start", seconds(pad.start));
        if (pad.end > 0)
            object.insert("end", seconds(pad.end));
        if (pad.tune != 0)
            object.insert("tune", pad.tune);
        auto setting = [&](const char *name, double value, double unset) {
            if (std::abs(value - unset) > 1e-9)
                object.insert(name, std::round(value * 1000) / 1000);
        };
        setting("pan", pad.pan, 0);
        setting("filter", pad.filter, 0);
        setting("resonance", pad.resonance, 0);
        setting("attack", pad.attack, 0);
        setting("decay", pad.decay, 1);
        if (pad.reverse)
            object.insert("reverse", true);
        if (pad.gate)
            object.insert("gate", true);
        if (pad.swing != 0)
            object.insert("swing", pad.swing);
        if (pad.loop != 0)
            object.insert("loop", pad.loop);
        if (pad.group != defaults[p].group)
            object.insert("group", pad.group);
        if (pad.mute)
            object.insert("mute", true);
        if (pad.solo)
            object.insert("solo", true);
        if (!pad.synth.isEmpty())
            object.insert("synth", slotToJson(pad.synth));
        if (pad.length != 1)
            object.insert("length", pad.length);
        if (!pad.effects.isEmpty())
            object.insert("effects", chainToJson(pad.effects));
        // The empty pads of banks B to D are left out.
        if (p >= kBankPads && pad == defaults[p])
            continue;
        pads.insert(QString::number(p + 1), object);
    }

    QJsonObject patterns;
    for (int p = 0; p < kPatterns; ++p) {
        const Pattern &pattern = song.patterns[p];
        if (pattern.isEmpty())
            continue;
        QJsonObject rows;
        if (pattern.length != kSteps)
            rows.insert("length", pattern.length);
        for (int pad = 0; pad < kPads; ++pad) {
            QString row;
            QMap<int, double> roll, chance, nudge;
            for (int i = 0; i < pattern.length; ++i) {
                const Step &step = pattern.steps[pad][i];
                row += levelChar(step.level);
                if (step.roll != 1)
                    roll.insert(i, step.roll);
                if (step.chance != 100)
                    chance.insert(i, step.chance);
                if (step.nudge != 0)
                    nudge.insert(i, step.nudge);
            }
            const Lanes lanes = pattern.lanes.value(pad);
            const QString key = QString::number(pad + 1);
            // A row of plain steps is a string; one with more to it, an object.
            if (roll.isEmpty() && chance.isEmpty() && nudge.isEmpty() && lanes.isEmpty()) {
                if (row != QString(pattern.length, '.'))
                    rows.insert(key, row);
                continue;
            }
            QJsonObject object{{"steps", row}};
            if (!roll.isEmpty())
                object.insert("roll", stepNumbers(roll));
            if (!chance.isEmpty())
                object.insert("chance", stepNumbers(chance));
            if (!nudge.isEmpty())
                object.insert("nudge", stepNumbers(nudge));
            if (!lanes.isEmpty()) {
                QJsonObject lanesJson;
                for (auto it = lanes.begin(); it != lanes.end(); ++it)
                    lanesJson.insert(it.key(), stepNumbers(it.value()));
                object.insert("lanes", lanesJson);
            }
            rows.insert(key, object);
        }
        patterns.insert(patternName(p), rows);
    }

    QJsonArray chain;
    for (int p : song.chain)
        chain.append(patternName(p));

    QJsonObject json{{"dole", 3},           {"bpm", song.bpm},     {"swing", song.swing}, {"grit", gritName(song.grit)},
                     {"vinyl", song.vinyl}, {"pads", pads},        {"patterns", patterns}, {"song", chain}};
    if (!song.master.isEmpty())
        json.insert("master", chainToJson(song.master));
    return json;
}

bool songFromJson(const QJsonObject &input, const QString &baseDirectory, Song *out, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const int version = input.value("dole").toInt();
    if (version < 1 || version > 3)
        return fail("Not a dole song: \"dole\" should be 3.");
    const QJsonObject json = version == 1 ? upgradeFromTracks(input) : input;

    Song song;
    song.bpm = json.value("bpm").toInt(song.bpm);
    if (song.bpm < kMinBpm || song.bpm > kMaxBpm)
        return fail(QString("\"bpm\" should be between %1 and %2.").arg(kMinBpm).arg(kMaxBpm));
    song.swing = json.value("swing").toInt(song.swing);
    if (song.swing < kMinSwing || song.swing > kMaxSwing)
        return fail(QString("\"swing\" should be between %1 and %2.").arg(kMinSwing).arg(kMaxSwing));
    const QString grit = json.value("grit").toString("clean");
    if (grit == "12-bit")
        song.grit = Grit::TwelveBit;
    else if (grit == "crushed")
        song.grit = Grit::Crushed;
    else if (grit != "clean")
        return fail(QString("\"grit\" is \"clean\", \"12-bit\", or \"crushed\", not \"%1\".").arg(grit));
    song.vinyl = json.value("vinyl").toBool();

    const QJsonObject pads = json.value("pads").toObject();
    for (auto it = pads.begin(); it != pads.end(); ++it) {
        const int p = padIndex(it.key());
        if (p < 0)
            return fail(QString("Unknown pad \"%1\"; pads are 1 to 64, or A1 to D16.").arg(it.key()));
        const QJsonObject object = it.value().toObject();
        Pad &pad = song.pads[p];
        pad.volume = qBound(0.0, object.value("volume").toDouble(pad.volume), 1.0);
        pad.tune = object.value("tune").toInt(0);
        if (pad.tune < kMinTune || pad.tune > kMaxTune)
            return fail(QString("Pad %1: \"tune\" should be between %2 and %3.").arg(p + 1).arg(kMinTune).arg(kMaxTune));
        pad.group = object.value("group").toInt(pad.group);
        if (pad.group < 0 || pad.group > kGroups)
            return fail(QString("Pad %1: \"group\" should be between 0 and %2.").arg(p + 1).arg(kGroups));
        pad.mute = object.value("mute").toBool();
        pad.solo = object.value("solo").toBool();
        const QString at = QString("Pad %1: ").arg(p + 1);
        auto setting = [&](const char *name, double *value, double low, double high) {
            *value = object.value(name).toDouble(*value);
            if (*value < low || *value > high) {
                fail(at + QString("\"%1\" should be between %2 and %3.").arg(name).arg(low).arg(high));
                return false;
            }
            return true;
        };
        if (!setting("pan", &pad.pan, -1, 1) || !setting("filter", &pad.filter, -1, 1)
            || !setting("resonance", &pad.resonance, 0, 1) || !setting("attack", &pad.attack, 0, 1)
            || !setting("decay", &pad.decay, 0, 1))
            return false;
        pad.reverse = object.value("reverse").toBool();
        pad.gate = object.value("gate").toBool();
        pad.swing = object.value("swing").toInt(0);
        if (pad.swing != 0 && (pad.swing < kMinSwing || pad.swing > kMaxSwing))
            return fail(at + QString("\"swing\" should be between %1 and %2.").arg(kMinSwing).arg(kMaxSwing));
        pad.loop = object.value("loop").toInt(0);
        if (pad.loop < 0 || pad.loop > kMaxSteps)
            return fail(at + QString("\"loop\" should be between 1 and %1 steps.").arg(kMaxSteps));
        const QString sample = object.value("sample").toString();
        if (!sample.isEmpty())
            pad.sample = QDir::cleanPath(QDir(baseDirectory).absoluteFilePath(sample));
        pad.start = std::max(0.0, object.value("start").toDouble());
        pad.end = std::max(0.0, object.value("end").toDouble());
        if (pad.end > 0 && pad.end <= pad.start)
            return fail(QString("Pad %1: \"end\" should be after \"start\".").arg(p + 1));
        const QString where = QString("Pad %1: ").arg(p + 1);
        QString problem;
        if (object.contains("synth") && !slotFromJson(object.value("synth"), where + "synth: ", &pad.synth, &problem))
            return fail(problem);
        pad.length = object.value("length").toInt(1);
        if (std::find(std::begin(kLengths), std::end(kLengths), pad.length) == std::end(kLengths))
            return fail(where + "\"length\" is 1, 2, 4, 8, or 16 steps.");
        if (!chainFromJson(object.value("effects"), where, &pad.effects, &problem))
            return fail(problem);
    }

    const QJsonObject patterns = json.value("patterns").toObject();
    for (auto it = patterns.begin(); it != patterns.end(); ++it) {
        const int p = patternIndex(it.key());
        if (p < 0)
            return fail(QString("Unknown pattern \"%1\"; patterns are A to H.").arg(it.key()));
        const QJsonObject rows = it.value().toObject();
        Pattern &pattern = song.patterns[p];
        pattern.length = rows.value("length").toInt(kSteps);
        if (pattern.length % kSteps != 0 || pattern.length < kSteps || pattern.length > kMaxSteps)
            return fail(QString("Pattern %1: \"length\" is 16, 32, 48, or 64 steps.").arg(it.key()));
        for (auto row = rows.begin(); row != rows.end(); ++row) {
            if (row.key() == "length")
                continue;
            const int pad = padIndex(row.key());
            const QString where = QString("Pattern %1, pad %2: ").arg(it.key(), row.key());
            if (pad < 0)
                return fail(where + "unknown pad; pads are 1 to 64, or A1 to D16.");
            const QJsonObject object = row.value().toObject();
            const QString compact = (row.value().isString() ? row.value().toString() : object.value("steps").toString())
                                        .remove(' ')
                                        .remove('|');
            if (compact.size() != pattern.length)
                return fail(where + QString("needs %1 steps, has %2.").arg(pattern.length).arg(compact.size()));
            QVector<Step> &steps = pattern.steps[pad];
            for (int s = 0; s < pattern.length; ++s) {
                const int level = charLevel(compact[s]);
                if (level < 0)
                    return fail(where + QString("\"%1\" is not a step; use . x X or 1 to 4.").arg(compact[s]));
                steps[s].level = level;
            }
            QMap<int, double> roll, chance, nudge;
            QString problem;
            if (!fromStepNumbers(object.value("roll"), pattern.length, where + "roll: ", &roll, &problem)
                || !fromStepNumbers(object.value("chance"), pattern.length, where + "chance: ", &chance, &problem)
                || !fromStepNumbers(object.value("nudge"), pattern.length, where + "nudge: ", &nudge, &problem))
                return fail(problem);
            for (auto r = roll.begin(); r != roll.end(); ++r) {
                if (r.value() < 1 || r.value() > kMaxRoll)
                    return fail(where + QString("a roll is 1 to %1 hits.").arg(kMaxRoll));
                steps[r.key()].roll = int(r.value());
            }
            for (auto c = chance.begin(); c != chance.end(); ++c) {
                if (c.value() < 0 || c.value() > 100)
                    return fail(where + "a chance is 0 to 100 percent.");
                steps[c.key()].chance = int(c.value());
            }
            for (auto n = nudge.begin(); n != nudge.end(); ++n) {
                if (std::abs(n.value()) > kMaxNudge)
                    return fail(where + QString("a nudge is -%1 to %1 percent of a step.").arg(kMaxNudge));
                steps[n.key()].nudge = int(n.value());
            }
            const QJsonObject lanes = object.value("lanes").toObject();
            for (auto lane = lanes.begin(); lane != lanes.end(); ++lane) {
                const LaneInfo info = laneInfo(lane.key());
                if (!info.valid)
                    return fail(where + QString("\"%1\" isn't a lane; lanes are pitch, volume, pan, filter, resonance, "
                                                "decay, synth.<parameter>, and fx<n>.<parameter>.")
                                            .arg(lane.key()));
                Lane points;
                if (!fromStepNumbers(lane.value(), pattern.length, where + lane.key() + ": ", &points, &problem))
                    return fail(problem);
                if (info.target == "pad")
                    for (auto point = points.begin(); point != points.end(); ++point)
                        if (point.value() < info.min || point.value() > info.max)
                            return fail(where + QString("%1 goes from %2 to %3.").arg(lane.key()).arg(info.min).arg(info.max));
                if (!points.isEmpty())
                    pattern.lanes[pad][lane.key()] = points;
            }
        }
    }

    for (const QJsonValue &value : json.value("song").toArray()) {
        const int p = patternIndex(value.toString());
        if (p < 0)
            return fail(QString("The song names \"%1\", which isn't a pattern from A to H.").arg(value.toString()));
        if (song.chain.size() == kMaxChain)
            return fail(QString("The song is longer than %1 patterns.").arg(kMaxChain));
        song.chain << p;
    }

    QString problem;
    if (!chainFromJson(json.value("master"), "Master: ", &song.master, &problem))
        return fail(problem);

    *out = song;
    return true;
}

namespace {

QJsonValue mergePatch(const QJsonValue &target, const QJsonValue &patch) {
    if (!patch.isObject())
        return patch;
    QJsonObject merged = target.isObject() ? target.toObject() : QJsonObject();
    const QJsonObject changes = patch.toObject();
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        if (it.value().isNull())
            merged.remove(it.key());
        else
            merged.insert(it.key(), mergePatch(merged.value(it.key()), it.value()));
    }
    return merged;
}

// The key songToJson writes for a pad, so a patch naming it another way
// lands on the same one. A key that names no pad is kept, for the song's
// own check to report.
QString padKey(const QString &key) {
    const int pad = padIndex(key);
    return pad < 0 ? key : QString::number(pad + 1);
}

QJsonObject patchPattern(QJsonObject rows, const QJsonObject &patch) {
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        const QString key = it.key() == "length" ? it.key() : padKey(it.key());
        if (it.value().isNull()) {
            rows.remove(key);
            continue;
        }
        QJsonValue row = rows.value(key);
        if (it.value().isObject() && row.isString())
            row = QJsonObject{{"steps", row}};
        rows.insert(key, mergePatch(row, it.value()));
    }
    return rows;
}

} // namespace

QJsonObject patchSongJson(const QJsonObject &song, const QJsonObject &patch) {
    QJsonObject merged = song;
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        const QString &key = it.key();
        if (it.value().isNull()) {
            merged.remove(key);
        } else if (key == "pads" && it.value().isObject()) {
            QJsonObject pads = merged.value(key).toObject();
            const QJsonObject changes = it.value().toObject();
            for (auto pad = changes.begin(); pad != changes.end(); ++pad) {
                if (pad.value().isNull())
                    pads.remove(padKey(pad.key()));
                else
                    pads.insert(padKey(pad.key()), mergePatch(pads.value(padKey(pad.key())), pad.value()));
            }
            merged.insert(key, pads);
        } else if (key == "patterns" && it.value().isObject()) {
            QJsonObject patterns = merged.value(key).toObject();
            const QJsonObject changes = it.value().toObject();
            for (auto pattern = changes.begin(); pattern != changes.end(); ++pattern) {
                const QString name = pattern.key().toUpper();
                if (pattern.value().isNull())
                    patterns.remove(name);
                else if (pattern.value().isObject())
                    patterns.insert(name, patchPattern(patterns.value(name).toObject(), pattern.value().toObject()));
                else
                    patterns.insert(name, pattern.value());
            }
            merged.insert(key, patterns);
        } else {
            merged.insert(key, mergePatch(merged.value(key), it.value()));
        }
    }
    return merged;
}

namespace {

// deep is how many levels down a part that went is taken out piece by
// piece: rows of a pattern and pads, never more, so a patch that removes
// what one person had leaves what someone else added beside it.
QJsonObject diffObjects(const QJsonObject &from, const QJsonObject &to, int deep) {
    QJsonObject patch;
    for (auto it = from.begin(); it != from.end(); ++it) {
        if (to.contains(it.key()))
            continue;
        if (deep > 0 && it.value().isObject())
            patch.insert(it.key(), diffObjects(it.value().toObject(), {}, deep - 1));
        else
            patch.insert(it.key(), QJsonValue::Null);
    }
    for (auto it = to.begin(); it != to.end(); ++it) {
        const QJsonValue before = from.value(it.key());
        if (before == it.value())
            continue;
        if (before.isObject() && it.value().isObject())
            patch.insert(it.key(), diffObjects(before.toObject(), it.value().toObject(), std::max(0, deep - 1)));
        else
            patch.insert(it.key(), it.value());
    }
    return patch;
}

} // namespace

QJsonObject diffSongJson(const QJsonObject &from, const QJsonObject &to) {
    QJsonObject patch = diffObjects(from, to, 0);
    // Patterns go row by row, and pads one by one.
    for (const char *key : {"patterns", "pads"}) {
        const int deep = QByteArray(key) == "patterns" ? 2 : 1;
        if (patch.contains(key) && from.value(key).isObject())
            patch.insert(key, diffObjects(from.value(key).toObject(), to.value(key).toObject(), deep));
    }
    return patch;
}

bool loadSong(const QString &path, Song *song, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = file.errorString();
        return false;
    }
    QJsonParseError parse;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError) {
        *error = QString("Not valid JSON at character %1: %2").arg(parse.offset).arg(parse.errorString());
        return false;
    }
    return songFromJson(document.object(), QFileInfo(path).absolutePath(), song, error);
}

bool saveSong(const QString &path, const Song &song, QString *error) {
    // QSaveFile writes beside the file and renames it into place; with no
    // direct-write fallback, a folder it can't write a temp file in fails the
    // save rather than having the song truncated and rewritten in place.
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    const QByteArray data = QJsonDocument(songToJson(song, QFileInfo(path).absolutePath())).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}

Song demoSong() {
    // Built through the file format, so it reads like a song file does: a
    // lazy boom-bap with a stab hanging over it and a bell answering.
    const QJsonObject json = QJsonDocument::fromJson(R"({
        "dole": 2, "bpm": 88, "swing": 58, "grit": "12-bit", "vinyl": true,
        "pads": {"13": {"volume": 0.7, "tune": -2}, "14": {"volume": 0.5, "tune": -5}, "15": {"volume": 0.9}},
        "patterns": {
            "A": {
                "1":  "X......x..x.....",
                "3":  "....X.......X...",
                "7":  "x.o.x.o.x.o.x.oo",
                "8":  "..............x.",
                "13": "x..........x....",
                "15": "x......x..x....."
            },
            "B": {
                "1":  "X......x..x...x.",
                "3":  "....X.......X..o",
                "7":  "x.o.x.o.x.o.x.o.",
                "12": "........x.......",
                "13": "x.....x.........",
                "14": "...........x..x.",
                "15": "x.....x...x....."
            }
        },
        "song": ["A", "A", "A", "B"]
    })").object();
    Song song;
    songFromJson(json, {}, &song, nullptr);
    return song;
}
