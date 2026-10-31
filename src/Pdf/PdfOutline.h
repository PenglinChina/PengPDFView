// =============================================================================
//  PdfOutline.h
//  Extracts the document outline (the "bookmarks" tree saved inside the PDF)
//  and flattens it into sidebar entries.
//
//  Flattening happens here rather than in the UI so that the sidebar only has to
//  deal with a vector and a nesting level.  The walk is bounded (depth, item
//  count, name tree depth) and cycle safe, so a malicious tree cannot hang the
//  loader; a damaged outline simply yields fewer entries.
// =============================================================================
#pragma once

#include <vector>

#include "Pdf/PdfParser.h"

namespace pdf {

// Walks /Root /Outlines and returns the entries in display order.  A nested
// entry is always immediately preceded by its parent, which allows the sidebar
// to reconstruct the hierarchy from the `level` field alone.
//
// Page indices are obtained from the page tree the document already built, so
// this must run after loading; named destinations are looked up lazily through
// the catalog's name tree and the legacy /Dests dictionary.  Entries whose
// destination cannot be resolved are still returned with pageIndex == -1.
std::vector<OutlineEntry> BuildOutline(Document& document);

}  // namespace pdf
