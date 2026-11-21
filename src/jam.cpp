#include "jam.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTcpSocket>
#include <algorithm>
#include <cmath>

#include "backend.h"

namespace {

// A message bigger than a song with a sample in it is no dole's.
constexpr qint64 kMaxMessage = 96 * 1024 * 1024;
// Samples over this aren't shared; a song can hold 120 seconds of one.
constexpr qint64 kMaxSample = 64 * 1024 * 1024;
constexpr int kProtocol = 1;

bool sameAddress(const QString &a, const QString &b) {
    return QHostAddress(a).isEqual(QHostAddress(b), QHostAddress::ConvertV4MappedToIPv4);
}

// Every sample a song's pads name.
QStringList samplesOf(const QJsonObject &song) {
    QStringList paths;
    const QJsonObject pads = song.value("pads").toObject();
    for (auto it = pads.begin(); it != pads.end(); ++it) {
        const QString path = it.value().toObject().value("sample").toString();
        if (!path.isEmpty() && !paths.contains(path))
            paths << path;
    }
    return paths;
}

// The song with each pad's sample renamed through a map.
QJsonObject renameSamples(QJsonObject song, const QHash<QString, QString> &names) {
    QJsonObject pads = song.value("pads").toObject();
    for (auto it = pads.begin(); it != pads.end(); ++it) {
        QJsonObject pad = it.value().toObject();
        const QString path = pad.value("sample").toString();
        if (!path.isEmpty() && names.contains(path)) {
            pad.insert("sample", names.value(path));
            it.value() = pad;
        }
    }
    song.insert("pads", pads);
    return song;
}

} // namespace

Jam::Jam(Backend *backend, Engine *engine, QObject *parent)
    : QObject(parent), m_backend(backend), m_engine(engine),
      m_cache(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/jam") {
    connect(&m_server, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket *socket = m_server.nextPendingConnection())
            accept(socket);
    });
    m_backend->setAboutToPlay([this](int chainStart) { aboutToPlay(chainStart); });
    connect(m_backend, &Backend::playingChanged, this, &Jam::playingChanged);
    connect(m_backend, &Backend::songChanged, this, &Jam::songChanged);
    connect(m_backend, &Backend::gridChanged, this, &Jam::viewChanged);
    connect(m_backend, &Backend::songModeChanged, this, &Jam::viewChanged);

    // Edits and songs go out a moment after they happen, so dragging a
    // slider sends a handful of songs, not hundreds.
    m_songTimer.setSingleShot(true);
    m_songTimer.setInterval(60);
    connect(&m_songTimer, &QTimer::timeout, this, &Jam::sendSongs);
    m_editTimer.setSingleShot(true);
    m_editTimer.setInterval(60);
    connect(&m_editTimer, &QTimer::timeout, this, &Jam::sendEdit);
    connect(&m_pingTimer, &QTimer::timeout, this, &Jam::ping);
}

Jam::~Jam() {
    m_backend->setAboutToPlay({});
    leave();
}

QVariantList Jam::members() const {
    QVariantList list;
    if (m_role == "hosting") {
        for (const Member *member : m_members)
            list << QVariantMap{{"name", member->name}, {"mode", member->mode}};
    } else {
        for (const QString &name : m_hostMembers)
            if (name != m_self.name)
                list << QVariantMap{{"name", name}};
    }
    return list;
}

