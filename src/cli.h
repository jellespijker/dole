#pragma once

#include <QStringList>

// Commands for scripts and agents that need no display: dole new, check,
// render, help, and skill.
bool isCliCommand(const QString &word);
QString cliSummary();
int runCli(const QStringList &arguments);
