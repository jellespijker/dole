#include <QDir>
#include <QFile>
#include <QJsonArray>
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
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>

#include <QSignalSpy>
#include <QtEndian>
#include <cmath>

#include "backend.h"
#include "jam.h"
#include "remote.h"
#include "theme.h"
#include "translator.h"

namespace {

bool write(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

// How many lines of a file mention a word.
int mentions(const QString &path, const QByteArray &word) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().count(word) : 0;
}

// A hyprctl that answers with response.json beside it and logs what it was
// asked, a line a call.
QString fakeHyprctl(const QString &directory) {
    const QString command = directory + "/hyprctl";
    write(command, "#!/bin/bash\necho \"$*\" >> '" + directory.toUtf8() + "/calls'\ncat '" + directory.toUtf8()
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


#include "builtins.h"
#include "chop.h"
#include "plugin.h"
#include "rack.h"
#include "renderer.h"
#include "song.h"
#include "sounds.h"
#include "wav.h"

namespace {

// Records what the backend asks of the sound, and plays nothing.
class FakeEngine : public Engine {
public:
    Song song;
    std::array<SampleBuffer, kPads> sounds;
    QStringList calls;

    void setSong(const Song &s) override { song = s; }
    void setSounds(const std::array<SampleBuffer, kPads> &s) override { sounds = s; }
    void play(bool songMode, int pattern, int chainIndex) override {
        calls << QString("play %1 %2 %3").arg(songMode).arg(pattern).arg(chainIndex);
    }
    void cue(int pattern) override { calls << QString("cue %1").arg(pattern); }
    void stop() override { calls << "stop"; }
    void hit(int pad, int level) override { calls << QString("hit %1 %2").arg(pad).arg(level); }
    void hold(int pad, bool held, int) override { calls << QString("hold %1 %2").arg(pad).arg(held); }
    void release(int pad) override { calls << QString("release %1").arg(pad); }
    void setRepeat(int division) override { calls << QString("repeat %1").arg(division); }
    void suppress(int pad, int pattern, int step) override {
        calls << QString("suppress %1 %2 %3").arg(pad).arg(pattern).arg(step);
    }
    void preview(const SampleBuffer &sound, double) override {
        calls << QString("preview %1").arg(sound ? sound->size() / 2 : 0);
    }
    void setRack(const Rack &r) override { rack = r; }
    void locked(const std::function<void()> &work) override { work(); }
    void setTimeline(const Timeline &t) override { timeline = t; }
    Rack rack;
    Timeline timeline;
};

// A tailscale that says this machine is one of two of the user's devices,
// each on its own loopback address, with a third that's someone else's.
QString fakeTailscale(const QString &directory, const QString &self, const QString &selfAddress, const QString &peer,
                      const QString &peerAddress) {
    const QString path = directory + "/tailscale-" + self;
    const QByteArray status = QJsonDocument(QJsonObject{
        {"BackendState", "Running"},
        {"Self", QJsonObject{{"HostName", self}, {"UserID", 7}, {"Online", true},
                             {"TailscaleIPs", QJsonArray{selfAddress, "fd7a::1"}}}},
        {"Peer", QJsonObject{
            {"a", QJsonObject{{"HostName", peer}, {"UserID", 7}, {"Online", true}, {"TailscaleIPs", QJsonArray{peerAddress}}}},
            {"b", QJsonObject{{"HostName", "stranger"}, {"UserID", 9}, {"Online", true},
                              {"TailscaleIPs", QJsonArray{"127.0.0.3"}}}}}}}).toJson(QJsonDocument::Compact);
    write(path, "#!/bin/sh\ncat <<'EOF'\n" + status + "\nEOF\n");
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return path;
}

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

std::array<SampleBuffer, kPads> kit() {
    std::array<SampleBuffer, kPads> sounds;
    for (int p = 0; p < kPads; ++p)
        sounds[p] = kitSound(p);
    return sounds;
}

// Renders bars of the song's first pattern (or its chain), and every step as it plays.
QVector<float> renderBars(const Song &song, int bars, QVector<QPair<int, int>> *steps = nullptr) {
    Renderer renderer;
    renderer.setSong(song);
    renderer.setSounds(kit());
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

qsizetype firstSound(const QVector<float> &audio, qsizetype from = 0) {
    for (qsizetype i = from * 2; i < audio.size(); ++i)
        if (std::abs(audio[i]) > 1e-4f)
            return i / 2;
    return -1;
}

qsizetype lastSound(const QVector<float> &audio) {
    for (qsizetype i = audio.size() - 1; i >= 0; --i)
        if (std::abs(audio[i]) > 1e-3f)
            return i / 2;
    return -1;
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

// A loop with a click every half second: something to chop.
QString writeClicks(const QString &path, int clicks) {
    QVector<float> audio(clicks * kRate, 0.f); // half a second each, stereo
    for (int c = 0; c < clicks; ++c)
        for (int i = 0; i < 2000; ++i)
            audio[(c * kRate / 2 + i) * 2] = audio[(c * kRate / 2 + i) * 2 + 1] = 0.8f * std::exp(-i / 300.f);
    QString error;
    writeWav(path, audio, &error);
    return path;
}

} // namespace

class DoleTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QQuickStyle::setStyle("Material");
        // The tests' own plugins, and none of the machine's.
        qputenv("CLAP_PATH", TEST_PLUGINS "/clap");
        qputenv("LV2_PATH", TEST_PLUGINS "/lv2");
    }

    // Plugins.

    void builtinEffectsMakeNoise() {
        const Transport transport{120, true, 0};
        auto run = [&](const QString &id, const QMap<QString, double> &params, std::function<float(int)> input, int frames) {
            QString error;
            auto effect = createProcessor(PluginSlot{id, params, {}, false}, &error);
            QVector<float> left(frames), right(frames);
            for (int i = 0; i < frames; ++i)
                left[i] = right[i] = input(i);
            for (int done = 0; done < frames; done += kMaxBlock) {
                const int n = std::min(kMaxBlock, frames - done);
                Transport t = transport;
                t.position = done / 6000.0;
                effect->process(left.data() + done, right.data() + done, n, nullptr, 0, t);
            }
            return left;
        };
        auto sine = [](int i) { return float(0.1 * std::sin(i * 2 * M_PI * 220 / kRate)); };
        auto impulse = [](int i) { return i == 0 ? 1.f : 0.f; };
        auto loudest = [](const QVector<float> &x, int from, int to) {
            float m = 0;
            for (int i = from; i < to && i < x.size(); ++i)
                m = std::max(m, std::abs(x[i]));
            return m;
        };

        // Overdrive turns a quiet sine into a loud, square one.
        QVERIFY(loudest(run("overdrive", {{"drive", 1}}, sine, 4800), 2400, 4800) > 0.4f);
        // Two bits leave hardly any levels.
        const QVector<float> crushed = run("bitcrush", {{"bits", 2}, {"rate", 48000}}, sine, 4800);
        QSet<float> levels(crushed.begin(), crushed.end());
        QVERIFY2(levels.size() <= 3, qPrintable(QString::number(levels.size())));
        // The comb rings on after a click, the delay echoes it.
        QVERIFY(loudest(run("comb", {{"freq", 100}, {"feedback", 1}, {"mix", 1}}, impulse, 24000), 9600, 24000) > 0.01f);
        const QVector<float> echoed = run("delay", {{"time", 0.25}, {"mix", 1}, {"feedback", 0}}, impulse, 24000);
        QVERIFY(loudest(echoed, 100, 11000) < 1e-3f);
        QVERIFY(loudest(echoed, 11990, 12010) > 0.1f);
        // The ring modulator flips a steady signal's sign.
        const QVector<float> rung = run("ringmod", {{"freq", 1000}, {"shape", 1}}, [](int) { return 0.5f; }, 480);
        QVERIFY(*std::min_element(rung.begin(), rung.end()) < -0.4f);
        // The stutter cuts every sixteenth in half, in time with the song.
        const QVector<float> cut = run("stutter", {{"rate", 2}, {"duty", 0.5}}, [](int) { return 1.f; }, 12000);
        QVERIFY(cut[1500] > 0.99f);
        QVERIFY(cut[4500] < 0.01f);
        QVERIFY(cut[7500] > 0.99f);
    }

    void pluginsAreFound() {
        const QVector<PluginInfo> found = scanPlugins();
        QStringList ids;
        for (const PluginInfo &info : found)
            ids << info.id;
        for (const char *id : {"overdrive", "bitcrush", "ringmod", "comb", "delay", "stutter", "clap:dole.test.gain",
                               "clap:dole.test.sine", "lv2:urn:dole:test-gain"})
            QVERIFY2(ids.contains(id), id);
        // Plugins installed on the machine show up too, so only dole's own
        // and the test plugins are checked.
        for (const PluginInfo &info : found)
            if (!info.id.contains(':') || info.id.contains("dole"))
                QCOMPARE(info.instrument, info.id == "clap:dole.test.sine");
    }

    void clapPluginsPlay() {
        QString error;
        auto gain = createProcessor(PluginSlot{"clap:dole.test.gain", {{"7", 0.5}}, {}, false}, &error);
        QVERIFY2(gain, qPrintable(error));
        QCOMPARE(gain->name(), QString("Test Gain"));
        QVERIFY(!gain->instrument());
        QVector<float> left(256, 1.f), right(256, 1.f);
        gain->process(left.data(), right.data(), 256, nullptr, 0, {});
        QCOMPARE(left[100], 0.5f);
        QCOMPARE(gain->valueText("7", 0.5), QString("50%"));

        // Its state carries over into another instance.
        const QByteArray state = gain->saveState();
        QCOMPARE(state.size(), int(sizeof(double)));
        auto copy = createProcessor(PluginSlot{"clap:dole.test.gain", {}, state, false}, &error);
        QCOMPARE(copy->parameters().first().value, 0.5);

        auto sine = createProcessor(PluginSlot{"clap:dole.test.sine", {}, {}, false}, &error);
        QVERIFY(sine->instrument());
        const NoteEvent notes[] = {{0, 69, 1, true}, {128, 69, 0, false}};
        std::fill(left.begin(), left.end(), 0.f);
        sine->process(left.data(), right.data(), 256, notes, 2, {});
        QVERIFY(*std::max_element(left.begin(), left.begin() + 128) > 0.3f);
        QCOMPARE(*std::max_element(left.begin() + 129, left.end()), 0.f);

        QVERIFY(!createProcessor(PluginSlot{"clap:no.such.plugin", {}, {}, false}, &error));
        QCOMPARE(error, QString("it isn't installed"));
    }

    void lv2PluginsPlay() {
        QString error;
        auto gain = createProcessor(PluginSlot{"lv2:urn:dole:test-gain", {{"gain", 1.5}}, {}, false}, &error);
        QVERIFY2(gain, qPrintable(error));
        QCOMPARE(gain->name(), QString("Test LV2 Gain"));
        QCOMPARE(gain->parameters().size(), 1);
        QCOMPARE(gain->parameters().first().max, 2.0);
        QVector<float> left(256, 0.5f), right(256, 0.5f);
        gain->process(left.data(), right.data(), 256, nullptr, 0, {});
        QCOMPARE(left[10], 0.75f);
        QVERIFY(!createProcessor(PluginSlot{"lv2:urn:nothing", {}, {}, false}, &error));
    }

    void songsKeepTheirPlugins() {
        QTemporaryDir dir;
        Song song;
        song.pads[3].synth = PluginSlot{"clap:dole.test.sine", {{"1", 0.25}}, QByteArray("\x01\x02", 2), false};
        song.pads[3].length = 4;
        song.pads[3].effects << PluginSlot{"overdrive", {{"drive", 0.9}}, {}, true} << PluginSlot{"comb", {}, {}, false};
        song.master << PluginSlot{"lv2:urn:dole:test-gain", {{"gain", 0.5}}, {}, false};
        QString error;
        QVERIFY(saveSong(dir.filePath("p.dole"), song, &error));
        Song loaded;
        QVERIFY2(loadSong(dir.filePath("p.dole"), &loaded, &error), qPrintable(error));
        QVERIFY(loaded == song);
        QVERIFY(!songFromJson(QJsonDocument::fromJson(R"({"dole": 2, "pads": {"1": {"length": 3}}})").object(), {}, &loaded, &error));
        QVERIFY(error.contains("\"length\""));
        QVERIFY(!songFromJson(QJsonDocument::fromJson(R"({"dole": 2, "master": [{"params": {}}]})").object(), {}, &loaded, &error));
        QVERIFY(error.contains("needs a \"plugin\""));
    }

    void padsPlayThroughTheirPlugins() {
        // At 120 BPM a step is 6000 frames; the synth's note lasts one.
        Song song = songFrom(R"({"dole": 2, "bpm": 120,
            "pads": {"15": {"synth": {"plugin": "clap:dole.test.sine"}},
                     "1": {"effects": [{"plugin": "clap:dole.test.gain", "params": {"7": 0}}]}},
            "patterns": {"A": {"15": "x...............", "1": "....x..........."}}, "song": ["A"]})");
        QStringList problems;
        Rack rack = buildRack(song, &problems);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
        QVector<float> audio = renderSong(song, kit(), rack, 0);
        QVERIFY(peak(audio, 0, 5000) > 0.1f);
        QVERIFY(peak(audio, 6500, 24000) < 1e-3f);
        // The gain at zero silences the boom; bypassed, it comes through.
        QVERIFY(peak(audio, 24000, 30000) < 1e-3f);
        song.pads[0].effects[0].bypass = true;
        rack = buildRack(song, &problems);
        audio = renderSong(song, kit(), rack, 0);
        QVERIFY(peak(audio, 24000, 30000) > 0.1f);
        // A master effect has the whole mix.
        song.master << PluginSlot{"clap:dole.test.gain", {{"7", 0}}, {}, false};
        rack = buildRack(song, &problems);
        QCOMPARE(peak(renderSong(song, kit(), rack, 0), 0, 48000), 0.f);
        // A plugin that isn't there is said, and left out.
        song.master << PluginSlot{"clap:gone", {}, {}, false};
        buildRack(song, &problems);
        QCOMPARE(problems.size(), 1);
        QVERIFY(problems.first().contains("isn't installed"));
    }

    void backendWiresPluginsIn() {
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);
        backend.setRackTarget(2);
        backend.addEffect(2, "overdrive");
        QCOMPARE(engine.rack.effects[2].size(), 1);
        const QVariantMap rack = backend.rack();
        const QVariantMap overdrive = rack.value("effects").toList().first().toMap();
        QCOMPARE(overdrive.value("name").toString(), QString("Overdrive"));
        QCOMPARE(overdrive.value("params").toList().size(), 4);
        QCOMPARE(backend.pads()[2].toMap().value("effects").toInt(), 1);

        // Sliding a control is heard at once, and undoes as one.
        auto processor = engine.rack.effects[2][0].processor;
        QCOMPARE(backend.setParameter(2, 0, "drive", 0.8), QString("80%"));
        backend.setParameter(2, 0, "drive", 0.9);
        QCOMPARE(processor->parameters().first().value, 0.9);
        backend.undo();
        QCOMPARE(processor->parameters().first().value, 0.5);
        QVERIFY(engine.rack.effects[2][0].processor == processor);

        backend.addEffect(2, "comb");
        backend.moveEffect(2, 1, -1);
        QCOMPARE(backend.song().pads[2].effects.first().id, QString("comb"));
        backend.toggleBypass(2, 0);
        QVERIFY(engine.rack.effects[2][0].bypass);
        backend.removeEffect(2, 0);
        QCOMPARE(engine.rack.effects[2].size(), 1);
        backend.addEffect(2, "delay");
        backend.addEffect(2, "delay");
        backend.addEffect(2, "delay");
        backend.addEffect(2, "delay");
        QCOMPARE(backend.message().value("kind").toString(), QString("chainFull"));

        backend.setSynth(14, "clap:dole.test.sine");
        QVERIFY(engine.rack.synths[14]);
        QCOMPARE(backend.pads()[14].toMap().value("synth").toString(), QString("Test Sine"));
        backend.cycleLength(14);
        QCOMPARE(backend.song().pads[14].length, 2);

        backend.addEffect(-1, "clap:no.such");
        QCOMPARE(backend.message().value("kind").toString(), QString("pluginFailed"));
        QVERIFY(engine.rack.master.isEmpty());
    }

    void aPluginsOwnChangesComeBackIntoTheSong() {
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);
        backend.addEffect(-1, "clap:dole.test.gain");
        auto processor = engine.rack.master.first().processor;
        const double quarter = 0.25;
        processor->loadState(QByteArray(reinterpret_cast<const char *>(&quarter), sizeof quarter));
        QVERIFY(processor->onEdited);
        processor->onEdited();
        QCOMPARE(backend.song().master.first().state, processor->saveState());
        QVERIFY(engine.rack.master.first().processor == processor);
    }