// Tailscale says which devices are the user's own, and where they are.
void Jam::askTailscale(const std::function<void(const Tailnet &)> &then) {
    m_waiting << then;
    if (m_asking)
        return;
    m_asking = true;
    emit foundChanged();
    auto *process = new QProcess(this);
    auto finish = [this, process](bool started) {
        Tailnet tailnet;
        if (!started) {
            setProblem("noTailscale");
        } else {
            const QJsonObject status = QJsonDocument::fromJson(process->readAllStandardOutput()).object();
            tailnet.running = status.value("BackendState").toString() == "Running";
            auto device = [](const QJsonObject &node) {
                Device found;
                found.name = node.value("HostName").toString();
                found.online = node.value("Online").toBool();
                found.user = node.value("UserID").toVariant().toLongLong();
                for (const QJsonValue &ip : node.value("TailscaleIPs").toArray())
                    if (ip.toString().contains('.') && found.address.isEmpty())
                        found.address = ip.toString();
                return found;
            };
            tailnet.self = device(status.value("Self").toObject());
            const QJsonObject peers = status.value("Peer").toObject();
            for (auto it = peers.begin(); it != peers.end(); ++it)
                tailnet.peers << device(it.value().toObject());
            if (!tailnet.running)
                setProblem("tailscaleStopped");
            else if (m_problem == "noTailscale" || m_problem == "tailscaleStopped")
                setProblem({});
        }
        m_tailnet = tailnet;
        if (tailnet.running && m_self.name != tailnet.self.name) {
            m_self = tailnet.self;
            emit changed();
        }
        m_asking = false;
        const auto waiting = std::exchange(m_waiting, {});
        for (const auto &callback : waiting)
            callback(tailnet);
        emit foundChanged();
        process->deleteLater();
    };
    connect(process, &QProcess::finished, this, [finish] { finish(true); });
    connect(process, &QProcess::errorOccurred, this, [finish, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            process->disconnect();
            finish(false);
        }
    });
    process->start(m_tailscale, {"status", "--json"});
}

// One of the user's own devices, or this one.
bool Jam::trusted(const QString &address) const {
    if (!m_tailnet.running)
        return false;
    if (sameAddress(address, m_tailnet.self.address))
        return true;
    return std::any_of(m_tailnet.peers.begin(), m_tailnet.peers.end(), [&](const Device &peer) {
        return peer.user == m_tailnet.self.user && sameAddress(address, peer.address);
    });
}

void Jam::start() {
    askTailscale([this](const Tailnet &) { listen(); });
}

void Jam::listen() {
    if (m_server.isListening() || m_self.address.isEmpty())
        return;
    // Only on the tailnet: nothing on the local network or the internet
    // can reach it. Another dole open here may hold the port; this one
    // can still join, it just can't be joined.
    m_server.listen(QHostAddress(m_self.address), m_port);
}

void Jam::accept(QTcpSocket *socket) {
    socket->setParent(this);
    auto admit = [this, socket = QPointer<QTcpSocket>(socket)] {
        if (!socket)
            return;
        if (!trusted(socket->peerAddress().toString())) {
            socket->abort();
            socket->deleteLater();
            return;
        }
        // The first message says what it wants: to know about this dole,
        // or to join it.
        auto *member = new Member;
        member->socket = socket;
        connect(socket, &QTcpSocket::disconnected, this, [this, member] { dropMember(member); });
        readLines(socket, [this, member](const QJsonObject &message) { hostMessage(member, message); });
    };
    // A device just added to the tailnet isn't known yet; ask again first.
    if (trusted(socket->peerAddress().toString()))
        admit();
    else
        askTailscale([admit](const Tailnet &) { admit(); });
}

void Jam::readLines(QTcpSocket *socket, const std::function<void(const QJsonObject &)> &handle) {
    connect(socket, &QTcpSocket::readyRead, this, [socket = QPointer<QTcpSocket>(socket), handle] {
        // A message can end the connection; what came after it is for no one.
        while (socket && socket->state() == QAbstractSocket::ConnectedState && socket->canReadLine()) {
            const QJsonObject message = QJsonDocument::fromJson(socket->readLine()).object();
            if (!message.isEmpty())
                handle(message);
        }
        if (socket && socket->bytesAvailable() > kMaxMessage)
            socket->abort();
    });
}

void Jam::send(QTcpSocket *socket, const QJsonObject &message) {
    if (socket && socket->state() == QAbstractSocket::ConnectedState)
        socket->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
}

void Jam::setProblem(const QString &problem, const QString &name) {
    if (m_problem == problem && m_problemName == name)
        return;
    m_problem = problem;
    m_problemName = name;
    emit changed();
}

