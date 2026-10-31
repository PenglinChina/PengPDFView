// =============================================================================
//  Log.h
//  Minimal diagnostic logger.  Rendering and parsing problems are written to
//  %APPDATA%\PengPDFView\pengpdfview.log, which makes it possible to debug a
//  user's malformed PDF without attaching a debugger.
// =============================================================================
#pragma once

namespace pengpdf {

// printf-style logging with wide strings: %s refers to a wchar_t* and %ls is
// not needed.  Both functions are thread safe and never throw; a message that
// does not fit the fixed buffer is truncated, and being unable to open the log
// file is silently ignored so logging can never break the application.
void LogInfo(const wchar_t* format, ...);
void LogError(const wchar_t* format, ...);

}  // namespace pengpdf
