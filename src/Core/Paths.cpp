// =============================================================================
//  Paths.cpp
//  Resolution of the per-user data directory used for settings, bookmarks and
//  the diagnostic log.
// =============================================================================
#include "Core/Paths.h"

#include <windows.h>
#include <shlobj.h>

#include "Core/Config.h"

namespace pengpdf {
namespace {

// Joins two path fragments and inserts a backslash only when needed.  It does
// not canonicalise anything: the caller passes either an absolute base and a
// plain file name, or an empty base when no profile directory is available.
std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) {
        return right;
    }
    if (right.empty()) {
        return left;
    }
    std::wstring result = left;
    if (result.back() != L'\\' && result.back() != L'/') {
        result.push_back(L'\\');
    }
    result.append(right);
    return result;
}

// Returns %APPDATA%.  When the environment variable is missing (rare, but can
// happen inside restricted service accounts) we fall back to the profile path.
std::wstring RoamingAppDataDirectory() {
    wchar_t buffer[MAX_PATH * 2] = {};
    const DWORD length =
        ::GetEnvironmentVariableW(L"APPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
    if (length > 0 && length < std::size(buffer)) {
        return std::wstring(buffer, length);
    }

    PWSTR knownFolder = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &knownFolder)) &&
        knownFolder != nullptr) {
        std::wstring result(knownFolder);
        // The shell allocates this string with the COM task allocator; it must
        // be released with CoTaskMemFree, not delete or LocalFree.
        ::CoTaskMemFree(knownFolder);
        return result;
    }
    return std::wstring();
}

}  // namespace

bool EnsureDirectory(const std::wstring& directory) {
    if (directory.empty()) {
        return false;
    }
    if (::CreateDirectoryW(directory.c_str(), nullptr) != FALSE) {
        return true;
    }
    // Another instance (or a previous run) already created it: that is success
    // for our purpose.  Access denied, a missing parent or an invalid name are
    // real failures, but the callers only log them later when a write fails.
    return ::GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring AppDataDirectory() {
    // Function-local static: the profile directory is resolved and created
    // exactly once per process, and C++11 guarantees the initialisation is
    // thread safe even if the render thread logs before the UI has started.
    static const std::wstring directory = [] {
        const std::wstring base = RoamingAppDataDirectory();
        const std::wstring path = JoinPath(base, kAppInternalName);
        EnsureDirectory(path);
        return path;
    }();
    return directory;
}

std::wstring SettingsFilePath() {
    return JoinPath(AppDataDirectory(), L"settings.ini");
}

std::wstring BookmarksFilePath() {
    return JoinPath(AppDataDirectory(), L"bookmarks.tsv");
}

std::wstring LogFilePath() {
    return JoinPath(AppDataDirectory(), L"pengpdfview.log");
}

}  // namespace pengpdf
