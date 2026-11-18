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

const char *const kNoteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// A drum row is one character per step: '.' is off, '1' to '4' a level,
// with 'o' for 2, 'x' for 3, and 'X' for 4 as easier names.
QChar levelChar(int level) {
    return level == 4 ? 'X' : level == 3 ? 'x' : QChar('0' + level);
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

QString drumRow(const std::array<Step, kSteps> &steps) {
    QString row;
    for (const Step &step : steps)
        row += step.kind == StepKind::Hit ? levelChar(step.level) : QChar('.');
    return row;
}

// The bass row is one word per step: a note ("C2", "Eb1"), '-' to hold the
// note before it, or '.' for silence.
QString bassRow(const std::array<Step, kSteps> &steps) {
    QStringList words;
    for (const Step &step : steps)
        words << (step.kind == StepKind::Hit ? noteName(step.note) : step.kind == StepKind::Hold ? "-" : ".");
    return words.join(' ');
}

QString relativeTo(const QString &baseDirectory, const QString &path) {
    if (baseDirectory.isEmpty())
        return path;
    const QString relative = QDir(baseDirectory).relativeFilePath(path);
    return relative.startsWith("..") ? path : relative;
}

} // namespace

bool Pattern::isEmpty() const {
    for (const auto &row : steps)
        for (const Step &step : row)
            if (step.kind != StepKind::Off)
                return false;
    return true;
}

bool Song::audible(int track) const {
    bool anySolo = false;
    for (const Track &t : tracks)
        anySolo = anySolo || t.solo;
    const Track &t = tracks[track];
    return !t.mute && (!anySolo || t.solo);
}

