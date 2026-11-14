// nudge — reminders for the Omarchy desktop: lists, due dates, repeats,
// subtasks, and a nudge when something is due.

#include <QCommandLineParser>
#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>
#include <cstdio>

#include "alerts.h"
#include "backend.h"
#include "cli.h"
#include "look.h"
#include "store.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    // Nudge follows the Omarchy theme itself; Qt's gtk3 platform theme would
    // only fight it.
    qputenv("QT_QPA_PLATFORMTHEME", "generic");

    const QString first = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();

    // Commands draw nothing, so they run without a display or the toolkit.
    // Bare nudge prints help, as a command line tool should; the launcher
    // says nudge open.
    if (first != "open" && first != "daemon") {
        QCoreApplication app(argc, argv);
        app.setApplicationName("nudge");
        app.setApplicationVersion(NUDGE_VERSION);
        if (first == "--version" || first == "-v") {
            printf("nudge %s\n", NUDGE_VERSION);
            return 0;
        }
        if (first.isEmpty() || first == "--help" || first == "-h")
            return runCli({app.arguments().value(0), "help"});
        if (!isCliCommand(first)) {
            fprintf(stderr, "nudge: there is no command %s\n\n%s\n", qPrintable(first), qPrintable(cliSummary()));
            return 1;
        }
        return runCli(app.arguments());
    }

    // The background service shows notifications in the desktop's language,
    // which needs the toolkit but no window.
    const bool daemon = first == "daemon";
    if (daemon)
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QGuiApplication app(argc, argv);
    app.setApplicationName("nudge");
    app.setApplicationVersion(NUDGE_VERSION);

    // Matches the window to nudge.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("nudge");
    app.setWindowIcon(QIcon::fromTheme("nudge"));

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    if (daemon)
        return runDaemon();

    QCommandLineParser args;
    args.setApplicationDescription("Reminders for the Omarchy desktop.\n\n" + cliSummary());
    args.addHelpOption();
    args.addPositionalArgument("open", "Open the window", "open");
    args.addOption({"view", "Start on a view: today, scheduled, all, flagged, completed", "view"});
    args.addOption({"details", "Start with the details pane open"});
    args.addOption({"help-overlay", "Start with the shortcuts showing"});
    args.addOption({"show", "Start with a dialog open: list, date, move, menu, delete", "dialog"});
    args.addOption({"screenshot", "Save a picture of the window and exit", "file"});
    args.addOption({"new-window", "Open another window even when one is open"});
    args.process(app);

    // Reminders are one thing to look at, so launching Nudge again brings
    // the window that is open to the front rather than opening a second.
    const bool screenshot = args.isSet("screenshot");
    if (!screenshot && !args.isSet("new-window") && focusNudgeWindow())
        return 0;

    // The plainest style, as Hype uses: every control is drawn by Nudge's
    // own components, in the colors and states of the Omarchy theme.
    QQuickStyle::setStyle("Basic");

    // A screenshot shows Omarchy's default look, whatever theme is on.
    Theme theme(screenshot ? QString("/nonexistent") : QString());
    Look look(screenshot ? QString("/nonexistent") : QString());

    // The Omarchy font: "monospace" is the alias `omarchy font set` points at
    // the chosen family, at the size the theme gives the shell.
    auto adoptFont = [&look] {
        QFont font(look.fontFamily());
        font.setStyleHint(QFont::Monospace);
        font.setPixelSize(look.fontSize());
        QGuiApplication::setFont(font);
    };
    adoptFont();
    QObject::connect(&look, &Look::changed, &app, adoptFont);
    Store store;
    store.watch();
    DesktopAlerts alerts(false);
    Backend backend(&store);
    if (args.isSet("view"))
        backend.setViewKey(args.value("view"));
    if (!screenshot)
        backend.attachAlerts(&alerts);

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &backend, &Backend::retranslate);
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    QObject::connect(&engine, &QQmlEngine::warnings, [](const QList<QQmlError> &errors) {
        for (const auto &error : errors)
            fprintf(stderr, "%s\n", qPrintable(error.toString()));
    });
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("look", &look);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    QObject *window = engine.rootObjects().first();
    if (args.isSet("details"))
        window->setProperty("detailsVisible", true);
    if (args.isSet("help-overlay"))
        window->setProperty("helpVisible", true);
    if (args.isSet("show"))
        QMetaObject::invokeMethod(window, "show", Q_ARG(QVariant, args.value("show")));
    if (screenshot) {
        QTimer::singleShot(900, &app, [&] {
            auto *quick = qobject_cast<QQuickWindow *>(window);
            const bool saved = quick && quick->grabWindow().save(args.value("screenshot"));
            app.exit(saved ? 0 : 1);
        });
    }
    return app.exec();
}
