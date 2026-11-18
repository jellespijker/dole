#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

#include <QSignalSpy>
#include <QtEndian>
#include <cmath>

#include "backend.h"
#include "theme.h"
#include "translator.h"

namespace {

bool write(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

int lines(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().count('\n') : 0;
}

// A hyprctl that answers with response.json beside it and counts its calls.
QString fakeHyprctl(const QString &directory) {
    const QString command = directory + "/hyprctl";
    write(command, "#!/bin/bash\necho >> '" + directory.toUtf8() + "/calls'\ncat '" + directory.toUtf8()
                       + "/response.json'\n");
    QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return command;
}

QJsonObject readCatalog(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

// The placeholders in a text, sorted: %1 to %99, and the count (%n, %Ln).
QStringList placeholders(const QString &text) {
    static const QRegularExpression placeholder("%L?(?:[1-9][0-9]?|n)");
    QStringList found;
    for (auto it = placeholder.globalMatch(text); it.hasNext();)
        found << it.next().captured();
    found.sort();
    return found;
}

// Every text the sources hand to qsTr() or tr(), and whether a call passes it
// a count. It walks the code a token at a time, so a string never hides a
// comment or the other way round, C++'s adjacent literals join, and QML's
// single quotes count. A text that isn't a literal can't be in a catalog.
QMap<QString, bool> translatables(const QString &folder, QStringList *problems) {
    QMap<QString, bool> found;
    for (const QFileInfo &info : QDir(folder).entryInfoList({"*.qml", "*.js", "*.cpp", "*.h"}, QDir::Files)) {
        QFile file(info.filePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QString code = QString::fromUtf8(file.readAll());
        qsizetype i = 0;
        auto literal = [&](QString *text) {
            const QChar quote = code[i++];
            for (; i < code.size() && code[i] != quote; ++i) {
                if (code[i] == '\\' && i + 1 < code.size()) {
                    const QChar escaped = code[++i];
                    const int digits = escaped == 'u' ? 4 : escaped == 'x' ? 2 : 0;
                    bool hex = false;
                    const ushort unit = code.mid(i + 1, digits).toUShort(&hex, 16);
                    if (digits && hex) {
                        *text += QChar(unit);
                        i += digits;
                    } else {
                        *text += escaped == 'n' ? QChar('\n') : escaped == 't' ? QChar('\t') : escaped;
                    }
                } else {
                    *text += code[i];
                }
            }
            ++i;
        };
        auto skipSpace = [&] {
            while (i < code.size() && code[i].isSpace())
                ++i;
        };
        while (i < code.size()) {
            const QChar c = code[i];
            if (code.mid(i, 2) == "//") {
                i = code.indexOf('\n', i);
                if (i < 0)
                    break;
            } else if (code.mid(i, 2) == "/*") {
                i = code.indexOf("*/", i + 2);
                if (i < 0)
                    break;
                i += 2;
            } else if (c == '"' || c == '\'') {
                QString skipped;
                literal(&skipped);
            } else if (c.isLetter() || c == '_') {
                const qsizetype start = i;
                while (i < code.size() && (code[i].isLetterOrNumber() || code[i] == '_'))
                    ++i;
                const QString word = code.mid(start, i - start);
                skipSpace();
                if ((word != "qsTr" && word != "tr") || i >= code.size() || code[i] != '(')
                    continue;
                ++i;
                skipSpace();
                if (i >= code.size() || (code[i] != '"' && code[i] != '\'')) {
                    *problems << info.fileName() + ": not a literal: " + code.mid(start, 40);
                    continue;
                }
                QString text;
                do {
                    literal(&text);
                    skipSpace();
                } while (i < code.size() && code[i] == '"');
                // The arguments after the text, to the call's closing parenthesis.
                int depth = 0, commas = 0;
                while (i < code.size()) {
                    const QChar d = code[i];
                    if (d == '"' || d == '\'') {
                        QString skipped;
                        literal(&skipped);
                        continue;
                    }
                    if (d == '(' || d == '[' || d == '{')
                        ++depth;
                    else if ((d == ')' || d == ']' || d == '}') && depth-- == 0)
                        break;
                    else if (d == ',' && depth == 0)
                        ++commas;
                    ++i;
                }
                found[text] = found.value(text) || commas >= 2;
            } else {
                ++i;
            }
        }
    }
    return found;
}

} // namespace


#include "renderer.h"
#include "song.h"
#include "sounds.h"
#include "wav.h"

namespace {

// Records what the backend asks of the sound, and plays nothing.
class FakeEngine : public Engine {
public:
    Song song;
    std::array<SampleBuffer, kTracks> sounds;
    QStringList calls;

    void setSong(const Song &s) override { song = s; }
    void setSounds(const std::array<SampleBuffer, kTracks> &s) override { sounds = s; }
    void play(bool songMode, int pattern, int chainIndex) override {
        calls << QString("play %1 %2 %3").arg(songMode).arg(pattern).arg(chainIndex);
    }
    void cue(int pattern) override { calls << QString("cue %1").arg(pattern); }
    void stop() override { calls << "stop"; }
    void audition(int track, const Step &) override { calls << QString("audition %1").arg(track); }
};

// Answers every dialog with one path, or dismisses it when there is none.
class FakePicker : public FilePicker {
public:
    QString answer;
    QStringList calls;

    void open(Kind kind) override {
        calls << QString("open %1").arg(kind);
        if (answer.isEmpty())
            emit cancelled(kind);
        else
            emit opened(kind, QUrl::fromLocalFile(answer));
    }
    void save(Kind kind, const QUrl &suggested) override {
        calls << QString("save %1 %2").arg(kind).arg(QFileInfo(suggested.toLocalFile()).fileName());
        if (answer.isEmpty())
            emit cancelled(kind);
        else
            emit saveChosen(kind, QUrl::fromLocalFile(answer));
    }
};

Song songFrom(const QByteArray &json) {
    Song song;
    QString error;
    if (!songFromJson(QJsonDocument::fromJson(json).object(), {}, &song, &error))
        qWarning() << "bad test song:" << error;
    return song;
}

std::array<SampleBuffer, kTracks> builtIns() {
    std::array<SampleBuffer, kTracks> sounds;
    for (int t = 0; t < kTracks; ++t)
        sounds[t] = builtInSound(t);
    return sounds;
}

// Renders bars of one pattern looping, and every step as it plays.
QVector<float> renderBars(const Song &song, int bars, QVector<QPair<int, int>> *steps = nullptr) {
    Renderer renderer;
    renderer.setSong(song);
    renderer.setSounds(builtIns());
    renderer.play(!song.chain.isEmpty(), 0, 0);
    const int frames = int(std::ceil(bars * kSteps * renderer.stepFrames()));
    QVector<float> out(frames * 2);
    renderer.render(out.data(), frames, [&](int, int pattern, int step, int) {
        if (steps)
            *steps << qMakePair(pattern, step);
    });
    return out;
}

// The loudest sample between two frames.
float peak(const QVector<float> &audio, double from, double to) {
    float loudest = 0;
    for (qsizetype i = qsizetype(from) * 2; i < qsizetype(to) * 2 && i < audio.size(); ++i)
        loudest = std::max(loudest, std::abs(audio[i]));
    return loudest;
}

// Items a Repeater made aren't QObject children of the window, so walk the
// item tree for them.
QQuickItem *findItem(QQuickItem *root, const QString &name) {
    if (root->objectName() == name)
        return root;
    for (QQuickItem *child : root->childItems())
        if (QQuickItem *found = findItem(child, name))
            return found;
    return nullptr;
}

qsizetype firstSound(const QVector<float> &audio) {
    for (qsizetype i = 0; i < audio.size(); ++i)
        if (std::abs(audio[i]) > 1e-4f)
            return i / 2;
    return -1;
}

} // namespace

class DoleTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // The song and its file.

    void notesHaveNames() {
        QCOMPARE(parseNote("C2"), 36);
        QCOMPARE(parseNote("Eb2"), 39);
        QCOMPARE(parseNote("D#2"), 39);
        QCOMPARE(parseNote("a1"), 33);
        QCOMPARE(parseNote("H2"), -1);
        QCOMPARE(noteName(39), QString("D#2"));
        QCOMPARE(noteName(kMinNote), QString("C1"));
    }

    void songRoundTripsThroughItsFile() {
        QTemporaryDir dir;
        Song song = demoSong();
        song.tracks[kBass].sample = dir.filePath("samples/bass.wav");
        song.tracks[1].mute = true;
        song.patterns[2].steps[0][5] = Step{StepKind::Hit, 1, kDefaultNote};

        const QJsonObject json = songToJson(song, dir.path());
        QCOMPARE(json.value("tracks").toObject().value("bass").toObject().value("sample").toString(),
                 QString("samples/bass.wav"));
        QCOMPARE(json.value("patterns").toObject().value("C").toObject().value("kick").toString(),
                 QString(".....1.........."));

        QString error;
        QVERIFY(saveSong(dir.filePath("song.dole"), song, &error));
        Song loaded;
        QVERIFY2(loadSong(dir.filePath("song.dole"), &loaded, &error), qPrintable(error));
        QVERIFY(loaded == song);
    }

    void songFilesSayWhatsWrong_data() {
        QTest::addColumn<QByteArray>("json");
        QTest::addColumn<QString>("error");
        QTest::newRow("not dole") << QByteArray(R"({"bpm": 90})") << "Not a dole song";
        QTest::newRow("tempo") << QByteArray(R"({"dole": 1, "bpm": 400})") << "\"bpm\" should be between";
        QTest::newRow("track") << QByteArray(R"({"dole": 1, "patterns": {"A": {"tuba": "x..............."}}})")
                               << "unknown track";
        QTest::newRow("length") << QByteArray(R"({"dole": 1, "patterns": {"A": {"kick": "x...x...x...x"}}})")
                                << "needs 16 steps, has 13";
        QTest::newRow("step") << QByteArray(R"({"dole": 1, "patterns": {"A": {"kick": "x...y...x...x..."}}})")
                              << "\"y\" is not a step";
        QTest::newRow("note") << QByteArray(R"({"dole": 1, "patterns": {"A": {"bass": "C9 . . . . . . . . . . . . . . ."}}})")
                              << "\"C9\" is not a note";
        QTest::newRow("song") << QByteArray(R"({"dole": 1, "song": ["A", "Z"]})") << "\"Z\"";
    }

    void songFilesSayWhatsWrong() {
        QFETCH(QByteArray, json);
        QFETCH(QString, error);
        Song song;
        QString message;
        QVERIFY(!songFromJson(QJsonDocument::fromJson(json).object(), {}, &song, &message));
        QVERIFY2(message.contains(error), qPrintable(message));
    }

    // The sound.

    void builtInSoundsAreMade() {
        for (int t = 0; t < kBass; ++t) {
            const SampleBuffer sound = builtInSound(t);
            QVERIFY(sound);
            QVERIFY(sound->size() > 1000);
            QVERIFY(peak(*sound, 0, sound->size() / 2) > 0.5f);
        }
        QVERIFY(!builtInSound(kBass));
    }

    void stepsPlayOnTheirBeat() {
        // At 120 BPM a sixteenth is exactly 6000 frames.
        const Song song = songFrom(R"({"dole": 1, "bpm": 120, "patterns": {"A": {"rim": "x.......x......."}}})");
        const QVector<float> audio = renderBars(song, 1);
        QCOMPARE(firstSound(audio), qsizetype(0));
        QVERIFY(peak(audio, 0, 2000) > 0.1f);
        QCOMPARE(peak(audio, 4 * 6000, 8 * 6000), 0.f);
        QVERIFY(peak(audio, 8 * 6000, 8 * 6000 + 2000) > 0.1f);
    }

    void swingPushesEverySecondSixteenth() {
        const Song song =
            songFrom(R"({"dole": 1, "bpm": 120, "swing": 75, "patterns": {"A": {"rim": ".x.............."}}})");
        // Halfway to the next step: 1.5 steps in.
        QVERIFY(qAbs(firstSound(renderBars(song, 1)) - 9000) <= 1);
    }

    void accentsAreLouder() {
        const QVector<float> soft = renderBars(songFrom(R"({"dole": 1, "patterns": {"A": {"rim": "1..............."}}})"), 1);
        const QVector<float> accent = renderBars(songFrom(R"({"dole": 1, "patterns": {"A": {"rim": "X..............."}}})"), 1);
        QVERIFY(peak(accent, 0, 4000) > 2 * peak(soft, 0, 4000));
    }

    void mutedAndUnsoloedTracksAreSilent() {
        Song song = songFrom(R"({"dole": 1, "patterns": {"A": {"rim": "x...............", "clap": "x..............."}}})");
        song.tracks[5].mute = true; // rim
        song.tracks[4].mute = true; // clap
        QCOMPARE(peak(renderBars(song, 1), 0, 6000), 0.f);
        song.tracks[4].mute = false;
        song.tracks[5].mute = false;
        song.tracks[5].solo = true;
        QVERIFY(song.audible(5));
        QVERIFY(!song.audible(4));
    }

    void bassHoldsThroughHolds() {
        const Song held = songFrom(R"({"dole": 1, "bpm": 120, "patterns": {"A": {"bass": "C2 - - - . . . . . . . . . . . ."}}})");
        const Song short_ = songFrom(R"({"dole": 1, "bpm": 120, "patterns": {"A": {"bass": "C2 . . . . . . . . . . . . . . ."}}})");
        QVERIFY(peak(renderBars(held, 1), 3 * 6000, 4 * 6000) > 0.1f);
        QVERIFY(peak(renderBars(short_, 1), 3 * 6000, 4 * 6000) < 0.01f);
        // And it lets go once the holds end.
        QVERIFY(peak(renderBars(held, 1), 8 * 6000, 16 * 6000) < 0.01f);
    }

    void songPlaysItsChain() {
        const Song song = songFrom(R"({"dole": 1, "patterns": {"A": {"rim": "x..............."},
                                                               "B": {"clap": "x..............."}},
                                         "song": ["A", "B", "B"]})");
        QVector<QPair<int, int>> steps;
        renderBars(song, 4, &steps);
        QVector<int> bars;
        for (const auto &[pattern, step] : steps)
            if (step == 0)
                bars << pattern;
        // Round again at the end.
        QCOMPARE(bars, QVector<int>({0, 1, 1, 0}));
    }

