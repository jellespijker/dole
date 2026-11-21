#include "remote.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>
#include <cstdio>

#include "backend.h"
#include "jam.h"

namespace {

// A request bigger than any song could be is someone else's data, not dole's.
constexpr qint64 kMaxRequest = 16 * 1024 * 1024;
constexpr int kMaxWaitSeconds = 300;

QByteArray line(const QJsonObject &message) {
    return QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
}

QString noWindow() {
    return "No dole window is open. Open one with dole open, or ask the person to.";
}

// The open windows' sockets, newest first.
QStringList sockets() {
    const QFileInfoList found =
        QDir(remoteDirectory()).entryInfoList({"*.sock"}, QDir::System | QDir::Files, QDir::Time);
    QStringList paths;
    for (const QFileInfo &info : found)
        paths << info.absoluteFilePath();
    return paths;
}

// Connects to the newest window that answers. A socket left behind by a
// window that didn't close cleanly answers nothing, and is cleared away.
bool connectToWindow(QLocalSocket *socket) {
    for (const QString &path : sockets()) {
        socket->connectToServer(path);
        if (socket->waitForConnected(1000))
            return true;
        if (socket->error() == QLocalSocket::ConnectionRefusedError || socket->error() == QLocalSocket::ServerNotFoundError)
            QFile::remove(path);
        socket->abort();
    }
    return false;
}

} // namespace

QString remoteDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + "/dole";
}

RemoteServer::RemoteServer(Backend *backend, QObject *parent) : QObject(parent), m_backend(backend) {
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_server, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *socket = m_server.nextPendingConnection())
            serve(socket);
    });

    connect(m_backend, &Backend::positionChanged, this, [this] {
        const int step = m_backend->playStep();
        if (step < 0)
            return;
        finish([step](const Wait &wait) {
            return wait.until == "step" || (wait.until == "bar" && step % kSteps == 0)
                   || (wait.until == "loop" && step == 0);
        });
        broadcast({{"event", "position"}, {"position", status().value("position")}});
    });
    connect(m_backend, &Backend::playingChanged, this, [this] {
        if (!m_backend->playing()) {
            finish([](const Wait &wait) { return wait.until == "stop"; });
            finish([](const Wait &wait) { return wait.until == "step" || wait.until == "bar" || wait.until == "loop"; },
                   "Playing stopped.");
        }
        broadcast({{"event", "playing"}, {"playing", m_backend->playing()}, {"recording", m_backend->recording()}});
    });
    connect(m_backend, &Backend::songChanged, this, [this] {
        if (!m_editing)
            finish([this](const Wait &wait) { return wait.until == "edit" && wait.song != m_backend->song(); });
        broadcast({{"event", "song"}, {"byRequest", m_editing}, {"dirty", m_backend->dirty()}});
    });
    connect(m_backend, &Backend::gridChanged, this, [this] {
        broadcast({{"event", "editing"}, {"pattern", patternName(m_backend->editPattern())}});
    });
}

RemoteServer::~RemoteServer() {
    // Closing removes the socket's file, so no client finds a window that's gone.
    m_server.close();
}

bool RemoteServer::listen(const QString &directory, QString *error) {
    if (!QDir().mkpath(directory)
        || !QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
        *error = "Could not make " + directory;
        return false;
    }
    const QString path = directory + "/" + QString::number(QCoreApplication::applicationPid()) + ".sock";
    QLocalServer::removeServer(path);
    if (!m_server.listen(path)) {
        *error = path + ": " + m_server.errorString();
        return false;
    }
    return true;
}

void RemoteServer::serve(QLocalSocket *socket) {
    connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
        m_watchers.remove(socket);
        socket->deleteLater();
    });
    connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
        while (socket->canReadLine()) {
            const QByteArray text = socket->readLine();
            QJsonParseError parse;
            const QJsonDocument document = QJsonDocument::fromJson(text, &parse);
            if (!document.isObject()) {
                fail(socket, QJsonValue(), "A request is a JSON object on one line: " + parse.errorString());
                continue;
            }
            handle(socket, document.object());
        }
        if (socket->bytesAvailable() > kMaxRequest)
            socket->abort();
    });
}

QString RemoteServer::songDirectory() const {
    return m_backend->path().isEmpty() ? QString() : QFileInfo(m_backend->path()).absolutePath();
}

