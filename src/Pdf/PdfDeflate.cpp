// =============================================================================
//  PdfDeflate.cpp
//  Portable DEFLATE decoder.
//
//  The implementation follows the reference algorithm from RFC 1951:
//
//    1. read a block header (final flag + block type),
//    2. stored blocks copy their payload verbatim,
//    3. fixed-Huffman blocks use the predefined code tables,
//    4. dynamic-Huffman blocks transmit their own code lengths first,
//    5. inside a Huffman block, literals are copied and (length, distance)
//       pairs replicate earlier output bytes (the LZ77 stage).
//
//  Huffman codes are canonical, so a table only needs the number of codes per
//  bit length plus the symbols ordered by length; decoding then walks the input
//  bit by bit.  This is slower than a 9-bit lookup table but much simpler to
//  verify, and inflate is never the bottleneck for a PDF viewer.
// =============================================================================
#include "Pdf/PdfDeflate.h"

#include <algorithm>
#include <array>

namespace pdf {
namespace {

// DEFLATE never uses a code longer than 15 bits (RFC 1951, 3.2.7); the table is
// sized exactly so decoding cannot index past it.
constexpr int kMaxCodeBits = 15;
constexpr int kMaxOutputBytes = 512 * 1024 * 1024;  // protects against zip bombs

// -----------------------------------------------------------------------------
//  Bit reader
//  DEFLATE stores fields least-significant-bit first inside each byte, so the
//  accumulator is shifted right after each bit rather than left.
// -----------------------------------------------------------------------------
class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    // Reads `count` bits (0..24) and returns them in `value`.
    bool ReadBits(int count, int& value) {
        value = 0;
        for (int i = 0; i < count; ++i) {
            if (bitCount_ == 0) {
                if (position_ >= size_) {
                    return false;
                }
                bitBuffer_ = data_[position_++];
                bitCount_ = 8;
            }
            value |= static_cast<int>(bitBuffer_ & 1u) << i;
            bitBuffer_ >>= 1;
            --bitCount_;
        }
        return true;
    }

    // Stored blocks begin on a byte boundary; discard the partial byte.
    void AlignToByte() { bitCount_ = 0; }

    // Reads one whole byte.  Only valid after AlignToByte().
    bool ReadByte(uint8_t& value) {
        if (bitCount_ != 0 || position_ >= size_) {
            return false;
        }
        value = data_[position_++];
        return true;
    }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t position_ = 0;
    uint32_t bitBuffer_ = 0;
    int bitCount_ = 0;
};

// -----------------------------------------------------------------------------
//  Canonical Huffman decoding table
// -----------------------------------------------------------------------------
struct HuffmanTable {
    std::array<int, kMaxCodeBits + 1> counts{};  // codes of each bit length
    std::vector<int> symbols;                   // symbols ordered by code

    bool Build(const uint8_t* lengths, int symbolCount) {
        counts.fill(0);
        symbols.clear();

        int usedSymbols = 0;
        for (int i = 0; i < symbolCount; ++i) {
            if (lengths[i] > kMaxCodeBits) {
                return false;
            }
            ++counts[lengths[i]];
            if (lengths[i] != 0) {
                ++usedSymbols;
            }
        }
        if (usedSymbols == 0) {
            return true;  // an empty table is legal (e.g. no distance codes)
        }

        // Reject over-subscribed code sets; an incomplete set is tolerated
        // because some encoders leave unused codes out.  The check is the Kraft
        // inequality: each length doubles the remaining code space.
        int left = 1;
        for (int len = 1; len <= kMaxCodeBits; ++len) {
            left <<= 1;
            left -= counts[len];
            if (left < 0) {
                return false;
            }
        }

        std::array<int, kMaxCodeBits + 1> offsets{};
        for (int len = 1; len < kMaxCodeBits; ++len) {
            offsets[len + 1] = offsets[len] + counts[len];
        }

        // Canonical order: symbols are sorted by code length and, within one
        // length, by symbol value.  The `offsets` array is mutated as a cursor.
        symbols.resize(static_cast<size_t>(usedSymbols));
        for (int symbol = 0; symbol < symbolCount; ++symbol) {
            const int len = lengths[symbol];
            if (len != 0) {
                symbols[static_cast<size_t>(offsets[len]++)] = symbol;
            }
        }
        return true;
    }

