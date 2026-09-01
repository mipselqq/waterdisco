#pragma once

void Windows_SetCrashHandler();

// Until this runs, a crash produces no files. Call once Logging::Init() has.
void Windows_SetCrashDumpPath();

// WER LocalDumps catches what the exception filter cannot (__fastfail, heap corruption); needs admin, so it no-ops otherwise.
void Windows_ConfigureWER();