QJsonObject RemoteServer::status() const {
    const Song &song = m_backend->song();
    QJsonValue position;
    if (m_backend->playing() && m_backend->playPattern() >= 0) {
        QJsonObject at{{"pattern", patternName(m_backend->playPattern())},
                       {"step", m_backend->playStep() + 1},
                       {"bar", m_backend->playStep() / kSteps + 1}};
        if (m_backend->songMode() && m_backend->playChainIndex() >= 0)
            at.insert("slot", m_backend->playChainIndex() + 1);
        position = at;
    }
    QJsonArray chain;
    for (int pattern : song.chain)
        chain.append(patternName(pattern));
    QJsonValue jam;
    if (m_jam && m_jam->role() != "alone") {
        QJsonArray others;
        for (const QVariant &member : m_jam->members())
            others.append(member.toMap().value("name").toString());
        jam = QJsonObject{{"role", m_jam->role()},
                          {"mode", m_jam->role() == "hosting" ? QJsonValue() : QJsonValue(m_jam->mode())},
                          {"host", m_jam->role() == "hosting" ? m_jam->selfName() : m_jam->hostName()},
                          {"members", others},
                          {"latency", m_jam->latency() < 0 ? QJsonValue() : QJsonValue(m_jam->latency())}};
    }
    return {{"jam", jam},
            {"file", m_backend->path().isEmpty() ? QJsonValue() : QJsonValue(m_backend->path())},
            {"dirty", m_backend->dirty()},
            {"playing", m_backend->playing()},
            {"recording", m_backend->recording()},
            {"plays", m_backend->songMode() ? "song" : "pattern"},
            {"bpm", song.bpm},
            {"swing", song.swing},
            {"editing", patternName(m_backend->editPattern())},
            {"length", m_backend->patternLength()},
            {"pad", padName(m_backend->selectedPad())},
            {"song", chain},
            {"position", position},
            {"canUndo", m_backend->canUndo()},
            {"canRedo", m_backend->canRedo()}};
}

void RemoteServer::send(QLocalSocket *socket, const QJsonObject &message) {
    if (socket && socket->state() == QLocalSocket::ConnectedState)
        socket->write(line(message));
}

void RemoteServer::reply(QLocalSocket *socket, const QJsonValue &id, const QJsonObject &result) {
    send(socket, {{"id", id}, {"result", result}});
}

void RemoteServer::fail(QLocalSocket *socket, const QJsonValue &id, const QString &message) {
    send(socket, {{"id", id}, {"error", message}});
}

void RemoteServer::broadcast(const QJsonObject &event) {
    for (QLocalSocket *socket : std::as_const(m_watchers))
        send(socket, event);
}

void RemoteServer::finish(const std::function<bool(const Wait &)> &done, const QString &error) {
    QList<Wait> finished;
    for (auto it = m_waits.begin(); it != m_waits.end();) {
        if (done(*it)) {
            finished << *it;
            it = m_waits.erase(it);
        } else {
            ++it;
        }
    }
    for (const Wait &wait : std::as_const(finished)) {
        wait.timer->stop();
        wait.timer->deleteLater();
        if (error.isEmpty())
            reply(wait.socket, wait.id, {{"until", wait.until}, {"timedOut", false}, {"status", status()}});
        else
            fail(wait.socket, wait.id, error);
    }
}

void RemoteServer::edit(const Song &next) {
    m_editing = true;
    m_backend->editSong(next);
    m_editing = false;
}

