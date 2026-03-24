#pragma once

#include <qglobal.h>

void AutoRun_SetEnabled(bool enable);

bool AutoRun_IsEnabled();

#ifdef Q_OS_WIN
void AutoRun_FixPrivilegeIfNeeded();

void AutoRun_MigrateIfNeeded();
#endif
