// nudge — reminders for the Omarchy desktop: lists, due dates, repeats,
// subtasks, and a nudge when something is due.

#include <QCommandLineParser>
#include <QDateTime>
#include <QFile>
#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QProcessEnvironment>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <cstdio>
#include <memory>

#include "alerts.h"
#include "backend.h"
#include "cli.h"
#include "look.h"
#include "store.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    // The environment as the session gave it, before Nudge sets its own
    // below: a window the background service opens gets this one back.
    const QProcessEnvironment session = QProcessEnvironment::systemEnvironment();

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
        // "at 5:30" is the same time typed here as in the window.
        return runCliInDesktopLocale(app.arguments());
    }

    // The background service shows notifications in the desktop's language,
    // which needs the toolkit but no window.
    const bool daemon = first == "daemon";
    if (daemon)
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QGuiApplication app(argc, argv);
    app.setApplicationName("nudge");
    app.setApplicationVersion(NUDGE_VERSION);
    quitOnStop(app);

    // Matches the window to nudge.desktop, so the compositor (Wayland app_id)
    // and the launcher pick up the installed icon.
    app.setDesktopFileName("nudge");
    app.setWindowIcon(QIcon::fromTheme("nudge"));

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    if (daemon)
        return runDaemon(session);

    QCommandLineParser args;
    args.setApplicationDescription("Reminders for the Omarchy desktop.\n\n" + cliSummary());
    args.addHelpOption();
    args.addPositionalArgument("open", "Open the window", "open");
    args.addOption({"view", "Start on a view: today, scheduled, all, flagged, completed", "view"});
    args.addOption({"reminder", "Show a reminder by its id", "id"});
    args.addOption({"details", "Start with the details pane open"});
    args.addOption({"help-overlay", "Start with the shortcuts showing"});
    args.addOption({"show", "Start with a dialog open: list, date, move, menu, delete", "dialog"});
    args.addOption({"screenshot", "Save a picture of the window and exit", "file"});
    // What a picture shows, so that it shows the same wherever it is taken.
    args.addOption({"size", "With --screenshot: the size of the window", "WxH"});
    args.addOption({"theme",
                    "With --screenshot: an Omarchy theme to show instead of the default: its name, or its folder",
                    "theme"});
    args.addOption({"now", "With --screenshot: the time to take it to be, like 2026-11-16T09:41", "time"});
    args.addOption({"type", "With --screenshot: start a new reminder with these words typed", "words"});
    args.addOption({"new-window", "Open another window even when one is open"});
    args.process(app);

    // Reminders are one thing to look at, so launching Nudge again brings
    // the window that is open to the front rather than opening a second.
    const bool screenshot = args.isSet("screenshot");
    const qint64 reminder = args.value("reminder").toLongLong();
    if (args.isSet("reminder") && reminder <= 0) {
        fprintf(stderr, "nudge: --reminder needs a positive reminder id\n");
        return 1;
    }
    if (!screenshot && !args.isSet("new-window")) {
        if (revealNudgeWindow(Store::defaultPath(), reminder) || (!reminder && focusNudgeWindow()))
            return 0;
    }

    // The plainest style, as Hype uses: every control is drawn by Nudge's
    // own components, in the colors and states of the Omarchy theme.
    QQuickStyle::setStyle("Basic");

    // A screenshot shows Omarchy's default look, whatever theme is on, or
    // the theme it asks for as that theme ships: a folder stands in for
    // Omarchy's own, with that theme as the current one.
    QTemporaryDir stand;
    QString showing;
    QString database;
    if (screenshot) {
        const QString source = Store::defaultPath();
        if (!stand.isValid() || Store::isDatabaseFile(source, args.value("screenshot"))) {
            fprintf(stderr, "nudge: choose a picture path separate from the reminders database\n");
            return 1;
        }
        database = stand.filePath("picture.db");
        const QString error = Store::copyForPicture(source, database);
        if (!error.isEmpty()) {
            fprintf(stderr, "nudge: could not copy reminders for the picture: %s\n", qPrintable(error));
            return 1;
        }
        showing = "/nonexistent";
        if (args.isSet("theme")) {
            const QString asked = args.value("theme");
            const QString found = Look::themeFolder(asked);
            if (found.isEmpty() || !QFile::link(found, stand.filePath("theme"))) {
                fprintf(stderr, "nudge: there is no theme %s\n", qPrintable(asked));
                return 1;
            }
            showing = stand.path();
        }
        // A field being typed in shows its caret, not a blink caught off.
        QGuiApplication::styleHints()->setCursorFlashTime(0);
    }
    Theme theme(showing);
    Look look(showing);

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
    Store store(database);
    std::unique_ptr<DesktopAlerts> alerts;
    Backend backend(&store);
    if (screenshot)
        backend.setService({}, {});
    else
        store.watch();
    if (screenshot && args.isSet("now")) {
        const QDateTime now = QDateTime::fromString(args.value("now"), Qt::ISODate);
        if (!now.isValid()) {
            fprintf(stderr, "nudge: %s is not a time like 2026-11-16T09:41\n", qPrintable(args.value("now")));
            return 1;
        }
        backend.setClock([now] { return now; });
    }
    if (args.isSet("view"))
        backend.setViewKey(args.value("view"));
    if (!screenshot) {
        alerts = std::make_unique<DesktopAlerts>(false);
        backend.attachAlerts(alerts.get());
    }

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
    if (screenshot && args.isSet("size")) {
        const QStringList parts = args.value("size").split('x');
        if (parts.size() == 2 && parts[0].toInt() > 0 && parts[1].toInt() > 0) {
            window->setProperty("width", parts[0].toInt());
            window->setProperty("height", parts[1].toInt());
        }
    }
    if (args.isSet("details"))
        window->setProperty("detailsVisible", true);
    if (args.isSet("help-overlay"))
        window->setProperty("helpVisible", true);
    if (args.isSet("show"))
        QMetaObject::invokeMethod(window, "show", Q_ARG(QVariant, args.value("show")));
    if (screenshot && args.isSet("type"))
        QMetaObject::invokeMethod(window, "type", Q_ARG(QVariant, args.value("type")));
    if (!screenshot) {
        registerNudgeWindow(&backend);
        if (reminder)
            backend.revealReminder(store.path(), reminder);
    }
    if (screenshot) {
        QTimer::singleShot(900, &app, [&] {
            auto *quick = qobject_cast<QQuickWindow *>(window);
            const bool saved = quick && quick->grabWindow().save(args.value("screenshot"));
            app.exit(saved ? 0 : 1);
        });
    }
    return app.exec();
}
