// =============================================================================
//  PdfTextTypes.cpp
//  Plain text reconstruction and per-line rectangle merging for extracted text.
// =============================================================================
#include "Pdf/PdfTextTypes.h"

#include <algorithm>

namespace pdf {

// Reconstructs readable plain text for a glyph range.  PDF text frequently
// lacks explicit spaces, so geometry fills in word boundaries and line breaks.
std::wstring TextPage::TextInRange(size_t first, size_t last) const {
    std::wstring result;
    if (glyphs.empty()) {
        return result;
    }
    first = std::min(first, glyphs.size() - 1);
    last = std::min(last, glyphs.size() - 1);
    if (last < first) {
        return result;
    }
    result.reserve(last - first + 1);

    for (size_t index = first; index <= last; ++index) {
        const TextGlyph& glyph = glyphs[index];
        if (index > first) {
            const TextGlyph& previous = glyphs[index - 1];
            if (StartsNewLine(previous, glyph)) {
                // Keep copied text readable: a vertical jump is a new line.
                result.push_back(L'\n');
            } else if (HasWordGap(previous, glyph) &&
                       !IsSpaceCharacter(previous.character) &&
                       !IsSpaceCharacter(glyph.character)) {
                // Generators often position words individually instead of
                // emitting space glyphs; restore the missing boundary.
                result.push_back(L' ');
            }
        }
        result.push_back(glyph.character);
    }
    return result;
}

std::vector<TextRect> MergeGlyphRects(const TextPage& page, size_t first,
                                      size_t last) {
    std::vector<TextRect> rects;
    if (page.glyphs.empty()) {
        return rects;
    }
    first = std::min(first, page.glyphs.size() - 1);
    last = std::min(last, page.glyphs.size() - 1);
    if (last < first) {
        return rects;
    }

    TextRect current = page.glyphs[first].bounds;
    for (size_t index = first + 1; index <= last; ++index) {
        const TextRect& box = page.glyphs[index].bounds;
        const float height = std::min(current.Height(), box.Height());
        const float overlapTop = std::max(current.top, box.top);
        const float overlapBottom = std::min(current.bottom, box.bottom);
        const bool sameLine = (overlapBottom - overlapTop) > height * 0.4f;
        if (sameLine) {
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

}  // namespace pdf