    bool Empty() const { return symbols.empty(); }

    // Returns the decoded symbol, or -1 when the bit stream is invalid.
    int Decode(BitReader& reader) const {
        // Canonical decoding without a lookup table: codes of one length form a
        // contiguous range [first, first + count), so after reading `len` bits
        // the symbol is either in that range or the prefix is shifted for the
        // next length.  table.Build() guaranteed the ranges fit the alphabet.
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= kMaxCodeBits; ++len) {
            int bit = 0;
            if (!reader.ReadBits(1, bit)) {
                return -1;
            }
            code |= bit;
            const int count = counts[len];
            if (code - first < count) {
                return symbols[static_cast<size_t>(index + (code - first))];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

// Length and distance code tables from RFC 1951, section 3.2.5.
constexpr int kLengthBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43,
    51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr int kLengthExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3,
    3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr int kDistanceBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
    513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr int kDistanceExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7,
    8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// The order in which the code-length alphabet is transmitted.
constexpr int kCodeLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
                                      11, 4, 12, 3, 13, 2, 14, 1, 15};

// Builds the two fixed Huffman tables described in RFC 1951, section 3.2.6.
// The return value of Build() is intentionally ignored: the fixed tables are
// constant legal code sets that can never fail the over-subscription check.
void BuildFixedTables(HuffmanTable& literals, HuffmanTable& distances) {
    uint8_t literalLengths[288] = {};
    for (int i = 0; i < 144; ++i) literalLengths[i] = 8;
    for (int i = 144; i < 256; ++i) literalLengths[i] = 9;
    for (int i = 256; i < 280; ++i) literalLengths[i] = 7;
    for (int i = 280; i < 288; ++i) literalLengths[i] = 8;
    literals.Build(literalLengths, 288);

    uint8_t distanceLengths[30] = {};
    for (int i = 0; i < 30; ++i) {
        distanceLengths[i] = 5;
    }
    distances.Build(distanceLengths, 30);
}

// Decodes one Huffman coded block until the end-of-block symbol (256) appears.
bool DecodeHuffmanBlock(BitReader& reader, const HuffmanTable& literals,
                        const HuffmanTable& distances,
                        std::vector<uint8_t>& output) {
    for (;;) {
        const int symbol = literals.Decode(reader);
        if (symbol < 0) {
            return false;
        }

        if (symbol < 256) {
            output.push_back(static_cast<uint8_t>(symbol));
            if (output.size() > kMaxOutputBytes) {
                return false;
            }
            continue;
        }
        if (symbol == 256) {
            return true;  // end of block
        }

        // Symbols 257..285 are length codes; each carries a base length plus a
        // variable number of extra bits.  286 and 287 exist in the fixed
        // alphabet but are never emitted.
        const int lengthIndex = symbol - 257;
        if (lengthIndex < 0 || lengthIndex >= 29) {
            return false;
        }
        int extra = 0;
        if (kLengthExtra[lengthIndex] > 0 &&
            !reader.ReadBits(kLengthExtra[lengthIndex], extra)) {
            return false;
        }
        const size_t length =
            static_cast<size_t>(kLengthBase[lengthIndex] + extra);

        if (distances.Empty()) {
            // A block that uses only literals is allowed to transmit an empty
            // distance alphabet, but a match with no alphabet is corrupt.
            return false;
        }
        const int distanceSymbol = distances.Decode(reader);
        if (distanceSymbol < 0 || distanceSymbol >= 30) {
            return false;  // 30 and 31 are reserved by the specification
        }
        extra = 0;
        if (kDistanceExtra[distanceSymbol] > 0 &&
            !reader.ReadBits(kDistanceExtra[distanceSymbol], extra)) {
            return false;
        }
        const size_t distance =
            static_cast<size_t>(kDistanceBase[distanceSymbol] + extra);
        if (distance == 0 || distance > output.size()) {
            return false;  // reference before the start of the stream
        }

        // LZ77 copy; overlapping copies (distance < length) are intentional and
        // must be performed byte by byte.
        for (size_t i = 0; i < length; ++i) {
            output.push_back(output[output.size() - distance]);
        }
        if (output.size() > kMaxOutputBytes) {
            return false;
        }
    }
}

// Reads the dynamic Huffman tables that precede a type-2 block.
bool ReadDynamicTables(BitReader& reader, HuffmanTable& literals,
                       HuffmanTable& distances) {
    // HLIT/HDIST/HCLEN are 5, 5 and 4 bits wide and biased by 257, 1 and 4
    // respectively (RFC 1951, 3.2.7).
    int literalCount = 0;
    int distanceCount = 0;
    int codeLengthCount = 0;
    if (!reader.ReadBits(5, literalCount) || !reader.ReadBits(5, distanceCount) ||
        !reader.ReadBits(4, codeLengthCount)) {
        return false;
    }
    literalCount += 257;   // 257..286
    distanceCount += 1;    // 1..32
    codeLengthCount += 4;  // 4..19

    if (literalCount > 286 || distanceCount > 32 || codeLengthCount > 19) {
        return false;
    }

    uint8_t codeLengthLengths[19] = {};
    // The code-length code lengths are sent in a permuted order so that the
    // most common entries come first; unused trailing entries stay zero.  Three
    // bits per entry, maximum seven - nothing can exceed the 15 bit limit here.
    for (int i = 0; i < codeLengthCount; ++i) {
        int value = 0;
        if (!reader.ReadBits(3, value)) {
            return false;
        }
        codeLengthLengths[kCodeLengthOrder[i]] = static_cast<uint8_t>(value);
    }

    HuffmanTable codeLengthTable;
    if (!codeLengthTable.Build(codeLengthLengths, 19) || codeLengthTable.Empty()) {
        return false;
    }

    // The literal and distance alphabets share one length sequence; the first
    // `literalCount` entries describe the literal alphabet.
    const int totalCount = literalCount + distanceCount;
    std::vector<uint8_t> lengths(static_cast<size_t>(totalCount), 0);

    int index = 0;
    while (index < totalCount) {
        const int symbol = codeLengthTable.Decode(reader);
        if (symbol < 0) {
            return false;
        }

        if (symbol < 16) {
            lengths[static_cast<size_t>(index++)] = static_cast<uint8_t>(symbol);
            continue;
        }

        int repeat = 0;
        uint8_t value = 0;
        // Codes 16..18 run-length encode the sequence: 16 repeats the previous
        // length, 17 and 18 emit zero lengths (unused symbols) in longer runs.
        if (symbol == 16) {
            if (index == 0) {
                return false;  // nothing to repeat
            }
            if (!reader.ReadBits(2, repeat)) {
                return false;
            }
            repeat += 3;
            value = lengths[static_cast<size_t>(index - 1)];
        } else if (symbol == 17) {
            if (!reader.ReadBits(3, repeat)) {
                return false;
            }
            repeat += 3;
        } else if (symbol == 18) {
            if (!reader.ReadBits(7, repeat)) {
                return false;
            }
            repeat += 11;
        } else {
            return false;
        }

        if (index + repeat > totalCount) {
            return false;
        }
        for (int i = 0; i < repeat; ++i) {
            lengths[static_cast<size_t>(index++)] = value;
        }
    }

    // The end-of-block symbol must exist, otherwise the stream cannot finish.
    if (lengths[256] == 0) {
        return false;
    }

    if (!literals.Build(lengths.data(), literalCount)) {
        return false;
    }
    if (!distances.Build(lengths.data() + literalCount, distanceCount)) {
        return false;
    }
    return true;
}

// Reads a stored (uncompressed) block.
bool ReadStoredBlock(BitReader& reader, std::vector<uint8_t>& output) {
    reader.AlignToByte();

    // LEN followed by its one's complement NLEN.  The complement catches a
    // decoder that has lost bit synchronisation before copying memory.
    uint8_t header[4] = {};
    for (uint8_t& byte : header) {
        if (!reader.ReadByte(byte)) {
            return false;
        }
    }
    const size_t length = static_cast<size_t>(header[0]) |
                          (static_cast<size_t>(header[1]) << 8);
    const size_t inverse = static_cast<size_t>(header[2]) |
                           (static_cast<size_t>(header[3]) << 8);
    if ((length ^ 0xFFFFu) != inverse) {
        return false;
    }
    if (output.size() + length > kMaxOutputBytes) {
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        uint8_t byte = 0;
        if (!reader.ReadByte(byte)) {
            return false;
        }
        output.push_back(byte);
    }
    return true;
}

}  // namespace

bool InflateRaw(const uint8_t* data, size_t size, std::vector<uint8_t>& output) {
    output.clear();
    if (data == nullptr || size == 0) {
        return false;
    }

    BitReader reader(data, size);

    // A rough size hint avoids repeated reallocations.  Clamping keeps the
    // reservation sane for small streams that expand a lot.
    output.reserve(std::min<size_t>(size * 3 + 64, 4 * 1024 * 1024));

    bool isFinalBlock = false;
    while (!isFinalBlock) {
        // Every block starts with BFINAL (1 bit) and BTYPE (2 bits).
        int finalFlag = 0;
        int blockType = 0;
        if (!reader.ReadBits(1, finalFlag) || !reader.ReadBits(2, blockType)) {
            return false;
        }
        isFinalBlock = finalFlag != 0;

        if (blockType == 0) {
            if (!ReadStoredBlock(reader, output)) {
                return false;
            }
            continue;
        }

        // Block types 1 and 2 share the decode loop; they differ only in where
        // the Huffman tables come from (built-in vs transmitted).
        if (blockType == 1 || blockType == 2) {
            HuffmanTable literals;
            HuffmanTable distances;
            if (blockType == 1) {
                BuildFixedTables(literals, distances);
            } else {
                if (!ReadDynamicTables(reader, literals, distances)) {
                    return false;
                }
            }
            if (!DecodeHuffmanBlock(reader, literals, distances, output)) {
                return false;
            }
            continue;
        }

        return false;  // block type 3 is reserved / invalid
    }
    return true;
}

bool Inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& output) {
    output.clear();
    if (data == nullptr || size < 2) {
        return false;
    }

    // zlib header: CMF/FLG.  CM must be 8 (DEFLATE) and the check value must
    // make the two-byte header a multiple of 31.
    const uint8_t compressionMethod = data[0];
    const uint8_t flags = data[1];
    const bool looksLikeZlib =
        (compressionMethod & 0x0Fu) == 8 &&
        ((static_cast<unsigned>(compressionMethod) << 8) | flags) % 31u == 0;

    if (looksLikeZlib) {
        size_t offset = 2;
        if ((flags & 0x20u) != 0) {
            // A preset dictionary id means the stream cannot be decoded without
            // the dictionary; skip it and let the attempt fail below.
            offset += 4;
        }
        if (offset < size && InflateRaw(data + offset, size - offset, output)) {
            return true;
        }
        output.clear();
    }

    // Fall back to a raw stream interpretation.  Some PDF writers emit a
    // header-less DEFLATE stream even though they declare FlateDecode.  The
    // output is cleared first so a failed zlib attempt leaves no partial data
    // behind when the retry also fails.
    return InflateRaw(data, size, output);
}

bool Inflate(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
    return Inflate(input.data(), input.size(), output);
}

}  // namespace pdf
