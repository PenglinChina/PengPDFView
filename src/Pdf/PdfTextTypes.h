// =============================================================================
//  PdfTextTypes.h
//  Value types shared between the PDF text extractor and the UI.
//
//  Coordinates are deliberately normalised: (0,0) is the top left corner of the
//  displayed page and (1,1) the bottom right.  Normalisation keeps zoom, DPI,
//  crop box offsets and page rotation out of every consumer, which is what
//  allows highlighting and selection to stay simple.
// =============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace pdf {

// Axis aligned rectangle in normalised page coordinates.
struct TextRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    float Width() const { return right - left; }
    float Height() const { return bottom - top; }
};

// One extracted character together with its box on the page.
struct TextGlyph {
    wchar_t character = L' ';
    TextRect bounds{};
    // Height of the glyph box in normalised units; used to group glyphs into
    // lines when plain text is requested (and to detect line changes).
    float fontSize = 0.0f;
};

// True for characters that represent whitespace in extracted text.  PDF text
// often lacks real space glyphs, so the layout helpers below also infer word
// boundaries from geometry.
inline bool IsSpaceCharacter(wchar_t character) {
    return character == L' ' || character == L'\t' || character == L'\r' ||
           character == L'\n' || character == 0x00A0 || character == 0x3000;
}

// True when `current` starts a new visual line relative to `previous`.  The
// tolerance scales with the font size so that sub-pixel baseline jitter inside
// one line is not mistaken for a line break.
inline bool StartsNewLine(const TextGlyph& previous, const TextGlyph& current) {
    const float previousCenter =
        (previous.bounds.top + previous.bounds.bottom) * 0.5f;
    const float center = (current.bounds.top + current.bounds.bottom) * 0.5f;
    const float tolerance =
        std::max(previous.fontSize, current.fontSize) * 0.55f + 0.002f;
    return std::fabs(center - previousCenter) > tolerance;
}

// True when the visual gap between two glyphs is wide enough to imply a word
// boundary even though no space glyph was emitted.  Both axes are checked
// because vertical writing modes advance along Y.
inline bool HasWordGap(const TextGlyph& previous, const TextGlyph& current) {
    const float gap = std::max(previous.fontSize, current.fontSize) * 0.3f;
    const float dx = current.bounds.left - previous.bounds.right;
    const float dy = current.bounds.top - previous.bounds.bottom;
    return dx > gap || dy > gap;
}

// The text of one page in content stream order.  `glyphs` is the single source
// of truth; plain text is derived on demand.
struct TextPage {
    std::vector<TextGlyph> glyphs;

    bool Empty() const { return glyphs.empty(); }
    void Clear() { glyphs.clear(); }

    // Plain text for the inclusive glyph range [first, last].  A newline is
    // inserted whenever the baseline jumps to another line, and a space where
    // geometry indicates a word boundary.
    std::wstring TextInRange(size_t first, size_t last) const;

    // Text of the whole page.
    std::wstring AllText() const { return TextInRange(0, glyphs.size()); }
};

// One search hit.  Glyph indices refer to TextPage::glyphs on the same page.
struct TextMatch {
    int pageIndex = 0;
    size_t firstGlyph = 0;
    size_t lastGlyph = 0;  // inclusive
    // Highlight rectangles merged per line, ready for the canvas to paint.
    std::vector<TextRect> rects;
};

// Merges the boxes of an inclusive glyph range into one rectangle per text
// line.  Used for search highlights.
std::vector<TextRect> MergeGlyphRects(const TextPage& page, size_t first,
                                      size_t last);

}  // namespace pdf