void Jam::dismissProblem() {
    setProblem({});
}

void Jam::setRole(const QString &role) {
    if (m_role == role)
        return;
    m_role = role;
    emit changed();
}

void Jam::applying(const std::function<void()> &work) {
    const bool was = m_applying;
    m_applying = true;
    work();
    m_applying = was;
}

qint64 Jam::hostNow() const {
    return steadyMicroseconds() + m_offset;
}

// The first step of a bar far enough ahead for every dole to hear of it
// before it comes: twice the round trip, and at least a fifth of a second.
qint64 Jam::nextStart() const {
    const double lead = std::max<double>(200000, 2.0 * std::max<qint64>(m_bestRtt, 0));
    const double step = m_timeline.stepAt(hostNow() + qint64(lead));
    return qint64(std::ceil(step / kSteps)) * kSteps;
}

QJsonObject Jam::songJson() const {
    return songToJson(m_backend->song(), {});
}

QJsonObject Jam::toHost(QJsonObject song) const {
    return renameSamples(std::move(song), m_hostPaths);
}

QJsonObject Jam::toLocal(QJsonObject song) const {
    return renameSamples(std::move(song), m_localPaths);
}

// The timeline in this machine's clock goes to the engine, which keeps the
// sound on it.
void Jam::useTimeline() {
    Timeline local = m_timeline;
    local.origin -= m_offset;
    m_engine->setTimeline(local);
}

void Jam::search() {
    m_found.clear();
    emit foundChanged();
    askTailscale([this](const Tailnet &tailnet) {
        if (!tailnet.running)
            return;
        listen();
        for (const Device &peer : tailnet.peers)
            if (peer.online && peer.user == tailnet.self.user && !peer.address.isEmpty())
                probe(peer);
    });
}

void Jam::probe(const Device &device) {
    auto *socket = new QTcpSocket(this);
    ++m_probing;
    emit foundChanged();
    auto done = std::make_shared<bool>(false);
    auto finish = [this, socket, done] {
        if (*done)
            return;
        *done = true;
        --m_probing;
        socket->abort();
        socket->deleteLater();
        emit foundChanged();
    };
    QTimer::singleShot(2500, socket, finish);
    connect(socket, &QTcpSocket::errorOccurred, this, finish);
    connect(socket, &QTcpSocket::connected, this, [this, socket] { send(socket, {{"t", "probe"}}); });
    readLines(socket, [this, device, finish](const QJsonObject &message) {
        if (message.value("t") != "about")
            return;
        m_found << QVariantMap{{"name", message.value("name").toString(device.name)},
                               {"address", device.address},
                               {"song", message.value("song").toString()},
                               {"playing", message.value("playing").toBool()},
                               {"role", message.value("role").toString()},
                               {"host", message.value("host").toString()}};
        finish();
    });
    socket->connectToHost(device.address, m_port);
}

// Hosting.

