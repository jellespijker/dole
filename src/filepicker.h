#pragma once

#include <QObject>
#include <QUrl>

// Open and save dialogs, behind an interface so the tests can answer them.
class FilePicker : public QObject {
    Q_OBJECT

public:
    enum Kind { Song, Sample, Audio };
    Q_ENUM(Kind)

    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~FilePicker() = default;

    virtual void open(Kind kind) = 0;
    virtual void save(Kind kind, const QUrl &suggestedUrl) = 0;

signals:
    void opened(FilePicker::Kind kind, const QUrl &url);
    void saveChosen(FilePicker::Kind kind, const QUrl &url);
    // The dialog was dismissed without a file.
    void cancelled(FilePicker::Kind kind);
    void failed(const QString &message);
};
