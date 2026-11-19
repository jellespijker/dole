#include "x11window.h"

#include <QSocketNotifier>
#include <cstdlib>
#include <cstring>
#include <xcb/xcb.h>

namespace {

xcb_atom_t atom(xcb_connection_t *connection, const char *name) {
    xcb_intern_atom_reply_t *reply =
        xcb_intern_atom_reply(connection, xcb_intern_atom(connection, 0, uint16_t(strlen(name)), name), nullptr);
    const xcb_atom_t result = reply ? reply->atom : xcb_atom_t(XCB_ATOM_NONE);
    free(reply);
    return result;
}

void setFixedSize(xcb_connection_t *connection, xcb_window_t window, int width, int height) {
    // WM_NORMAL_HINTS with the minimum and maximum size both set: the
    // window manager floats a window that can't be resized.
    uint32_t hints[18] = {};
    hints[0] = (1 << 4) | (1 << 5); // PMinSize | PMaxSize
    hints[5] = uint32_t(width);
    hints[6] = uint32_t(height);
    hints[7] = uint32_t(width);
    hints[8] = uint32_t(height);
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS,
                        32, 18, hints);
}

} // namespace

std::unique_ptr<X11Window> X11Window::create(const QString &title, int width, int height, QString *error) {
    if (qEnvironmentVariableIsEmpty("DISPLAY")) {
        *error = "no X server (DISPLAY isn't set); plugin windows need XWayland";
        return {};
    }
    xcb_connection_t *connection = xcb_connect(nullptr, nullptr);
    if (xcb_connection_has_error(connection)) {
        xcb_disconnect(connection);
        *error = "the X server can't be reached";
        return {};
    }
    std::unique_ptr<X11Window> window(new X11Window);
    window->m_connection = connection;
    const xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    window->m_window = xcb_generate_id(connection);
    const uint32_t values[] = {screen->black_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    xcb_create_window(connection, XCB_COPY_FROM_PARENT, window->m_window, screen->root, 0, 0, uint16_t(width),
                      uint16_t(height), 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);

    const QByteArray name = title.toUtf8();
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window->m_window, atom(connection, "_NET_WM_NAME"),
                        atom(connection, "UTF8_STRING"), 8, uint32_t(name.size()), name.constData());
    // Instance and class, so window rules can match dole's plugin windows.
    const char wmClass[] = "dole-plugin\0dole";
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window->m_window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8,
                        sizeof(wmClass), wmClass);
    const xcb_atom_t dialog = atom(connection, "_NET_WM_WINDOW_TYPE_DIALOG");
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window->m_window, atom(connection, "_NET_WM_WINDOW_TYPE"),
                        XCB_ATOM_ATOM, 32, 1, &dialog);
    window->m_deleteAtom = atom(connection, "WM_DELETE_WINDOW");
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window->m_window, atom(connection, "WM_PROTOCOLS"),
                        XCB_ATOM_ATOM, 32, 1, &window->m_deleteAtom);
    setFixedSize(connection, window->m_window, width, height);
    xcb_flush(connection);

    // The X connection says when something happened, so nothing polls it.
    window->m_notifier = new QSocketNotifier(xcb_get_file_descriptor(connection), QSocketNotifier::Read, window.get());
    connect(window->m_notifier, &QSocketNotifier::activated, window.get(), &X11Window::readEvents);
    return window;
}

X11Window::~X11Window() {
    auto *connection = static_cast<xcb_connection_t *>(m_connection);
    xcb_destroy_window(connection, m_window);
    xcb_flush(connection);
    delete m_notifier;
    xcb_disconnect(connection);
}

void X11Window::resize(int width, int height) {
    auto *connection = static_cast<xcb_connection_t *>(m_connection);
    setFixedSize(connection, m_window, width, height);
    const uint32_t size[] = {uint32_t(width), uint32_t(height)};
    xcb_configure_window(connection, m_window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);
    xcb_flush(connection);
}

void X11Window::show() {
    xcb_map_window(static_cast<xcb_connection_t *>(m_connection), m_window);
    xcb_flush(static_cast<xcb_connection_t *>(m_connection));
}

void X11Window::hide() {
    xcb_unmap_window(static_cast<xcb_connection_t *>(m_connection), m_window);
    xcb_flush(static_cast<xcb_connection_t *>(m_connection));
}

void X11Window::readEvents() {
    auto *connection = static_cast<xcb_connection_t *>(m_connection);
    while (xcb_generic_event_t *event = xcb_poll_for_event(connection)) {
        if ((event->response_type & 0x7f) == XCB_CLIENT_MESSAGE) {
            const auto *message = reinterpret_cast<xcb_client_message_event_t *>(event);
            if (message->data.data32[0] == m_deleteAtom) {
                hide();
                emit closed();
            }
        }
        free(event);
    }
}
