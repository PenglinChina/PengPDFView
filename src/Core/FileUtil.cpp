// =============================================================================
//  FileUtil.cpp
//  Thin wrappers around the Win32 file and path APIs, deliberately free of any
//  PDF or UI dependency so that the parser, the settings store and the test
//  harness can all share them.  All paths stay UTF-16 end to end, which keeps
//  non-ASCII file names working regardless of the system code page.
// =============================================================================
#include "Core/FileUtil.h"

#include <windows.h>

#include "Core/Utf.h"

namespace pengpdf {

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out,
                   std::wstring* errorMessage) {
    out.clear();

    // FILE_SHARE_READ | FILE_SHARE_WRITE lets us open documents that are also
    // open in another application (for example a PDF that is still downloading
    // or one that the user views in a browser at the same time).
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (errorMessage != nullptr) {
            *errorMessage = Format(L"Cannot open the file (error %lu).",
                                   ::GetLastError());
        }
        return false;
    }

    LARGE_INTEGER size = {};
    if (::GetFileSizeEx(file, &size) == FALSE) {
        if (errorMessage != nullptr) {
            *errorMessage = L"Cannot determine the file size.";
        }
        ::CloseHandle(file);
        return false;
    }

    if (size.QuadPart <= 0) {
        if (errorMessage != nullptr) {
            *errorMessage = L"The file is empty.";
        }
        ::CloseHandle(file);
        return false;
    }

    // Reject absurdly large files up front: they cannot be a sensible document
    // and would otherwise exhaust memory before the parser reports an error.
    constexpr int64_t kMaxFileSize = 1024LL * 1024LL * 1024LL;  // 1 GiB
    if (size.QuadPart > kMaxFileSize) {
        if (errorMessage != nullptr) {
            *errorMessage = L"The file is larger than 1 GB.";
        }
        ::CloseHandle(file);
        return false;
    }

    try {
        out.resize(static_cast<size_t>(size.QuadPart));
    } catch (const std::bad_alloc&) {
        if (errorMessage != nullptr) {
            *errorMessage = L"Not enough memory to load the document.";
        }
        ::CloseHandle(file);
        return false;
    }

    size_t totalRead = 0;
    // ReadFile() takes a DWORD byte count, so the file is consumed in chunks of
    // at most 256 MiB.  A file that shrinks between GetFileSizeEx() and the
    // read (or a share-mode writer truncating it) ends the loop at chunk == 0
    // and is returned in its shorter form instead of failing.
    while (totalRead < out.size()) {
        DWORD chunk = 0;
        const DWORD remaining = static_cast<DWORD>(
            (out.size() - totalRead > 0x10000000) ? 0x10000000 : out.size() - totalRead);
        if (::ReadFile(file, out.data() + totalRead, remaining, &chunk, nullptr) == FALSE) {
            if (errorMessage != nullptr) {
                *errorMessage = L"Reading the file failed.";
            }
            ::CloseHandle(file);
            out.clear();
            return false;
        }
        if (chunk == 0) {
            break;
        }
        totalRead += chunk;
    }
    ::CloseHandle(file);

    if (totalRead != out.size()) {
        out.resize(totalRead);
    }
    return true;
}

std::wstring GetFileName(const std::wstring& path) {
    // Both separators are accepted so that forward slashes coming from the
    // command line or from shell drag and drop behave like native paths.
    const size_t position = path.find_last_of(L"\\/");
    if (position == std::wstring::npos) {
        return path;
    }
    return path.substr(position + 1);
}

std::wstring GetFileStem(const std::wstring& path) {
    std::wstring name = GetFileName(path);
    const size_t dot = name.find_last_of(L'.');
    // dot == 0 is a hidden dotfile (".gitignore"): keep the whole name instead
    // of turning it into an empty stem.
    if (dot != std::wstring::npos && dot > 0) {
        name.resize(dot);
    }
    return name;
}

std::wstring GetDirectoryName(const std::wstring& path) {
    const size_t position = path.find_last_of(L"\\/");
    if (position == std::wstring::npos) {
        return std::wstring();
    }
    return path.substr(0, position);
}

std::wstring GetFullPath(const std::wstring& path) {
    if (path.empty()) {
        return std::wstring();
    }
    // The buffer covers 520 characters, well past MAX_PATH.  If the input does
    // not fit (or GetFullPathNameW fails for another reason), return it
    // untouched rather than a truncated path: a truncated path could silently
    // refer to a different file.
    wchar_t buffer[MAX_PATH * 2] = {};
    const DWORD length = ::GetFullPathNameW(path.c_str(),
                                            static_cast<DWORD>(std::size(buffer)),
                                            buffer, nullptr);
    if (length == 0 || length >= std::size(buffer)) {
        return path;
    }
    return std::wstring(buffer, length);
}

std::wstring MakePathKey(const std::wstring& path) {
    // Windows file names are case-insensitive; normalising the path keeps
    // bookmark entries stable when a document is opened through a different
    // spelling of the same location.
    return ToLower(GetFullPath(path));
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

}  // namespace pengpdf
