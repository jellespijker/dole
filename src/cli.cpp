// The command line follows Hype's (MIT, David Heinemeier Hansson).
#include "cli.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <cstdio>

#include "mcp.h"
#include "plugin.h"
#include "remote.h"
#include "rack.h"
#include "renderer.h"
#include "song.h"
#include "sounds.h"
#include "wav.h"

namespace {

void print(FILE *stream, const QString &text) {
    fprintf(stream, "%s\n", qPrintable(text));
}

void print(const QJsonObject &object) {
    fprintf(stdout, "%s", QJsonDocument(object).toJson().constData());
}

int fail(const QString &message) {
    print(stderr, message);
    return 1;
}

struct Command {
    QCommandLineParser parser;
    Command(const QString &name, const QString &description, bool song = true) {
        parser.setApplicationDescription(description);
        parser.addHelpOption();
        parser.addPositionalArgument(name, description, name);
        if (song)
            parser.addPositionalArgument("song", "A .dole song file", "<song>");
    }
    void json() { parser.addOption({"json", "Print the result as JSON"}); }
    bool wantsJson() const { return parser.isSet("json"); }
    QString argument(int index) const { return parser.positionalArguments().value(index); }
};

double seconds(const Song &song, int steps) {
    return steps * 60.0 / song.bpm / 4.0;
}

// Loads the song and decodes its samples; a problem anywhere is an error.
bool load(const QString &path, Song *song, std::array<SampleBuffer, kPads> *sounds, QStringList *problems) {
    if (path.isEmpty()) {
        *problems << "Name a .dole song file.";
        return false;
    }
    QString error;
    if (!loadSong(path, song, &error)) {
        *problems << path + ": " + error;
        return false;
    }
    QHash<QString, SampleBuffer> decoded;
    for (int p = 0; p < kPads; ++p) {
        const Pad &pad = song->pads[p];
        if (pad.sample.isEmpty()) {
            (*sounds)[p] = kitSound(p); // nothing, past bank A
            continue;
        }
        if (!decoded.contains(pad.sample))
            decoded.insert(pad.sample, decodeSample(pad.sample, &error));
        const SampleBuffer whole = decoded.value(pad.sample);
        if (!whole) {
            *problems << QString("Pad %1: %2: %3")
                             .arg(p + 1)
                             .arg(pad.sample, error == kNoFfmpeg ? "ffmpeg is not installed" : error);
            continue;
        }
        (*sounds)[p] = slice(whole, pad.start, pad.end);
        if (!(*sounds)[p])
            *problems << QString("Pad %1: the chop from %2 s to %3 s is outside the sample.").arg(p + 1).arg(pad.start).arg(pad.end);
    }
    return problems->isEmpty();
}

int create(const QStringList &arguments) {
    Command command("new", "Start a song: a dark boom-bap loop, or with --empty nothing at all.");
    command.parser.addOption({"empty", "Start with no steps"});
    command.json();
    command.parser.process(arguments);
    const QString path = command.argument(1);
    if (path.isEmpty())
        return fail("Name the song file to create, such as song.dole.");
    if (QFileInfo::exists(path))
        return fail(path + " already exists; leaving it alone.");
    QString error;
    if (!saveSong(path, command.parser.isSet("empty") ? Song{} : demoSong(), &error))
        return fail(path + ": " + error);
    if (command.wantsJson())
        print({{"song", QFileInfo(path).absoluteFilePath()}});
    else
        print(stdout, "Created " + path);
    return 0;
}

int check(const QStringList &arguments) {
    Command command("check", "Check a song and its samples, and summarize it.");
    command.json();
    command.parser.process(arguments);
    Song song;
    std::array<SampleBuffer, kPads> sounds;
    QStringList problems;
    bool ok = load(command.argument(1), &song, &sounds, &problems);
    if (ok) {
        // Every plugin the song names is made, to see that it can be.
        buildRack(song, &problems);
        ok = problems.isEmpty();
    }

    QStringList used;
    for (int p = 0; p < kPatterns; ++p)
        if (!song.patterns[p].isEmpty())
            used << patternName(p);
    QStringList chain;
    for (int p : song.chain)
        chain << patternName(p);
    QStringList warnings;
    for (int p : song.chain)
        if (song.patterns[p].isEmpty() && !warnings.contains("Pattern " + patternName(p) + " is empty."))
            warnings << "Pattern " + patternName(p) + " is empty.";
    if (song.chain.isEmpty())
        warnings << "The song chain is empty; render plays pattern A once.";
    // A pattern is one to four bars long.
    const int steps = songSteps(song, 0);
    const int bars = steps / kSteps;

    if (command.wantsJson()) {
        QJsonObject result{{"ok", ok},
                           {"errors", QJsonArray::fromStringList(problems)},
                           {"warnings", QJsonArray::fromStringList(ok ? warnings : QStringList())}};
        if (ok) {
            result.insert("bpm", song.bpm);
            result.insert("swing", song.swing);
            result.insert("grit", gritName(song.grit));
            result.insert("vinyl", song.vinyl);
            result.insert("patterns", QJsonArray::fromStringList(used));
            result.insert("song", QJsonArray::fromStringList(chain));
            result.insert("bars", bars);
            result.insert("seconds", qRound(seconds(song, steps) * 10) / 10.0);
        }
        print(result);
    } else {
        for (const QString &problem : problems)
            print(stderr, "error: " + problem);
        if (ok) {
            for (const QString &warning : warnings)
                print(stdout, "warning: " + warning);
            print(stdout, QString("%1 BPM, swing %2%, %3%4, patterns %5, song %6 (%7 bars, %8 s)")
                              .arg(song.bpm)
                              .arg(song.swing)
                              .arg(gritName(song.grit), song.vinyl ? " with vinyl" : "")
                              .arg(used.isEmpty() ? "none" : used.join(" "), chain.isEmpty() ? "empty" : chain.join(" "))
                              .arg(bars)
                              .arg(seconds(song, steps), 0, 'f', 1));
        }
    }
    return ok ? 0 : 1;
}

int render(const QStringList &arguments) {
    Command command("render", "Render a song to a 16-bit stereo WAV file.");
    command.parser.addOption({{"o", "output"}, "The WAV file to write", "file"});
    command.parser.addOption({"pattern", "Render one pattern instead of the song", "letter"});
    command.parser.addOption({"bars", "With --pattern, how many times to play it (1)", "count", "1"});
    command.json();
    command.parser.process(arguments);
    const QString output = command.parser.value("output");
    if (output.isEmpty())
        return fail("Name the WAV file to write with -o.");

    Song song;
    std::array<SampleBuffer, kPads> sounds;
    QStringList problems;
    if (!load(command.argument(1), &song, &sounds, &problems)) {
        for (const QString &problem : problems)
            print(stderr, problem);
        return 1;
    }
    if (command.parser.isSet("pattern")) {
        const int pattern = patternIndex(command.parser.value("pattern"));
        const int bars = command.parser.value("bars").toInt();
        if (pattern < 0)
            return fail("--pattern takes a letter from A to H.");
        if (bars < 1 || bars > kMaxChain)
            return fail(QString("--bars takes a count from 1 to %1.").arg(kMaxChain));
        song.chain = QVector<int>(bars, pattern);
    }

    const Rack rack = buildRack(song, &problems);
    if (!problems.isEmpty()) {
        for (const QString &problem : problems)
            print(stderr, problem);
        return 1;
    }
    const QVector<float> audio = renderSong(song, sounds, rack, 0);
    QString error;
    if (!writeWav(output, audio, &error))
        return fail(output + ": " + error);
    const double length = audio.size() / 2.0 / kRate;
    if (command.wantsJson())
        print({{"output", QFileInfo(output).absoluteFilePath()}, {"seconds", qRound(length * 10) / 10.0}});
    else
        print(stdout, QString("Wrote %1 (%2 s)").arg(output).arg(length, 0, 'f', 1));
    return 0;
}

int plugins(const QStringList &arguments) {
    Command command("plugins", "List the effects and synths dole can load: its own, CLAP, and LV2.", false);
    command.json();
    command.parser.process(arguments);
    const QVector<PluginInfo> found = scanPlugins();
    if (command.wantsJson()) {
        QJsonArray list;
        for (const PluginInfo &info : found)
            list.append(QJsonObject{{"id", info.id}, {"name", info.name}, {"vendor", info.vendor},
                                    {"format", info.format}, {"instrument", info.instrument}});
        print({{"plugins", list}});
    } else {
        for (const PluginInfo &info : found)
            print(stdout, QString("%1  %2%3  (%4)")
                              .arg(info.instrument ? "synth " : "effect", info.name,
                                   info.vendor.isEmpty() || info.vendor == "dole" ? QString() : ", " + info.vendor, info.id));
    }
    return 0;
}

// As Hype does: one shared copy, linked into Claude Code, found directly by Codex.
int skill(const QStringList &arguments) {
    Command command("skill", "Print the agent skill, or install it for your coding agents with: skill install.", false);
    command.parser.addPositionalArgument("install", "Copy to ~/.agents/skills/dole and link into ~/.claude/skills",
                                         "[install]");
    command.parser.process(arguments);
    QFile bundled(":/skill.md");
    if (!bundled.open(QIODevice::ReadOnly))
        return 1;
    const QByteArray text = bundled.readAll();
    if (command.argument(1).isEmpty()) {
        fputs(text.constData(), stdout);
        return 0;
    }
    if (command.argument(1) != "install")
        return fail("Use dole skill to print the skill, or dole skill install.");
    const QDir home = QDir::home();
    const QString directory = home.filePath(".agents/skills/dole");
    QSaveFile file(directory + "/SKILL.md");
    if (!QDir().mkpath(directory) || !file.open(QIODevice::WriteOnly) || file.write(text) < 0 || !file.commit())
        return fail("Could not write " + file.fileName());
    print(stdout, "Installed " + file.fileName());
    if (!home.exists(".claude"))
        return 0;
    const QString link = home.filePath(".claude/skills/dole");
    const QFileInfo existing(link);
    if (existing.isSymLink())
        QFile::remove(link);
    else if (existing.exists())
        return fail(link + " exists and is not a link; leaving it alone.");
    if (!home.mkpath(".claude/skills") || !QFile::link("../../.agents/skills/dole", link))
        return fail("Could not link " + link);
    print(stdout, "Linked " + link);
    return 0;
}

// One request to the open window. The common ones have short forms
// (dole remote play); any other takes its parameters as JSON, or - to read
// them from stdin.
int remote(const QStringList &arguments) {
    Command command("remote", "Play the open window: dole remote <request> [JSON parameters, or - for stdin].", false);
    command.parser.addPositionalArgument("request",
                                         "status, song, edit, play, stop, record, select, hit, undo, redo, save, "
                                         "wait, jam, or watch",
                                         "<request>");
    command.parser.addPositionalArgument("parameters", "A JSON object, or - to read it from stdin", "[parameters]");
    command.parser.addPositionalArgument("mode", "With jam <machine>: song to share its song", "[song]");
    command.parser.process(arguments);
    QString method = command.argument(1);
    const QString given = command.argument(2);
    if (method.isEmpty())
        return fail("Name a request, such as dole remote status; dole help remote lists them.");
    if (method == "watch")
        return remoteWatch();

    QJsonObject params;
    if (method == "play" || method == "stop" || method == "record") {
        params.insert("action", method);
        method = "transport";
    } else if (method == "hit" && !given.isEmpty() && !given.startsWith('{')) {
        params.insert("pad", given);
    } else if (method == "select" && !given.isEmpty() && !given.startsWith('{')) {
        params.insert(patternIndex(given) >= 0 ? "pattern" : "pad", given);
    } else if (method == "jam" && !given.isEmpty() && !given.startsWith('{')) {
        // dole remote jam search, jam leave, or jam <machine> [song].
        if (given == "search" || given == "leave")
            params.insert(given, true);
        else {
            params.insert("join", given);
            params.insert("mode", command.argument(3) == "song" ? "song" : "clock");
        }
    } else if (method == "wait" && !given.isEmpty() && !given.startsWith('{')) {
        params.insert("until", given);
    } else if (!given.isEmpty()) {
        QByteArray text = given.toUtf8();
        if (given == "-") {
            QFile in;
            if (!in.open(stdin, QIODevice::ReadOnly))
                return fail("Could not read stdin.");
            text = in.readAll();
        }
        QJsonParseError parse;
        const QJsonDocument document = QJsonDocument::fromJson(text, &parse);
        if (!document.isObject())
            return fail("The parameters are a JSON object: " + parse.errorString());
        params = document.object();
    }
    if (method == "edit")
        params.insert("base", QDir::currentPath());
    const int timeout = method == "wait" ? (params.value("timeout").toInt(30) + 5) * 1000 : method == "jam" ? 15000 : 10000;
    const RemoteReply reply = remoteRequest(method, params, timeout);
    if (!reply.ok)
        return fail(reply.error);
    print(reply.result);
    return 0;
}

int help(const QStringList &arguments) {
    const QString topic = arguments.value(2);
    if (topic == "format") {
        QFile guide(":/format.md");
        if (!guide.open(QIODevice::ReadOnly))
            return 1;
        fputs(guide.readAll().constData(), stdout);
        return 0;
    }
    if (isCliCommand(topic) && topic != "help")
        return runCli({arguments[0], topic, "--help"});
    print(stdout, "Usage: dole <command> [options]\n\n" + cliSummary());
    return 0;
}

} // namespace

