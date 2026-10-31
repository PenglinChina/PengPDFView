// =============================================================================
//  Bookmarks.h
//  User bookmarks, stored separately from the document outline.
//
//  Bookmarks are remembered per document (keyed by the normalised absolute
//  path) in %APPDATA%\PengPDFView\bookmarks.tsv so that the PDF file itself is
//  never modified.
// =============================================================================
#pragma once

#include <map>
#include <string>
#include <vector>

namespace pengpdf {

// One entry of a document's bookmark list.  The title is a snapshot taken when
// the bookmark was created (normally the outline entry that starts on the
// page).  It may be empty, in which case the sidebar falls back to a localised
// "Page N" label; the bookmark stays valid even if the outline changes later.
struct Bookmark {
    int page = 0;           // zero based page index
    std::wstring title;     // user visible label
};

// Owns the bookmark lists of every document seen during this session.  Not
// thread safe: call it from the UI thread only.
class BookmarkStore {
public:
    // Replaces the in-memory state with the contents of bookmarks.tsv.  A
    // missing or unreadable file quietly leaves the store empty, because the
    // application must still start on a fresh profile.
    void Load();

    // Rewrites the whole file.  Write failures are logged only: bookmarks are
    // not important enough to interrupt the user with a message box.
    void Save();

    // Returns the bookmark list of one document, creating an empty list on
    // first use.  std::map keeps its nodes alive, so the reference survives
    // operations on other documents; the vector's own elements are of course
    // invalidated by Add()/RemoveAt().
    std::vector<Bookmark>& ForDocument(const std::wstring& documentPath);

    // True when `page` already carries a bookmark.  Uses a linear scan: the
    // lists are short and this avoids maintaining a second index.
    bool Has(const std::wstring& documentPath, int page) const;

    // Adds a bookmark unless the page is already bookmarked.  The list is kept
    // sorted by page number so that the sidebar reads in document order.
    void Add(const std::wstring& documentPath, int page, const std::wstring& title);

    // Removes the bookmark at `index` (an index into the vector returned by
    // ForDocument).  Returns false when the index is out of range.
    bool RemoveAt(const std::wstring& documentPath, size_t index);

    bool IsDirty() const { return m_dirty; }

private:
    // Key = lower case absolute path, value = ordered bookmark list.
    std::map<std::wstring, std::vector<Bookmark>> m_documents;
    bool m_dirty = false;
};

}  // namespace pengpdf