void RemoteServer::handle(QLocalSocket *socket, const QJsonObject &request) {
    const QJsonValue id = request.value("id");
    const QString method = request.value("method").toString();
    const QJsonObject params = request.value("params").toObject();

    if (method == "status") {
        reply(socket, id, status());
    } else if (method == "song") {
        reply(socket, id, {{"song", songToJson(m_backend->song(), songDirectory())}, {"status", status()}});
    } else if (method == "edit") {
        // Samples named relative to the song are found beside it; in a song
        // not saved yet, beside where the request came from.
        const QString base = m_backend->path().isEmpty() ? params.value("base").toString() : songDirectory();
        QJsonObject json;
        if (params.value("song").isObject()) {
            json = params.value("song").toObject();
            if (!json.contains("dole"))
                json.insert("dole", 3);
        } else if (params.value("patch").isObject()) {
            json = patchSongJson(songToJson(m_backend->song(), songDirectory()), params.value("patch").toObject());
        } else {
            fail(socket, id, "edit takes a \"patch\" (part of a song, merged in) or a \"song\" (the whole of one).");
            return;
        }
        Song next;
        QString error;
        if (!songFromJson(json, base, &next, &error)) {
            fail(socket, id, error);
            return;
        }
        const bool changed = next != m_backend->song();
        edit(next);
        reply(socket, id, {{"changed", changed}, {"status", status()}});
    } else if (method == "transport") {
        const QString action = params.value("action").toString();
        const QString plays = params.value("plays").toString();
        const int from = params.value("from").toInt(1) - 1;
        if (!plays.isEmpty() && plays != "song" && plays != "pattern") {
            fail(socket, id, "plays is \"song\" or \"pattern\".");
            return;
        }
        if (from < 0 || (from > 0 && from >= m_backend->song().chain.size())) {
            fail(socket, id, QString("from is a slot of the song, 1 to %1.").arg(std::max<qsizetype>(1, m_backend->song().chain.size())));
            return;
        }
        if (!QStringList{"play", "stop", "record", ""}.contains(action)) {
            fail(socket, id, "action is play, stop, or record.");
            return;
        }
        if (!plays.isEmpty() && (plays == "song") != m_backend->songMode())
            m_backend->toggleSongMode();
        if (action == "play" && !m_backend->playing())
            m_backend->togglePlay(from);
        else if (action == "record" && !m_backend->recording())
            m_backend->toggleRecord(from);
        else if (action == "stop")
            m_backend->stop();
        reply(socket, id, status());
    } else if (method == "select") {
        if (params.contains("pattern")) {
            const int pattern = patternIndex(params.value("pattern").toString());
            if (pattern < 0) {
                fail(socket, id, "pattern is a letter from A to H.");
                return;
            }
            m_backend->selectPattern(pattern);
        }
        if (params.contains("pad")) {
            const int pad = padFromKey(params.value("pad").toVariant().toString());
            if (pad < 0) {
                fail(socket, id, "pad is 1 to 64, A1 to D16, or a kit sound's name.");
                return;
            }
            m_backend->selectPad(pad);
        }
        reply(socket, id, status());
    } else if (method == "hit") {
        const int pad = padFromKey(params.value("pad").toVariant().toString());
        const int level = params.value("level").toInt(kDefaultLevel);
        if (pad < 0 || level < 1 || level > 4) {
            fail(socket, id, "hit takes a pad (1 to 64, A1 to D16, or a kit sound's name) and a level from 1 to 4.");
            return;
        }
        m_backend->padDown(pad, level);
        // Let go as a finger would: after the pad's note length, so a gated
        // sample or a synth's note is heard for as long as a step plays it.
        const double stepMs = 60000.0 / m_backend->song().bpm / 4;
        QTimer::singleShot(int(stepMs * m_backend->song().pads[pad].length), m_backend, [this, pad] { m_backend->padUp(pad); });
        reply(socket, id, status());
    } else if (method == "undo" || method == "redo") {
        const bool undo = method == "undo";
        if (undo ? !m_backend->canUndo() : !m_backend->canRedo()) {
            fail(socket, id, undo ? "Nothing to undo." : "Nothing to redo.");
            return;
        }
        m_editing = true;
        undo ? m_backend->undo() : m_backend->redo();
        m_editing = false;
        reply(socket, id, status());
    } else if (method == "save") {
        if (m_backend->path().isEmpty()) {
            fail(socket, id, "The song has no file yet. Ask the person to save it with Ctrl+S, which asks where.");
            return;
        }
        if (!m_backend->saveTo(m_backend->path())) {
            fail(socket, id, "Saving failed; the window says why.");
            return;
        }
        reply(socket, id, status());
    } else if (method == "wait") {
        const QString until = params.value("until").toString();
        const int timeout = std::clamp(params.value("timeout").toInt(30), 1, kMaxWaitSeconds);
        if (!QStringList{"step", "bar", "loop", "edit", "stop"}.contains(until)) {
            fail(socket, id, "until is step, bar, loop, edit, or stop.");
            return;
        }
        const bool timed = until == "step" || until == "bar" || until == "loop" || until == "stop";
        if (timed && !m_backend->playing()) {
            fail(socket, id, "Nothing is playing; start it with transport play.");
            return;
        }
        Wait wait{socket, id, until, m_backend->song(), new QTimer(this)};
        wait.timer->setSingleShot(true);
        QTimer *timer = wait.timer;
        connect(timer, &QTimer::timeout, this, [this, timer] {
            for (int i = 0; i < m_waits.size(); ++i) {
                if (m_waits[i].timer != timer)
                    continue;
                const Wait done = m_waits.takeAt(i);
                reply(done.socket, done.id, {{"until", done.until}, {"timedOut", true}, {"status", status()}});
                break;
            }
            timer->deleteLater();
        });
        timer->start(timeout * 1000);
        m_waits << wait;
    } else if (method == "jam") {
        if (!m_jam) {
            fail(socket, id, "This window can't jam.");
            return;
        }
        auto found = [this] {
            QJsonArray list;
            for (const QVariant &dole : m_jam->found())
                list.append(QJsonObject::fromVariantMap(dole.toMap()));
            return list;
        };
        auto answer = [this, socket = QPointer<QLocalSocket>(socket), id, found] {
            QJsonObject result{{"status", status()}, {"found", found()}};
            if (!m_jam->problem().isEmpty())
                result.insert("problem", m_jam->problem() + (m_jam->problemName().isEmpty() ? QString() : ": " + m_jam->problemName()));
            reply(socket, id, result);
        };
        if (params.value("leave").toBool()) {
            m_jam->leave();
            answer();
        } else if (params.contains("join")) {
            // A machine by its name from the last search, or its address.
            QString address = params.value("join").toString();
            for (const QVariant &dole : m_jam->found())
                if (dole.toMap().value("name").toString().compare(address, Qt::CaseInsensitive) == 0)
                    address = dole.toMap().value("address").toString();
            m_jam->join(address, params.value("mode").toString() == "song" ? "song" : "clock");
            // Answered once it's in, or it's clear it won't be.
            auto *watch = new QObject(this);
            connect(m_jam, &Jam::changed, watch, [this, watch, answer] {
                if (m_jam->role() == "joining" || (m_jam->role() == "joined" && m_jam->latency() < 0))
                    return;
                answer();
                watch->deleteLater();
            });
        } else if (params.value("search").toBool()) {
            m_jam->search();
            auto *watch = new QObject(this);
            connect(m_jam, &Jam::foundChanged, watch, [this, watch, answer] {
                if (m_jam->searching())
                    return;
                answer();
                watch->deleteLater();
            });
        } else {
            answer();
        }
    } else if (method == "watch") {
        m_watchers.insert(socket);
        reply(socket, id, status());
    } else {
        fail(socket, id, "Unknown method " + method
                             + "; the methods are status, song, edit, transport, select, hit, undo, redo, save, wait, jam, and watch.");
    }
}

