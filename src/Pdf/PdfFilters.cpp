// =============================================================================
//  PdfFilters.cpp
//  Predictor and ASCII filter implementations.
//
//  These functions are deliberately separate from the lexer: they only see the
//  raw stream payload and know nothing about PDF objects.  That keeps them
//  testable in isolation and lets the parser retry a decode without any state
//  to reset.
// =============================================================================
#include "Pdf/PdfFilters.h"

#include <algorithm>
#include <cctype>

namespace pdf {
namespace {

int HexValue(uint8_t character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

// Local copy of the PDF whitespace set.  The parser has an equivalent helper,
// but it is private to that translation unit; filters only ever see stream
// payloads, so the small duplication keeps this module self contained.
bool IsPdfWhitespace(uint8_t character) {
    return character == ' ' || character == '\t' || character == '\r' ||
           character == '\n' || character == '\f' || character == 0;
}

// Applies the PNG predictor filter to a single row (five filter types).
// The row is reconstructed in place: every filter type reads bytes that have
// already been decoded (the left neighbour and the previous row), so the loop
// order must stay strictly left to right.
void UndoPngRow(int filterType, uint8_t* row, const uint8_t* previousRow,
                size_t rowBytes, size_t bytesPerPixel) {
    switch (filterType) {
        case 0:  // None
            break;
        case 1:  // Sub: current byte += byte to the left
            for (size_t i = bytesPerPixel; i < rowBytes; ++i) {
                row[i] = static_cast<uint8_t>(row[i] + row[i - bytesPerPixel]);
            }
            break;
        case 2:  // Up: current byte += byte above
            if (previousRow != nullptr) {
                for (size_t i = 0; i < rowBytes; ++i) {
                    row[i] = static_cast<uint8_t>(row[i] + previousRow[i]);
                }
            }
            break;
        case 3:  // Average
            for (size_t i = 0; i < rowBytes; ++i) {
                const int left = (i >= bytesPerPixel) ? row[i - bytesPerPixel] : 0;
                const int above = (previousRow != nullptr) ? previousRow[i] : 0;
                row[i] = static_cast<uint8_t>(row[i] + ((left + above) >> 1));
            }
            break;
        case 4:  // Paeth
            for (size_t i = 0; i < rowBytes; ++i) {
                const int left = (i >= bytesPerPixel) ? row[i - bytesPerPixel] : 0;
                const int above = (previousRow != nullptr) ? previousRow[i] : 0;
                const int upperLeft =
                    (previousRow != nullptr && i >= bytesPerPixel) ? previousRow[i - bytesPerPixel] : 0;

                const int estimate = left + above - upperLeft;
                const int distanceLeft = std::abs(estimate - left);
                const int distanceAbove = std::abs(estimate - above);
                const int distanceUpperLeft = std::abs(estimate - upperLeft);

                int predictor = left;
                if (distanceAbove < distanceLeft) {
                    predictor = above;
                    if (distanceUpperLeft < distanceAbove) {
                        predictor = upperLeft;
                    }
                } else if (distanceUpperLeft < distanceLeft) {
                    predictor = upperLeft;
                }
                row[i] = static_cast<uint8_t>(row[i] + predictor);
            }
            break;
        default:
            break;  // unknown filter types are treated as "None"
    }
}

}  // namespace

bool ApplyPredictor(const std::vector<uint8_t>& input, int predictor, int colors,
                    int bitsPerComponent, int columns,
                    std::vector<uint8_t>& output) {
    output.clear();
    if (predictor <= 1) {
        // No prediction: the data passes through, which is also the cheapest way
        // to keep the caller's logic uniform.
        output = input;
        return true;
    }

    // Missing or nonsensical parameters fall back to the specification defaults
    // (one component, 8 bits per component); a zero column count is unusable.
    if (colors <= 0) colors = 1;
    if (bitsPerComponent <= 0) bitsPerComponent = 8;
    if (columns <= 0) return false;

    // Samples are packed and each row is padded up to a byte boundary, which is
    // why the multiplication happens before the division.
    const size_t rowBytes =
        (static_cast<size_t>(colors) * static_cast<size_t>(bitsPerComponent) *
             static_cast<size_t>(columns) +
         7) /
        8;
    if (rowBytes == 0) {
        return false;
    }
    // Distance to the corresponding byte of the previous pixel.  Samples smaller
    // than a byte are packed together, so the minimum distance is one byte.
    const size_t bytesPerPixel =
        std::max<size_t>(1, static_cast<size_t>(colors * bitsPerComponent) / 8);

    if (predictor == 2) {
        // TIFF predictor 2 only makes sense for 8-bit samples; anything else is
        // passed through untouched rather than producing garbage.
        if (bitsPerComponent != 8) {
            output = input;
            return true;
        }
        output = input;
        // Horizontal differencing: each sample is a delta against the sample
        // `colors` bytes earlier in the same row, applied row by row.
        const size_t rowCount = output.size() / rowBytes;
        for (size_t row = 0; row < rowCount; ++row) {
            uint8_t* current = output.data() + row * rowBytes;
            for (size_t i = static_cast<size_t>(colors); i < rowBytes; ++i) {
                current[i] = static_cast<uint8_t>(current[i] + current[i - colors]);
            }
        }
        return true;
    }

    // PNG predictors (10..15) prefix every row with a filter-type byte.  The
    // spec leaves 3..9 unused; those are treated as unfiltered rows rather than
    // rejected, since the byte layout is still otherwise usable.
    const bool hasFilterByte = predictor >= 10;
    output.clear();
    output.reserve(input.size());

    const size_t sourceStride = rowBytes + (hasFilterByte ? 1 : 0);
    if (input.size() < sourceStride) {
        return false;
    }
    // Integer division drops a trailing partial row, which is preferable to
    // reading past the end for data truncated by a broken /Length.
    const size_t rowCount = input.size() / sourceStride;

    std::vector<uint8_t> previous(rowBytes, 0);
    std::vector<uint8_t> current(rowBytes, 0);

    for (size_t row = 0; row < rowCount; ++row) {
        const uint8_t* source = input.data() + row * sourceStride;
        int filterType = 0;
        if (hasFilterByte) {
            filterType = source[0];
            ++source;
        }
        std::copy(source, source + rowBytes, current.begin());
        UndoPngRow(filterType, current.data(),
                   (row == 0) ? nullptr : previous.data(), rowBytes,
                   bytesPerPixel);
        output.insert(output.end(), current.begin(), current.end());
        previous = current;
    }
    return true;
}

bool AsciiHexDecode(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
    output.clear();
    // '>' terminates the payload; a missing terminator is tolerated because some
    // generators omit it.  Any non-hex character before the terminator is an
    // error, so silently skipping garbage would only hide corruption.
    int highNibble = -1;
    for (uint8_t character : input) {
        if (character == '>') {
            break;
        }
        if (IsPdfWhitespace(character)) {
            continue;
        }
        const int value = HexValue(character);
        if (value < 0) {
            return false;
        }
        if (highNibble < 0) {
            highNibble = value;
        } else {
            output.push_back(static_cast<uint8_t>((highNibble << 4) | value));
            highNibble = -1;
        }
    }
    // An odd number of digits is padded with a trailing zero nibble.
    if (highNibble >= 0) {
        output.push_back(static_cast<uint8_t>(highNibble << 4));
    }
    return true;
}

bool Ascii85Decode(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
    output.clear();
    // Each full group of five characters encodes four bytes as a base-85 number
    // in the range '!'..'u'.  `tuple` accumulates one group at a time.
    uint32_t tuple = 0;
    int tupleBytes = 0;

    for (size_t i = 0; i < input.size(); ++i) {
        const uint8_t character = input[i];
        if (character == '~') {
            break;  // end of data marker "~>"
        }
        if (IsPdfWhitespace(character)) {
            continue;
        }
        if (character == '<' && i + 1 < input.size() && input[i + 1] == '~') {
            ++i;  // skip the optional "<~" start marker
            continue;
        }
        if (character == 'z' && tupleBytes == 0) {
            // The "z" shortcut stands for four zero bytes.  It is only legal at
            // a group boundary; inside a group it is an ordinary character.
            output.insert(output.end(), 4, 0);
            continue;
        }
        if (character < '!' || character > 'u') {
            return false;  // outside the base-85 alphabet
        }

        tuple = tuple * 85 + static_cast<uint32_t>(character - '!');
        ++tupleBytes;
        if (tupleBytes == 5) {
            for (int shift = 24; shift >= 0; shift -= 8) {
                output.push_back(static_cast<uint8_t>((tuple >> shift) & 0xFF));
            }
            tuple = 0;
            tupleBytes = 0;
        }
    }

    if (tupleBytes > 0) {
        // Pad the final partial group with 'u' (84) and emit the valid bytes.
        for (int i = tupleBytes; i < 5; ++i) {
            tuple = tuple * 85 + 84;
        }
        for (int i = 0; i < tupleBytes - 1; ++i) {
            output.push_back(static_cast<uint8_t>((tuple >> (24 - i * 8)) & 0xFF));
        }
    }
    return true;
}

}  // namespace pdf