    void pluginsAreFoundInAProcessOfTheirOwn() {
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);
        QCOMPARE(backend.plugins().size(), 6);
        backend.setScanCommand(DOLE_BINARY, {"plugins", "--json"});
        backend.scanPlugins();
        QVERIFY(backend.scanning());
        QTRY_VERIFY_WITH_TIMEOUT(!backend.scanning(), 10000);
        QStringList ids;
        for (const QVariant &plugin : backend.plugins())
            ids << plugin.toMap().value("id").toString();
        for (const char *id : {"overdrive", "clap:dole.test.gain", "clap:dole.test.sine", "lv2:urn:dole:test-gain"})
            QVERIFY2(ids.contains(id), id);
    }

    // The song and its file.

    void songRoundTripsThroughItsFile() {
        QTemporaryDir dir;
        Song song = demoSong();
        song.pads[2].sample = dir.filePath("samples/soul.wav");
        song.pads[2].start = 1.25;
        song.pads[2].end = 1.5;
        song.pads[2].tune = -3;
        song.pads[3].group = 2;
        song.pads[1].mute = true;
        song.grit = Grit::Crushed;
        song.patterns[2].steps[0][5].level = 1;

        const QJsonObject json = songToJson(song, dir.path());
        const QJsonObject pad3 = json.value("pads").toObject().value("3").toObject();
        QCOMPARE(pad3.value("sample").toString(), QString("samples/soul.wav"));
        QCOMPARE(pad3.value("start").toDouble(), 1.25);
        QCOMPARE(pad3.value("tune").toInt(), -3);
        QCOMPARE(json.value("grit").toString(), QString("crushed"));
        QCOMPARE(json.value("patterns").toObject().value("C").toObject().value("1").toString(),
                 QString(".....1.........."));

        QString error;
        QVERIFY(saveSong(dir.filePath("song.dole"), song, &error));
        Song loaded;
        QVERIFY2(loadSong(dir.filePath("song.dole"), &loaded, &error), qPrintable(error));
        QVERIFY(loaded == song);
        QVERIFY(loaded.pads[2].isChop());
    }

    void padsGoByNumberOrSound() {
        const Song song = songFrom(R"({"dole": 2, "patterns": {"A": {"boom": "x...............", "16": "...x............"}}})");
        QVERIFY(song.patterns[0].steps[0][0].on());
        QVERIFY(song.patterns[0].steps[15][3].on());
    }

    void firstFormatSongsStillOpen() {
        const Song song = songFrom(R"({"dole": 1, "bpm": 95,
            "tracks": {"snare": {"volume": 0.5}},
            "patterns": {"A": {"kick": "x...............", "snare": "....x...........",
                               "bass": "C2 - . . D2 . . . . . . . . . . ."}}})");
        QCOMPARE(song.bpm, 95);
        QVERIFY(song.patterns[0].steps[0][0].on());  // kick to boom
        QVERIFY(song.patterns[0].steps[2][4].on());  // snare to crack
        QVERIFY(song.patterns[0].steps[14][0].on()); // bass notes to the bass pad
        QVERIFY(song.patterns[0].steps[14][4].on());
        QVERIFY(!song.patterns[0].steps[14][1].on());
        QCOMPARE(song.pads[2].volume, 0.5);
    }

    void songFilesSayWhatsWrong_data() {
        QTest::addColumn<QByteArray>("json");
        QTest::addColumn<QString>("error");
        QTest::newRow("not dole") << QByteArray(R"({"bpm": 90})") << "Not a dole song";
        QTest::newRow("tempo") << QByteArray(R"({"dole": 2, "bpm": 400})") << "\"bpm\" should be between";
        QTest::newRow("grit") << QByteArray(R"({"dole": 2, "grit": "lofi"})") << "\"grit\" is";
        QTest::newRow("pad") << QByteArray(R"({"dole": 2, "patterns": {"A": {"65": "x..............."}}})")
                             << "unknown pad";
        QTest::newRow("length") << QByteArray(R"({"dole": 2, "patterns": {"A": {"1": "x...x...x...x"}}})")
                                << "needs 16 steps, has 13";
        QTest::newRow("step") << QByteArray(R"({"dole": 2, "patterns": {"A": {"1": "x...y...x...x..."}}})")
                              << "\"y\" is not a step";
        QTest::newRow("tune") << QByteArray(R"({"dole": 2, "pads": {"3": {"tune": 30}}})") << "\"tune\" should be";
        QTest::newRow("chop") << QByteArray(R"({"dole": 2, "pads": {"3": {"start": 2, "end": 1}}})")
                              << "\"end\" should be after";
        QTest::newRow("song") << QByteArray(R"({"dole": 2, "song": ["A", "Z"]})") << "\"Z\"";
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

    void theKitHasSixteenSounds() {
        for (int p = 0; p < kBankPads; ++p) {
            const SampleBuffer sound = kitSound(p);
            QVERIFY2(sound && sound->size() > 1000, qPrintable(kitIds()[p]));
            QVERIFY(peak(*sound, 0, sound->size() / 2) > 0.5f);
        }
        // Banks B to D start empty, for the user's own sounds.
        QVERIFY(!kitSound(kBankPads));
    }

    void slicesCutCleanly() {
        const SampleBuffer whole = kitSound(12); // the stab, two seconds
        const SampleBuffer part = slice(whole, 0.5, 1.0);
        QCOMPARE(part->size(), qsizetype(kRate)); // half a second, stereo
        QCOMPARE((*part)[0], 0.f);
        QCOMPARE(part->last(), 0.f);
        QVERIFY(slice(whole, 0, 0) == whole);
        QVERIFY(!slice(whole, 5, 6));
        QCOMPARE(slice(whole, 1.5, 0)->size(), whole->size() - qsizetype(1.5 * kRate) * 2);
    }

    void stepsPlayOnTheirBeat() {
        // At 120 BPM a sixteenth is exactly 6000 frames.
        const Song song = songFrom(R"({"dole": 2, "bpm": 120, "patterns": {"A": {"rim": "x.......x......."}}})");
        const QVector<float> audio = renderBars(song, 1);
        QCOMPARE(firstSound(audio), qsizetype(0));
        QVERIFY(peak(audio, 0, 2000) > 0.1f);
        QCOMPARE(peak(audio, 4 * 6000, 8 * 6000), 0.f);
        QVERIFY(peak(audio, 8 * 6000, 8 * 6000 + 2000) > 0.1f);
    }

    void swingPushesEverySecondSixteenth() {
        const Song song =
            songFrom(R"({"dole": 2, "bpm": 120, "swing": 75, "patterns": {"A": {"rim": ".x.............."}}})");
        QVERIFY(qAbs(firstSound(renderBars(song, 1)) - 9000) <= 1);
    }

    void accentsAreLouder() {
        const QVector<float> soft = renderBars(songFrom(R"({"dole": 2, "patterns": {"A": {"rim": "1..............."}}})"), 1);
        const QVector<float> accent = renderBars(songFrom(R"({"dole": 2, "patterns": {"A": {"rim": "X..............."}}})"), 1);
        QVERIFY(peak(accent, 0, 4000) > 2 * peak(soft, 0, 4000));
    }

    void tuningChangesPitchAndLength() {
        // An octave up plays twice as fast, so the sound is over in half the time.
        Song song = songFrom(R"({"dole": 2, "bpm": 60, "patterns": {"A": {"bass": "x..............."}}})");
        const qsizetype normal = lastSound(renderBars(song, 1));
        song.pads[14].tune = 12;
        const qsizetype up = lastSound(renderBars(song, 1));
        QVERIFY2(qAbs(up * 2 - normal) < kRate / 10, qPrintable(QString("%1 %2").arg(up).arg(normal)));
    }

    void mutedAndUnsoloedPadsAreSilent() {
        Song song = songFrom(R"({"dole": 2, "patterns": {"A": {"rim": "x...............", "clap": "x..............."}}})");
        song.pads[4].mute = true;
        song.pads[5].mute = true;
        QCOMPARE(peak(renderBars(song, 1), 0, 6000), 0.f);
        song.pads[4].mute = song.pads[5].mute = false;
        song.pads[4].solo = true;
        QVERIFY(song.audible(4));
        QVERIFY(!song.audible(5));
    }

    // At 120 BPM a step is 6000 frames, and a rim with no decay is over in
    // well under one, so each hit can be told apart.
    static Song shortRims(const QByteArray &row, const QByteArray &extra = {}) {
        return songFrom(R"({"dole": 3, "bpm": 120, "pads": {"5": {"decay": 0)" + extra + R"(}},
                            "patterns": {"A": {"5": )" + row + "}}}");
    }

    void patternsRunUpToFourBarsAndTracksLoopOnTheirOwn() {
        Song song = songFrom(R"({"dole": 3, "bpm": 120, "pads": {"5": {"decay": 0}},
            "patterns": {"A": {"length": 32, "5": "................x..............."}}})");
        QCOMPARE(song.patterns[0].length, 32);
        QVector<QPair<int, int>> steps;
        const QVector<float> audio = renderBars(song, 2, &steps);
        QVERIFY(qAbs(firstSound(audio) - 16 * 6000) < 50);
        QCOMPARE(steps.size(), 32);
        QCOMPARE(steps.last().second, 31);
        QCOMPARE(songSteps(song, 0), 32);

        // A three-step loop drifts against the bar: steps 0, 3, 6, ... 15, 18.
        song = shortRims(R"("x...............")", R"(, "loop": 3)");
        const QVector<float> looped = renderBars(song, 2);
        for (int step : {3, 6, 15, 18})
            QVERIFY2(peak(looped, step * 6000, step * 6000 + 2000) > 0.05f, qPrintable(QString::number(step)));
        for (int step : {1, 2, 16, 17})
            QVERIFY2(peak(looped, step * 6000 + 500, step * 6000 + 5500) < 0.01f, qPrintable(QString::number(step)));
    }

    void stepsRollTakeTheirChanceAndNudge() {
        const QVector<float> once = renderBars(shortRims(R"("x...............")"), 1);
        const QVector<float> rolled = renderBars(songFrom(R"({"dole": 3, "bpm": 120, "pads": {"5": {"decay": 0}},
            "patterns": {"A": {"5": {"steps": "x...............", "roll": {"1": 4}}}}})"), 1);
        QVERIFY(peak(once, 3000, 3600) < 0.01f);
        QVERIFY(peak(rolled, 3000, 3600) > 0.05f);

        const QVector<float> never = renderBars(songFrom(R"({"dole": 3, "patterns": {"A":
            {"5": {"steps": "x.x.x.x.x.x.x.x.", "chance": {"1": 0, "3": 0, "5": 0, "7": 0, "9": 0, "11": 0, "13": 0, "15": 0}}}}})"), 1);
        QCOMPARE(peak(never, 0, 96000), 0.f);
        // Half the time is the same half every time, so an export matches.
        const QByteArray half = R"({"dole": 3, "patterns": {"A": {"5": {"steps": "xxxxxxxxxxxxxxxx",
            "chance": {"1": 50, "2": 50, "3": 50, "4": 50, "5": 50, "6": 50, "7": 50, "8": 50}}}}})";
        QCOMPARE(renderBars(songFrom(half), 1), renderBars(songFrom(half), 1));

        const QVector<float> late = renderBars(songFrom(R"({"dole": 3, "bpm": 120, "pads": {"5": {"decay": 0}},
            "patterns": {"A": {"5": {"steps": "....x...........", "nudge": {"5": 50}}}}})"), 1);
        QVERIFY(qAbs(firstSound(late) - (4 * 6000 + 3000)) < 50);
        const QVector<float> early = renderBars(songFrom(R"({"dole": 3, "bpm": 120, "pads": {"5": {"decay": 0}},
            "patterns": {"A": {"5": {"steps": "....x...........", "nudge": {"5": -25}}}}})"), 1);
        QVERIFY(qAbs(firstSound(early) - (4 * 6000 - 1500)) < 50);

        // A track swings on its own: 75% puts the second step halfway late.
        const QVector<float> swung = renderBars(shortRims(R"(".x..............")", R"(, "swing": 75)"), 1);
        QVERIFY(qAbs(firstSound(swung) - 9000) < 50);
    }

    void tracksAreShapedAndPlaced() {
        // Panned hard left, the right side is silent.
        const QVector<float> left = renderBars(shortRims(R"("x...............")", R"(, "pan": -1)"), 1);
        float right = 0, leftPeak = 0;
        for (int i = 0; i < 6000; ++i) {
            leftPeak = std::max(leftPeak, std::abs(left[2 * i]));
            right = std::max(right, std::abs(left[2 * i + 1]));
        }
        QVERIFY(leftPeak > 0.1f);
        QVERIFY(right < 1e-3f);

        // A closed low-pass takes the crack out of a rim.
        const float open = peak(renderBars(shortRims(R"("x...............")"), 1), 0, 6000);
        const float closed = peak(renderBars(shortRims(R"("x...............")", R"(, "filter": -1)"), 1), 0, 6000);
        QVERIFY2(closed < open / 4, qPrintable(QString("%1 %2").arg(closed).arg(open)));

        // Decay shortens the bass; a gate cuts it after its length.
        Song song = songFrom(R"({"dole": 3, "bpm": 120, "patterns": {"A": {"bass": "x..............."}}})");
        const qsizetype full = lastSound(renderBars(song, 1));
        song.pads[14].decay = 0.3;
        const qsizetype decayed = lastSound(renderBars(song, 1));
        song.pads[14].decay = 1;
        song.pads[14].gate = true;
        const qsizetype gated = lastSound(renderBars(song, 1));
        QVERIFY2(decayed < full / 2, qPrintable(QString("%1 %2").arg(decayed).arg(full)));
        QVERIFY2(qAbs(gated - 6000) < 600, qPrintable(QString::number(gated)));

        // Reversed, it swells instead of striking; with attack, it fades in.
        song.pads[14].gate = false;
        const float strike = peak(renderBars(song, 1), 0, 1000);
        song.pads[14].reverse = true;
        QVERIFY(peak(renderBars(song, 1), 0, 1000) < strike / 4);
        song.pads[14].reverse = false;
        song.pads[14].attack = 1;
        QVERIFY(peak(renderBars(song, 1), 0, 1000) < strike / 4);
    }

    void lanesMoveATrackAsItPlays() {
        // The volume lane silences the second hit from its step on.
        const QVector<float> ducked = renderBars(songFrom(R"({"dole": 3, "bpm": 120, "pads": {"5": {"decay": 0}},
            "patterns": {"A": {"5": {"steps": "x...x...x.......", "lanes": {"volume": {"5": 0, "9": 0.8}}}}}})"), 1);
        QVERIFY(peak(ducked, 0, 2000) > 0.05f);
        QVERIFY(peak(ducked, 4 * 6000, 5 * 6000) < 1e-3f);
        QVERIFY(peak(ducked, 8 * 6000, 9 * 6000) > 0.05f);

        // A pitch lane an octave up makes the bass twice as quick.
        Song song = songFrom(R"({"dole": 3, "bpm": 60, "patterns": {"A": {"bass": "x..............."}}})");
        const qsizetype normal = lastSound(renderBars(song, 1));
        song.patterns[0].lanes[14]["pitch"][0] = 12;
        const qsizetype up = lastSound(renderBars(song, 1));
        QVERIFY2(qAbs(up * 2 - normal) < kRate / 10, qPrintable(QString("%1 %2").arg(up).arg(normal)));

        // A plugin's lane sets its parameter on the step, and stopping puts
        // the song's setting back.
        song = songFrom(R"({"dole": 3, "pads": {"5": {"effects": [{"plugin": "overdrive", "params": {"drive": 0.2}}]}},
            "patterns": {"A": {"5": {"steps": "x...............", "lanes": {"fx1.drive": {"1": 0.9}}}}}})");
        QStringList problems;
        const Rack rack = buildRack(song, &problems);
        Renderer renderer;
        renderer.setSong(song);
        renderer.setSounds(kit());
        renderer.setRack(rack);
        renderer.play(false, 0, 0);
        QVector<float> out(2 * 4000);
        renderer.render(out.data(), 4000);
        auto drive = [&] {
            for (const Parameter &p : rack.effects[4][0].processor->parameters())
                if (p.id == "drive")
                    return p.value;
            return -1.0;
        };
        QCOMPARE(drive(), 0.9);
        renderer.stop();
        QCOMPARE(drive(), 0.2);
    }

    void songFilesKeepEverything() {
        Song song = songFrom(R"({"dole": 3, "pads": {
                "B3": {"sample": "/tmp/x.wav", "pan": -0.5, "filter": 0.25, "resonance": 0.5, "attack": 0.1,
                       "decay": 0.4, "reverse": true, "gate": true, "swing": 66, "loop": 12, "length": 4}},
            "patterns": {"C": {"length": 48, "B3": {"steps": "x...............x...............x...............",
                "roll": {"1": 3}, "chance": {"17": 50}, "nudge": {"33": -10},
                "lanes": {"pitch": {"1": -5, "17": 7}, "filter": {"9": -0.75}}}}}})");
        const Pad &pad = song.pads[18];
        QCOMPARE(pad.pan, -0.5);
        QVERIFY(pad.reverse && pad.gate);
        QCOMPARE(pad.loop, 12);
        QCOMPARE(song.patterns[2].length, 48);
        QCOMPARE(song.patterns[2].steps[18][0].roll, 3);
        QCOMPARE(song.patterns[2].steps[18][16].chance, 50);
        QCOMPARE(song.patterns[2].steps[18][32].nudge, -10);
        QCOMPARE(laneValue(song.patterns[2].lanes[18]["pitch"], 10), -5.0);
        QCOMPARE(laneValue(song.patterns[2].lanes[18]["pitch"], 20), 7.0);
        QVERIFY(std::isnan(laneValue(song.patterns[2].lanes[18]["filter"], 3)));

        Song back;
        QString error;
        QVERIFY2(songFromJson(songToJson(song, {}), {}, &back, &error), qPrintable(error));
        QVERIFY(back == song);
        // The demo still reads as it did, and an old song still opens.
        QVERIFY(songFromJson(songToJson(demoSong(), {}), {}, &back, &error));
        QVERIFY(back == demoSong());

        QVERIFY(!songFromJson(QJsonDocument::fromJson(R"({"dole": 3, "patterns": {"A": {"1": {"steps": "x...............",
            "lanes": {"wobble": {"1": 1}}}}}})").object(), {}, &back, &error));
        QVERIFY(error.contains("\"wobble\" isn't a lane"));
    }

    void patchesNamePadsAnyWayAndKeepRows() {
        const QJsonObject song = songToJson(songFrom(R"({"dole": 3, "patterns": {"A": {
            "1": "X.......X.......",
            "7": "x.x.x.x.x.x.x.x."}}})"), {});
        const QJsonObject patched = patchSongJson(song, QJsonDocument::fromJson(R"({
            "bpm": 96,
            "pads": {"hat": {"volume": 0.5}, "B1": {"tune": -3}},
            "patterns": {"a": {"hat": {"roll": {"16": 3}}, "boom": null, "A3": "....X.......X..."},
                         "B": {"length": 32}}})").object());
        Song result;
        QString error;
        QVERIFY2(songFromJson(patched, {}, &result, &error), qPrintable(error));
        QCOMPARE(result.bpm, 96);
        QCOMPARE(result.pads[6].volume, 0.5);
        QCOMPARE(result.pads[16].tune, -3);
        // The roll joined the hat's row, which kept its steps.
        QCOMPARE(result.patterns[0].steps[6][0].level, 3);
        QCOMPARE(result.patterns[0].steps[6][15].roll, 3);
        QCOMPARE(result.patterns[0].steps[0][0].level, 0);
        QCOMPARE(result.patterns[0].steps[2][4].level, 4);
        QCOMPARE(result.patterns[1].length, 32);
    }

    void agentsPlayTheOpenWindow() {
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);
        backend.newSong();
        RemoteServer server(&backend);
        QTemporaryDir directory;
        QString error;
        QVERIFY2(server.listen(directory.filePath("dole"), &error), qPrintable(error));
        QCOMPARE(QFileInfo(directory.filePath("dole")).permissions() & (QFileDevice::ReadOther | QFileDevice::ReadGroup),
                 QFileDevice::Permissions());

        QLocalSocket socket;
        socket.connectToServer(server.path());
        QVERIFY(socket.waitForConnected(1000));
        int next = 0;
        auto ask = [&](const QString &method, const QJsonObject &params = {}) {
            const int id = ++next;
            socket.write(QJsonDocument(QJsonObject{{"id", id}, {"method", method}, {"params", params}})
                             .toJson(QJsonDocument::Compact)
                         + '\n');
            QElapsedTimer clock;
            clock.start();
            while (clock.elapsed() < 5000) {
                while (socket.canReadLine()) {
                    const QJsonObject message = QJsonDocument::fromJson(socket.readLine()).object();
                    if (message.value("id").toInt() == id)
                        return message;
                }
                QTest::qWait(5);
            }
            return QJsonObject{{"error", "no answer"}};
        };
        auto json = [](const char *text) { return QJsonDocument::fromJson(text).object(); };

        QCOMPARE(ask("status").value("result").toObject().value("playing").toBool(), false);

        // A patch lands in one undoable step, heard at once.
        QJsonObject reply = ask("edit", json(R"({"patch": {"patterns": {"A": {"hat": "x.x.x.x.x.x.x.x."}}}})"));
        QVERIFY2(!reply.contains("error"), qPrintable(reply.value("error").toString()));
        QCOMPARE(backend.song().patterns[0].steps[6][2].level, 3);
        QCOMPARE(engine.song.patterns[0].steps[6][2].level, 3);
        QVERIFY(backend.canUndo());
        QCOMPARE(ask("song").value("result").toObject().value("song").toObject().value("patterns").toObject()
                     .value("A").toObject().value("7").toString(),
                 QString("x.x.x.x.x.x.x.x."));
        QVERIFY(!ask("undo").contains("error"));
        QCOMPARE(backend.song().patterns[0].steps[6][2].level, 0);
        QVERIFY(!ask("redo").contains("error"));
        QCOMPARE(backend.song().patterns[0].steps[6][2].level, 3);

        // What's wrong is said, and nothing changes.
        reply = ask("edit", json(R"({"patch": {"patterns": {"A": {"hat": "x.x"}}}})"));
        QVERIFY(reply.value("error").toString().contains("16 steps"));
        QCOMPARE(backend.song().patterns[0].steps[6][2].level, 3);
        QVERIFY(ask("fly").value("error").toString().contains("Unknown method"));

        QVERIFY(!ask("select", json(R"({"pattern": "B", "pad": "clap"})")).contains("error"));
        QCOMPARE(backend.editPattern(), 1);
        QCOMPARE(backend.selectedPad(), 5);
        QVERIFY(ask("wait", json(R"({"until": "bar"})")).value("error").toString().contains("Nothing is playing"));

        reply = ask("transport", json(R"({"action": "play"})"));
        QCOMPARE(reply.value("result").toObject().value("playing").toBool(), true);
        QVERIFY(engine.calls.contains("play 0 1 0"));

        // A wait for the bar is answered when the playhead comes to it.
        const int id = ++next;
        socket.write(QJsonDocument(QJsonObject{{"id", id}, {"method", "wait"}, {"params", json(R"({"until": "bar"})")}})
                         .toJson(QJsonDocument::Compact)
                     + '\n');
        QTest::qWait(20);
        emit engine.positionChanged(-1, 1, 5);
        QTest::qWait(20);
        QVERIFY(!socket.canReadLine());
        emit engine.positionChanged(-1, 1, 0);
        QTRY_VERIFY(socket.canReadLine());
        const QJsonObject waited = QJsonDocument::fromJson(socket.readLine()).object();
        QCOMPARE(waited.value("id").toInt(), id);
        QCOMPARE(waited.value("result").toObject().value("status").toObject().value("position").toObject()
                     .value("step").toInt(), 1);

        // A wait for an edit is answered by the person's, not the agent's own.
        const int editId = ++next;
        socket.write(QJsonDocument(QJsonObject{{"id", editId}, {"method", "wait"}, {"params", json(R"({"until": "edit"})")}})
                         .toJson(QJsonDocument::Compact)
                     + '\n');
        QTest::qWait(20);
        backend.toggleStep(0, 0);
        QTRY_VERIFY(socket.canReadLine());
        QCOMPARE(QJsonDocument::fromJson(socket.readLine()).object().value("id").toInt(), editId);

        QVERIFY(!ask("hit", json(R"({"pad": "A2", "level": 4})")).contains("error"));
        QVERIFY(engine.calls.contains("hit 1 4"));
        QVERIFY(ask("save").value("error").toString().contains("no file yet"));
        QVERIFY(!ask("transport", json(R"({"action": "stop"})")).contains("error"));
        QVERIFY(!backend.playing());
    }

    void undoTakesBackOnlyWhatWasDoneHere() {
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);
        backend.newSong();
        backend.toggleStep(0, 0);
        // Someone else, in a jam, adds a hat.
        Song theirs = backend.song();
        theirs.patterns[0].steps[6][2].level = 3;
        backend.receiveSong(theirs);
        backend.undo();
        QCOMPARE(backend.song().patterns[0].steps[0][0].level, 0);
        QCOMPARE(backend.song().patterns[0].steps[6][2].level, 3);
        backend.redo();
        QCOMPARE(backend.song().patterns[0].steps[0][0].level, kDefaultLevel);
        QCOMPARE(backend.song().patterns[0].steps[6][2].level, 3);
    }

    void seekingPlaysOnAsIfFromTheTop() {
        Song song = songFrom(R"({"dole": 3, "song": ["A", "B"], "patterns": {
            "A": {"1": "x...............", "7": {"steps": "x...............", "lanes": {"volume": {"1": 0.2}}}},
            "B": {"length": 32, "3": "x..............................."}}})");
        Renderer renderer;
        renderer.setSong(song);
        renderer.play(true, 0, 0);
        int chain = -1, pattern = -1, step = -1;
        std::vector<float> out(2 * 64);
        // Step 7 of B, in the song's second slot, is the one heard next.
        renderer.seek(16 + 6);
        renderer.render(out.data(), 1, [&](int c, int p, int s, int) { chain = c, pattern = p, step = s; });
        QCOMPARE(chain, 1);
        QCOMPARE(pattern, 1);
        QCOMPARE(step, 6);
        // Before the start it counts in.
        renderer.seek(-2);
        QCOMPARE(renderer.clock(), -2.0);
        // A playhead a little behind catches up by playing a touch faster.
        renderer.seek(3);
        renderer.follow(3.02, 1024);
        const double before = renderer.clock();
        renderer.render(out.data(), 64);
        QVERIFY(renderer.clock() - before > 64 / renderer.stepFrames());
        renderer.follow(10, 1024);
        QCOMPARE(renderer.clock(), 10.0);
    }

    void jamsKeepOneBeatAndShareASong() {
        QTemporaryDir directory;
        QTcpServer free;
        QVERIFY(free.listen(QHostAddress("127.0.0.1")));
        const quint16 port = free.serverPort();
        free.close();

        FakeEngine engineA, engineB;
        FakePicker picker;
        Backend alpha(&engineA, &picker), beta(&engineB, &picker);
        alpha.newSong();
        beta.newSong();
        Jam jamA(&alpha, &engineA), jamB(&beta, &engineB);
        jamA.setTailscale(fakeTailscale(directory.path(), "alpha", "127.0.0.1", "beta", "127.0.0.2"));
        jamB.setTailscale(fakeTailscale(directory.path(), "beta", "127.0.0.2", "alpha", "127.0.0.1"));
        jamA.setCacheDirectory(directory.filePath("cacheA"));
        jamB.setCacheDirectory(directory.filePath("cacheB"));
        jamA.setPort(port);
        jamB.setPort(port);
        jamA.start();
        jamB.start();
        QTRY_VERIFY(jamA.listening() && jamB.listening());

        // Only the user's own devices are found, not someone else's.
        jamB.search();
        QTRY_VERIFY(!jamB.searching());
        QCOMPARE(jamB.found().size(), 1);
        QCOMPARE(jamB.found()[0].toMap().value("name").toString(), QString("alpha"));

        // Playing along: one beat, each its own song.
        jamB.join("127.0.0.1", "clock");
        QTRY_COMPARE(jamB.role(), QString("joined"));
        QTRY_COMPARE(jamA.role(), QString("hosting"));
        QTRY_VERIFY(jamB.latency() >= 0);
        QTRY_VERIFY(engineB.timeline.on);
        QCOMPARE(jamA.members().size(), 1);

        alpha.changeBpm(6);
        QTRY_COMPARE(beta.bpm(), alpha.bpm());
        beta.changeBpm(-2);
        QTRY_COMPARE(alpha.bpm(), beta.bpm());
        QTRY_COMPARE(engineA.timeline.bpm, double(beta.bpm()));

        beta.togglePlay();
        QTRY_VERIFY(alpha.playing());
        QVERIFY(engineB.timeline.started);
        QCOMPARE(engineA.timeline.startStep, engineB.timeline.startStep);
        QCOMPARE(engineA.timeline.startStep % kSteps, 0);
        // One beat: both timelines put the same step at the same moment.
        const qint64 now = steadyMicroseconds();
        QVERIFY(std::abs(engineA.timeline.stepAt(now) - engineB.timeline.stepAt(now)) < 0.05);
        alpha.stop();
        QTRY_VERIFY(!beta.playing());

        // Another song stays its own.
        beta.toggleStep(3, 3);
        QTest::qWait(200);
        QCOMPARE(alpha.song().patterns[0].steps[3][3].level, 0);

        jamB.leave();
        QTRY_COMPARE(jamA.role(), QString("alone"));
        QVERIFY(!engineA.timeline.on);

        // Sharing the song: beta takes alpha's, and edits go both ways.
        alpha.toggleStep(0, 0);
        jamB.join("127.0.0.1", "song");
        QTRY_COMPARE(jamB.role(), QString("joined"));
        QTRY_VERIFY(beta.song() == alpha.song());
        QVERIFY(!beta.canUndo());

        beta.toggleStep(6, 2);
        QTRY_COMPARE(alpha.song().patterns[0].steps[6][2].level, kDefaultLevel);
        alpha.toggleStep(2, 4);
        QTRY_COMPARE(beta.song().patterns[0].steps[2][4].level, kDefaultLevel);

        // At the same moment, on both: neither edit is lost.
        alpha.toggleStep(1, 8);
        beta.toggleStep(5, 12);
        QTRY_VERIFY(alpha.song() == beta.song());
        QCOMPARE(alpha.song().patterns[0].steps[1][8].level, kDefaultLevel);
        QCOMPARE(alpha.song().patterns[0].steps[5][12].level, kDefaultLevel);

        // Undo here takes back only what was done here.
        beta.undo();
        QTRY_COMPARE(alpha.song().patterns[0].steps[5][12].level, 0);
        QCOMPARE(alpha.song().patterns[0].steps[1][8].level, kDefaultLevel);
        QTRY_VERIFY(alpha.song() == beta.song());

        // The pattern on screen is the same on both.
        alpha.selectPattern(2);
        QTRY_COMPARE(beta.editPattern(), 2);

        // A sample on alpha's pad is copied to beta, which plays its copy.
        const QString wav = directory.filePath("soul.wav");
        QString error;
        QVERIFY(writeWav(wav, QVector<float>(2 * 4800, 0.25f), &error));
        alpha.setSample(16, wav);
        QTRY_VERIFY(beta.song().pads[16].sample.startsWith(directory.filePath("cacheB")));
        QVERIFY(QFileInfo::exists(beta.song().pads[16].sample));
        QCOMPARE(alpha.song().pads[16].sample, wav);
        // And edits to that pad on beta don't break alpha's path.
        beta.changeTune(16, 2);
        QTRY_COMPARE(alpha.song().pads[16].tune, 2);
        QCOMPARE(alpha.song().pads[16].sample, wav);

        // When the host goes, the member says so and plays on alone.
        jamA.leave();
        QTRY_COMPARE(jamB.role(), QString("alone"));
        QCOMPARE(jamB.problem(), QString("ended"));
    }

    void banksStepsAndLanesFromTheWindow() {
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);
        backend.newSong();
        backend.setBank(1);
        QCOMPARE(backend.bank(), 1);
        QCOMPARE(backend.selectedPad(), 16);
        QCOMPARE(backend.grid().size(), 16);
        QCOMPARE(backend.pads()[16].toMap().value("state").toString(), QString("empty"));
        QCOMPARE(backend.pads()[16].toMap().value("name").toString(), QString("B1"));
        // Choosing a pad in another bank shows that bank.
        backend.selectPad(40);
        QCOMPARE(backend.bank(), 2);
        backend.selectPad(16);

        backend.toggleStep(16, 2);
        backend.cycleRoll(16, 2);
        backend.cycleChance(16, 2);
        backend.nudgeStep(16, 2, 10);
        const QVariantMap row = backend.grid()[0].toMap();
        QCOMPARE(row.value("roll").toList()[2].toInt(), 2);
        QCOMPARE(row.value("chance").toList()[2].toInt(), 75);
        QCOMPARE(row.value("nudge").toList()[2].toInt(), 10);

        backend.cyclePatternLength();
        QCOMPARE(backend.patternLength(), 32);
        QCOMPARE(backend.grid()[0].toMap().value("steps").toList().size(), 32);

        backend.addLane(16, "filter");
        backend.setLanePoint(16, "filter", 4, -0.5);
        backend.setLanePoint(16, "filter", 5, -9); // held to its range
        QVariantMap lane = backend.grid()[0].toMap().value("lanes").toList()[0].toMap();
        QCOMPARE(lane.value("id").toString(), QString("filter"));
        QCOMPARE(lane.value("values").toList()[0].toDouble(), 0.0);
        QCOMPARE(lane.value("values").toList()[4].toDouble(), -0.5);
        QCOMPARE(lane.value("values").toList()[5].toDouble(), -1.0);
        QVERIFY(lane.value("values").toList()[6].isNull());
        backend.clearLanePoint(16, "filter", 5);
        backend.addLane(16, "pitch");
        backend.changeLanePoint(16, "pitch", 3, 0.01); // a semitone, however small the nudge
        QCOMPARE(backend.song().patterns[0].lanes[16]["pitch"][3], 1.0);

        // An effect's lane follows it when it moves, and goes with it.
        backend.addEffect(16, "overdrive");
        backend.addEffect(16, "bitcrush");
        QVERIFY(backend.laneChoices(16).size() > 6);
        backend.addLane(16, "fx2.bits");
        backend.moveEffect(16, 1, -1);
        QVERIFY(backend.song().patterns[0].lanes[16].contains("fx1.bits"));
        backend.removeEffect(16, 0);
        QVERIFY(!backend.song().patterns[0].lanes[16].contains("fx1.bits"));
        backend.removeLane(16, "pitch");
        backend.removeLane(16, "filter");
        QVERIFY(!backend.song().patterns[0].lanes.contains(16));

        backend.setPadSetting(16, "pan", -2);
        backend.toggleReverse(16);
        backend.toggleGate(16);
        backend.changePadSwing(16, 4);
        backend.changeLoop(16, -20);
        const QVariantMap pad = backend.pads()[16].toMap();
        QCOMPARE(pad.value("pan").toDouble(), -1.0);
        QVERIFY(pad.value("reverse").toBool() && pad.value("gate").toBool());
        QCOMPARE(pad.value("swing").toInt(), 54);
        QCOMPARE(pad.value("loop").toInt(), 12);
        backend.changeLoop(16, 20);
        QCOMPARE(backend.pads()[16].toMap().value("loop").toInt(), 0); // back to the pattern's

        backend.padUp(16);
        QVERIFY(engine.calls.contains("release 16"));
    }

    void muteGroupsCutEachOtherOff() {
        // The open hat rings for most of a second unless the closed one cuts it.
        const char *json = R"({"dole": 2, "bpm": 120, "patterns": {"A": {"openhat": "x...............", "hat": "....x..........."}}})";
        Song song = songFrom(json);
        song.pads[6].volume = 0; // only the open hat is heard
        QVERIFY(peak(renderBars(song, 1), 6 * 6000, 8 * 6000) < 1e-3f);
        song.pads[6].group = song.pads[7].group = 0;
        QVERIFY(peak(renderBars(song, 1), 6 * 6000, 8 * 6000) > 0.01f);
    }

    void songPlaysItsChain() {
        const Song song = songFrom(R"({"dole": 2, "patterns": {"A": {"rim": "x..............."},
                                                               "B": {"clap": "x..............."}},
                                         "song": ["A", "B", "B"]})");
        QVector<QPair<int, int>> steps;
        renderBars(song, 4, &steps);
        QVector<int> bars;
        for (const auto &[pattern, step] : steps)
            if (step == 0)
                bars << pattern;
        QCOMPARE(bars, QVector<int>({0, 1, 1, 0}));
    }

    void patternChangesWaitForTheBar() {
        Renderer renderer;
        renderer.setSong(demoSong());
        renderer.setSounds(kit());
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

    void noteRepeatPlaysHeldPadsInTime() {
        Renderer renderer;
        renderer.setSong(songFrom(R"({"dole": 2, "bpm": 120})"));
        renderer.setSounds(kit());
        renderer.setRepeat(2); // 1/16
        renderer.play(false, 0, 0);
        renderer.hold(4, true, 3);
        QVector<int> steps;
        QVector<float> out(16 * 6000 * 2);
        renderer.render(out.data(), 16 * 6000, {}, [&](int pad, int, int step, int) {
            QCOMPARE(pad, 4);
            steps << step;
        });
        // Every sixteenth after the press, which was itself the first hit.
        QCOMPARE(steps.size(), 15);
        QCOMPARE(steps.first(), 1);
        QCOMPARE(steps.last(), 15);
        QVERIFY(peak(out, 4 * 6000, 4 * 6000 + 2000) > 0.1f);
        // Let go, and it stops.
        renderer.hold(4, false);
        steps.clear();
        renderer.render(out.data(), 16 * 6000, {}, [&](int, int, int step, int) { steps << step; });
        QVERIFY(steps.isEmpty());
    }

    void aSuppressedStepSkipsOnce() {
        Renderer renderer;
        renderer.setSong(songFrom(R"({"dole": 2, "bpm": 120, "patterns": {"A": {"rim": "....x..........."}}})"));
        renderer.setSounds(kit());
        renderer.play(false, 0, 0);
        renderer.suppress(4, 0, 4);
        QVector<float> out(16 * 6000 * 2);
        renderer.render(out.data(), 16 * 6000);
        QCOMPARE(peak(out, 4 * 6000, 5 * 6000), 0.f);
        renderer.render(out.data(), 16 * 6000);
        QVERIFY(peak(out, 4 * 6000, 5 * 6000) > 0.1f);
    }

    void gritCrushesTheMaster() {
        Song song = songFrom(R"({"dole": 2, "patterns": {"A": {"stab": "x..............."}}})");
        const QVector<float> clean = renderBars(song, 1);
        song.grit = Grit::Crushed;
        const QVector<float> crushed = renderBars(song, 1);
        double difference = 0;
        for (int i = 0; i < 20000; ++i)
            difference += std::abs(clean[i] - crushed[i]);
        QVERIFY(difference > 100);
        // Vinyl crackles only while it plays.
        song.grit = Grit::Clean;
        song.vinyl = true;
        song.patterns[0] = Pattern{};
        QVERIFY(peak(renderBars(song, 1), 0, 16 * 7800) > 0.001f);
        Renderer stopped;
        stopped.setSong(song);
        QVector<float> out(20000);
        stopped.render(out.data(), 10000);
        QCOMPARE(peak(out, 0, 10000), 0.f);
    }

    void exportRendersTheSongAndItsTail() {
        const Song song = songFrom(R"({"dole": 2, "bpm": 120, "patterns": {"A": {"boom": "x...............",
                                                                                   "openhat": "..............x."}},
                                         "song": ["A", "A"]})");
        const QVector<float> audio = renderSong(song, kit(), Rack{}, 0);
        const qsizetype bars = 2 * 16 * 6000;
        QVERIFY(audio.size() / 2 > bars);
        QVERIFY(audio.size() / 2 < bars + kRate);
        QVERIFY(peak(audio, bars - 100, bars) > 0.01f);
    }

    // Chopping.

    void choppingFindsTheHits() {
        QTemporaryDir dir;
        QString error;
        const SampleBuffer clicks = decodeSample(writeClicks(dir.filePath("clicks.wav"), 4), &error);
        QVERIFY2(clicks, qPrintable(error));
        QCOMPARE(duration(clicks), 2.0);
        const QVector<double> hits = transientSlices(clicks, 16);
        QCOMPARE(hits.size(), 4);
        for (int i = 0; i < 4; ++i)
            QVERIFY2(qAbs(hits[i] - i * 0.5) < 0.011, qPrintable(QString::number(hits[i])));
        QCOMPARE(transientSlices(clicks, 2).size(), 2);
        QCOMPARE(evenSlices(2.0, 4), QVector<double>({0, 0.5, 1, 1.5}));
        const QVector<float> wave = peaks(clicks, 100);
        QCOMPARE(wave.size(), 100);
        QCOMPARE(*std::max_element(wave.begin(), wave.end()), 1.f);
        QCOMPARE(wave[20], 0.f);
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
        QCOMPARE(qFromLittleEndian<quint16>(data.constData() + 22), quint16(2));
        QCOMPARE(qFromLittleEndian<quint32>(data.constData() + 24), quint32(kRate));
        QCOMPARE(qFromLittleEndian<qint16>(data.constData() + 46), qint16(16384));
        QCOMPARE(qFromLittleEndian<qint16>(data.constData() + 50), qint16(32767));
        QVERIFY(!writeWav(dir.filePath("missing/a.wav"), audio, &error));
    }

    void samplesDecodeThroughFfmpeg() {
        QTemporaryDir dir;
        QString error;
        QVERIFY(writeWav(dir.filePath("rim.wav"), *kitSound(4), &error));
        const SampleBuffer sound = decodeSample(dir.filePath("rim.wav"), &error);
        QVERIFY2(sound, qPrintable(error));
        QCOMPARE(sound->size(), kitSound(4)->size());
        QVERIFY(write(dir.filePath("noise.wav"), "not audio at all"));
        QVERIFY(!decodeSample(dir.filePath("noise.wav"), &error));
        QVERIFY(!error.isEmpty() && error != kNoFfmpeg);
    }

    // The backend.

    void editsUndoAndKnowWhatIsUnsaved() {
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);
        QVERIFY(!backend.dirty());
        QVERIFY(engine.song == backend.song());

        backend.toggleStep(4, 3);
        QVERIFY(backend.dirty());
        QVERIFY(engine.song.patterns[0].steps[4][3].on());
        QCOMPARE(engine.calls.last(), QString("hit 4 3"));
        QCOMPARE(backend.selectedPad(), 4);
        QCOMPARE(backend.grid()[4].toMap().value("steps").toList()[3].toInt(), 3);
        backend.undo();
        QVERIFY(!backend.dirty());
        backend.redo();
        backend.toggleStep(4, 3);
        QVERIFY(!backend.dirty());

        const int bpm = backend.bpm();
        for (int i = 0; i < 5; ++i)
            backend.changeBpm(1);
        backend.undo();
        QCOMPARE(backend.bpm(), bpm);
        backend.cycleGrit();
        QCOMPARE(backend.grit(), 2);
        backend.cycleGrit();
        QCOMPARE(backend.grit(), 0);
        backend.toggleVinyl();
        QVERIFY(!backend.vinyl());
    }

    void padsPlayAndChangeTheirSound() {
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);
        QSignalSpy hits(&backend, &Backend::padHit);
        backend.padDown(9, 4);
        QCOMPARE(engine.calls.last(), QString("hit 9 4"));
        QCOMPARE(backend.selectedPad(), 9);
        QCOMPARE(hits.count(), 1);
        backend.padUp(9);
        // Nothing held without note repeat on, so nothing to let go.
        QVERIFY(!engine.calls.contains("hold 9 0"));

        backend.changeTune(9, -5);
        QCOMPARE(backend.song().pads[9].tune, -5);
        backend.changeTune(9, -100);
        QCOMPARE(backend.song().pads[9].tune, kMinTune);
        backend.cycleGroup(9);
        QCOMPARE(backend.song().pads[9].group, 1);
        backend.resetPad(9);
        QCOMPARE(backend.song().pads[9].tune, 0);
        QCOMPARE(backend.song().pads[9].group, 0);
        QCOMPARE(backend.pads()[9].toMap().value("id").toString(), QString("tom"));
    }

    void noteRepeatHoldsPads() {
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);
        backend.toggleRepeat();
        QCOMPARE(backend.repeat(), 2);
        QCOMPARE(backend.repeatName(), QString("1/16"));
        QCOMPARE(engine.calls.last(), QString("repeat 2"));
        backend.padDown(0);
        QCOMPARE(engine.calls.last(), QString("hold 0 1"));
        backend.padUp(0);
        QCOMPARE(engine.calls.last(), QString("hold 0 0"));
        backend.cycleRepeat();
        QCOMPARE(backend.repeatName(), QString("1/32"));
        backend.padDown(3);
        backend.toggleRepeat();
        QCOMPARE(backend.repeat(), 0);
        // Turning it off lets go of whatever was held.
        QVERIFY(engine.calls.contains("hold 3 0"));
    }

    void recordingLandsOnTheNearestStep() {
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);
        backend.newSong();
        backend.toggleRecord();
        QVERIFY(backend.recording());
        QVERIFY(backend.playing());

        // Hit just after step 4 was heard: step 4, already played.
        emit engine.positionChanged(0, 0, 4);
        backend.padDown(2);
        QVERIFY(backend.song().patterns[0].steps[2][4].on());
        QVERIFY(!engine.calls.last().startsWith("suppress"));

        // Hit late, past halfway to the next: step 5, which mustn't sound twice.
        backend.changeBpm(-50); // 40 BPM: a step is 375 ms
        emit engine.positionChanged(0, 0, 4);
        QTest::qWait(250);
        backend.padDown(6);
        QVERIFY(backend.song().patterns[0].steps[6][5].on());
        QVERIFY(engine.calls.contains("suppress 6 0 5"));

        // Note repeat's hits are recorded too.
        emit engine.repeated(0, 0, 12);
        QVERIFY(backend.song().patterns[0].steps[0][12].on());

        // Stopping ends the take; what was recorded since the tempo changed
        // undoes as one.
        backend.stop();
        QVERIFY(!backend.recording());
        backend.undo();
        QVERIFY(!backend.song().patterns[0].steps[0][12].on());
        QVERIFY(!backend.song().patterns[0].steps[6][5].on());
        QVERIFY(backend.song().patterns[0].steps[2][4].on());
    }

    void chopsLandOnPads() {
        QTemporaryDir dir;
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);
        picker.answer = writeClicks(dir.filePath("break.wav"), 4);
        backend.chooseChop();
        QCOMPARE(backend.chopState(), QString("loading"));
        QTRY_COMPARE(backend.chopState(), QString("ready"));
        QCOMPARE(backend.chopName(), QString("break"));
        QCOMPARE(backend.chopPeaks().size(), 600);
        // Cut where it hits, at first.
        QCOMPARE(backend.chopSlices().size(), 4);

        backend.sliceEven(8);
        QCOMPARE(backend.chopSlices().size(), 8);
        QCOMPARE(backend.chopSlices()[1].toDouble(), 0.125);
        backend.previewSlice(1);
        QCOMPARE(engine.calls.last(), QString("preview %1").arg(kRate / 4));

        backend.sliceEven(4);
        QCOMPARE(backend.assignChops(13), 3); // only pads 14 to 16 are left
        QCOMPARE(backend.assignChops(2), 4);
        const Pad &third = backend.song().pads[4];
        QCOMPARE(third.sample, dir.filePath("break.wav"));
        QCOMPARE(third.start, 1.0);
        QCOMPARE(third.end, 1.5);
        QVERIFY(backend.pads()[4].toMap().value("chop").toBool());
        QCOMPARE(engine.sounds[4]->size(), qsizetype(kRate));
        QCOMPARE(backend.message().value("kind").toString(), QString("chopped"));
    }

    void playingFollowsTheSong() {
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);
        QSignalSpy hits(&backend, &Backend::padHit);
        backend.togglePlay();
        QCOMPARE(engine.calls.last(), QString("play 0 0 0"));
        emit engine.positionChanged(0, 0, 0);
        // The demo's first step strikes the boom, the hat, the stab, and the bass.
        QCOMPARE(hits.count(), 4);
        backend.selectPattern(1);
        QCOMPARE(engine.calls.last(), QString("cue 1"));
        backend.stop();
        QCOMPARE(backend.playStep(), -1);

        backend.toggleSongMode();
        backend.togglePlay(2);
        QCOMPARE(engine.calls.last(), QString("play 1 1 2"));
        emit engine.positionChanged(3, 0, 4);
        QCOMPARE(backend.editPattern(), 0);
        backend.togglePlay();
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

        Backend other(&engine, &picker);
        QVERIFY(other.openPath(dir.filePath("groove.dole")));
        QVERIFY(other.song() == backend.song());
        QVERIFY(!other.openPath(dir.filePath("nothing.dole")));
        QCOMPARE(other.message().value("kind").toString(), QString("openFailed"));
        QVERIFY(!other.saveTo(dir.filePath("no/such/folder.dole")));
        QCOMPARE(other.message().value("kind").toString(), QString("saveFailed"));
    }

    void samplesLoadOntoPads() {
        QTemporaryDir dir;
        QString error;
        QVERIFY(writeWav(dir.filePath("hit.wav"), *kitSound(1), &error));
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);

        picker.answer = dir.filePath("hit.wav");
        backend.chooseSample(0);
        QCOMPARE(backend.pads()[0].toMap().value("state").toString(), QString("loading"));
        QVERIFY(!engine.sounds[0]);
        QTRY_COMPARE(backend.pads()[0].toMap().value("state").toString(), QString("ready"));
        QCOMPARE(backend.pads()[0].toMap().value("sample").toString(), QString("hit"));
        QCOMPARE(engine.sounds[0]->size(), kitSound(1)->size());

        backend.resetPad(0);
        QVERIFY(engine.sounds[0] == kitSound(0));
        backend.undo();
        QCOMPARE(backend.pads()[0].toMap().value("state").toString(), QString("ready"));

        backend.setSample(1, dir.filePath("gone.wav"));
        QTRY_COMPARE(backend.pads()[1].toMap().value("state").toString(), QString("failed"));
        QCOMPARE(backend.message().value("kind").toString(), QString("sampleFailed"));
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
        // The demo song: four bars at 88 BPM, and its tail.
        const double seconds = (QFileInfo(dir.filePath("out.wav")).size() - 44) / 4.0 / kRate;
        QVERIFY2(seconds > 4 * 4 * 60.0 / 88 && seconds < 4 * 4 * 60.0 / 88 + 3, qPrintable(QString::number(seconds)));
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
        QTRY_COMPARE(mentions(hypr.filePath("calls"), "rounding"), 2);
        QTRY_COMPARE(theme.radius(), 4);

        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":10}"));
        QTest::qWait(300);
        QCOMPARE(mentions(hypr.filePath("calls"), "rounding"), 2);
        // Events arrive in pieces; only a whole configreloaded line counts.
        hyprland->write("activewindow>>kitty,~\nconfigrel");
        hyprland->flush();
        QTest::qWait(100);
        QCOMPARE(mentions(hypr.filePath("calls"), "rounding"), 2);
        hyprland->write("oaded>>\n");
        QTRY_COMPARE(theme.radius(), 10);
        QCOMPARE(mentions(hypr.filePath("calls"), "rounding"), 3);
    }

    void themeFollowsHyprlandsAnimations() {
        QTemporaryDir dir, hypr;
        const QString command = fakeHyprctl(hypr.path());
        // The fake answers every question alike: 0 is no rounding, and no animations.
        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":0}"));
        QLocalServer events;
        QVERIFY(events.listen(hypr.filePath("events.sock")));
        Theme theme(dir.path(), nullptr, command, events.fullServerName());
        QVERIFY(theme.motion());
        QTRY_VERIFY(!theme.motion());
        QVERIFY(mentions(hypr.filePath("calls"), "animations:enabled") >= 1);

        QTRY_VERIFY(events.hasPendingConnections());
        QLocalSocket *hyprland = events.nextPendingConnection();
        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":1}"));
        hyprland->write("configreloaded>>\n");
        hyprland->flush();
        QTRY_VERIFY(theme.motion());
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

    void lanesAreDrawnAndNudgedWithAFinger() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language");
        QCoreApplication::installTranslator(&translator);
        Theme theme(dir.path());
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);
        backend.newSong();
        QQmlApplicationEngine qml;
        QStringList warnings;
        connect(&qml, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        qml.rootContext()->setContextProperty("translator", &translator);
        qml.rootContext()->setContextProperty("theme", &theme);
        qml.rootContext()->setContextProperty("backend", &backend);
        Jam jam(&backend, &engine);
        jam.setTailscale(dir.path() + "/no-tailscale");
        qml.rootContext()->setContextProperty("jam", &jam);
        qml.load(QUrl("qrc:/Main.qml"));
        auto *window = qobject_cast<QQuickWindow *>(qml.rootObjects().first());
        QVERIFY(window);
        window->resize(1400, 900);
        QVERIFY(QTest::qWaitForWindowExposed(window));

        backend.selectPad(0);
        backend.addLane(0, "pan");
        QMetaObject::invokeMethod(window, "expand", Q_ARG(QVariant, 0));
        auto lane = [&]() -> QQuickItem * {
            std::function<QQuickItem *(QQuickItem *)> find = [&](QQuickItem *item) -> QQuickItem * {
                if (item->objectName() == "lane0_pan")
                    return item;
                for (QQuickItem *child : item->childItems())
                    if (QQuickItem *found = find(child))
                        return found;
                return nullptr;
            };
            return find(window->contentItem());
        };
        QTRY_VERIFY(lane());
        // Until the opened track has settled, its lanes aren't where they'll be.
        QTest::qWait(300);
        // A step's centre, and a height within the lane.
        auto at = [&](int step, double y) {
            QQuickItem *steps = lane()->property("steps").value<QQuickItem *>();
            const double cell = (steps->width() - 15 * 4) / 16;
            return steps->mapToScene(QPointF(step * (cell + 4) + cell / 2, y)).toPoint();
        };
        auto drag = [&](const QList<QPoint> &points) {
            QTest::mousePress(window, Qt::LeftButton, {}, points.first());
            for (const QPoint &point : points.mid(1)) {
                QMouseEvent move(QEvent::MouseMove, point, window->mapToGlobal(point), Qt::NoButton, Qt::LeftButton, {});
                QGuiApplication::sendEvent(window, &move);
                QTest::qWait(20);
            }
            QTest::mouseRelease(window, Qt::LeftButton, {}, points.last());
        };
        auto pan = [&](int step) { return backend.song().patterns[0].lanes.value(0).value("pan").value(step, std::nan("")); };

        // Sliding across the steps draws every one of them, though each
        // change redraws the lane under the finger.
        drag({at(2, 10), at(3, 10), at(4, 10), at(5, 10), at(6, 10)});
        for (int step = 2; step <= 6; ++step)
            QVERIFY2(pan(step) > 0.5, qPrintable(QString("step %1 is %2").arg(step + 1).arg(pan(step))));

        // Holding one step and moving up nudges it from where it was, a
        // little at a time: far less than the finger moved across the lane.
        const QPoint start = at(10, 40);
        drag({start, start - QPoint(0, 6), start - QPoint(0, 24), start - QPoint(0, 42)});
        // It starts from the value carried from the drawn steps before it.
        const double nudged = pan(10) - pan(6);
        QVERIFY2(nudged > 0.05 && nudged < 0.3, qPrintable(QString::number(nudged)));
        // One undo takes the nudge back.
        backend.undo();
        QVERIFY(std::isnan(pan(10)));

        // A tap only picks the step; it sets nothing.
        drag({at(12, 40)});
        QVERIFY(std::isnan(pan(12)));
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }

    void windowPlaysFromTheKeysAndTheScreen() {
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
        Jam jam(&backend, &engine);
        jam.setTailscale(dir.path() + "/no-tailscale");
        qml.rootContext()->setContextProperty("jam", &jam);
        qml.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!qml.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(qml.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));

        auto *status = window->findChild<QObject *>("status");
        QVERIFY(status);
        QVERIFY(status->property("text").toString().contains("Press ? for the shortcuts"));

        // The keys play the pads, pad 1 at the bottom left: Z, then 4 at the top right.
        QTest::keyPress(window, Qt::Key_Z);
        QCOMPARE(engine.calls.last(), QString("hit 0 3"));
        QTest::keyRelease(window, Qt::Key_Z);
        QTest::keyClick(window, Qt::Key_4, Qt::ShiftModifier);
        QVERIFY(engine.calls.contains("hit 15 4"));
        QCOMPARE(engine.calls.last(), QString("release 15")); // a gated pad would stop here
        QCOMPARE(backend.selectedPad(), 15);

        // Note repeat holds a pad while its key is down.
        QTest::keyClick(window, Qt::Key_N);
        QCOMPARE(backend.repeat(), 2);
        QTest::keyPress(window, Qt::Key_W);
        QCOMPARE(engine.calls.last(), QString("hold 9 1"));
        QTest::keyRelease(window, Qt::Key_W);
        QCOMPARE(engine.calls.last(), QString("hold 9 0"));
        QTest::keyClick(window, Qt::Key_N);

        // A finger on a pad strikes it on the press.
        auto *pad = findItem(window->contentItem(), "pad6");
        QVERIFY(pad);
        const QPoint centre = pad->mapToScene(QPointF(pad->width() / 2, pad->height() / 2)).toPoint();
        QTest::touchEvent(window, QTest::createTouchDevice()).press(0, centre, window);
        QTRY_COMPARE(engine.calls.last(), QString("hit 6 3"));
        QTest::touchEvent(window, QTest::createTouchDevice()).release(0, centre, window);

        // Space plays, Shift+Space records.
        QTest::keyClick(window, Qt::Key_Space);
        QVERIFY(backend.playing());
        QTest::keyClick(window, Qt::Key_Space, Qt::ShiftModifier);
        QVERIFY(backend.recording());
        QCOMPARE(status->property("text").toString(), QString("Recording: play the pads, they land on the nearest step"));
        QTest::keyClick(window, Qt::Key_Space);
        QVERIFY(!backend.playing());
        QVERIFY(!backend.recording());

        // Arrows and Enter edit the steps of the pad chosen.
        QCOMPARE(backend.selectedPad(), 6); // the pad last struck
        QTest::keyClick(window, Qt::Key_Up);
        QTest::keyClick(window, Qt::Key_Up);
        QCOMPARE(backend.selectedPad(), 4);
        QTest::keyClick(window, Qt::Key_Right);
        QTest::keyClick(window, Qt::Key_Right);
        QTest::keyClick(window, Qt::Key_Return);
        QVERIFY(backend.song().patterns[0].steps[4][2].on());
        QTest::keyClick(window, Qt::Key_Up, Qt::ShiftModifier);
        QCOMPARE(backend.song().patterns[0].steps[4][2].level, 4);
        auto *cell = findItem(window->contentItem(), "cell4_2");
        QVERIFY(cell);
        QCOMPARE(cell->property("color").value<QColor>().name(), theme.accent().name());
        QVERIFY(window->title().contains("•"));
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(backend.song().patterns[0].steps[4][2].level, 3);

        // Clicking a step turns it on too.
        auto *other = findItem(window->contentItem(), "cell1_9");
        QTest::mouseClick(window, Qt::LeftButton, {}, other->mapToScene(QPointF(4, 4)).toPoint());
        QVERIFY(backend.song().patterns[0].steps[1][9].on());

        // The pad's tune, mute, grit, tempo, and swing.
        QTest::keyClick(window, Qt::Key_Minus);
        QCOMPARE(backend.song().pads[1].tune, -1);
        QCOMPARE(findItem(window->contentItem(), "tune")->property("text").toString(), QString("-1"));
        QTest::keyClick(window, Qt::Key_M);
        QVERIFY(backend.song().pads[1].mute);
        QTest::keyClick(window, Qt::Key_G);
        QCOMPARE(backend.grit(), 2);
        QTest::keyClick(window, Qt::Key_BracketRight);
        QCOMPARE(window->findChild<QObject *>("bpm")->property("text").toString(), QString("89"));
        QTest::keyClick(window, Qt::Key_Period);
        QCOMPARE(backend.swing(), 59);

        // Ctrl and a number picks the pattern; in the song, it writes the chain.
        QTest::keyClick(window, Qt::Key_3, Qt::ControlModifier);
        QCOMPARE(backend.editPattern(), 2);
        const int length = int(backend.song().chain.size());
        QTest::keyClick(window, Qt::Key_Tab);
        for (int i = 0; i < length; ++i)
            QTest::keyClick(window, Qt::Key_Right);
        QTest::keyClick(window, Qt::Key_2, Qt::ControlModifier);
        QCOMPARE(backend.song().chain.size(), length + 1);
        QCOMPARE(backend.song().chain.last(), 1);
        QTest::keyClick(window, Qt::Key_Escape);

        auto *help = window->findChild<QObject *>("help");
        QTest::keyClick(window, '?');
        QTRY_VERIFY(help->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!help->property("visible").toBool());

        // A step's roll, chance, and nudge, from the keys and the step bar.
        QTest::keyClick(window, Qt::Key_Escape);
        QTest::keyClick(window, Qt::Key_1, Qt::ControlModifier);
        backend.selectPad(4);
        for (int i = 0; i < 7; ++i) // from step 10, clicked last, back to step 3
            QTest::keyClick(window, Qt::Key_Left);
        QTest::keyClick(window, Qt::Key_T);
        QCOMPARE(backend.song().patterns[0].steps[4][2].roll, 2);
        QTest::keyClick(window, Qt::Key_Y);
        QCOMPARE(backend.song().patterns[0].steps[4][2].chance, 75);
        QTest::keyClick(window, Qt::Key_Right, Qt::AltModifier);
        QCOMPARE(backend.song().patterns[0].steps[4][2].nudge, 5);
        QCOMPARE(findItem(window->contentItem(), "nudge")->property("text").toString(), QString("+5%"));

        // Two bars: the cursor goes on into the second, and the grid shows it.
        QTest::keyClick(window, Qt::Key_B, Qt::ControlModifier);
        QCOMPARE(backend.patternLength(), 32);
        QTest::keyClick(window, Qt::Key_Right, Qt::ControlModifier);
        QTest::keyClick(window, Qt::Key_Return);
        QVERIFY(backend.song().patterns[0].steps[4][18].on());
        QVERIFY(findItem(window->contentItem(), "cell4_18"));
        QCOMPARE(findItem(window->contentItem(), "barLabel")->property("text").toString(), QString("Bar 2 of 2"));

        // L on a track with no lanes asks which to add; then the lane opens
        // under the track, and the cursor is on it.
        auto *lanes = window->findChild<QObject *>("lanePicker");
        QTest::keyClick(window, Qt::Key_L);
        QTRY_VERIFY(lanes->property("opened").toBool());
        for (char c : QByteArray("filt"))
            QTest::keyClick(window, c);
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!lanes->property("visible").toBool());
        QVERIFY(backend.song().patterns[0].lanes[4].contains("filter"));
        QVERIFY(findItem(window->contentItem(), "lane4_filter"));
        QTest::keyClick(window, Qt::Key_Down, Qt::ShiftModifier);
        QCOMPARE(backend.song().patterns[0].lanes[4]["filter"][18], -0.1);
        QTest::keyClick(window, Qt::Key_Delete);
        QVERIFY(!backend.song().patterns[0].lanes[4]["filter"].contains(18));
        QVERIFY(backend.song().patterns[0].steps[4][18].on()); // the step is still there
        QTest::keyClick(window, Qt::Key_Down);
        QCOMPARE(backend.selectedPad(), 5);

        // B goes to the next bank: the same keys play its pads.
        QTest::keyClick(window, Qt::Key_B);
        QCOMPARE(backend.bank(), 1);
        QTest::keyPress(window, Qt::Key_Z);
        QVERIFY(engine.calls.contains("hit 16 3"));
        QTest::keyRelease(window, Qt::Key_Z);
        QVERIFY(findItem(window->contentItem(), "pad16"));
        QTest::keyClick(window, Qt::Key_B, Qt::ShiftModifier);
        QCOMPARE(backend.bank(), 0);

        // The mixer: a fader and a meter for every track of the bank.
        auto *mixer = window->findChild<QObject *>("mixer");
        QTest::keyClick(window, Qt::Key_M, Qt::ControlModifier);
        QTRY_VERIFY(mixer->property("opened").toBool());
        // A popup's content hangs off the window's overlay, inside its content item.
        QQuickItem *fader = findItem(window->contentItem(), "fader15");
        QVERIFY(fader);
        QVERIFY(findItem(window->contentItem(), "meter15"));
        QMetaObject::invokeMethod(fader, "increase");
        QMetaObject::invokeMethod(fader, "moved");
        QVERIFY(backend.song().pads[15].volume > 0.8);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!mixer->property("visible").toBool());

        // K opens the chop view, which asks for a loop; Escape closes it.
        auto *chop = window->findChild<QObject *>("chop");
        QVERIFY(chop);
        QTest::keyClick(window, Qt::Key_K);
        QTRY_VERIFY(chop->property("opened").toBool());
        QCOMPARE(picker.calls.last(), QString("open 1"));
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!chop->property("visible").toBool());

        // Quitting with unsaved work asks first, and Escape keeps everything.
        auto *confirm = window->findChild<QObject *>("confirm");
        QTest::keyClick(window, Qt::Key_Q, Qt::ControlModifier);
        QTRY_VERIFY(confirm->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!confirm->property("visible").toBool());
        QVERIFY(window->isVisible());

        // Saving from the question saves, then quits.
        QTemporaryDir songs;
        picker.answer = songs.filePath("keep.dole");
        QTest::keyClick(window, Qt::Key_Q, Qt::ControlModifier);
        QTRY_VERIFY(confirm->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!window->isVisible());
        QVERIFY(QFile::exists(songs.filePath("keep.dole")));
        QVERIFY(!backend.dirty());

        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QCoreApplication::removeTranslator(&translator);
    }

    void chopViewChopsALoop() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language");
        Theme theme(dir.path());
        FakeEngine engine;
        FakePicker picker;
        Backend backend(&engine, &picker);
        backend.openChop(writeClicks(dir.filePath("loop.wav"), 4));
        QTRY_COMPARE(backend.chopState(), QString("ready"));

        QQmlApplicationEngine qml;
        QStringList warnings;
        connect(&qml, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        qml.rootContext()->setContextProperty("translator", &translator);
        qml.rootContext()->setContextProperty("theme", &theme);
        qml.rootContext()->setContextProperty("backend", &backend);
        Jam jam(&backend, &engine);
        jam.setTailscale(dir.path() + "/no-tailscale");
        qml.rootContext()->setContextProperty("jam", &jam);
        qml.load(QUrl("qrc:/Main.qml"));
        auto *window = qobject_cast<QQuickWindow *>(qml.rootObjects().first());
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));

        auto *chop = window->findChild<QObject *>("chop");
        QTest::keyClick(window, Qt::Key_K);
        QTRY_VERIFY(chop->property("opened").toBool());
        // A loop is open already, so it doesn't ask for one.
        QVERIFY(picker.calls.isEmpty());
        QCOMPARE(findItem(window->contentItem(), "chopCount")
                     ->property("text").toString(), QString("4 chops"));
        QTest::keyClick(window, Qt::Key_Right);
        QCOMPARE(engine.calls.last(), QString("preview %1").arg(kRate / 2));
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!chop->property("visible").toBool());
        QCOMPARE(backend.song().pads[3].start, 1.5);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
    }

    void chainViewAddsEffects() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language");
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
        Jam jam(&backend, &engine);
        jam.setTailscale(dir.path() + "/no-tailscale");
        qml.rootContext()->setContextProperty("jam", &jam);
        qml.load(QUrl("qrc:/Main.qml"));
        auto *window = qobject_cast<QQuickWindow *>(qml.rootObjects().first());
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));

        backend.selectPad(3);
        auto *chain = window->findChild<QObject *>("chain");
        QTest::keyClick(window, Qt::Key_J);
        QTRY_VERIFY(chain->property("opened").toBool());
        QCOMPARE(backend.rackTarget(), 3);

        auto *add = findItem(window->contentItem(), "addEffect");
        QVERIFY(add);
        QTest::mouseClick(window, Qt::LeftButton, {}, add->mapToScene(QPointF(10, 10)).toPoint());
        auto *browser = window->findChild<QObject *>("browser");
        QTRY_VERIFY(browser->property("opened").toBool());
        QTest::keyClick(window, Qt::Key_Down); // Bitcrush
        QTest::keyClick(window, Qt::Key_Return);
        QTRY_VERIFY(!browser->property("visible").toBool());
        QCOMPARE(backend.song().pads[3].effects.size(), 1);
        QCOMPARE(backend.song().pads[3].effects.first().id, QString("bitcrush"));
        QVERIFY(findItem(window->contentItem(), "param_bits"));

        // Space turns it off, Delete takes it away.
        QTest::keyClick(window, Qt::Key_Space);
        QVERIFY(backend.song().pads[3].effects.first().bypass);
        QTest::keyClick(window, Qt::Key_Delete);
        QVERIFY(backend.song().pads[3].effects.isEmpty());
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!chain->property("visible").toBool());

        // Shift+J is the master's.
        QTest::keyClick(window, Qt::Key_J, Qt::ShiftModifier);
        QTRY_VERIFY(chain->property("opened").toBool());
        QCOMPARE(backend.rackTarget(), -1);
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
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
        Jam jam(&backend, &engine);
        jam.setTailscale(dir.path() + "/no-tailscale");
        qml.rootContext()->setContextProperty("jam", &jam);
        qml.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!qml.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(qml.rootObjects().first());
        QVERIFY(QTest::qWaitForWindowExposed(window));
        if (language != "ar")
            QVERIFY(!window->findChild<QObject *>("status")->property("text").toString().contains("Press ?"));
        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QGuiApplication::setLayoutDirection(Qt::LeftToRight);
        QLocale::setDefault(QLocale(QLocale::English));
        QCoreApplication::removeTranslator(&translator);
    }
};

QTEST_MAIN(DoleTests)
#include "dole_tests.moc"