void Jam::hostMessage(Member *member, const QJsonObject &message) {
    const QString type = message.value("t").toString();
    if (type == "probe") {
        send(member->socket, {{"t", "about"},
                              {"name", m_self.name},
                              {"song", m_backend->fileName()},
                              {"playing", m_backend->playing()},
                              {"role", m_role},
                              {"host", m_role == "joined" ? m_hostName : QString()}});
        member->socket->disconnectFromHost();
        return;
    }
    if (type == "hello") {
        // A dole in someone else's jam sends joiners on to its host, so a
        // jam has one beat.
        if (m_role == "joined" || m_role == "joining") {
            send(member->socket, {{"t", "refused"}, {"why", "busy"}, {"host", m_hostName}, {"address", m_hostAddress}});
            member->socket->disconnectFromHost();
            return;
        }
        member->name = message.value("name").toString();
        member->mode = message.value("mode").toString() == "song" ? "song" : "clock";
        m_members << member;
        welcome(member);
        return;
    }
    if (!m_members.contains(member))
        return;

    if (type == "ping") {
        send(member->socket, {{"t", "pong"}, {"a", message.value("a")}, {"b", double(steadyMicroseconds())}});
    } else if (type == "start") {
        if (!m_timeline.started) {
            m_timeline.started = true;
            m_timeline.startStep = message.value("startStep").toVariant().toLongLong();
            useTimeline();
            applying([&] {
                if (member->mode == "song") {
                    if (message.value("songMode").toBool() != m_backend->songMode())
                        m_backend->toggleSongMode();
                    m_backend->selectPattern(message.value("pattern").toInt());
                }
                if (!m_backend->playing())
                    m_backend->togglePlay(message.value("chain").toInt());
            });
        }
        broadcastTimeline();
    } else if (type == "stop") {
        if (m_timeline.started) {
            m_timeline.started = false;
            useTimeline();
            applying([&] { m_backend->stop(); });
        }
        broadcastTimeline();
    } else if (type == "tempo") {
        m_timeline.bpm = message.value("bpm").toDouble(m_timeline.bpm);
        m_timeline.origin = message.value("origin").toVariant().toLongLong();
        useTimeline();
        if (m_backend->bpm() != int(m_timeline.bpm)) {
            Song song = m_backend->song();
            song.bpm = int(m_timeline.bpm);
            applying([&] { m_backend->receiveSong(song); });
            scheduleSongs();
        }
        broadcastTimeline();
    } else if (type == "edit" && member->mode == "song") {
        const QJsonObject patch = message.value("patch").toObject();
        // Its own samples are named as it sent them; here they are kept.
        Song next;
        const QJsonObject merged = renameSamples(patchSongJson(songJson(), patch), member->uploads);
        if (songFromJson(merged, {}, &next, nullptr))
            applying([&] { m_backend->receiveSong(next); });
        member->lastEdit = message.value("id").toInt();
        if (m_backend->bpm() != int(m_timeline.bpm)) {
            const qint64 now = hostNow();
            const double step = m_timeline.stepAt(now);
            m_timeline.bpm = m_backend->bpm();
            m_timeline.origin = now - qint64(step * 15e6 / m_timeline.bpm);
            useTimeline();
            broadcastTimeline();
        }
        scheduleSongs();
    } else if (type == "view" && member->mode == "song") {
        applying([&] {
            if (message.value("songMode").toBool() != m_backend->songMode())
                m_backend->toggleSongMode();
            m_backend->selectPattern(message.value("pattern").toInt());
        });
        m_view = QJsonObject{{"pattern", m_backend->editPattern()}, {"songMode", m_backend->songMode()}};
        sendView();
    } else if (type == "sample" && member->mode == "song") {
        // Only a sample the shared song plays, never any other file.
        const QString path = message.value("path").toString();
        if (!samplesOf(songJson()).contains(path))
            return;
        QFile file(path);
        if (file.size() > kMaxSample || !file.open(QIODevice::ReadOnly))
            return;
        send(member->socket, {{"t", "sampleData"},
                              {"path", path},
                              {"name", QFileInfo(path).fileName()},
                              {"data", QString::fromLatin1(file.readAll().toBase64())}});
    } else if (type == "upload" && member->mode == "song") {
        const QString path = message.value("path").toString();
        const QByteArray data = QByteArray::fromBase64(message.value("data").toString().toLatin1());
        const QString kept = keep(message.value("name").toString(), data, member->name + ":" + path);
        if (kept.isEmpty())
            return;
        member->uploads.insert(path, kept);
        send(member->socket, {{"t", "uploaded"}, {"path", path}, {"as", kept}});
    } else if (type == "bye") {
        member->socket->disconnectFromHost();
    }
}

