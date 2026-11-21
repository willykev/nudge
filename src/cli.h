#pragma once

#include <QCoreApplication>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <optional>

// The commands that need no window, for scripts and coding agents. Their
// output stays in English whatever the desktop's language is.
bool isCliCommand(const QString &word);
QString cliSummary();
int runCli(const QStringList &arguments, const QString &unitFile = "/usr/lib/systemd/user/nudge.service");
int runCliInDesktopLocale(const QStringList &arguments, const QString &languageFile = {});

// The background service: alerts for reminders while no window is open.
// `session` is the environment it was started with, before Nudge set its own
// for drawing offscreen; the windows it opens get that one back.
int runDaemon(const QProcessEnvironment &session);
std::optional<bool> backgroundEnabled(const QString &output, int exitCode);

// What clicking a background alert runs when no window is open: `nudge open`
// in the session's environment, through systemd-run when there is one, so the
// window has a scope of its own and turning the service off leaves it open.
struct WindowLaunch {
    QString program;
    QStringList arguments;
    QProcessEnvironment environment;
};
WindowLaunch windowLaunch(const QString &nudge, const QProcessEnvironment &session, const QString &systemdRun,
                          qint64 reminderId = 0);

// The window and the background service quit through the event loop on
// SIGTERM, SIGINT and SIGHUP, except one they were started ignoring.
void quitOnStop(QCoreApplication &app);
