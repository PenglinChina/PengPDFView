// =============================================================================
//  Log.cpp
//  Appends timestamped UTF-8 lines to the per-user log file.  Every line is
//  written with its own short-lived handle opened for append, which keeps the
//  handle lifetime trivial and lets several instances of the application (or
//  the render thread and the UI thread) log without sharing file state.
// =============================================================================
#include "Core/Log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

#include "Core/Paths.h"
#include "Core/Utf.h"

namespace pengpdf {
namespace {

std::mutex g_logMutex;

// Appends one formatted line to the log file.  The log is deliberately kept
// small (it is truncated once it grows past a couple of megabytes) because it
// only exists to help with field diagnostics.
//
// The whole function runs under one mutex: formatting and writing must not be
// split, otherwise concurrent lines would interleave inside the file.
void WriteLine(const wchar_t* level, const wchar_t* format, va_list args) {
    std::lock_guard<std::mutex> lock(g_logMutex);

    // Truncate the log when it becomes unreasonably large.  Only the attribute
    // block is fetched, never the contents.  The deletion can legitimately
    // fail (another instance may still hold an open handle) - in that case the
    // log simply keeps growing, which is harmless.
    const std::wstring path = LogFilePath();
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) != FALSE &&
        attributes.nFileSizeHigh == 0 && attributes.nFileSizeLow > 2u * 1024u * 1024u) {
        ::DeleteFileW(path.c_str());
    }

    // Format the message body first so that formatting errors cannot corrupt
    // the log structure.  _TRUNCATE keeps an over-long message from failing
    // the whole call; it is simply cut at the buffer limit.
    wchar_t message[2048] = {};
    _vsnwprintf_s(message, _TRUNCATE, format, args);

    SYSTEMTIME now = {};
    ::GetLocalTime(&now);

    const std::wstring line = Format(
        L"%04u-%02u-%02u %02u:%02u:%02u [%s] %s\r\n", now.wYear, now.wMonth,
        now.wDay, now.wHour, now.wMinute, now.wSecond, level, message);

    const std::string utf8 = WideToUtf8(line);

    // FILE_APPEND_DATA moves every write to the end of the file, so a second
    // instance appending at the same time does not overwrite these bytes.
    // FILE_SHARE_READ lets the user tail the log while the application runs.
    HANDLE file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    ::WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    ::CloseHandle(file);
}

}  // namespace

void LogInfo(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    WriteLine(L"INFO", format, args);
    va_end(args);
}

void LogError(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    WriteLine(L"ERROR", format, args);
    va_end(args);
}

}  // namespace pengpdf
