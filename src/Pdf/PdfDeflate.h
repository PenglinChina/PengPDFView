// =============================================================================
//  PdfDeflate.h
//  A self-contained DEFLATE (RFC 1951) / zlib (RFC 1950) decoder.
//
//  PDF files compress almost every structural stream - the cross-reference
//  stream, object streams and the outline of modern documents - with zlib.
//  Pulling in a third-party library just for inflate would add a dependency to
//  the executable, so the decoder lives here in ~350 lines of portable C++.
//
//  Scope: inflate only (no deflate), RFC 1950/1951 only (no gzip headers, no
//  preset dictionaries) and a hard 512 MB output cap so that a crafted "zip
//  bomb" cannot exhaust memory.
// =============================================================================
#pragma once

#include <cstdint>
#include <vector>

namespace pdf {

// Decodes a raw DEFLATE stream (no zlib wrapper).  `output` is cleared first and
// may contain partial data when the function returns false; callers must treat
// failure as "no usable data" rather than inspecting the buffer.
bool InflateRaw(const uint8_t* data, size_t size, std::vector<uint8_t>& output);

// Decodes a zlib stream.  When the two-byte zlib header is missing the input is
// treated as a raw DEFLATE stream instead, because some PDF producers omit it.
// A preset-dictionary stream cannot be decoded and is retried as raw DEFLATE,
// which normally fails - no known PDF producer emits one.
bool Inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& output);

// Convenience overload that works directly on a byte vector.
bool Inflate(const std::vector<uint8_t>& input, std::vector<uint8_t>& output);

}  // namespace pdf