void Jam::welcome(Member *member) {
    if (m_role != "hosting") {
        // The beat starts here: where the song is now, if it's playing.
        m_timeline = Timeline{};
        m_timeline.on = true;
        m_timeline.bpm = m_backend->bpm();
        m_offset = 0;
        const qint64 now = steadyMicroseconds();
        if (m_backend->playing()) {
            m_timeline.started = true;
            m_timeline.origin = now - qint64(m_engine->heardClock() * 15e6 / m_timeline.bpm);
        } else {
            m_timeline.origin = now;
        }
        m_view = QJsonObject{{"pattern", m_backend->editPattern()}, {"songMode", m_backend->songMode()}};
        useTimeline();
        setProblem({});
        setRole("hosting");
    }
    send(member->socket, {{"t", "welcome"}, {"name", m_self.name}, {"protocol", kProtocol}});
    if (member->mode == "song") {
        member->sent = songJson();
        send(member->socket, {{"t", "song"}, {"rev", ++m_revision}, {"ack", 0}, {"song", member->sent}});
        send(member->socket, QJsonObject{{"t", "view"}, {"pattern", m_view.value("pattern")}, {"songMode", m_view.value("songMode")}});
    }
    broadcastTimeline();
    emit changed();
}

void Jam::dropMember(Member *member) {
    const bool joined = m_members.removeOne(member);
    member->socket->deleteLater();
    delete member;
    if (!joined)
        return;
    if (m_members.isEmpty())
        endHosting();
    else
        broadcastTimeline();
    emit changed();
}

void Jam::endHosting() {
    m_songTimer.stop();
    m_timeline = Timeline{};
    m_offset = 0;
    m_engine->setTimeline(Timeline{});
    setRole("alone");
}

QJsonObject Jam::timelineMessage() const {
    QJsonArray names{m_self.name};
    for (const Member *member : m_members)
        names.append(member->name);
    return {{"t", "timeline"},
            {"origin", double(m_timeline.origin)},
            {"bpm", m_timeline.bpm},
            {"started", m_timeline.started},
            {"startStep", double(m_timeline.startStep)},
            {"songMode", m_backend->songMode()},
            {"pattern", m_backend->editPattern()},
            {"members", names}};
}

void Jam::broadcastTimeline() {
    const QJsonObject message = timelineMessage();
    for (const Member *member : m_members)
        send(member->socket, message);
}

void Jam::scheduleSongs() {
    if (std::any_of(m_members.begin(), m_members.end(), [](const Member *m) { return m->mode == "song"; }))
        m_songTimer.start();
}

// Each sharing member gets the song as it is here, with its last edit
// acknowledged, so it can tell its own edits are in.
void Jam::sendSongs() {
    const QJsonObject song = songJson();
    ++m_revision;
    for (Member *member : m_members) {
        if (member->mode != "song")
            continue;
        member->sent = song;
        send(member->socket, {{"t", "song"}, {"rev", m_revision}, {"ack", member->lastEdit}, {"song", song}});
    }
}

void Jam::sendView() {
    const QJsonObject message{{"t", "view"}, {"pattern", m_view.value("pattern")}, {"songMode", m_view.value("songMode")}};
    for (const Member *member : m_members)
        if (member->mode == "song")
            send(member->socket, message);
}

// Joining.

void Jam::join(const QString &address, const QString &mode) {
    if (m_role != "alone")
        leave();
    m_mode = mode == "song" ? "song" : "clock";
    m_hostAddress = address;
    m_hostName = address;
    for (const QVariant &found : std::as_const(m_found))
        if (found.toMap().value("address").toString() == address)
            m_hostName = found.toMap().value("name").toString();
    m_offset = 0;
    m_bestRtt = -1;
    m_samples.clear();
    m_synced = false;
    m_pending = {};
    m_pings = 0;
    m_firstSong = true;
    m_sentState = {};
    m_sentId = 0;
    m_hostMembers.clear();
    m_requested.clear();
    setProblem({});
    setRole("joining");

    auto *socket = new QTcpSocket(this);
    m_host = socket;
    auto connectedInTime = std::make_shared<bool>(false);
    QTimer::singleShot(4000, socket, [this, socket, connectedInTime] {
        if (*connectedInTime || m_host != socket)
            return;
        setProblem("unreachable", m_hostName);
        leave();
    });
    connect(socket, &QTcpSocket::connected, this, [this, socket, connectedInTime] {
        *connectedInTime = true;
        send(socket, {{"t", "hello"}, {"name", m_self.name}, {"mode", m_mode}, {"protocol", kProtocol}});
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
        if (m_host != socket)
            return;
        // Not by leave(): the host went, or the network did.
        if (m_problem.isEmpty())
            setProblem(m_role == "joining" ? "unreachable" : "lost", m_hostName);
        leave();
    });
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket] {
        if (m_host == socket && m_role == "joining" && socket->state() != QAbstractSocket::ConnectedState) {
            setProblem("unreachable", m_hostName);
            leave();
        }
    });
    readLines(socket, [this](const QJsonObject &message) { memberMessage(message); });
    // Joining needs this machine's name, so the host can say who joined.
    askTailscale([this, socket, address](const Tailnet &) {
        if (m_host == socket)
            socket->connectToHost(address, m_port);
    });
}

