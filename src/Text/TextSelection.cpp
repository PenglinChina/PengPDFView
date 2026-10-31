// =============================================================================
//  TextSelection.cpp
//  Groups glyphs into visual lines and exposes reading-order helpers.
// =============================================================================
#include "Text/TextSelection.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace pengpdf {
namespace {

// A visual line is a band of glyphs with significant vertical overlap.  Bands
// grow while glyphs are added; a glyph is assigned to the most recent band it
// overlaps, which keeps superscripts and mixed font sizes on their line.
struct LineBand {
    float top = 0.0f;
    float bottom = 0.0f;
    std::vector<size_t> glyphs;
};

// True when a glyph box overlaps a line band enough to belong to that line.
bool VerticalOverlap(const LineBand& band, const pdf::TextRect& box) {
    const float overlapTop = std::max(band.top, box.top);
    const float overlapBottom = std::min(band.bottom, box.bottom);
    const float overlap = overlapBottom - overlapTop;
    const float bandHeight = band.bottom - band.top;
    const float boxHeight = box.bottom - box.top;
    const float smallest = std::max(0.0005f, std::min(bandHeight, boxHeight));
    return overlap > smallest * 0.45f;
}

}  // namespace

// Builds both directions of the mapping and the line table.  Cost is
// O(n log n) once per selection start, which is negligible for a page.
VisualOrder::VisualOrder(const pdf::TextPage& page) : m_page(page) {
    const size_t count = page.glyphs.size();
    m_glyphs.resize(count);
    m_rankOf.assign(count, 0);
    m_lineOfRank.assign(count, 0);
    if (count == 0) {
        return;
    }

    // Sort glyph indices by their top edge, then cluster them into lines.
    // Sorting first means the clustering loop only looks at the last few bands.
    std::vector<size_t> indices(count);
    std::iota(indices.begin(), indices.end(), 0);
    std::sort(indices.begin(), indices.end(), [&page](size_t left, size_t right) {
        const pdf::TextRect& a = page.glyphs[left].bounds;
        const pdf::TextRect& b = page.glyphs[right].bounds;
        if (a.top != b.top) {
            return a.top < b.top;
        }
        return a.left < b.left;
    });

    std::vector<LineBand> bands;
    bands.reserve(count / 4 + 4);
    for (size_t glyphIndex : indices) {
        const pdf::TextRect& box = page.glyphs[glyphIndex].bounds;
        LineBand* target = nullptr;
        // Only the most recent lines can still overlap the current glyph.
        for (size_t back = 0; back < std::min<size_t>(bands.size(), 4); ++back) {
            LineBand& candidate = bands[bands.size() - 1 - back];
            if (VerticalOverlap(candidate, box)) {
                target = &candidate;
                break;
            }
        }
        if (target == nullptr) {
            LineBand band;
            band.top = box.top;
            band.bottom = box.bottom;
            band.glyphs.push_back(glyphIndex);
            bands.push_back(std::move(band));
        } else {
            target->top = std::min(target->top, box.top);
            target->bottom = std::max(target->bottom, box.bottom);
            target->glyphs.push_back(glyphIndex);
        }
    }

    // Read the lines from top to bottom, each line from left to right.
    std::sort(bands.begin(), bands.end(),
              [](const LineBand& left, const LineBand& right) {
                  return left.top < right.top;
              });

    size_t rank = 0;
    for (size_t lineIndex = 0; lineIndex < bands.size(); ++lineIndex) {
        LineBand& band = bands[lineIndex];
        std::sort(band.glyphs.begin(), band.glyphs.end(),
                  [&page](size_t left, size_t right) {
                      const pdf::TextRect& a = page.glyphs[left].bounds;
                      const pdf::TextRect& b = page.glyphs[right].bounds;
                      if (a.left != b.left) {
                          return a.left < b.left;
                      }
                      return a.top < b.top;
                  });
        LineInfo info;
        info.top = band.top;
        info.bottom = band.bottom;
        info.firstRank = rank;
        for (size_t glyphIndex : band.glyphs) {
            if (rank >= count) {
                break;
            }
            m_glyphs[rank] = glyphIndex;
            m_rankOf[glyphIndex] = rank;
            m_lineOfRank[rank] = static_cast<int>(lineIndex);
            ++rank;
        }
        info.lastRank = rank == 0 ? 0 : rank - 1;
        if (rank > info.firstRank) {
            m_lines.push_back(info);
        }
    }
}

// Clamps out-of-range requests to the first rank; callers normally pass a hit
// test result, so this is only a safety net.
size_t VisualOrder::GlyphAtRank(size_t rank) const {
    if (rank >= m_glyphs.size()) {
        return 0;
    }
    return m_glyphs[rank];
}

// Inverse of GlyphAtRank; unknown indices clamp to the first rank.
size_t VisualOrder::RankOfGlyph(size_t glyphIndex) const {
    if (glyphIndex >= m_rankOf.size()) {
        return 0;
    }
    return m_rankOf[glyphIndex];
}

// True when both ranks belong to the same visual line; used to decide whether
// text needs a space or a newline between two glyphs.
bool VisualOrder::SameLine(size_t rankA, size_t rankB) const {
    if (rankA >= m_lineOfRank.size() || rankB >= m_lineOfRank.size()) {
        return false;
    }
    return m_lineOfRank[rankA] == m_lineOfRank[rankB];
}