const QStringList &trackIds() {
    static const QStringList ids{"kick", "snare", "hat", "openhat", "clap", "rim", "cowbell", "bass"};
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

QString noteName(int note) {
    return QString("%1%2").arg(kNoteNames[note % 12]).arg(note / 12 - 1);
}

int parseNote(const QString &name) {
    static const QRegularExpression re("^([A-Ga-g])([#b]?)(-?[0-9])$");
    const auto match = re.match(name);
    if (!match.hasMatch())
        return -1;
    static const int offsets[] = {9, 11, 0, 2, 4, 5, 7}; // A to G
    int note = offsets[match.captured(1).at(0).toUpper().unicode() - 'A'];
    if (match.captured(2) == "#")
        ++note;
    else if (match.captured(2) == "b")
        --note;
    return note + (match.captured(3).toInt() + 1) * 12;
}

float levelGain(int level) {
    static const float gains[] = {0.f, 0.3f, 0.55f, 0.8f, 1.f};
    return gains[qBound(0, level, kLevels)];
}

QJsonObject songToJson(const Song &song, const QString &baseDirectory) {
    QJsonObject tracks;
    for (int t = 0; t < kTracks; ++t) {
        const Track &track = song.tracks[t];
        QJsonObject object{{"volume", std::round(track.volume * 100) / 100}};
        if (!track.sample.isEmpty())
            object.insert("sample", relativeTo(baseDirectory, track.sample));
        if (track.mute)
            object.insert("mute", true);
        if (track.solo)
            object.insert("solo", true);
        tracks.insert(trackIds()[t], object);
    }

    QJsonObject patterns;
    for (int p = 0; p < kPatterns; ++p) {
        const Pattern &pattern = song.patterns[p];
        if (pattern.isEmpty())
            continue;
        QJsonObject rows;
        for (int t = 0; t < kTracks; ++t) {
            const auto &steps = pattern.steps[t];
            const bool empty = std::all_of(steps.begin(), steps.end(),
                                           [](const Step &s) { return s.kind == StepKind::Off; });
            if (!empty)
                rows.insert(trackIds()[t], t == kBass ? bassRow(steps) : drumRow(steps));
        }
        patterns.insert(patternName(p), rows);
    }

    QJsonArray chain;
    for (int p : song.chain)
        chain.append(patternName(p));

    return {{"dole", 1},          {"bpm", song.bpm},   {"swing", song.swing},
            {"tracks", tracks},   {"patterns", patterns}, {"song", chain}};
}

bool songFromJson(const QJsonObject &json, const QString &baseDirectory, Song *out, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (json.value("dole").toInt() != 1)
        return fail("Not a dole song: \"dole\" should be 1.");

    Song song;
    song.bpm = json.value("bpm").toInt(song.bpm);
    if (song.bpm < kMinBpm || song.bpm > kMaxBpm)
        return fail(QString("\"bpm\" should be between %1 and %2.").arg(kMinBpm).arg(kMaxBpm));
    song.swing = json.value("swing").toInt(song.swing);
    if (song.swing < kMinSwing || song.swing > kMaxSwing)
        return fail(QString("\"swing\" should be between %1 and %2.").arg(kMinSwing).arg(kMaxSwing));

    const QJsonObject tracks = json.value("tracks").toObject();
    for (auto it = tracks.begin(); it != tracks.end(); ++it) {
        const int t = trackIds().indexOf(it.key());
        if (t < 0)
            return fail(QString("Unknown track \"%1\"; the tracks are %2.").arg(it.key(), trackIds().join(", ")));
        const QJsonObject object = it.value().toObject();
        Track &track = song.tracks[t];
        track.volume = qBound(0.0, object.value("volume").toDouble(track.volume), 1.0);
        track.mute = object.value("mute").toBool();
        track.solo = object.value("solo").toBool();
        const QString sample = object.value("sample").toString();
        if (!sample.isEmpty())
            track.sample = QDir::cleanPath(QDir(baseDirectory).absoluteFilePath(sample));
    }

    const QJsonObject patterns = json.value("patterns").toObject();
    for (auto it = patterns.begin(); it != patterns.end(); ++it) {
        const int p = patternIndex(it.key());
        if (p < 0)
            return fail(QString("Unknown pattern \"%1\"; patterns are A to H.").arg(it.key()));
        const QJsonObject rows = it.value().toObject();
        for (auto row = rows.begin(); row != rows.end(); ++row) {
            const int t = trackIds().indexOf(row.key());
            const QString where = QString("Pattern %1, %2: ").arg(it.key(), row.key());
            if (t < 0)
                return fail(where + "unknown track; the tracks are " + trackIds().join(", ") + ".");
            auto &steps = song.patterns[p].steps[t];
            const QString text = row.value().toString();
            if (t == kBass) {
                const QStringList words = text.split(' ', Qt::SkipEmptyParts);
                if (words.size() != kSteps)
                    return fail(where + QString("needs %1 steps, has %2.").arg(kSteps).arg(words.size()));
                for (int s = 0; s < kSteps; ++s) {
                    if (words[s] == ".")
                        continue;
                    if (words[s] == "-") {
                        steps[s].kind = StepKind::Hold;
                        continue;
                    }
                    const int note = parseNote(words[s]);
                    if (note < kMinNote || note > kMaxNote)
                        return fail(where + QString("\"%1\" is not a note from C1 to C4.").arg(words[s]));
                    steps[s].kind = StepKind::Hit;
                    steps[s].note = note;
                }
            } else {
                const QString compact = QString(text).remove(' ').remove('|');
                if (compact.size() != kSteps)
                    return fail(where + QString("needs %1 steps, has %2.").arg(kSteps).arg(compact.size()));
                for (int s = 0; s < kSteps; ++s) {
                    const int level = charLevel(compact[s]);
                    if (level < 0)
                        return fail(where + QString("\"%1\" is not a step; use . x X or 1 to 4.").arg(compact[s]));
                    if (level > 0) {
                        steps[s].kind = StepKind::Hit;
                        steps[s].level = level;
                    }
                }
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
    // Built through the file format, so it reads like a song file does.
    const QJsonObject json = QJsonDocument::fromJson(R"({
        "dole": 1, "bpm": 92, "swing": 54,
        "patterns": {
            "A": {
                "kick":    "X.....x...X.....",
                "snare":   "....X.......X...",
                "hat":     "x.o.x.o.x.o.x.o.",
                "openhat": "..............x.",
                "bass":    "C2 - - - . . C2 . D#2 - . F2 . . G1 -"
            },
            "B": {
                "kick":    "X.....x...X...x.",
                "snare":   "....X.......X...",
                "hat":     "x.o.x.o.x.o.x.o.",
                "clap":    "............x...",
                "bass":    "F1 - - - . . F1 . G#1 - . A#1 . . C2 -"
            }
        },
        "song": ["A", "A", "B", "A"]
    })").object();
    Song song;
    songFromJson(json, {}, &song, nullptr);
    return song;
}
