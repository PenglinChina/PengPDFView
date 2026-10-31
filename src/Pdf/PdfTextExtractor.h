// =============================================================================
//  PdfTextExtractor.h
//  Turns the content streams of one PDF page into positioned Unicode glyphs.
//
//  The extractor understands the text related part of the PDF imaging model:
//  graphics state and CTM, text objects, font resources, showing operators
//  (Tj/TJ/'/"), form XObjects that contain text and inline images (which must
//  be skipped).  It intentionally ignores paths, colours and clipping because
//  text selection does not need them.
//
//  The interface is a single class with a pimpl so callers never depend on the
//  interpreter internals.
//
//  Output coordinates are normalised to the displayed page: (0,0) is the top
//  left corner and (1,1) the bottom right, with the page rotation already
//  applied.  Keeping the view/DPI out of the extractor is what lets the same
//  glyphs be reused for selection, search and highlighting.
//
//  Extraction is bounded (instruction count, content bytes, glyph count, form
//  nesting).  When a budget runs out the page is returned truncated rather than
//  rejected; the document itself is never modified.
// =============================================================================
#pragma once

#include <memory>

#include "Pdf/PdfParser.h"
#include "Pdf/PdfTextTypes.h"

namespace pdf {

// Interprets content streams for one page at a time.  A single instance is meant
// to be reused for all pages of a document because it caches decoded fonts;
// that cache is not synchronised, so an instance must not be shared between
// threads (the Document it reads from is thread safe).
class TextExtractor {
public:
    // The document must outlive the extractor; extraction is read only.
    explicit TextExtractor(Document& document);
    ~TextExtractor();

    TextExtractor(const TextExtractor&) = delete;
    TextExtractor& operator=(const TextExtractor&) = delete;

    // Extracts the text of `pageIndex` in content stream order.  Returns false
    // only when the page itself cannot be resolved; an empty vector is a valid
    // result (scanned pages contain no text at all).  The page is cleared first,
    // so it is safe to reuse the same TextPage for consecutive pages.
    bool ExtractPage(int pageIndex, TextPage& page);

private:
    struct Impl;

    // The content stream interpreter is implemented in the .cpp file.  The
    // friendship keeps Impl private while letting the interpreter use it.
    friend class TextInterpreter;

    std::unique_ptr<Impl> m_impl;
};

}  // namespace pdf