// First selects the visual line (vertical distance), then the glyph on that
// line (horizontal distance).  The two-step choice is what keeps a drag past
// the end of a line from jumping to the line above.
size_t VisualOrder::RankForPoint(float x, float y) const {
    if (m_glyphs.empty() || m_lines.empty()) {
        return 0;
    }

    // Pick the visual line with the smallest vertical distance to the point.
    size_t bestLine = 0;
    float bestDistance = 1.0e30f;
    for (size_t index = 0; index < m_lines.size(); ++index) {
        const LineInfo& line = m_lines[index];
        float dy = 0.0f;
        if (y < line.top) {
            dy = line.top - y;
        } else if (y > line.bottom) {
            dy = y - line.bottom;
        }
        if (dy < bestDistance) {
            bestDistance = dy;
            bestLine = index;
        }
    }

    // Then choose the glyph of that line closest horizontally.  This is what
    // makes dragging past the line end clamp instead of jumping lines.
    const LineInfo& line = m_lines[bestLine];
    size_t bestRank = line.firstRank;
    float bestHorizontal = 1.0e30f;
    for (size_t rank = line.firstRank; rank <= line.lastRank; ++rank) {
        const pdf::TextRect& box = m_page.glyphs[m_glyphs[rank]].bounds;
        float dx = 0.0f;
        if (x < box.left) {
            dx = box.left - x;
        } else if (x > box.right) {
            dx = x - box.right;
        }
        if (dx < bestHorizontal) {
            bestHorizontal = dx;
            bestRank = rank;
        }
    }
    return bestRank;
}

// Used by the clipboard path; line breaks and inferred spaces keep the copied
// text readable even when the PDF omitted whitespace glyphs.
std::wstring VisualOrder::TextInRange(size_t firstRank, size_t lastRank) const {
    std::wstring result;
    if (m_glyphs.empty()) {
        return result;
    }
    firstRank = std::min(firstRank, m_glyphs.size() - 1);
    lastRank = std::min(lastRank, m_glyphs.size() - 1);
    if (lastRank < firstRank) {
        return result;
    }
    result.reserve(lastRank - firstRank + 1);

    for (size_t rank = firstRank; rank <= lastRank; ++rank) {
        const pdf::TextGlyph& glyph = m_page.glyphs[m_glyphs[rank]];
        if (rank > firstRank) {
            const pdf::TextGlyph& previous = m_page.glyphs[m_glyphs[rank - 1]];
            if (!SameLine(rank - 1, rank)) {
                result.push_back(L'\n');
            } else if (pdf::HasWordGap(previous, glyph) &&
                       !pdf::IsSpaceCharacter(previous.character) &&
                       !pdf::IsSpaceCharacter(glyph.character)) {
                result.push_back(L' ');
            }
        }
        result.push_back(glyph.character);
    }
    return result;
}

std::vector<pdf::TextRect> VisualOrder::RectanglesInRange(size_t firstRank,
                                                          size_t lastRank) const {
    std::vector<pdf::TextRect> rects;
    if (m_glyphs.empty()) {
        return rects;
    }
    firstRank = std::min(firstRank, m_glyphs.size() - 1);
    lastRank = std::min(lastRank, m_glyphs.size() - 1);
    if (lastRank < firstRank) {
        return rects;
    }

    // Ranks are already in reading order, so a rectangle only has to grow
    // until the visual line changes.
    pdf::TextRect current = m_page.glyphs[m_glyphs[firstRank]].bounds;
    for (size_t rank = firstRank + 1; rank <= lastRank; ++rank) {
        const pdf::TextRect& box = m_page.glyphs[m_glyphs[rank]].bounds;
        if (SameLine(rank - 1, rank)) {
            current.left = std::min(current.left, box.left);
            current.top = std::min(current.top, box.top);
            current.right = std::max(current.right, box.right);
            current.bottom = std::max(current.bottom, box.bottom);
        } else {
            rects.push_back(current);
            current = box;
        }
    }
    rects.push_back(current);
    return rects;
}

void VisualOrder::WordRangeAt(size_t rank, size_t& firstRank,
                              size_t& lastRank) const {
    firstRank = rank;
    lastRank = rank;
    if (m_glyphs.empty() || rank >= m_glyphs.size()) {
        return;
    }

    // Walk outwards while the neighbouring glyph is on the same line, is not
    // whitespace, and is close enough to belong to the same word.  The 0.5em
    // threshold tolerates normal letter spacing and kerning.
    const pdf::TextPage& page = m_page;
    while (firstRank > 0 && SameLine(firstRank - 1, firstRank)) {
        const pdf::TextGlyph& previous = page.glyphs[m_glyphs[firstRank - 1]];
        const pdf::TextGlyph& current = page.glyphs[m_glyphs[firstRank]];
        if (pdf::IsSpaceCharacter(previous.character) ||
            pdf::IsSpaceCharacter(current.character)) {
            break;
        }
        const float gap = std::max(previous.fontSize, current.fontSize) * 0.5f;
        if (std::fabs(current.bounds.left - previous.bounds.right) > gap) {
            break;
        }
        --firstRank;
    }
    while (lastRank + 1 < m_glyphs.size() && SameLine(lastRank, lastRank + 1)) {
        const pdf::TextGlyph& current = page.glyphs[m_glyphs[lastRank]];
        const pdf::TextGlyph& next = page.glyphs[m_glyphs[lastRank + 1]];
        if (pdf::IsSpaceCharacter(current.character) ||
            pdf::IsSpaceCharacter(next.character)) {
            break;
        }
        const float gap = std::max(current.fontSize, next.fontSize) * 0.5f;
        if (std::fabs(next.bounds.left - current.bounds.right) > gap) {
            break;
        }
        ++lastRank;
    }
}

}  // namespace pengpdf
