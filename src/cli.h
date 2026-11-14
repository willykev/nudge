#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

// The commands that need no window, for scripts and coding agents. Their
// output stays in English whatever the desktop's language is.
bool isCliCommand(const QString &word);
QString cliSummary();
int runCli(const QStringList &arguments);

// The background service: alerts for reminders while no window is open.
// `session` is the environment it was started with, before Nudge set its own
// for drawing offscreen; the windows it opens get that one back.
int runDaemon(const QProcessEnvironment &session);

// What clicking a background alert runs when no window is open: `nudge open`
// in the session's environment, through systemd-run when there is one, so the
// window has a scope of its own and turning the service off leaves it open.
struct WindowLaunch {
    QString program;
    QStringList arguments;
    QProcessEnvironment environment;
};
WindowLaunch windowLaunch(const QString &nudge, const QProcessEnvironment &session, const QString &systemdRun);