QString cliSummary() {
    return "Sampler and sequencer:\n"
           "  open [song]                   Open the window, on a song or a demo loop\n\n"
           "Commands that need no display (dole help <command> for options):\n"
           "  new <song>                    Start a song file\n"
           "  check <song>                  Report every problem, and summarize the song\n"
           "  render <song> -o <file.wav>   Render the song, or one pattern, to WAV\n"
           "  plugins                       List the effects and synths to load\n"
           "  remote <request>              Play the open window: edit it, start it, wait for the bar\n"
           "  mcp                           Serve the open window to coding agents over MCP\n"
           "  help format                   How a .dole song file is written\n"
           "  skill [install]               Print the skill for coding agents, or install it";
}

bool isCliCommand(const QString &word) {
    return QStringList{"new", "check", "render", "plugins", "remote", "mcp", "skill", "help"}.contains(word);
}

int runCli(const QStringList &arguments) {
    const QString command = arguments.value(1);
    return command == "new"      ? create(arguments)
           : command == "check"  ? check(arguments)
           : command == "render" ? render(arguments)
           : command == "plugins" ? plugins(arguments)
           : command == "remote" ? remote(arguments)
           : command == "mcp"    ? runMcp()
           : command == "skill"  ? skill(arguments)
                                 : help(arguments);
}