void Jam::leave() {
    m_editTimer.stop();
    m_pingTimer.stop();
    if (m_role == "hosting") {
        const auto members = m_members;
        for (Member *member : members) {
            send(member->socket, {{"t", "bye"}});
            member->socket->flush();
            member->socket->disconnect(this);
            member->socket->disconnectFromHost();
            member->socket->deleteLater();
            delete member;
        }
        m_members.clear();
        endHosting();
        return;
    }
    if (QTcpSocket *socket = m_host) {
        m_host = nullptr;
        send(socket, {{"t", "bye"}});
        socket->flush();
        socket->disconnect(this);
        socket->disconnectFromHost();
        socket->deleteLater();
    }
    m_synced = false;
    m_timeline = Timeline{};
    m_offset = 0;
    m_bestRtt = -1;
    m_hostMembers.clear();
    m_engine->setTimeline(Timeline{});
    setRole("alone");
    emit changed();
}

void Jam::memberMessage(const QJsonObject &message) {
    const QString type = message.value("t").toString();
    if (type == "welcome") {
        m_hostName = message.value("name").toString(m_hostName);
        setRole("joined");
        // A few quick round trips learn the clock; then one every couple of
        // seconds follows it.
        ping();
        m_pingTimer.start(100);
    } else if (type == "refused") {
        const QString address = message.value("address").toString();
        if (!m_redirected && !address.isEmpty()) {
            m_redirected = true;
            const QString mode = m_mode;
            QTimer::singleShot(0, this, [this, address, mode] { join(address, mode); });
            return;
        }
        m_redirected = false;
        setProblem("busy", message.value("host").toString());
        leave();
    } else if (type == "pong") {
        m_redirected = false;
        const qint64 now = steadyMicroseconds();
        const qint64 sent = message.value("a").toVariant().toLongLong();
        const qint64 hostTime = message.value("b").toVariant().toLongLong();
        const qint64 rtt = now - sent;
        // The host read its clock about halfway through the round trip;
        // the quickest round trips say it most exactly.
        m_samples << qMakePair(rtt, hostTime - (sent + now) / 2);
        if (m_samples.size() > 8)
            m_samples.removeFirst();
        const auto best = std::min_element(m_samples.begin(), m_samples.end());
        const bool first = m_bestRtt < 0;
        m_bestRtt = best->first;
        m_offset = best->second;
        if (++m_pings == 5)
            m_pingTimer.start(2000);
        if (!m_synced && m_pings >= 3) {
            m_synced = true;
            if (!m_pending.isEmpty())
                apply(std::exchange(m_pending, {}));
        } else if (m_synced) {
            useTimeline();
        }
        if (first || m_pings % 5 == 0)
            emit changed();
    } else if (type == "timeline") {
        apply(message);
    } else if (type == "song" && m_mode == "song") {
        if (!m_firstSong && message.value("ack").toInt() < m_sentId)
            return; // our last edit isn't in it yet; a song with it follows
        adopt(message.value("song").toObject(), m_firstSong);
    } else if (type == "view" && m_mode == "song") {
        m_view = QJsonObject{{"pattern", message.value("pattern")}, {"songMode", message.value("songMode")}};
        applying([&] {
            if (message.value("songMode").toBool() != m_backend->songMode())
                m_backend->toggleSongMode();
            m_backend->selectPattern(message.value("pattern").toInt());
        });
    } else if (type == "sampleData" && m_mode == "song") {
        const QString path = message.value("path").toString();
        const QString kept = keep(message.value("name").toString(),
                                  QByteArray::fromBase64(message.value("data").toString().toLatin1()), m_hostName + ":" + path);
        if (kept.isEmpty())
            return;
        m_localPaths.insert(path, kept);
        m_hostPaths.insert(kept, path);
        // The pads that wait for it play it now.
        Song song;
        if (songFromJson(toLocal(toHost(songJson())), {}, &song, nullptr))
            applying([&] { m_backend->receiveSong(song); });
    } else if (type == "uploaded") {
        m_localPaths.insert(message.value("as").toString(), message.value("path").toString());
        m_hostPaths.insert(message.value("path").toString(), message.value("as").toString());
    } else if (type == "bye") {
        setProblem("ended", m_hostName);
        leave();
    }
}