RemoteReply remoteRequest(const QString &method, const QJsonObject &params, int timeoutMs) {
    RemoteReply answer;
    QLocalSocket socket;
    if (!connectToWindow(&socket)) {
        answer.error = noWindow();
        return answer;
    }
    socket.write(line({{"id", 1}, {"method", method}, {"params", params}}));
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        while (socket.canReadLine()) {
            const QJsonObject message = QJsonDocument::fromJson(socket.readLine()).object();
            if (message.value("id") != QJsonValue(1))
                continue; // an event, not the answer
            answer.ok = !message.contains("error");
            answer.result = message.value("result").toObject();
            answer.error = message.value("error").toString();
            return answer;
        }
        if (socket.state() != QLocalSocket::ConnectedState) {
            answer.error = "The dole window closed before it answered.";
            return answer;
        }
        socket.waitForReadyRead(int(std::max<qint64>(1, timeoutMs - clock.elapsed())));
    }
    answer.error = "The dole window didn't answer in time.";
    return answer;
}

int remoteWatch() {
    QLocalSocket socket;
    if (!connectToWindow(&socket)) {
        fprintf(stderr, "%s\n", qPrintable(noWindow()));
        return 1;
    }
    socket.write(line({{"id", 1}, {"method", "watch"}}));
    while (socket.state() == QLocalSocket::ConnectedState) {
        while (socket.canReadLine()) {
            fputs(socket.readLine().constData(), stdout);
            fflush(stdout);
        }
        socket.waitForReadyRead(-1);
    }
    return 0;
}
