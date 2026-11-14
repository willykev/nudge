#pragma once

#include <QString>
#include <QStringList>

// The commands that need no window, for scripts and coding agents. Their
// output stays in English whatever the desktop's language is.
bool isCliCommand(const QString &word);
QString cliSummary();
int runCli(const QStringList &arguments);

// The background service: alerts for reminders while no window is open.
int runDaemon();
