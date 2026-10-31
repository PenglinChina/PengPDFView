// =============================================================================
//  TextSearch.cpp
//  Page flattening and hit reporting; see TextSearch.h for the model.
// =============================================================================
#include "Text/TextSearch.h"

#include <algorithm>
#include <cwctype>

namespace pengpdf {
namespace {

// Maximum number of hits kept for one page; protects the UI from pathological
// documents such as a page full of the letter "e".
constexpr size_t kMaxMatchesPerPage = 2000;

// Folds one character to lower case.  ASCII uses a branch-free fast path; the
// rest goes through towlower so accented Latin and CJK stay consistent.
wchar_t FoldCase(wchar_t character) {
    if (character < 0x80) {
        return (character >= L'A' && character <= L'Z')
                   ? static_cast<wchar_t>(character - L'A' + L'a')
                   : character;
    }
    return static_cast<wchar_t>(std::towlower(character));
}

}  // namespace

// Flattens the page once so repeated searches of the same query are simple
// substring scans.
PageTextIndex::PageTextIndex(const pdf::TextPage& page) : m_page(&page) {
    m_text.reserve(page.glyphs.size());
    m_glyphOfCharacter.reserve(page.glyphs.size());

    bool havePrevious = false;
    pdf::TextGlyph previous;

    for (size_t index = 0; index < page.glyphs.size(); ++index) {
        const pdf::TextGlyph& glyph = page.glyphs[index];

        if (havePrevious) {
            if (pdf::StartsNewLine(previous, glyph)) {
                // Line breaks behave like word separators for search.  A space
                // already emitted by the document is never duplicated.
                if (!m_text.empty() && !pdf::IsSpaceCharacter(m_text.back())) {
                    m_text.push_back(L' ');
                    m_glyphOfCharacter.push_back(index);
                }
            } else if (pdf::HasWordGap(previous, glyph)) {
                if (!m_text.empty() && !pdf::IsSpaceCharacter(m_text.back()) &&
                    !pdf::IsSpaceCharacter(glyph.character)) {
                    m_text.push_back(L' ');
                    m_glyphOfCharacter.push_back(index);
                }
            }
        }

        const wchar_t character = FoldCase(glyph.character);
        if (pdf::IsSpaceCharacter(character)) {
            // Collapse runs of whitespace so that line breaks and explicit
            // spaces behave identically.
            if (m_text.empty() || !pdf::IsSpaceCharacter(m_text.back())) {
                m_text.push_back(L' ');
                m_glyphOfCharacter.push_back(index);
            }
        } else {
            m_text.push_back(character);
            m_glyphOfCharacter.push_back(index);
        }

        previous = glyph;
        havePrevious = true;
    }

    // Trailing whitespace never helps a search.
    while (!m_text.empty() && pdf::IsSpaceCharacter(m_text.back())) {
        m_text.pop_back();
        m_glyphOfCharacter.pop_back();
    }
}

std::vector<pdf::TextMatch> PageTextIndex::FindAll(std::wstring_view query,
                                                   int pageIndex) const {
    return FindAllNormalized(NormalizeSearchQuery(query), pageIndex);
}

std::vector<pdf::TextMatch> PageTextIndex::FindAllNormalized(
    std::wstring_view normalizedQuery, int pageIndex) const {
    std::vector<pdf::TextMatch> matches;
    if (normalizedQuery.empty() || m_text.size() < normalizedQuery.size()) {
        return matches;
    }

    // Non-overlapping search: advance past each hit, which matches how readers
    // expect repeated words ("test test") to be counted.
    size_t position = 0;
    while (position + normalizedQuery.size() <= m_text.size() &&
           matches.size() < kMaxMatchesPerPage) {
        const size_t found = m_text.find(normalizedQuery, position);
        if (found == std::wstring::npos) {
            break;
        }

        const size_t firstCharacter = found;
        const size_t lastCharacter = found + normalizedQuery.size() - 1;
        if (lastCharacter < m_glyphOfCharacter.size()) {
            pdf::TextMatch match;
            match.pageIndex = pageIndex;
            match.firstGlyph = m_glyphOfCharacter[firstCharacter];
            match.lastGlyph = m_glyphOfCharacter[lastCharacter];
            if (match.lastGlyph >= match.firstGlyph) {
                match.rects =
                    pdf::MergeGlyphRects(*m_page, match.firstGlyph, match.lastGlyph);
                matches.push_back(std::move(match));
            }
        }
        position = found + normalizedQuery.size();
    }
    return matches;
}

// Query normalisation is public so the service can normalise once per search
// instead of once per page.
std::wstring NormalizeSearchQuery(std::wstring_view query) {
    std::wstring result;
    result.reserve(query.size());
    for (wchar_t character : query) {
        if (pdf::IsSpaceCharacter(character)) {
            if (!result.empty() && !pdf::IsSpaceCharacter(result.back())) {
                result.push_back(L' ');
            }
        } else {
            result.push_back(FoldCase(character));
        }
    }
    while (!result.empty() && pdf::IsSpaceCharacter(result.back())) {
        result.pop_back();
    }
    return result;
}

}  // namespace pengpdf