void Jam::ping() {
    send(m_host, {{"t", "ping"}, {"a", double(steadyMicroseconds())}});
}

// The jam's beat, and what plays on it.
void Jam::apply(const QJsonObject &message) {
    QStringList names;
    for (const QJsonValue &name : message.value("members").toArray())
        names << name.toString();
    if (names != m_hostMembers) {
        m_hostMembers = names;
        emit changed();
    }
    m_timeline.on = true;
    m_timeline.origin = message.value("origin").toVariant().toLongLong();
    m_timeline.bpm = message.value("bpm").toDouble(90);
    m_timeline.started = message.value("started").toBool();
    m_timeline.startStep = message.value("startStep").toVariant().toLongLong();
    if (!m_synced) {
        m_pending = message;
        return;
    }
    useTimeline();
    applying([&] {
        if (m_backend->bpm() != int(m_timeline.bpm)) {
            Song song = m_backend->song();
            song.bpm = int(m_timeline.bpm);
            m_backend->receiveSong(song);
        }
        if (m_mode == "song") {
            if (message.value("songMode").toBool() != m_backend->songMode())
                m_backend->toggleSongMode();
            if (!(m_backend->songMode() && m_backend->playing()))
                m_backend->selectPattern(message.value("pattern").toInt());
        }
        if (m_timeline.started && !m_backend->playing())
            m_backend->togglePlay(0);
        else if (!m_timeline.started && m_backend->playing())
            m_backend->stop();
    });
}

// The host's song, with this dole's edits it doesn't have yet laid on top,
// so nothing done here is lost while it travels.
void Jam::adopt(const QJsonObject &hostSong, bool first) {
    Song song;
    if (first) {
        m_firstSong = false;
        m_sentState = hostSong;
        if (songFromJson(toLocal(hostSong), {}, &song, nullptr))
            applying([&] { m_backend->adoptSong(song); });
        requestSamples(hostSong);
        return;
    }
    const QJsonObject unsent = diffSongJson(m_sentState, toHost(songJson()));
    m_sentState = hostSong;
    if (songFromJson(toLocal(patchSongJson(hostSong, unsent)), {}, &song, nullptr))
        applying([&] { m_backend->receiveSong(song); });
    if (!unsent.isEmpty())
        scheduleEdit();
    requestSamples(hostSong);
}

void Jam::requestSamples(const QJsonObject &hostSong) {
    for (const QString &path : samplesOf(hostSong)) {
        if (m_localPaths.contains(path) || m_requested.contains(path))
            continue;
        m_requested.insert(path);
        send(m_host, {{"t", "sample"}, {"path", path}});
    }
}

void Jam::scheduleEdit() {
    if (m_role == "joined" && m_mode == "song")
        m_editTimer.start();
}

