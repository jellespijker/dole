#pragma once

#include <QObject>
#include <QString>
#include <memory>

class QSocketNotifier;

// A plain X11 window for a plugin to draw its editor into. Plugins on Linux
// draw with X11, which on a Wayland desktop runs through XWayland, beside
// dole's own Wayland window. It asks to float, as a fixed-size dialog, and
// first shows in the middle of dole's monitor.
class X11Window : public QObject {
    Q_OBJECT

public:
    // Empty, with the reason in error, when there is no X server to reach.
    static std::unique_ptr<X11Window> create(const QString &title, int width, int height, QString *error);
    ~X11Window() override;

    unsigned long id() const { return m_window; }
    void resize(int width, int height);
    void show();
    void hide();

signals:
    // The window manager's close button.
    void closed();

private:
    X11Window() = default;
    void readEvents();

    void *m_connection = nullptr; // xcb_connection_t
    unsigned int m_window = 0;
    unsigned int m_deleteAtom = 0;
    unsigned int m_root = 0;
    int m_width = 0;
    int m_height = 0;
    bool m_placed = false;
    QSocketNotifier *m_notifier = nullptr;
};
