// =============================================================================
//  PdfFilters.h
//  Stream filter implementations used by the PDF parser.
//
//  Only the filters that appear in the document structure (cross-reference
//  streams, object streams and outline dictionaries) are implemented here.
//  The page content itself is rasterised by the Windows PDF engine, so we do
//  not need the colour-space dependent filters such as DCTDecode.
//
//  FlateDecode itself lives in PdfDeflate.h; this module adds the predictor that
//  is often combined with it and the two ASCII transport encodings.  All three
//  functions clear `output` and return false on malformed input.
// =============================================================================
#pragma once

#include <cstdint>
#include <vector>

#include "Pdf/PdfDeflate.h"

namespace pdf {

// Undoes the PNG/TIFF predictor that some Flate streams apply to their data.
// `colors` is the number of colour components, `bitsPerComponent` the sample
// width and `columns` the number of samples per row.
//
// Predictor 1 (none) copies the data, 2 is the TIFF horizontal differencing
// scheme (8-bit samples only; other widths are passed through unchanged) and
// 10..15 are the PNG predictors, whose rows each carry a leading filter-type
// byte.  The row stride is derived from colors * bitsPerComponent * columns and
// rounded up to whole bytes.  A trailing incomplete row is dropped.
bool ApplyPredictor(const std::vector<uint8_t>& input, int predictor, int colors,
                    int bitsPerComponent, int columns,
                    std::vector<uint8_t>& output);

// ASCIIHexDecode: pairs of hex digits terminated by '>'.  Whitespace is
// ignored, the terminator may be missing, and an odd number of digits is
// padded with a zero nibble.
bool AsciiHexDecode(const std::vector<uint8_t>& input, std::vector<uint8_t>& output);

// ASCII85Decode: base-85 payload typically wrapped in "<~" and "~>".  The "z"
// shortcut for four zero bytes and a final partial 5-character group are
// supported, as required by the specification.
bool Ascii85Decode(const std::vector<uint8_t>& input, std::vector<uint8_t>& output);

}  // namespace pdf
