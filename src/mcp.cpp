#include "mcp.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <iostream>
#include <string>

#include "remote.h"

namespace {

// The tools, as MCP lists them. Each but format is one request to the open
// window over its control socket (see remote.h).
const char *kTools = R"json([
  {
    "name": "status",
    "description": "What the open dole window is doing: its file, whether it plays the pattern being edited or the song, the tempo, the pattern on screen, the selected pad, and, while playing, the pattern, step (from 1), and bar under the playhead.",
    "inputSchema": {"type": "object", "properties": {}}
  },
  {
    "name": "get_song",
    "description": "The song open in the window, as .dole JSON (the format tool describes it), with its status. Read it before editing, since the person may have changed it by hand.",
    "inputSchema": {"type": "object", "properties": {}}
  },
  {
    "name": "edit_song",
    "description": "Change the song in the window while it plays, heard from the next step. Pass a patch, part of a .dole song merged in as JSON Merge Patch: objects merge, anything else replaces, null removes. Pads and rows can be named 7, A7, or by kit sound (hat); a row given as an object keeps its steps, so {\"patterns\": {\"A\": {\"hat\": {\"roll\": {\"16\": 3}}}}} adds a roll. Or pass song, a whole song, to replace it. Either is one undoable step, so Ctrl+Z in the window takes it back. Nothing is saved to disk.",
    "inputSchema": {
      "type": "object",
      "properties": {
        "patch": {"type": "object", "description": "Part of a .dole song to merge into the open one."},
        "song": {"type": "object", "description": "A whole .dole song to replace the open one."}
      }
    }
  },
  {
    "name": "transport",
    "description": "Play, record, or stop. plays picks whether Space plays the pattern on screen or the song's chain; from starts the song at one of its slots.",
    "inputSchema": {
      "type": "object",
      "properties": {
        "action": {"type": "string", "enum": ["play", "stop", "record"]},
        "plays": {"type": "string", "enum": ["pattern", "song"]},
        "from": {"type": "integer", "minimum": 1, "description": "The song slot to start from, counting from 1."}
      }
    }
  },
  {
    "name": "select",
    "description": "Show a pattern in the window's grid and choose a pad, so the person sees what you're working on. While the pattern plays (not the song), the pattern shown is the one heard, from the next bar.",
    "inputSchema": {
      "type": "object",
      "properties": {
        "pattern": {"type": "string", "description": "A to H."},
        "pad": {"type": "string", "description": "1 to 64, A1 to D16, or a kit sound such as boom or hat."}
      }
    }
  },
  {
    "name": "hit_pad",
    "description": "Strike a pad now, to audition its sound. It isn't in time; to play in time, write steps with edit_song. While recording, the hit is recorded on the nearest step.",
    "inputSchema": {
      "type": "object",
      "properties": {
        "pad": {"type": "string", "description": "1 to 64, A1 to D16, or a kit sound such as boom."},
        "level": {"type": "integer", "minimum": 1, "maximum": 4, "description": "How hard: 1 soft to 4 accent. Default 3."}
      },
      "required": ["pad"]
    }
  },
  {
    "name": "undo",
    "description": "Undo the last change to the song, yours or the person's, or redo one.",
    "inputSchema": {"type": "object", "properties": {"redo": {"type": "boolean"}}}
  },
  {
    "name": "wait",
    "description": "Wait for the music: the next step, the next bar, the pattern coming round to its start (loop), playing stopping, or the person editing the song by hand. Use it to land a change on the bar, or to follow along. Returns the status, or timedOut.",
    "inputSchema": {
      "type": "object",
      "properties": {
        "until": {"type": "string", "enum": ["step", "bar", "loop", "stop", "edit"]},
        "timeout": {"type": "integer", "minimum": 1, "maximum": 300, "description": "Seconds; default 30."}
      },
      "required": ["until"]
    }
  },
  {
    "name": "save",
    "description": "Save the song to the file it was opened from. A song with no file yet can't be: the person picks where with Ctrl+S.",
    "inputSchema": {"type": "object", "properties": {}}
  },
  {
    "name": "format",
    "description": "How a .dole song is written: tempo, pads and their settings, the kit on each pad, patterns, rows, rolls, chance, nudges, lanes, plugins, and the song chain. Read it before the first edit.",
    "inputSchema": {"type": "object", "properties": {}}
  }
])json";

