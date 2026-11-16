#pragma once

namespace hs {

enum class LogLevel { Debug, Info, Warn, Error };

// Messages below this level are dropped before any formatting happens.
void log_set_level(LogLevel level);
// The level is process-wide; tests read it so they can restore it afterwards.
LogLevel log_level();

// printf-style, one line to stderr: "YYYY-MM-DD HH:MM:SS.mmm [LEVEL] message\n".
// Safe to call from many threads; each line is emitted with one write(2).
// The format attribute lets -Wformat check every call site.
void log_msg(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

}  // namespace hs
