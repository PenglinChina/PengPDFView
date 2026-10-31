// =============================================================================
//  TextSelection.h
//  Reading-order model for text selection.
//
//  PDF content streams are stored in *drawing* order, which is not always the
//  order a human reads: table-of-contents pages, for example, often draw every
//  page number in a separate pass.  Selecting by glyph index would therefore
//  jump across the page.  VisualOrder groups glyphs into lines and sorts them
//  top-to-bottom / left-to-right, giving selection and copy a natural order
//  while the extractor keeps the raw content order for search.
// =============================================================================
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "Pdf/PdfTextTypes.h"

namespace pengpdf {

class VisualOrder {
public:
    // The page must outlive the order object.
    explicit VisualOrder(const pdf::TextPage& page);

    size_t Count() const { return m_glyphs.size(); }

    // Rank -> index into TextPage::glyphs, and the inverse mapping.  Ranks are
    // reading-order positions; glyph indices are content-stream positions.
    size_t GlyphAtRank(size_t rank) const;
    size_t RankOfGlyph(size_t glyphIndex) const;

    // Reading order rank closest to a normalised point.  The visual line is
    // chosen first and the horizontal position second, so dragging past the
    // end of a line keeps the selection on that line instead of jumping to a
    // neighbouring one.
    size_t RankForPoint(float x, float y) const;

    // Plain text of the inclusive rank range, with line breaks inserted where
    // the visual line changes and spaces where geometry implies a word gap.
    std::wstring TextInRange(size_t firstRank, size_t lastRank) const;

    // One rectangle per visual line covered by the rank range.
    std::vector<pdf::TextRect> RectanglesInRange(size_t firstRank,
                                                 size_t lastRank) const;

    // Expands a rank to the surrounding word on the same visual line.
    void WordRangeAt(size_t rank, size_t& firstRank, size_t& lastRank) const;

private:
    bool SameLine(size_t rankA, size_t rankB) const;

    // Vertical extent and rank range of one visual line.
    struct LineInfo {
        float top = 0.0f;
        float bottom = 0.0f;
        size_t firstRank = 0;
        size_t lastRank = 0;
    };

    const pdf::TextPage& m_page;
    std::vector<size_t> m_glyphs;    // rank -> glyph index
    std::vector<size_t> m_rankOf;    // glyph index -> rank
    std::vector<int> m_lineOfRank;   // rank -> visual line number
    std::vector<LineInfo> m_lines;   // visual lines in reading order
};

}  // namespace pengpdf
