#include "song.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
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

// A pad by its number ("1" to "16") or its kit sound's name ("boom").
int padIndex(const QString &key) {
    bool number = false;
    const int n = key.toInt(&number);
    if (number)
        return n >= 1 && n <= kPads ? n - 1 : -1;
    return kitIds().indexOf(key);
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

bool Pattern::isEmpty() const {
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
        if (pad.group != defaults[p].group)
            object.insert("group", pad.group);
        if (pad.mute)
            object.insert("mute", true);
        if (pad.solo)
            object.insert("solo", true);
        pads.insert(QString::number(p + 1), object);
    }

    QJsonObject patterns;
    for (int p = 0; p < kPatterns; ++p) {
        const Pattern &pattern = song.patterns[p];
        if (pattern.isEmpty())
            continue;
        QJsonObject rows;
        for (int pad = 0; pad < kPads; ++pad) {
            QString row;
            for (const Step &step : pattern.steps[pad])
                row += levelChar(step.level);
            if (row != QString(kSteps, '.'))
                rows.insert(QString::number(pad + 1), row);
        }
        patterns.insert(patternName(p), rows);
    }

    QJsonArray chain;
    for (int p : song.chain)
        chain.append(patternName(p));

    return {{"dole", 2},           {"bpm", song.bpm},     {"swing", song.swing}, {"grit", gritName(song.grit)},
            {"vinyl", song.vinyl}, {"pads", pads},        {"patterns", patterns}, {"song", chain}};
}

bool songFromJson(const QJsonObject &input, const QString &baseDirectory, Song *out, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const int version = input.value("dole").toInt();
    if (version != 1 && version != 2)
        return fail("Not a dole song: \"dole\" should be 2.");
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
            return fail(QString("Unknown pad \"%1\"; pads are 1 to 16.").arg(it.key()));
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
        const QString sample = object.value("sample").toString();
        if (!sample.isEmpty())
            pad.sample = QDir::cleanPath(QDir(baseDirectory).absoluteFilePath(sample));
        pad.start = std::max(0.0, object.value("start").toDouble());
        pad.end = std::max(0.0, object.value("end").toDouble());
        if (pad.end > 0 && pad.end <= pad.start)
            return fail(QString("Pad %1: \"end\" should be after \"start\".").arg(p + 1));
    }

    const QJsonObject patterns = json.value("patterns").toObject();
    for (auto it = patterns.begin(); it != patterns.end(); ++it) {
        const int p = patternIndex(it.key());
        if (p < 0)
            return fail(QString("Unknown pattern \"%1\"; patterns are A to H.").arg(it.key()));
        const QJsonObject rows = it.value().toObject();
        for (auto row = rows.begin(); row != rows.end(); ++row) {
            const int pad = padIndex(row.key());
            const QString where = QString("Pattern %1, pad %2: ").arg(it.key(), row.key());
            if (pad < 0)
                return fail(where + "unknown pad; pads are 1 to 16.");
            const QString compact = row.value().toString().remove(' ').remove('|');
            if (compact.size() != kSteps)
                return fail(where + QString("needs %1 steps, has %2.").arg(kSteps).arg(compact.size()));
            for (int s = 0; s < kSteps; ++s) {
                const int level = charLevel(compact[s]);
                if (level < 0)
                    return fail(where + QString("\"%1\" is not a step; use . x X or 1 to 4.").arg(compact[s]));
                song.patterns[p].steps[pad][s].level = level;
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

    *out = song;
    return true;
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
