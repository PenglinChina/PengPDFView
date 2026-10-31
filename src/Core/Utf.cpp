// =============================================================================
//  Utf.cpp
//  Implementation of the UTF-8 / UTF-16 conversion helpers.
//
//  Every Win32 call in the application uses the W (UTF-16) entry points; the
//  "A" variants would go through the system code page and are the classic
//  source of mojibake on Chinese Windows.  UTF-8 only appears at the file
//  format boundary (PDF strings, bookmarks, settings, the log), so the
//  conversions are kept in this one file.
// =============================================================================
#include "Core/Utf.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace pengpdf {

std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }

    // MultiByteToWideChar() takes an int length; an input larger than 2 GiB is
    // not realistic for this application, so the narrowing cast is safe.
    const int byteCount = static_cast<int>(utf8.size());
    const int wideCount = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), byteCount, nullptr, 0);

    if (wideCount <= 0) {
        // The buffer was not valid UTF-8 (for example a legacy ANSI file path).
        // Fall back to the system ANSI code page so that we show *something*
        // readable instead of dropping the text entirely.
        const int ansiCount = ::MultiByteToWideChar(
            CP_ACP, 0, utf8.data(), byteCount, nullptr, 0);
        if (ansiCount <= 0) {
            return std::wstring();
        }
        std::wstring fallback(static_cast<size_t>(ansiCount), L'\0');
        ::MultiByteToWideChar(CP_ACP, 0, utf8.data(), byteCount, fallback.data(),
                              ansiCount);
        return fallback;
    }

    std::wstring result(static_cast<size_t>(wideCount), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), byteCount,
                          result.data(), wideCount);
    return result;
}

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) {
        return std::string();
    }

    const int wideCount = static_cast<int>(wide.size());
    const int byteCount = ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), wideCount, nullptr, 0, nullptr, nullptr);
    if (byteCount <= 0) {
        return std::string();
    }

    std::string result(static_cast<size_t>(byteCount), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideCount, result.data(),
                          byteCount, nullptr, nullptr);
    return result;
}

std::wstring ToLower(std::wstring_view text) {
    // Hand written instead of towlower() so that the result is independent of
    // the C locale (the Turkish dotless-i rule would otherwise change path
    // keys depending on the user's regional settings).
    std::wstring result(text);
    for (wchar_t& ch : result) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return result;
}

std::string ToLowerAscii(std::string_view text) {
    // Same deliberate restriction to A-Z for UTF-8 strings: folding bytes
    // above 0x7F would corrupt multi-byte sequences.
    std::string result(text);
    for (char& ch : result) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return result;
}

std::wstring_view TrimWide(std::wstring_view text) {
    // Only ASCII whitespace is trimmed; the settings parser and the search
    // query normaliser only ever deal with these four characters.
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == L' ' || text[begin] == L'\t' ||
                           text[begin] == L'\r' || text[begin] == L'\n')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == L' ' || text[end - 1] == L'\t' ||
                           text[end - 1] == L'\r' || text[end - 1] == L'\n')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::wstring ReplaceAll(std::wstring_view subject, std::wstring_view needle,
                        std::wstring_view replacement) {
    if (needle.empty()) {
        return std::wstring(subject);
    }

    // Single pass, appending the untouched spans between matches.  The
    // replacement is never rescanned, which makes the function safe to use for
    // things like escaping where a replacement may contain the needle.
    std::wstring result;
    result.reserve(subject.size());
    size_t position = 0;
    for (;;) {
        const size_t found = subject.find(needle, position);
        if (found == std::wstring_view::npos) {
            result.append(subject.substr(position));
            break;
        }
        result.append(subject.substr(position, found - position));
        result.append(replacement);
        position = found + needle.size();
    }
    return result;
}

std::wstring Format(const wchar_t* format, ...) {
    if (format == nullptr) {
        return std::wstring();
    }

    va_list args;
    va_start(args, format);
    // Two pass formatting: _vscwprintf() only measures the result, then the
    // buffer below is sized exactly.  The list has to be copied because the
    // first pass already consumes it.
    va_list copy;
    va_copy(copy, args);

    const int count = _vscwprintf(format, copy);
    va_end(copy);

    if (count < 0) {
        // Invalid format string: return it verbatim so the caller still gets
        // something displayable instead of an empty label.
        va_end(args);
        return std::wstring(format);
    }

    std::wstring result(static_cast<size_t>(count), L'\0');
    // size() + 1 because _vsnwprintf_s also stores the terminating null.
    // _TRUNCATE protects the buffer if the second pass ever produces more
    // characters than the first one (it truncates instead of invoking the
    // invalid-parameter handler).
    _vsnwprintf_s(result.data(), result.size() + 1, _TRUNCATE, format, args);
    va_end(args);
    return result;
}

}  // namespace pengpdf
