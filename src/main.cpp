// dole — a loop sequencer for drums and a bassline.

#include <QCommandLineParser>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QTimer>
#include <QUrl>

#include "backend.h"
#include "cli.h"
#include "engine.h"
#include "portalfilepicker.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    // Commands and help draw no window, so they must not need a display, even
    // where the desktop exports QT_QPA_PLATFORM=wayland. Bare dole prints help,
    // as a command line tool should; launchers say dole open.
    const bool command = argc == 1 || isCliCommand(argv[1]);
    bool windowless = command;
    for (int i = 1; i < argc; ++i) {
        const QByteArray argument(argv[i]);
        windowless = windowless || argument == "--help" || argument == "-h" || argument == "--version"
                     || argument == "-v";
    }
    if (windowless)
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QGuiApplication app(argc, argv);
    app.setApplicationName("dole");
    app.setApplicationVersion(DOLE_VERSION);
    if (command)
        return runCli(app.arguments());

    QCommandLineParser args;
    args.setApplicationDescription("A loop sequencer for drums and a bassline.\n\n" + cliSummary());
    args.addHelpOption();
    args.addVersionOption();
    args.addPositionalArgument("open", "Open the window", "open");
    args.addPositionalArgument("song", "A .dole song file", "[song]");
    args.process(app);
    if (args.positionalArguments().value(0) != "open") {
        fprintf(stderr, "Unknown command %s; see dole --help.\n", qPrintable(args.positionalArguments().value(0)));
        return 1;
    }

    // Matches the window to dole.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon. The compiled-in copy covers
    // a run from build/, and needs qt6-svg's icon engine.
    app.setDesktopFileName("dole");
    app.setWindowIcon(QIcon::fromTheme("dole", QIcon(":/dole.svg")));

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    Theme theme;
    AudioEngine audio;
    PortalFilePicker picker;
    Backend backend(&audio, &picker);
    const QString song = args.positionalArguments().value(1);
    if (!song.isEmpty())
        backend.openPath(QFileInfo(song).absoluteFilePath());

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;
    QTimer::singleShot(200, &audio, &AudioEngine::prepare);

    return app.exec();
}
