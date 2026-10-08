#pragma once
// Diagnostic log at %LOCALAPPDATA%\MyCam\mycam.log (restarted each launch). Thread-safe.

namespace mycam {

void LogInit();
void Log(const char* fmt, ...);

} // namespace mycam
