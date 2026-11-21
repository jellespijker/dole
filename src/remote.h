#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QLocalServer>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <functional>

#include "song.h"

class Backend;
class Jam;
class QLocalSocket;
class QTimer;

// Where open windows listen: a socket each, named by the process, in a
// folder of the user's runtime directory that only they can open.
QString remoteDirectory();

// Lets an agent, or a script, play the open window live: read the song,
// change it in one undoable step, start and stop it, and wait for the next
// bar. Requests and replies are JSON, a line each:
//   {"id": 1, "method": "edit", "params": {"patch": {...}}}
//   {"id": 1, "result": {...}}  or  {"id": 1, "error": "what went wrong"}
// A connection that asks to "watch" is sent events as they happen instead
// of being polled: {"event": "position", ...}.
class RemoteServer : public QObject {
    Q_OBJECT
public:
    explicit RemoteServer(Backend *backend, QObject *parent = nullptr);
    ~RemoteServer() override;

    // Listens in directory, or says why it can't.
    bool listen(const QString &directory, QString *error);
    QString path() const { return m_server.fullServerName(); }
    // The jam this window is in, for the status.
    void setJam(Jam *jam) { m_jam = jam; }

private:
    struct Wait {
        QPointer<QLocalSocket> socket;
        QJsonValue id;
        QString until;
        Song song; // as it was, so an edit is a real change
        QTimer *timer = nullptr;
    };

    void serve(QLocalSocket *socket);
    void handle(QLocalSocket *socket, const QJsonObject &request);
    QJsonObject status() const;
    QString songDirectory() const;
    void reply(QLocalSocket *socket, const QJsonValue &id, const QJsonObject &result);
    void fail(QLocalSocket *socket, const QJsonValue &id, const QString &message);
    void send(QLocalSocket *socket, const QJsonObject &message);
    void broadcast(const QJsonObject &event);
    // Answers the waits that are done: with the status, or with an error.
    void finish(const std::function<bool(const Wait &)> &done, const QString &error = {});
    void edit(const Song &next);

    Backend *m_backend;
    Jam *m_jam = nullptr;
    QLocalServer m_server;
    QList<Wait> m_waits;
    QSet<QLocalSocket *> m_watchers;
    bool m_editing = false; // the song is changing at a request's word
};

// One request to the newest open window, for the command line: it blocks
// until the answer comes, or timeoutMs passes.
struct RemoteReply {
    bool ok = false;
    QJsonObject result;
    QString error;
};
RemoteReply remoteRequest(const QString &method, const QJsonObject &params, int timeoutMs);
// Prints every event from the newest open window until it closes.
int remoteWatch();
