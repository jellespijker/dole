#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTcpServer>
#include <QTimer>
#include <QVariantList>
#include <functional>

#include "engine.h"
#include "song.h"

class Backend;
class QTcpSocket;

// The port doles listen on for each other, on the tailnet only.
constexpr quint16 kJamPort = 47660;

// Plays along with the doles on the user's other machines, found over
// Tailscale. One dole hosts and the others join it, each in one of two ways:
//
// - "clock": its own song, on one shared beat. Tempo, play, and stop are
//   the jam's, and every dole lands on the same step at the same moment.
// - "song": the host's song, shared. An edit on any dole is heard on all
//   of them, and the pattern on screen is the same everywhere.
//
// The host keeps the beat as a timeline (see Timeline in engine.h) in its
// own steady clock; each member learns how far its clock is from the
// host's from round trips, and plays the timeline in its own. Pressing play
// anywhere starts everyone on the next bar.
//
// Only the user's own devices on the tailnet can reach it: it listens on the
// machine's tailnet address, and lets in an address only when Tailscale says
// it belongs to a device of the same user.
class Jam : public QObject {
    Q_OBJECT
    // "alone", "joining", "hosting", or "joined".
    Q_PROPERTY(QString role READ role NOTIFY changed)
    // A member's way of joining: "clock" or "song".
    Q_PROPERTY(QString mode READ mode NOTIFY changed)
    Q_PROPERTY(QString hostName READ hostName NOTIFY changed)
    Q_PROPERTY(QString selfName READ selfName NOTIFY changed)
    // The others in the jam: [{name, mode}].
    Q_PROPERTY(QVariantList members READ members NOTIFY changed)
    // A member's round trip to the host, in milliseconds.
    Q_PROPERTY(int latency READ latency NOTIFY changed)
    // What went wrong, if anything: "noTailscale", "tailscaleStopped",
    // "unreachable", "busy", "lost", or "ended"; problemName is the machine.
    Q_PROPERTY(QString problem READ problem NOTIFY changed)
    Q_PROPERTY(QString problemName READ problemName NOTIFY changed)
    // The doles found on the tailnet: [{name, address, song, playing, role, host}].
    Q_PROPERTY(QVariantList found READ found NOTIFY foundChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY foundChanged)
    Q_PROPERTY(int port READ port CONSTANT)

public:
    Jam(Backend *backend, Engine *engine, QObject *parent = nullptr);
    ~Jam() override;

    // For tests: the tailscale to ask, the port, and where shared samples go.
    void setTailscale(const QString &program) { m_tailscale = program; }
    void setPort(quint16 port) { m_port = port; }
    void setCacheDirectory(const QString &directory) { m_cache = directory; }

    // Asks Tailscale for this machine's address, and listens there.
    void start();
    bool listening() const { return m_server.isListening(); }
    int port() const { return m_port; }

    QString role() const { return m_role; }
    QString mode() const { return m_mode; }
    QString hostName() const { return m_hostName; }
    QString selfName() const { return m_self.name; }
    QVariantList members() const;
    int latency() const { return m_bestRtt < 0 ? -1 : int((m_bestRtt + 500) / 1000); }
    QString problem() const { return m_problem; }
    QString problemName() const { return m_problemName; }
    QVariantList found() const { return m_found; }
    bool searching() const { return m_probing > 0 || m_asking; }

    Q_INVOKABLE void search();
    Q_INVOKABLE void join(const QString &address, const QString &mode);
    Q_INVOKABLE void leave();
    Q_INVOKABLE void dismissProblem();

signals:
    void changed();
    void foundChanged();

private:
    struct Device {
        QString name;
        QString address; // its tailnet IPv4 address
        bool online = false;
        qint64 user = 0;
    };
    struct Tailnet {
        bool running = false;
        Device self;
        QList<Device> peers;
    };
    // A member, as the host sees it.
    struct Member {
        QTcpSocket *socket = nullptr;
        QString name;
        QString mode;
        int lastEdit = 0;              // the last of its edits applied
        QHash<QString, QString> uploads; // its own sample paths, to where they are kept here
        QJsonObject sent;              // the song as last sent to it
    };

    void askTailscale(const std::function<void(const Tailnet &)> &then);
    bool trusted(const QString &address) const;
    void listen();
    void accept(QTcpSocket *socket);
    void probe(const Device &device);
    void readLines(QTcpSocket *socket, const std::function<void(const QJsonObject &)> &handle);
    void send(QTcpSocket *socket, const QJsonObject &message);
    void setProblem(const QString &problem, const QString &name = {});
    void setRole(const QString &role);

    // Hosting.
    void hostMessage(Member *member, const QJsonObject &message);
    void welcome(Member *member);
    void dropMember(Member *member);
    QJsonObject timelineMessage() const;
    void broadcastTimeline();
    void scheduleSongs();
    void sendSongs();
    void sendView();
    void endHosting();

    // Joined.
    void memberMessage(const QJsonObject &message);
    void ping();
    void adopt(const QJsonObject &hostSong, bool first);
    void scheduleEdit();
    void sendEdit();
    void requestSamples(const QJsonObject &hostSong);
    QString keep(const QString &name, const QByteArray &data, const QString &origin);

    // Both.
    void apply(const QJsonObject &timeline);
    void useTimeline();
    void aboutToPlay(int chainStart);
    void playingChanged();
    void songChanged();
    void viewChanged();
    qint64 hostNow() const;
    qint64 nextStart() const;
    QJsonObject songJson() const;
    // The song's sample paths turned to the host's names for them, or back.
    QJsonObject toHost(QJsonObject song) const;
    QJsonObject toLocal(QJsonObject song) const;
    bool inJam() const { return m_role == "hosting" || (m_role == "joined" && m_synced); }
    // Runs work as the jam's, not the person's: what it changes isn't sent back.
    void applying(const std::function<void()> &work);

    Backend *m_backend;
    Engine *m_engine;
    QString m_tailscale = "tailscale";
    quint16 m_port = kJamPort;
    QString m_cache;
    QTcpServer m_server;
    Tailnet m_tailnet;
    Device m_self;
    bool m_asking = false;
    QList<std::function<void(const Tailnet &)>> m_waiting;

    QString m_role = "alone";
    QString m_mode;
    QString m_hostName;
    QString m_problem;
    QString m_problemName;
    QVariantList m_found;
    int m_probing = 0;
    bool m_applying = false;

    // The beat, in the host's clock.
    Timeline m_timeline;
    QJsonObject m_view; // the pattern on screen, and whether the song plays

    // Hosting.
    QList<Member *> m_members;
    QTimer m_songTimer;
    int m_revision = 0;

    // Joined.
    QPointer<QTcpSocket> m_host;
    QString m_hostAddress;
    bool m_redirected = false;
    QTimer m_pingTimer;
    int m_pings = 0;
    qint64 m_offset = 0;   // the host's clock less this one's
    qint64 m_bestRtt = -1; // microseconds
    QList<QPair<qint64, qint64>> m_samples; // round trips and the offsets they gave
    bool m_synced = false;  // the offset is known, so the beat can be followed
    QJsonObject m_pending;  // a timeline that came before the offset was known
    QStringList m_hostMembers;
    bool m_firstSong = true;
    QJsonObject m_sentState; // the song, in host names, as the host has it with what was sent
    int m_sentId = 0;
    QTimer m_editTimer;
    QHash<QString, QString> m_localPaths; // a host's sample path to the copy here
    QHash<QString, QString> m_hostPaths;  // a sample here to the host's name for it
    QSet<QString> m_requested;
};
