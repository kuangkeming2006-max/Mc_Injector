#pragma once

#include "ThreadQueue.h"

#include <windows.h>

#include <functional>
#include <optional>
#include <string>

namespace cli::console {

// Call once before any output: detects whether stdout is a real console and
// aligns the console input codepage with UTF-8 so pasted keys/names decode.
void initialize();

void print(const std::wstring &line);   // appends CRLF
void printRaw(const std::wstring &text);
void prompt();

// Blocking stdin reader thread. drainInput() delivers each line to the
// callback on the caller's thread; nullopt signals EOF (Ctrl+Z or closed
// pipe). Must be called from the main loop whenever inputEvent() is signaled.
void startInput(std::function<void(const std::optional<std::wstring> &)> onLine);
// Releases the reader thread without joining it: it may still be blocked in
// ReadFile, and the process exit terminates it. Call once at shutdown.
void stopInput();
void drainInput();
HANDLE inputEvent();

} // namespace cli::console
