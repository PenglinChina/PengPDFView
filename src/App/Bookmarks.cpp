// =============================================================================
//  Bookmarks.cpp
//  Tab separated storage format:
//
//      PengPDFView-Bookmarks 1
//      <path key>\t<page>\t<escaped title>
//
//  Tabs, newlines and backslashes inside the fields are backslash escaped.
// =============================================================================
#include "App/Bookmarks.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <string>

#include "Core/FileUtil.h"
#include "Core/Log.h"
#include "Core/Paths.h"
#include "Core/Utf.h"

namespace pengpdf {
namespace {

// First line of the file.  The trailing number is a format version, but the
// loader only checks the prefix (see Load()), so it is informational for now.
constexpr wchar_t kFileHeader[] = L"PengPDFView-Bookmarks 1";

// Serialises one TSV field.  Titles come from the outline and can contain
// tabs, line breaks or backslashes, all of which would destroy the three
// column layout; escaping keeps the file line based while the payload itself
// stays plain UTF-8 so it can still be inspected with a text editor.
std::string EscapeField(const std::wstring& text) {
    const std::string utf8 = WideToUtf8(text);
    std::string result;
    result.reserve(utf8.size() + 8);
    for (char character : utf8) {
        switch (character) {
            case '\\': result += "\\\\"; break;
            case '\t': result += "\\t"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            default: result.push_back(character); break;
        }
    }
    return result;
}

// Inverse of EscapeField().  Unknown escape sequences are copied verbatim and
// a trailing lone backslash is kept, so a hand-edited or future-version file
// loses as little text as possible instead of being rejected.  Bytes that are
// not valid UTF-8 fall back to the ANSI code page inside Utf8ToWide().
std::wstring UnescapeField(const std::string& text) {
    std::string decoded;
    decoded.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (character != '\\' || index + 1 >= text.size()) {
            decoded.push_back(character);
            continue;
        }
        const char escaped = text[++index];
        switch (escaped) {
            case 't': decoded.push_back('\t'); break;
            case 'n': decoded.push_back('\n'); break;
            case 'r': decoded.push_back('\r'); break;
            case '\\': decoded.push_back('\\'); break;
            default: decoded.push_back(escaped); break;
        }
    }
    return Utf8ToWide(decoded);
}

// Writes the file in place: CREATE_ALWAYS truncates an existing file and the
// null share mode prevents another instance from reading a half-written file.
// The write is not atomic (no temp file plus rename), so a crash mid-write
// loses the previous content as well; acceptable for this low-value data.
bool WriteUtf8File(const std::wstring& path, const std::string& content) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok =
        ::WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written,
                    nullptr);
    ::CloseHandle(file);
    return ok != FALSE;
}

}  // namespace

void BookmarkStore::Load() {
    m_documents.clear();

    std::vector<uint8_t> bytes;
    std::wstring error;
    if (!ReadFileBytes(BookmarksFilePath(), bytes, &error)) {
        return;
    }

    const std::string text(bytes.begin(), bytes.end());
    size_t start = 0;
    bool firstLine = true;
    // Line based parse.  The file is written with CRLF but may have been
    // copied or edited anywhere, so both CRLF and LF endings are accepted.
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        start = end + 1;

        // The header is optional: files written by older builds start straight
        // with data.  Only the prefix is checked, never the version number.
        if (firstLine) {
            firstLine = false;
            if (line.rfind("PengPDFView-Bookmarks", 0) == 0) {
                continue;
            }
        }
        if (line.empty()) {
            continue;
        }

        // Split into exactly three fields.
        std::string fields[3];
        size_t fieldIndex = 0;
        for (size_t index = 0; index < line.size() && fieldIndex < 3; ++index) {
            const char character = line[index];
            if (character == '\t' && fieldIndex < 2) {
                ++fieldIndex;
                continue;
            }
            fields[fieldIndex].push_back(character);
        }
        // A malformed record (wrong column count, empty path, negative page)
        // is skipped rather than aborting the load: a single corrupt line must
        // not discard the bookmarks of every other document.
        if (fieldIndex != 2) {
            continue;
        }

        const std::wstring key = Utf8ToWide(fields[0]);
        // _wtoi is deliberately lenient: a page field with trailing junk still
        // yields a usable prefix instead of losing the whole record.
        const int page = _wtoi(UnescapeField(fields[1]).c_str());
        if (key.empty() || page < 0) {
            continue;
        }
        Bookmark bookmark;
        bookmark.page = page;
        bookmark.title = UnescapeField(fields[2]);
        m_documents[key].push_back(std::move(bookmark));
    }

    m_dirty = false;
}

void BookmarkStore::Save() {
    // Iterating the map yields a deterministic path order, which keeps the
    // file stable across runs (and diff friendly); each document's list is
    // already sorted by page.
    std::string output = WideToUtf8(kFileHeader) + "\r\n";
    for (const auto& document : m_documents) {
        for (const Bookmark& bookmark : document.second) {
            output += EscapeField(document.first);
            output += '\t';
            output += std::to_string(bookmark.page);
            output += '\t';
            output += EscapeField(bookmark.title);
            output += "\r\n";
        }
    }
    if (!WriteUtf8File(BookmarksFilePath(), output)) {
        // Keeping m_dirty set means a later Save() retries the whole file.
        LogError(L"Cannot write the bookmark file %s", BookmarksFilePath().c_str());
        return;
    }
    m_dirty = false;
}

std::vector<Bookmark>& BookmarkStore::ForDocument(const std::wstring& documentPath) {
    // operator[] default-constructs an empty list for unknown documents.  Such
    // a list produces no lines in Save(), so lookups never add junk to disk.
    return m_documents[MakePathKey(documentPath)];
}

bool BookmarkStore::Has(const std::wstring& documentPath, int page) const {
    const auto it = m_documents.find(MakePathKey(documentPath));
    if (it == m_documents.end()) {
        return false;
    }
    return std::any_of(it->second.begin(), it->second.end(),
                       [page](const Bookmark& bookmark) { return bookmark.page == page; });
}

void BookmarkStore::Add(const std::wstring& documentPath, int page,
                        const std::wstring& title) {
    std::vector<Bookmark>& bookmarks = ForDocument(documentPath);
    if (std::any_of(bookmarks.begin(), bookmarks.end(),
                    [page](const Bookmark& bookmark) { return bookmark.page == page; })) {
        return;  // already bookmarked
    }
    Bookmark bookmark;
    bookmark.page = page;
    bookmark.title = title;
    bookmarks.push_back(std::move(bookmark));

    // Keep the list ordered by page so the sidebar reads naturally.
    std::sort(bookmarks.begin(), bookmarks.end(),
              [](const Bookmark& left, const Bookmark& right) {
                  return left.page < right.page;
              });
    m_dirty = true;
}

bool BookmarkStore::RemoveAt(const std::wstring& documentPath, size_t index) {
    std::vector<Bookmark>& bookmarks = ForDocument(documentPath);
    if (index >= bookmarks.size()) {
        return false;
    }
    bookmarks.erase(bookmarks.begin() + static_cast<std::ptrdiff_t>(index));
    m_dirty = true;
    return true;
}

}  // namespace pengpdf