void Jam::sendEdit() {
    if (m_role != "joined" || m_mode != "song" || m_firstSong)
        return;
    const QJsonObject current = toHost(songJson());
    // A sample chosen here goes to the host before the edit that plays it.
    for (const QString &path : samplesOf(current)) {
        if (m_hostPaths.contains(path) || m_localPaths.contains(path) || m_requested.contains(path))
            continue;
        QFile file(path);
        m_hostPaths.insert(path, path); // until the host says where it keeps it
        if (file.size() > kMaxSample || !file.open(QIODevice::ReadOnly))
            continue;
        send(m_host, {{"t", "upload"},
                      {"path", path},
                      {"name", QFileInfo(path).fileName()},
                      {"data", QString::fromLatin1(file.readAll().toBase64())}});
    }
    const QJsonObject patch = diffSongJson(m_sentState, current);
    if (patch.isEmpty())
        return;
    send(m_host, {{"t", "edit"}, {"id", ++m_sentId}, {"patch", patch}});
    m_sentState = patchSongJson(m_sentState, patch);
}

// A shared sample, kept where only this user's cache is.
QString Jam::keep(const QString &name, const QByteArray &data, const QString &origin) {
    if (data.isEmpty())
        return {};
    const QString folder =
        m_cache + "/" + QString::fromLatin1(QCryptographicHash::hash(origin.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
    const QString file = QFileInfo(name).fileName().isEmpty() ? QString("sample") : QFileInfo(name).fileName();
    if (!QDir().mkpath(folder))
        return {};
    QSaveFile out(folder + "/" + file);
    out.setDirectWriteFallback(false);
    if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit())
        return {};
    return folder + "/" + file;
}

// What the person does here.

void Jam::aboutToPlay(int chainStart) {
    if (!inJam() || m_applying || m_timeline.started)
        return;
    m_timeline.started = true;
    m_timeline.startStep = nextStart();
    useTimeline();
    if (m_role == "hosting")
        broadcastTimeline();
    else
        send(m_host, {{"t", "start"},
                      {"startStep", double(m_timeline.startStep)},
                      {"chain", chainStart},
                      {"songMode", m_backend->songMode()},
                      {"pattern", m_backend->editPattern()}});
}

void Jam::playingChanged() {
    if (!inJam() || m_applying || m_backend->playing() || !m_timeline.started)
        return;
    m_timeline.started = false;
    useTimeline();
    if (m_role == "hosting")
        broadcastTimeline();
    else
        send(m_host, {{"t", "stop"}});
}

void Jam::songChanged() {
    if (m_applying || (m_role != "hosting" && m_role != "joined"))
        return;
    // The tempo is the beat's, which needs the host's clock; the song itself
    // doesn't.
    if (inJam() && m_backend->bpm() != int(m_timeline.bpm)) {
        // The beat carries on from where it is, at the new tempo.
        const qint64 now = hostNow();
        const double step = m_timeline.stepAt(now);
        m_timeline.bpm = m_backend->bpm();
        m_timeline.origin = now - qint64(step * 15e6 / m_timeline.bpm);
        useTimeline();
        if (m_role == "hosting")
            broadcastTimeline();
        else
            send(m_host, {{"t", "tempo"}, {"bpm", m_timeline.bpm}, {"origin", double(m_timeline.origin)}});
    }
    if (m_role == "hosting")
        scheduleSongs();
    else
        scheduleEdit();
}

void Jam::viewChanged() {
    if (m_applying || (m_role != "hosting" && m_role != "joined"))
        return;
    // Playing the song, the pattern on screen follows it on every dole.
    if (m_backend->songMode() && m_backend->playing() && m_view.value("songMode").toBool())
        return;
    const QJsonObject view{{"pattern", m_backend->editPattern()}, {"songMode", m_backend->songMode()}};
    if (view == m_view)
        return;
    m_view = view;
    if (m_role == "hosting")
        sendView();
    else if (m_mode == "song")
        send(m_host, QJsonObject{{"t", "view"}, {"pattern", view.value("pattern")}, {"songMode", view.value("songMode")}});
}