    void patternChangesWaitForTheBar() {
        Renderer renderer;
        renderer.setSong(demoSong());
        renderer.setSounds(builtIns());
        renderer.play(false, 0, 0);
        QVector<int> patterns;
        const auto record = [&](int, int pattern, int step, int) {
            if (step == 0 || step == 8)
                patterns << pattern;
        };
        const int bar = int(std::ceil(kSteps * renderer.stepFrames()));
        QVector<float> out(bar * 2);
        renderer.render(out.data(), bar / 4, record);
        renderer.cue(1);
        renderer.render(out.data(), bar, record);
        renderer.render(out.data(), bar / 2, record);
        QCOMPARE(patterns, QVector<int>({0, 0, 1, 1}));
    }

    void exportRendersTheSongAndItsTail() {
        const Song song = songFrom(R"({"dole": 1, "bpm": 120, "patterns": {"A": {"kick": "x...............",
                                                                                   "openhat": "..............x."}},
                                         "song": ["A", "A"]})");
        const QVector<float> audio = renderSong(song, builtIns(), 0);
        const qsizetype bars = 2 * 16 * 6000;
        // The whole song, the open hat ringing past its end, then trimmed.
        QVERIFY(audio.size() / 2 > bars);
        QVERIFY(audio.size() / 2 < bars + kRate);
        QVERIFY(peak(audio, bars - 100, bars) > 0.01f);
    }

    void wavFilesAreSixteenBitStereo() {
        QTemporaryDir dir;
        const QVector<float> audio{0.f, 0.5f, -1.f, 2.f};
        QString error;
        QVERIFY(writeWav(dir.filePath("a.wav"), audio, &error));
        QFile file(dir.filePath("a.wav"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray data = file.readAll();
        QCOMPARE(data.size(), 44 + 8);
        QVERIFY(data.startsWith("RIFF"));
        QCOMPARE(data.mid(8, 8), QByteArray("WAVEfmt "));
        QCOMPARE(qFromLittleEndian<quint16>(data.constData() + 22), quint16(2));
        QCOMPARE(qFromLittleEndian<quint32>(data.constData() + 24), quint32(kRate));
        QCOMPARE(qFromLittleEndian<qint16>(data.constData() + 46), qint16(16384));
        QCOMPARE(qFromLittleEndian<qint16>(data.constData() + 48), qint16(-32767));
        QCOMPARE(qFromLittleEndian<qint16>(data.constData() + 50), qint16(32767));
        QVERIFY(!writeWav(dir.filePath("missing/a.wav"), audio, &error));
    }

    void samplesDecodeThroughFfmpeg() {
        QTemporaryDir dir;
        QString error;
        QVERIFY(writeWav(dir.filePath("rim.wav"), *builtInSound(5), &error));
        const SampleBuffer sound = decodeSample(dir.filePath("rim.wav"), &error);
        QVERIFY2(sound, qPrintable(error));
        QCOMPARE(sound->size(), builtInSound(5)->size());

        QVERIFY(write(dir.filePath("noise.wav"), "not audio at all"));
        QVERIFY(!decodeSample(dir.filePath("noise.wav"), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(error != kNoFfmpeg);
    }

    // The backend.

    void editsUndoAndKnowWhatIsUnsaved() {
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);
        QVERIFY(!backend.dirty());
        QVERIFY(engine.song == backend.song());

        backend.toggleStep(5, 3);
        QVERIFY(backend.dirty());
        QCOMPARE(engine.song.patterns[0].steps[5][3].kind, StepKind::Hit);
        QCOMPARE(engine.calls.last(), QString("audition 5"));
        QCOMPARE(backend.grid()[5].toList()[3].toMap().value("kind").toInt(), 1);

        backend.undo();
        QVERIFY(!backend.dirty());
        backend.redo();
        QVERIFY(backend.dirty());
        backend.toggleStep(5, 3);
        // Back to what was saved is clean, however it got there.
        QVERIFY(!backend.dirty());

        // Nudging the tempo undoes as one step.
        const int bpm = backend.bpm();
        for (int i = 0; i < 5; ++i)
            backend.changeBpm(1);
        QCOMPARE(backend.bpm(), bpm + 5);
        backend.undo();
        QCOMPARE(backend.bpm(), bpm);
        backend.changeBpm(1000);
        QCOMPARE(backend.bpm(), kMaxBpm);
    }

    void bassStepsHoldAndChangePitch() {
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);
        backend.newSong();
        backend.toggleStep(kBass, 0);
        backend.changeStep(kBass, 0, 3);
        QCOMPARE(backend.song().patterns[0].steps[kBass][0].note, kDefaultNote + 3);
        // The next note starts where the last one was set.
        backend.toggleStep(kBass, 4);
        QCOMPARE(backend.song().patterns[0].steps[kBass][4].note, kDefaultNote + 3);
        backend.holdStep(kBass, 1);
        QCOMPARE(backend.song().patterns[0].steps[kBass][1].kind, StepKind::Hold);
        backend.changeStep(kBass, 0, 100);
        QCOMPARE(backend.song().patterns[0].steps[kBass][0].note, kMaxNote);
        // Drums don't hold.
        backend.holdStep(0, 1);
        QCOMPARE(backend.song().patterns[0].steps[0][1].kind, StepKind::Off);
    }

    void patternsCopyPasteAndChain() {
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);
        backend.copyPattern();
        backend.selectPattern(5);
        QCOMPARE(backend.editPattern(), 5);
        backend.pastePattern();
        QVERIFY(backend.song().patterns[5] == backend.song().patterns[0]);
        QCOMPARE(backend.message().value("kind").toString(), QString("pasted"));
        QVERIFY(backend.usedPatterns()[5].toBool());

        const int length = int(backend.song().chain.size());
        backend.chainInsert(length, 5);
        QCOMPARE(backend.song().chain.last(), 5);
        backend.chainSet(0, 2);
        QCOMPARE(backend.song().chain.first(), 2);
        backend.chainRemove(0);
        QCOMPARE(backend.song().chain.size(), length);
        backend.clearPattern();
        QVERIFY(backend.song().patterns[5].isEmpty());
    }

    void playingFollowsTheSong() {
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);
        backend.togglePlay();
        QVERIFY(backend.playing());
        QCOMPARE(engine.calls.last(), QString("play 0 0 0"));
        // In pattern mode another pattern waits for the bar.
        backend.selectPattern(1);
        QCOMPARE(engine.calls.last(), QString("cue 1"));
        backend.stop();
        QCOMPARE(engine.calls.last(), QString("stop"));
        QCOMPARE(backend.playStep(), -1);

        backend.toggleSongMode();
        backend.togglePlay(2);
        QCOMPARE(engine.calls.last(), QString("play 1 1 2"));
        emit engine.positionChanged(2, 1, 0);
        emit engine.positionChanged(3, 0, 4);
        QCOMPARE(backend.playStep(), 4);
        QCOMPARE(backend.playChainIndex(), 3);
        QCOMPARE(backend.editPattern(), 0);
        backend.togglePlay();
        // A step already on its way when it stopped.
        emit engine.positionChanged(0, 1, 5);
        QCOMPARE(backend.playStep(), -1);
    }

    void songsSaveAndOpen() {
        QTemporaryDir dir;
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);
        QSignalSpy saved(&backend, &Backend::saved);
        QSignalSpy aborted(&backend, &Backend::saveAborted);

        // Untitled, so it asks where; dismissing the dialog saves nothing.
        backend.toggleStep(0, 1);
        backend.save();
        QCOMPARE(picker.calls.last(), QString("save 0 Untitled.dole"));
        QCOMPARE(aborted.count(), 1);
        QVERIFY(backend.dirty());

        picker.answer = dir.filePath("groove");
        backend.save();
        QCOMPARE(saved.count(), 1);
        QVERIFY(!backend.dirty());
        QCOMPARE(backend.fileName(), QString("groove"));
        QVERIFY(QFile::exists(dir.filePath("groove.dole")));

        Backend other(&engine, &picker);
        QVERIFY(other.openPath(dir.filePath("groove.dole")));
        QVERIFY(other.song() == backend.song());
        QVERIFY(!other.dirty());

        QVERIFY(!other.openPath(dir.filePath("nothing.dole")));
        QCOMPARE(other.message().value("kind").toString(), QString("openFailed"));
        QVERIFY(!other.saveTo(dir.filePath("no/such/folder.dole")));
        QCOMPARE(other.message().value("kind").toString(), QString("saveFailed"));
    }

    void samplesLoadOntoTracks() {
        QTemporaryDir dir;
        QString error;
        QVERIFY(writeWav(dir.filePath("hit.wav"), *builtInSound(1), &error));
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);

        picker.answer = dir.filePath("hit.wav");
        backend.chooseSample(0);
        QCOMPARE(backend.tracks()[0].toMap().value("state").toString(), QString("loading"));
        QVERIFY(!engine.sounds[0]);
        QTRY_COMPARE(backend.tracks()[0].toMap().value("state").toString(), QString("ready"));
        QCOMPARE(backend.tracks()[0].toMap().value("sample").toString(), QString("hit.wav"));
        QVERIFY(engine.sounds[0]);
        QCOMPARE(engine.sounds[0]->size(), builtInSound(1)->size());

        backend.resetSample(0);
        QVERIFY(engine.sounds[0] == builtInSound(0));
        // Undo brings the sample back at once, from what was loaded.
        backend.undo();
        QCOMPARE(backend.tracks()[0].toMap().value("state").toString(), QString("ready"));

        backend.setSample(1, dir.filePath("gone.wav"));
        QTRY_COMPARE(backend.tracks()[1].toMap().value("state").toString(), QString("failed"));
        QCOMPARE(backend.message().value("kind").toString(), QString("sampleFailed"));
        QCOMPARE(backend.message().value("arg").toString(), QString("gone.wav"));
    }

    void exportWritesTheSong() {
        QTemporaryDir dir;
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);
        picker.answer = dir.filePath("out");
        QSignalSpy exported(&backend, &Backend::exported);
        backend.exportAudio();
        QVERIFY(backend.exporting());
        QTRY_VERIFY(!backend.exporting());
        QCOMPARE(exported.count(), 1);
        QCOMPARE(backend.message().value("arg").toString(), QString("out.wav"));
        // The demo song: four bars at 92 BPM, and a little tail.
        const qint64 bytes = QFileInfo(dir.filePath("out.wav")).size();
        const double seconds = (bytes - 44) / 4.0 / kRate;
        QVERIFY2(seconds > 4 * 4 * 60.0 / 92 && seconds < 4 * 4 * 60.0 / 92 + 1, qPrintable(QString::number(seconds)));
    }

    void themeFallsBack() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QColor("black"));
        QVERIFY(!theme.light());
        QCOMPARE(theme.radius(), 0);
    }

    void themeReadsThePalette() {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("theme"));
        QVERIFY(write(dir.path() + "/theme/colors.toml", "mode = \"light\"\n"
                                                         "theme_type = \"dark\"\n"
                                                         "accent = \"#112233\"\n"
                                                         "bg = #fafafa # paper\n"
                                                         "foreground = '#202020'\n"
                                                         "color1 = \"#aa0000\"\n"));

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#112233"));
        QCOMPARE(theme.accentForeground(), QColor("white"));
        QCOMPARE(theme.background(), QColor("#fafafa"));
        QCOMPARE(theme.foreground(), QColor("#202020"));
        QCOMPARE(theme.colors().value("red").value<QColor>(), QColor("#aa0000"));
        QVERIFY(theme.light());
    }

    void themeAsksHyprlandAgainOnlyWhenItReloads() {
        // The fake Hyprland lives outside the theme directory, whose changes
        // the Theme also follows.
        QTemporaryDir dir, hypr;
        const QString command = fakeHyprctl(hypr.path());
        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":4}"));
        QLocalServer events;
        QVERIFY(events.listen(hypr.filePath("events.sock")));

        // Once on start, and once more when the event socket connects.
        Theme theme(dir.path(), nullptr, command, events.fullServerName());
        QTRY_VERIFY(events.hasPendingConnections());
        QLocalSocket *hyprland = events.nextPendingConnection();
        QTRY_COMPARE(lines(hypr.filePath("calls")), 2);
        QTRY_COMPARE(theme.radius(), 4);

        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":10}"));
        QTest::qWait(300);
        QCOMPARE(lines(hypr.filePath("calls")), 2);
        // Events arrive in pieces; only a whole configreloaded line counts.
        hyprland->write("activewindow>>kitty,~\nconfigrel");
        hyprland->flush();
        QTest::qWait(100);
        QCOMPARE(lines(hypr.filePath("calls")), 2);
        hyprland->write("oaded>>\n");
        QTRY_COMPARE(theme.radius(), 10);
        QCOMPARE(lines(hypr.filePath("calls")), 3);
    }

    void themeReachesHyprlandOnceItListens() {
        QTemporaryDir dir, hypr;
        const QString command = fakeHyprctl(hypr.path());
        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":4}"));
        Theme theme(dir.path(), nullptr, command, hypr.filePath("events.sock"));
        QTRY_COMPARE(theme.radius(), 4);

        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":8}"));
        QLocalServer events;
        QVERIFY(events.listen(hypr.filePath("events.sock")));
        QTRY_VERIFY_WITH_TIMEOUT(events.hasPendingConnections(), 5000);
        QTRY_COMPARE(theme.radius(), 8);
    }

    void translatorFallsBackToEnglish() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile() {
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/language", "pt-BR\n"));
        QVERIFY(write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})"));
        QVERIFY(write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})"));

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorSeesAFirstPick() {
        // Before the first pick there is no ~/.config/omarchy to watch.
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/da.json", R"({"Save": "Gem"})"));
        Translator translator(dir.path() + "/omarchy/language", dir.path());
        QVERIFY(QDir(dir.path()).mkpath("omarchy"));
        QVERIFY(write(dir.path() + "/omarchy/language", "da\n"));
        QTRY_COMPARE(translator.translate("Main", "Save"), QString("Gem"));
    }

    void translatorPicksPluralForms() {
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/language", "ru\n"));
        QVERIFY(write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"},
                                                   "%n files": {"one": "%n file", "other": "%n files"}})"));
        QVERIFY(write(dir.path() + "/ru.json", R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})"));

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 22), QString("%n клипа"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
        // An untranslated counted string falls back to English, with English rules.
        QCOMPARE(translator.translate("Main", "%n files", nullptr, 21), QString("%n files"));
    }

    void englishCatalogListsEveryString() {
        // en.json is what translators work from: every text the sources
        // translate is in it, as itself or, when counted, as its English
        // plural forms, and nothing else is.
        const QString sources = QFileInfo(QFINDTESTDATA("../src/Main.qml")).absolutePath();
        QStringList problems;
        const QMap<QString, bool> used = translatables(sources, &problems);
        const QJsonObject english = readCatalog(sources + "/i18n/en.json");
        for (auto it = used.begin(); it != used.end(); ++it) {
            const QJsonValue value = english.value(it.key());
            if (value.isUndefined())
                problems << "missing from en.json: " + it.key();
            else if (it.value() && (value.toObject().keys() != QStringList{"one", "other"}
                                    || value.toObject().value("other").toString() != it.key()))
                problems << "counted, so needs its one and other forms: " + it.key();
            else if (!it.value() && value.toString() != it.key())
                problems << "should be itself: " + it.key();
        }
        for (const QString &key : english.keys())
            if (!used.contains(key))
                problems << "in en.json but no source translates it: " + key;
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void catalogsMatchTheEnglishOne() {
        // A catalog may lack a string (it shows in English meanwhile), but what
        // it has translates something in en.json, with the same placeholders,
        // and a counted string has every form its language uses.
        const QString folder = QFileInfo(QFINDTESTDATA("../src/i18n/en.json")).absolutePath();
        const QJsonObject english = readCatalog(folder + "/en.json");
        QStringList problems;
        for (const QFileInfo &file : QDir(folder).entryInfoList({"*.json"}, QDir::Files)) {
            const QString code = file.completeBaseName();
            const QJsonObject catalog = readCatalog(file.filePath());
            if (catalog.isEmpty())
                problems << code + ": not a JSON object, or empty";
            // Some languages reach a form only at a million ("many" in French).
            QSet<QString> categories{"other", Translator::pluralCategory(code, 1000000)};
            for (long long n = 0; n <= 1000; ++n)
                categories << Translator::pluralCategory(code, n);
            for (auto it = catalog.begin(); it != catalog.end(); ++it) {
                const QString where = code + ": " + it.key() + ": ";
                const QJsonValue source = english.value(it.key());
                if (source.isUndefined()) {
                    problems << where + "not in en.json";
                } else if (source.isString()) {
                    if (!it.value().isString() || it.value().toString().isEmpty())
                        problems << where + "needs a translation";
                    else if (placeholders(it.value().toString()) != placeholders(it.key()))
                        problems << where + "has other placeholders than the English";
                } else {
                    const QJsonObject forms = it.value().toObject();
                    const QStringList wanted = placeholders(source.toObject().value("other").toString());
                    for (const QString &category : categories)
                        if (forms.value(category).toString().isEmpty())
                            problems << where + "needs its " + category + " form";
                    // A form other than "other" may say its number in words.
                    for (auto form = forms.begin(); form != forms.end(); ++form) {
                        const QStringList has = placeholders(form.value().toString());
                        QStringList needed = wanted;
                        if (form.key() != "other")
                            needed.removeIf([&](const QString &p) { return p.endsWith('n') && !has.contains(p); });
                        if (has != needed)
                            problems << where + form.key() + " has other placeholders than the English";
                    }
                }
            }
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void windowLoadsAndAnswersTheKeyboard() {
        // A temp language and theme, so the test never depends on the desktop's.
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language");
        QCoreApplication::installTranslator(&translator);
        Theme theme(dir.path());
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);

        QQmlApplicationEngine qml;
        QStringList warnings;
        connect(&qml, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        qml.rootContext()->setContextProperty("translator", &translator);
        qml.rootContext()->setContextProperty("theme", &theme);
        qml.rootContext()->setContextProperty("backend", &backend);
        qml.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!qml.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(qml.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));

        auto *status = window->findChild<QObject *>("status");
        QVERIFY(status);
        QCOMPARE(status->property("text").toString(), QString("Press ? for the shortcuts"));

        // Space plays the pattern, and stops it.
        QTest::keyClick(window, Qt::Key_Space);
        QVERIFY(backend.playing());
        QCOMPARE(engine.calls.last(), QString("play 0 0 0"));
        emit engine.positionChanged(0, 0, 3);
        QCOMPARE(backend.playStep(), 3);
        QTest::keyClick(window, Qt::Key_Space);
        QVERIFY(!backend.playing());

        // Move to the rim's third step and turn it on, then make it louder.
        for (int i = 0; i < 5; ++i)
            QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Right);
        QTest::keyClick(window, Qt::Key_L);
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(backend.song().patterns[0].steps[5][2].kind, StepKind::Hit);
        QTest::keyClick(window, Qt::Key_Up, Qt::ShiftModifier);
        QCOMPARE(backend.song().patterns[0].steps[5][2].level, 4);
        auto *cell = findItem(window->contentItem(), "cell5_2");
        QVERIFY(cell);
        QCOMPARE(cell->property("color").value<QColor>().name(), theme.accent().name());
        QVERIFY(window->title().contains("•"));
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(backend.song().patterns[0].steps[5][2].level, 3);

        // Clicking a step turns it on too.
        auto *other = findItem(window->contentItem(), "cell1_9");
        QVERIFY(other);
        QTest::mouseClick(window, Qt::LeftButton, {}, other->mapToScene(QPointF(4, 4)).toPoint());
        QCOMPARE(backend.song().patterns[0].steps[1][9].kind, StepKind::Hit);

        // Pattern keys pick the pattern to edit.
        QTest::keyClick(window, Qt::Key_3);
        QCOMPARE(backend.editPattern(), 2);

        // In the song, they write the chain.
        const int length = int(backend.song().chain.size());
        QTest::keyClick(window, Qt::Key_Tab);
        for (int i = 0; i < length; ++i)
            QTest::keyClick(window, Qt::Key_Right);
        QTest::keyClick(window, Qt::Key_2);
        QTest::keyClick(window, Qt::Key_2);
        QCOMPARE(backend.song().chain.size(), length + 2);
        QCOMPARE(backend.song().chain.last(), 1);
        QTest::keyClick(window, Qt::Key_Backspace);
        QCOMPARE(backend.song().chain.size(), length + 1);
        QVERIFY(window->findChild<QObject *>("songLength"));
        QCOMPARE(window->findChild<QObject *>("songLength")->property("text").toString(),
                 QString("%1 bars").arg(length + 1));
        QTest::keyClick(window, Qt::Key_Escape);

        // Tempo, swing, mute.
        QTest::keyClick(window, Qt::Key_BracketRight);
        QVERIFY(window->findChild<QObject *>("bpm"));
        QCOMPARE(window->findChild<QObject *>("bpm")->property("text").toString(), QString::number(93));
        QTest::keyClick(window, Qt::Key_Period);
        QCOMPARE(backend.swing(), 55);
        QTest::keyClick(window, Qt::Key_M);
        QVERIFY(backend.song().tracks[1].mute);

        auto *help = window->findChild<QObject *>("help");
        QVERIFY(help);
        QTest::keyClick(window, '?');
        QTRY_VERIFY(help->property("opened").toBool());
        // A Material popup animates out: `opened` drops at once, `visible` after.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!help->property("visible").toBool());

        // Quitting with unsaved work asks first, and Escape keeps everything.
        auto *confirm = window->findChild<QObject *>("confirm");
        QVERIFY(confirm);
        QTest::keyClick(window, Qt::Key_Q);
        QTRY_VERIFY(confirm->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!confirm->property("visible").toBool());
        QVERIFY(window->isVisible());
        QVERIFY(backend.dirty());

        // Saving from the question saves, then quits.
        QTemporaryDir songs;
        picker.answer = songs.filePath("keep.dole");
        QTest::keyClick(window, Qt::Key_Q);
        QTRY_VERIFY(confirm->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!window->isVisible());
        QVERIFY(QFile::exists(songs.filePath("keep.dole")));
        QVERIFY(!backend.dirty());

        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QCoreApplication::removeTranslator(&translator);
    }

    void windowLoadsInEveryShippedLanguage_data() {
        QTest::addColumn<QString>("language");
        for (const char *language : {"da", "de", "nl", "ar"})
            QTest::newRow(language) << QString(language);
    }

    void windowLoadsInEveryShippedLanguage() {
        QFETCH(QString, language);
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("language"), language.toUtf8() + "\n"));
        Translator translator(dir.filePath("language"));
        QCoreApplication::installTranslator(&translator);
        Theme theme(dir.path());
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);

        QQmlApplicationEngine qml;
        QStringList warnings;
        connect(&qml, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        qml.rootContext()->setContextProperty("translator", &translator);
        qml.rootContext()->setContextProperty("theme", &theme);
        qml.rootContext()->setContextProperty("backend", &backend);
        qml.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!qml.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(qml.rootObjects().first());
        QVERIFY(QTest::qWaitForWindowExposed(window));
        if (language != "ar")
            QVERIFY(window->findChild<QObject *>("status")->property("text").toString() != "Press ? for the shortcuts");
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QGuiApplication::setLayoutDirection(Qt::LeftToRight);
        QLocale::setDefault(QLocale(QLocale::English));
        QCoreApplication::removeTranslator(&translator);
    }
};

QTEST_MAIN(DoleTests)
#include "dole_tests.moc"
