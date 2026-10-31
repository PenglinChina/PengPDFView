// =============================================================================
//  TextSearch.h
//  Case-insensitive page search over extracted glyphs.
//
//  Raw PDF text frequently omits spaces (words are positioned individually) and
//  splits sentences across lines.  The index therefore flattens a page into a
//  whitespace-normalised, lower-cased string while remembering which glyph
//  produced every character.  Matches are reported as glyph ranges so the UI
//  can paint the exact boxes without knowing anything about PDF syntax.
// =============================================================================
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "Pdf/PdfTextTypes.h"

namespace pengpdf {

class PageTextIndex {
public:
    // The page must outlive the index (the service keeps cached pages alive
    // for the duration of a search).
    explicit PageTextIndex(const pdf::TextPage& page);

    const std::wstring& Text() const { return m_text; }
    bool Empty() const { return m_text.empty(); }

    // Finds every non-overlapping occurrence of `query`.  `pageIndex` is copied
    // into the returned matches so callers can keep one flat result list.
    std::vector<pdf::TextMatch> FindAll(std::wstring_view query, int pageIndex) const;

    // Same as FindAll but expects a query already passed through
    // NormalizeSearchQuery.  Searching many pages reuses one normalised query
    // instead of normalising it once per page.
    std::vector<pdf::TextMatch> FindAllNormalized(
        std::wstring_view normalizedQuery, int pageIndex) const;

private:
    const pdf::TextPage* m_page = nullptr;
    std::wstring m_text;
    // Character index in m_text -> glyph index in the source page.
    std::vector<size_t> m_glyphOfCharacter;
};

// Applies the same normalisation as PageTextIndex to a user query: trims and
// collapses whitespace, lower-cases letters and drops empty queries.
std::wstring NormalizeSearchQuery(std::wstring_view query);

}  // namespace pengpdf