const char *kInstructions =
    "Plays the dole window that is open on this desktop, live. Read the format once, then get_song, "
    "and change the song with edit_song: each change is heard from the next step and undoes with "
    "Ctrl+Z in the window. Replies take seconds, so play by writing steps, not by hitting pads in "
    "time; wait for the bar or the loop to land a change on the beat. Nothing is saved unless you "
    "call save.";

void write(const QJsonObject &message) {
    const QByteArray text = QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    fwrite(text.constData(), 1, size_t(text.size()), stdout);
    fflush(stdout);
}

void answer(const QJsonValue &id, const QJsonObject &result) {
    write({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
}

void refuse(const QJsonValue &id, int code, const QString &message) {
    write({{"jsonrpc", "2.0"}, {"id", id}, {"error", QJsonObject{{"code", code}, {"message", message}}}});
}

QJsonObject text(const QString &body, bool error) {
    return {{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", body}}}}, {"isError", error}};
}

QJsonObject call(const QString &tool, const QJsonObject &arguments) {
    if (tool == "format") {
        QFile guide(":/format.md");
        return guide.open(QIODevice::ReadOnly) ? text(QString::fromUtf8(guide.readAll()), false)
                                               : text("The format guide is missing from this build.", true);
    }
    QString method;
    QJsonObject params = arguments;
    int timeout = 10000;
    if (tool == "status")
        method = "status";
    else if (tool == "get_song")
        method = "song";
    else if (tool == "edit_song") {
        method = "edit";
        // A sample named relative to a song not saved yet is found from here.
        params.insert("base", QDir::currentPath());
    } else if (tool == "transport")
        method = "transport";
    else if (tool == "select")
        method = "select";
    else if (tool == "hit_pad")
        method = "hit";
    else if (tool == "undo")
        method = arguments.value("redo").toBool() ? "redo" : "undo";
    else if (tool == "save")
        method = "save";
    else if (tool == "wait") {
        method = "wait";
        timeout = (arguments.value("timeout").toInt(30) + 5) * 1000;
    } else
        return text("There is no tool " + tool + ".", true);

    const RemoteReply reply = remoteRequest(method, params, timeout);
    if (!reply.ok)
        return text(reply.error, true);
    return text(QString::fromUtf8(QJsonDocument(reply.result).toJson(QJsonDocument::Indented)), false);
}

} // namespace

int runMcp() {
    const QStringList versions{"2025-06-18", "2025-03-26", "2024-11-05"};
    std::string input;
    while (std::getline(std::cin, input)) {
        QJsonParseError parse;
        const QJsonDocument document = QJsonDocument::fromJson(QByteArray::fromStdString(input), &parse);
        if (!document.isObject()) {
            if (!QByteArray::fromStdString(input).trimmed().isEmpty())
                refuse(QJsonValue(), -32700, parse.errorString());
            continue;
        }
        const QJsonObject message = document.object();
        const QString method = message.value("method").toString();
        const QJsonObject params = message.value("params").toObject();
        // Notifications (initialized, cancelled) have no id and want no answer.
        if (!message.contains("id"))
            continue;
        const QJsonValue id = message.value("id");
        if (method == "initialize") {
            const QString asked = params.value("protocolVersion").toString();
            answer(id, {{"protocolVersion", versions.contains(asked) ? asked : versions.first()},
                        {"capabilities", QJsonObject{{"tools", QJsonObject{}}}},
                        {"serverInfo", QJsonObject{{"name", "dole"}, {"version", DOLE_VERSION}}},
                        {"instructions", kInstructions}});
        } else if (method == "ping") {
            answer(id, {});
        } else if (method == "tools/list") {
            answer(id, {{"tools", QJsonDocument::fromJson(kTools).array()}});
        } else if (method == "tools/call") {
            answer(id, call(params.value("name").toString(), params.value("arguments").toObject()));
        } else {
            refuse(id, -32601, "dole has no " + method + ".");
        }
    }
    return 0;
}
