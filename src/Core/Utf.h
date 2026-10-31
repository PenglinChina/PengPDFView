// =============================================================================
//  Utf.h
//  Small helpers that convert between the two string flavours used by the app:
//
//    * std::string  - always UTF-8.  Used for PDF syntax and for the file
//                     formats (bookmarks, settings, the log).
//    * std::wstring - UTF-16.  Used for every Win32 API call and for all text
//                     that is measured, painted or stored in the UI.
//
//  Mixing the two by accident is the classic source of mojibake on Chinese
//  Windows installations, so every conversion goes through these functions.
// =============================================================================
#pragma once

#include <string>
#include <string_view>

namespace pengpdf {

// UTF-8 <-> UTF-16 ------------------------------------------------------------
// Utf8ToWide() falls back to the ANSI code page when the input is not valid
// UTF-8 (legacy files, console output): showing slightly wrong text beats
// dropping it.  WideToUtf8() converts through the UTF-8 code page and only
// returns an empty string for empty input or a conversion failure.
std::wstring Utf8ToWide(std::string_view utf8);
std::string  WideToUtf8(std::wstring_view wide);

// Case transformations for ASCII-only identifiers (paths, language tags).
// Only A-Z are folded; non-ASCII letters are copied through unchanged, so a
// path key never gains or loses characters on a Chinese system.
std::wstring ToLower(std::wstring_view text);
std::string  ToLowerAscii(std::string_view text);

// Trim leading/trailing ASCII whitespace.  The result is a view into `text`,
// so the caller must keep the source string alive while using it.
std::wstring_view TrimWide(std::wstring_view text);

// Replace every occurrence of `needle` inside `subject`.  Replacement text is
// not rescanned (no recursive expansion); an empty needle yields a copy of the
// subject.
std::wstring ReplaceAll(std::wstring_view subject, std::wstring_view needle,
                        std::wstring_view replacement);

// printf-style formatting for wide strings.  Used by the localization layer to
// substitute placeholders such as "%d" or "%s".  If formatting itself fails the
// format string is returned unchanged, so the result is never empty by
// accident (the caller always receives a usable string).
std::wstring Format(const wchar_t* format, ...);

}  // namespace pengpdf
