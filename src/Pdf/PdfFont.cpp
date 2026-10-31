// =============================================================================
//  PdfFont.cpp
//  Font encoding, width and ToUnicode CMap handling.
//
//  The decoding order mirrors the specification:
//    1. start from a base encoding (WinAnsi by default, since that is what the
//       vast majority of Latin fonts declare),
//    2. apply /Encoding /Differences for named glyphs,
//    3. apply ToUnicode as the authoritative code -> Unicode mapping,
//    4. read widths from /Widths (simple) or /W + /DW (composite).
//
//  Nothing here validates a font enough to reject it: a font with no usable
//  widths still moves the text cursor by the fallback width, which keeps the
//  remaining text roughly aligned instead of collapsing onto one spot.
// =============================================================================
#include "Pdf/PdfFont.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace pdf {
namespace {

// Resolves a dictionary entry, returning a null object when it is missing.
// The font code uses this everywhere so that every "/Key may be indirect" case
// is handled in one place.
Object ResolveIn(Document& document, const Object& dictionary, const char* key) {
    if (!dictionary.IsDict()) {
        return Object();
    }
    const Object* value = dictionary.Get(key);
    return value == nullptr ? Object() : document.Resolve(*value);
}

int HexValue(unsigned char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

// Decodes a ToUnicode destination string (UTF-16BE).  Surrogate pairs are kept
// as two UTF-16 code units, which is exactly what std::wstring stores on
// Windows.
//
// The BOM is optional here: unlike a PDF text string, a CMap destination is
// defined to be UTF-16BE, so the bytes are decoded as big-endian either way.
std::wstring DecodeUtf16Be(const std::string& bytes) {
    std::wstring result;
    size_t index = 0;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE &&
        static_cast<unsigned char>(bytes[1]) == 0xFF) {
        index = 2;
    }
    for (; index + 1 < bytes.size(); index += 2) {
        const uint16_t unit = static_cast<uint16_t>(
            (static_cast<unsigned char>(bytes[index]) << 8) |
            static_cast<unsigned char>(bytes[index + 1]));
        result.push_back(static_cast<wchar_t>(unit));
    }
    // Odd trailing byte: treat it as a single byte character, but only when
    // nothing else was produced.  Otherwise it is one half of a truncated pair
    // and turning it into a Latin-1 character would only add noise.
    if (index < bytes.size() && result.empty()) {
        result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(bytes[index])));
    }
    return result;
}

uint32_t BytesToCode(const std::string& bytes) {
    // CMap source strings are big-endian; one to four bytes are legal, and the
    // caller has already established the width from the codespace range.
    uint32_t code = 0;
    for (unsigned char character : bytes) {
        code = (code << 8) | character;
    }
    return code;
}

// Fills a 256 entry table from a Windows code page.  Used for the WinAnsi and
// MacRoman base encodings instead of embedding two large tables by hand.
//
// A byte the code page leaves undefined (some values in cp1252 are unassigned)
// falls back to the Latin-1 code point of the same value, which mirrors what
// Acrobat shows for such files.  The two tables are built once and copied
// afterwards: other fonts used to repeat 256 MultiByteToWideChar calls each,
// which is measurable in documents with many fonts.
std::array<wchar_t, 256> BuildCodepageTable(WORD codepage) {
    std::array<wchar_t, 256> table{};
    for (int value = 0; value < 256; ++value) {
        const char single = static_cast<char>(value);
        wchar_t wide = 0;
        const int written = ::MultiByteToWideChar(codepage, 0, &single, 1, &wide, 1);
        table[static_cast<size_t>(value)] =
            written == 1 ? wide : static_cast<wchar_t>(value);
    }
    return table;
}

void FillCodepageTable(WORD codepage, std::array<wchar_t, 256>& table) {
    static const std::array<wchar_t, 256> winAnsi = BuildCodepageTable(1252);
    static const std::array<wchar_t, 256> macRoman = BuildCodepageTable(10000);
    if (codepage == 1252) {
        table = winAnsi;
    } else if (codepage == 10000) {
        table = macRoman;
    } else {
        // Future encodings still work; they simply do not get the cached table.
        table = BuildCodepageTable(codepage);
    }
}

// Glyph names that appear in /Differences arrays.  The table covers ASCII
// punctuation, Latin-1 letters and the typographic names produced by common
// PDF generators.  Names of the form "uniXXXX" and "uXXXX" are handled
// separately.
struct GlyphNameEntry {
    const char* name;
    wchar_t unicode;
};

constexpr GlyphNameEntry kGlyphNames[] = {
    {"space", 0x0020},       {"exclam", 0x0021},      {"quotedbl", 0x0022},
    {"numbersign", 0x0023},  {"dollar", 0x0024},      {"percent", 0x0025},
    {"ampersand", 0x0026},   {"quotesingle", 0x0027}, {"parenleft", 0x0028},
    {"parenright", 0x0029},  {"asterisk", 0x002A},    {"plus", 0x002B},
    {"comma", 0x002C},       {"hyphen", 0x002D},      {"period", 0x002E},
    {"slash", 0x002F},       {"colon", 0x003A},       {"semicolon", 0x003B},
    {"less", 0x003C},        {"equal", 0x003D},       {"greater", 0x003E},
    {"question", 0x003F},    {"at", 0x0040},          {"bracketleft", 0x005B},
    {"backslash", 0x005C},   {"bracketright", 0x005D}, {"asciicircum", 0x005E},
    {"underscore", 0x005F},  {"grave", 0x0060},       {"braceleft", 0x007B},
    {"bar", 0x007C},         {"braceright", 0x007D},  {"asciitilde", 0x007E},
    {"exclamdown", 0x00A1},  {"cent", 0x00A2},        {"sterling", 0x00A3},
    {"currency", 0x00A4},    {"yen", 0x00A5},         {"brokenbar", 0x00A6},
    {"section", 0x00A7},     {"dieresis", 0x00A8},    {"copyright", 0x00A9},
    {"ordfeminine", 0x00AA}, {"guillemotleft", 0x00AB}, {"logicalnot", 0x00AC},
    {"registered", 0x00AE},  {"macron", 0x00AF},      {"degree", 0x00B0},
    {"plusminus", 0x00B1},   {"twosuperior", 0x00B2}, {"threesuperior", 0x00B3},
    {"acute", 0x00B4},       {"mu", 0x00B5},          {"paragraph", 0x00B6},
    {"periodcentered", 0x00B7}, {"cedilla", 0x00B8},  {"onesuperior", 0x00B9},
    {"ordmasculine", 0x00BA}, {"guillemotright", 0x00BB}, {"onequarter", 0x00BC},
    {"onehalf", 0x00BD},     {"threequarters", 0x00BE}, {"questiondown", 0x00BF},
    {"Agrave", 0x00C0},      {"Aacute", 0x00C1},      {"Acircumflex", 0x00C2},
    {"Atilde", 0x00C3},      {"Adieresis", 0x00C4},   {"Aring", 0x00C5},
    {"AE", 0x00C6},          {"Ccedilla", 0x00C7},    {"Egrave", 0x00C8},
    {"Eacute", 0x00C9},      {"Ecircumflex", 0x00CA}, {"Edieresis", 0x00CB},
    {"Igrave", 0x00CC},      {"Iacute", 0x00CD},      {"Icircumflex", 0x00CE},
    {"Idieresis", 0x00CF},   {"Eth", 0x00D0},         {"Ntilde", 0x00D1},
    {"Ograve", 0x00D2},      {"Oacute", 0x00D3},      {"Ocircumflex", 0x00D4},
    {"Otilde", 0x00D5},      {"Odieresis", 0x00D6},   {"multiply", 0x00D7},
    {"Oslash", 0x00D8},      {"Ugrave", 0x00D9},      {"Uacute", 0x00DA},
    {"Ucircumflex", 0x00DB}, {"Udieresis", 0x00DC},   {"Yacute", 0x00DD},
    {"Thorn", 0x00DE},       {"germandbls", 0x00DF},  {"agrave", 0x00E0},
    {"aacute", 0x00E1},      {"acircumflex", 0x00E2}, {"atilde", 0x00E3},
    {"adieresis", 0x00E4},   {"aring", 0x00E5},       {"ae", 0x00E6},
    {"ccedilla", 0x00E7},    {"egrave", 0x00E8},      {"eacute", 0x00E9},
    {"ecircumflex", 0x00EA}, {"edieresis", 0x00EB},   {"igrave", 0x00EC},
    {"iacute", 0x00ED},      {"icircumflex", 0x00EE}, {"idieresis", 0x00EF},
    {"eth", 0x00F0},         {"ntilde", 0x00F1},      {"ograve", 0x00F2},
    {"oacute", 0x00F3},      {"ocircumflex", 0x00F4}, {"otilde", 0x00F5},
    {"odieresis", 0x00F6},   {"divide", 0x00F7},      {"oslash", 0x00F8},
    {"ugrave", 0x00F9},      {"uacute", 0x00FA},      {"ucircumflex", 0x00FB},
    {"udieresis", 0x00FC},   {"yacute", 0x00FD},      {"thorn", 0x00FE},
    {"ydieresis", 0x00FF},   {"nbspace", 0x00A0},     {"sfthyphen", 0x00AD},
    {"OE", 0x0152},          {"oe", 0x0153},          {"Lslash", 0x0141},
    {"lslash", 0x0142},      {"Scaron", 0x0160},      {"scaron", 0x0161},
    {"Zcaron", 0x017D},      {"zcaron", 0x017E},      {"Ydieresis", 0x0178},
    {"dotlessi", 0x0131},    {"circumflex", 0x02C6},  {"caron", 0x02C7},
    {"breve", 0x02D8},       {"dotaccent", 0x02D9},   {"ring", 0x02DA},
    {"ogonek", 0x02DB},      {"tilde", 0x02DC},       {"hungarumlaut", 0x02DD},
    {"endash", 0x2013},      {"emdash", 0x2014},      {"quoteleft", 0x2018},
    {"quoteright", 0x2019},  {"quotesinglbase", 0x201A}, {"quotedblleft", 0x201C},
    {"quotedblright", 0x201D}, {"quotedblbase", 0x201E}, {"guilsinglleft", 0x2039},
    {"guilsinglright", 0x203A}, {"bullet", 0x2022},   {"ellipsis", 0x2026},
    {"dagger", 0x2020},      {"daggerdbl", 0x2021},   {"perthousand", 0x2030},
    {"fraction", 0x2044},    {"florin", 0x0192},      {"minus", 0x2212},
    {"trademark", 0x2122},   {"Euro", 0x20AC},        {"fi", 0xFB01},
    {"fl", 0xFB02},          {"ff", 0xFB00},          {"ffi", 0xFB03},
    {"ffl", 0xFB04},
};

std::wstring GlyphNameToUnicode(const std::string& name) {
    if (name.empty()) {
        return std::wstring();
    }
    // "uniXXXX" may contain several 4 digit code points ("uni00660069").
    if (name.size() >= 7 && name.compare(0, 3, "uni") == 0) {
        std::wstring result;
        for (size_t offset = 3; offset + 4 <= name.size(); offset += 4) {
            wchar_t unit = 0;
            bool valid = true;
            for (size_t digit = 0; digit < 4; ++digit) {
                const int value = HexValue(static_cast<unsigned char>(name[offset + digit]));
                if (value < 0) {
                    valid = false;
                    break;
                }
                unit = static_cast<wchar_t>((unit << 4) | value);
            }
            if (!valid) {
                break;
            }
            result.push_back(unit);
        }
        if (!result.empty()) {
            return result;
        }
    }
    // "uXXXX" or "uXXXXX" style names.  Four to six hex digits are accepted; a
    // code point above the BMP is stored as a UTF-16 surrogate pair because
    // wchar_t is 16 bits on Windows.
    if (name.size() >= 5 && name[0] == 'u') {
        uint32_t codePoint = 0;
        bool valid = true;
        for (size_t index = 1; index < name.size() && index <= 6; ++index) {
            const int value = HexValue(static_cast<unsigned char>(name[index]));
            if (value < 0) {
                valid = false;
                break;
            }
            codePoint = (codePoint << 4) | static_cast<uint32_t>(value);
        }
        if (valid && codePoint > 0 && codePoint <= 0x10FFFF) {
            std::wstring result;
            if (codePoint <= 0xFFFF) {
                result.push_back(static_cast<wchar_t>(codePoint));
            } else {
                codePoint -= 0x10000;
                result.push_back(static_cast<wchar_t>(0xD800 + (codePoint >> 10)));
                result.push_back(static_cast<wchar_t>(0xDC00 + (codePoint & 0x3FF)));
            }
            return result;
        }
    }
    // Linear scan of the table: it has fewer than 200 entries and this path runs
    // once per /Differences name while loading a font.
    for (const GlyphNameEntry& entry : kGlyphNames) {
        if (name == entry.name) {
            return std::wstring(1, entry.unicode);
        }
    }
    // Single letter names such as "A" or "z" are also valid glyph names.
    if (name.size() == 1) {
        return std::wstring(1, static_cast<wchar_t>(name[0]));
    }
    return std::wstring();
}

// -----------------------------------------------------------------------------
//  CMap reader
//  ToUnicode streams are small PostScript programs.  Only the three blocks that
//  matter for text extraction are parsed: codespace ranges, bfchar and bfrange.
//  Everything else (the CMap header, usecmap, begincidrange ...) is skipped a
//  byte at a time, which is slow but robust against unusual syntax.
// -----------------------------------------------------------------------------
class CMapCursor {
public:
    explicit CMapCursor(const std::string& text) : m_text(text) {}

    bool AtEnd() const { return m_position >= m_text.size(); }

    void SkipWhitespace() {
        while (m_position < m_text.size()) {
            const unsigned char character =
                static_cast<unsigned char>(m_text[m_position]);
            if (std::isspace(character)) {
                ++m_position;
            } else if (character == '%') {
                while (m_position < m_text.size() && m_text[m_position] != '\n' &&
                       m_text[m_position] != '\r') {
                    ++m_position;
                }
            } else {
                break;
            }
        }
    }

    bool MatchKeyword(const char* keyword) {
        // Note: this only checks for the keyword, it does not consume it.  The
        // caller advances explicitly so the "-range" keywords can share a prefix
        // with the "-char" keywords without ambiguity.
        SkipWhitespace();
        const size_t length = std::strlen(keyword);
        if (m_position + length > m_text.size()) {
            return false;
        }
        if (m_text.compare(m_position, length, keyword) != 0) {
            return false;
        }
        const size_t end = m_position + length;
        if (end < m_text.size()) {
            const unsigned char next = static_cast<unsigned char>(m_text[end]);
            if (std::isalnum(next) || next == '_') {
                return false;
            }
        }
        return true;
    }

    bool ReadHex(std::string& bytes) {
        SkipWhitespace();
        if (m_position >= m_text.size() || m_text[m_position] != '<') {
            return false;
        }
        if (m_position + 1 < m_text.size() && m_text[m_position + 1] == '<') {
            return false;  // dictionary, not a hex string
        }
        ++m_position;
        bytes.clear();
        int highNibble = -1;
        while (m_position < m_text.size() && m_text[m_position] != '>') {
            const int value =
                HexValue(static_cast<unsigned char>(m_text[m_position]));
            ++m_position;
            // Whitespace and other stray bytes inside the delimiters are skipped
            // instead of failing: real ToUnicode CMaps routinely wrap long hex
            // strings across lines.
            if (value < 0) {
                continue;
            }
            if (highNibble < 0) {
                highNibble = value;
            } else {
                bytes.push_back(static_cast<char>((highNibble << 4) | value));
                highNibble = -1;
            }
        }
        if (m_position < m_text.size()) {
            ++m_position;  // closing '>'
        }
        if (highNibble >= 0) {
            bytes.push_back(static_cast<char>(highNibble << 4));
        }
        return true;
    }

    bool ReadHexArray(std::vector<std::string>& items) {
        // Used by the bfrange array form: [<dst1> <dst2> ...].  The caller
        // decides whether the array or a single hex string follows a range.
        SkipWhitespace();
        if (m_position >= m_text.size() || m_text[m_position] != '[') {
            return false;
        }
        ++m_position;
        for (;;) {
            SkipWhitespace();
            if (m_position >= m_text.size()) {
                return false;
            }
            if (m_text[m_position] == ']') {
                ++m_position;
                return true;
            }
            std::string item;
            if (!ReadHex(item)) {
                return false;
            }
            items.push_back(std::move(item));
        }
    }

    // Moves the cursor forward, clamped to the end of the text so a malformed
    // keyword close to the end cannot move it past the buffer.
    void Advance(size_t count) { m_position += std::min(count, m_text.size() - m_position); }

private:
    const std::string& m_text;
    size_t m_position = 0;
};

void ParseToUnicode(const std::string& text,
                    std::unordered_map<uint32_t, std::wstring>& mapping,
                    int& codeBytes) {
    // The scanner is intentionally forgiving: any keyword it does not recognise
    // is skipped one byte at a time, so a CMap with unusual operators still
    // yields the bfchar/bfrange mappings around them.  `totalEntries` caps the
    // work for a pathological file.
    CMapCursor cursor(text);
    int totalEntries = 0;

    while (!cursor.AtEnd() && totalEntries < 100000) {
        if (cursor.MatchKeyword("begincodespacerange")) {
            cursor.Advance(std::strlen("begincodespacerange"));
            std::string low;
            std::string high;
            // The codespace range is the only reliable statement of how many
            // bytes form a code; the last valid range wins, which matches the
            // "one width per font" assumption made by the extractor.
            while (cursor.ReadHex(low) && cursor.ReadHex(high)) {
                if (!low.empty() && low.size() == high.size() && low.size() <= 4) {
                    codeBytes = static_cast<int>(low.size());
                }
            }
            continue;
        }

        if (cursor.MatchKeyword("beginbfchar")) {
            cursor.Advance(std::strlen("beginbfchar"));
            // bfchar lists explicit <source> <destination> pairs.
            std::string source;
            std::string destination;
            while (cursor.ReadHex(source) && cursor.ReadHex(destination)) {
                const std::wstring unicode = DecodeUtf16Be(destination);
                if (!unicode.empty()) {
                    mapping[BytesToCode(source)] = unicode;
                    ++totalEntries;
                }
            }
            continue;
        }

        if (cursor.MatchKeyword("beginbfrange")) {
            cursor.Advance(std::strlen("beginbfrange"));
            // bfrange lists <low> <high> followed by either one destination for
            // a sequential range or an array with one destination per code.
            std::string low;
            std::string high;
            while (cursor.ReadHex(low) && cursor.ReadHex(high)) {
                const uint32_t first = BytesToCode(low);
                const uint32_t last = BytesToCode(high);
                if (last < first || last - first > 65535) {
                    // Nonsensical range: consume its destination so the scanner
                    // stays aligned with the next range.
                    std::string ignored;
                    std::vector<std::string> ignoredArray;
                    cursor.SkipWhitespace();
                    if (!cursor.ReadHex(ignored)) {
                        cursor.ReadHexArray(ignoredArray);
                    }
                    continue;
                }

                cursor.SkipWhitespace();
                std::vector<std::string> destinations;
                std::string single;
                if (!cursor.ReadHexArray(destinations)) {
                    if (!cursor.ReadHex(single)) {
                        break;
                    }
                }

                if (!destinations.empty()) {
                    // The array form may be shorter than the range; the missing
                    // tail is left unmapped rather than guessed.
                    for (size_t index = 0;
                         index < destinations.size() && first + index <= last; ++index) {
                        const std::wstring unicode = DecodeUtf16Be(destinations[index]);
                        if (!unicode.empty()) {
                            mapping[first + static_cast<uint32_t>(index)] = unicode;
                            ++totalEntries;
                        }
                    }
                } else {
                    // A single destination means "start here and increment with
                    // the code".  Ranges are limited to 65536 entries above, so
                    // this loop is bounded.
                    const std::wstring base = DecodeUtf16Be(single);
                    if (base.empty()) {
                        continue;
                    }
                    for (uint32_t code = first; code <= last; ++code) {
                        std::wstring unicode = base;
                        // Sequential ranges increment the last UTF-16 unit.
                        if (!unicode.empty() &&
                            (unicode.back() < 0xD800 || unicode.back() > 0xDFFF)) {
                            unicode.back() = static_cast<wchar_t>(
                                unicode.back() + (code - first));
                        }
                        mapping[code] = unicode;
                        ++totalEntries;
                    }
                }
            }
            continue;
        }

        cursor.Advance(1);
    }
}

}  // namespace

FontDecoder::FontDecoder() {
    BuildSimpleFallback();
}

void FontDecoder::BuildSimpleFallback() {
    // WinAnsi is the most common base encoding; StandardEncoding differs only
    // in a couple of quote positions, which are overridden here.
    FillCodepageTable(1252, m_simpleCharacters);
    m_simpleCharacters[0x27] = 0x2019;  // quoteright
    m_simpleCharacters[0x60] = 0x2018;  // quoteleft
    // NBSP becomes a real space: the word-gap heuristics in TextPage treat both
    // as separators, and copied text looks better with a plain space.
    m_simpleCharacters[0xA0] = 0x0020;  // nbspace
}

bool FontDecoder::Load(Document& document, const Object& fontDictionary) {
    if (!fontDictionary.IsDict()) {
        return false;
    }

    // A missing /Subtype is treated as a simple font; that is the safe default
    // because a one-byte code would only be torn apart by two-byte handling.
    const Object subtype = ResolveIn(document, fontDictionary, "Subtype");
    m_composite = subtype.IsName() && subtype.text == "Type0";

    // Type3 fonts measure widths in an arbitrary glyph space.
    if (subtype.IsName() && subtype.text == "Type3") {
        const Object matrix = ResolveIn(document, fontDictionary, "FontMatrix");
        if (matrix.IsArray() && !matrix.items.empty()) {
            const Object scale = document.Resolve(matrix.items.front());
            if (scale.IsNumber() && scale.Number() > 0.0) {
                m_widthScale = scale.Number();
            }
        }
    }

    if (m_composite) {
        LoadType0(document, fontDictionary);
    } else {
        // /Encoding is either a name (a base encoding only) or a dictionary with
        // an optional /BaseEncoding plus /Differences on top of it.
        BuildSimpleFallback();

        const Object encoding = ResolveIn(document, fontDictionary, "Encoding");
        if (encoding.IsName()) {
            ApplyBaseEncodingName(encoding.text);
        } else if (encoding.IsDict()) {
            const Object base = ResolveIn(document, encoding, "BaseEncoding");
            if (base.IsName()) {
                ApplyBaseEncodingName(base.text);
            }
            const Object differences = ResolveIn(document, encoding, "Differences");
            if (differences.IsArray()) {
                ApplyDifferences(document, differences);
            }
        }
        LoadSimpleWidths(document, fontDictionary);
    }

    // ToUnicode wins over the encoding tables and may refine the code length.
    const Object toUnicode = ResolveIn(document, fontDictionary, "ToUnicode");
    if (toUnicode.IsStream()) {
        LoadToUnicode(document, toUnicode);
    }
    return true;
}

void FontDecoder::ApplyBaseEncodingName(const std::string& name) {
    // Only the encodings that appear in practice are mapped.  An unknown name
    // keeps the WinAnsi fallback established by the caller; guessing at some
    // other encoding would be worse than a slightly wrong table.
    if (name == "WinAnsiEncoding") {
        FillCodepageTable(1252, m_simpleCharacters);
        m_simpleCharacters[0xA0] = 0x0020;
    } else if (name == "MacRomanEncoding") {
        FillCodepageTable(10000, m_simpleCharacters);
    } else if (name == "StandardEncoding" || name == "PDFDocEncoding") {
        BuildSimpleFallback();
    }
}

void FontDecoder::ApplyDifferences(Document& document, const Object& differences) {
    // The array alternates a start code and a run of glyph names; each number
    // resets the current code, each name consumes the next code.  Names may also
    // be indirect references, which is unusual but legal.
    uint32_t code = 0;
    for (const Object& item : differences.items) {
        const Object value = document.Resolve(item);
        if (value.IsNumber()) {
            code = static_cast<uint32_t>(value.Integer());
        } else if (value.IsName()) {
            ApplyDifference(value.text, code);
            ++code;
        }
    }
}

void FontDecoder::ApplyDifference(const std::string& glyphName, uint32_t code) {
    // The simple table stores one wchar_t per code, so only the first code point
    // of a multi-character name (a ligature such as "ffi") fits there.  The full
    // sequence is kept in the ToUnicode-style map, which Decode prefers.
    if (code >= 256) {
        return;
    }
    const std::wstring unicode = GlyphNameToUnicode(glyphName);
    if (unicode.empty()) {
        return;
    }
    m_simpleCharacters[code] = unicode.front();
    if (unicode.size() > 1) {
        m_unicode[code] = unicode;
    }
}

void FontDecoder::LoadSimpleWidths(Document& document,
                                   const Object& fontDictionary) {
    // 0.5 em is a neutral guess for a font with no width information at all.
    m_missingWidthEm = 0.5;

    // /MissingWidth lives in the font descriptor and is in glyph space units
    // (1/1000 em for everything except Type3).
    const Object descriptor =
        ResolveIn(document, fontDictionary, "FontDescriptor");
    if (descriptor.IsDict()) {
        const Object missing = ResolveIn(document, descriptor, "MissingWidth");
        if (missing.IsNumber() && missing.Number() > 0.0) {
            m_missingWidthEm = missing.Number() * m_widthScale;
        }
    }

    const Object firstChar = ResolveIn(document, fontDictionary, "FirstChar");
    const Object widths = ResolveIn(document, fontDictionary, "Widths");
    if (!widths.IsArray() || widths.items.empty()) {
        return;
    }
    if (firstChar.IsNumber() && firstChar.Integer() >= 0) {
        m_firstChar = static_cast<uint32_t>(firstChar.Integer());
    }
    m_simpleWidths.reserve(widths.items.size());
    for (const Object& item : widths.items) {
        const Object value = document.Resolve(item);
        // Widths are glyph space, not text space, hence the /FontMatrix scale.
        // A non-positive width is replaced instead of trusted: a zero advance
        // would pile every glyph of the string onto one point.
        const double width =
            value.IsNumber() ? value.Number() * m_widthScale : m_missingWidthEm;
        m_simpleWidths.push_back(width > 0.0 ? width : m_missingWidthEm);
    }
    m_hasSimpleWidths = true;
}

void FontDecoder::LoadType0(Document& document, const Object& fontDictionary) {
    // Two bytes per code is the assumption that makes Identity-H/V work, which
    // covers almost every CJK document.  A ToUnicode codespace range can refine
    // this later for other CMaps.
    m_codeBytes = 2;
    m_defaultWidthEm = 1.0;  // /DW defaults to 1000

    const Object encoding = ResolveIn(document, fontDictionary, "Encoding");
    if (encoding.IsName()) {
        m_vertical = encoding.text == "Identity-V";
    } else if (encoding.IsStream()) {
        // Embedded CMap: only the writing mode is needed here.  The code length
        // stays at two bytes for the supported identity style CMaps; the string
        // search is a deliberate shortcut over parsing the CMap properly.
        std::vector<uint8_t> bytes;
        if (document.DecodeStream(encoding, bytes)) {
            const std::string text(bytes.begin(), bytes.end());
            if (text.find("/WMode 1") != std::string::npos) {
                m_vertical = true;
            }
        }
    }

    // A Type0 font has exactly one descendant CIDFont in practice; if there are
    // several, the first one describes the text that is actually shown.
    const Object descendants =
        ResolveIn(document, fontDictionary, "DescendantFonts");
    if (descendants.IsArray() && !descendants.items.empty()) {
        const Object descendant = document.Resolve(descendants.items.front());
        if (descendant.IsDict()) {
            LoadCidWidths(document, descendant);
        }
    }
}

void FontDecoder::LoadCidWidths(Document& document, const Object& descendantFont) {
    // /DW is the default advance for CIDs without an explicit /W entry; its
    // value is in glyph space units of 1/1000 em, unlike the simple-font path
    // where /FontMatrix may rescale it.
    const Object defaultWidth = ResolveIn(document, descendantFont, "DW");
    if (defaultWidth.IsNumber() && defaultWidth.Number() > 0.0) {
        m_defaultWidthEm = defaultWidth.Number() * 0.001;
    }

    const Object descriptor =
        ResolveIn(document, descendantFont, "FontDescriptor");
    if (descriptor.IsDict()) {
        const Object missing = ResolveIn(document, descriptor, "MissingWidth");
        if (missing.IsNumber() && missing.Number() > 0.0) {
            m_missingWidthEm = missing.Number() * 0.001;
        } else {
            m_missingWidthEm = m_defaultWidthEm;
        }
    } else {
        m_missingWidthEm = m_defaultWidthEm;
    }

    const Object widths = ResolveIn(document, descendantFont, "W");
    if (!widths.IsArray()) {
        return;
    }

    // /W mixes two forms in one array:
    //   c [w1 w2 ...]   individual widths for consecutive CIDs starting at c
    //   cFirst cLast w  one width shared by a whole CID range
    // The loop advances by two or three entries depending on the form, and
    // skips non-numeric junk instead of giving up on the whole array.
    size_t index = 0;
    while (index < widths.items.size()) {
        const Object first = document.Resolve(widths.items[index]);
        if (!first.IsNumber()) {
            ++index;
            continue;
        }
        const uint32_t firstCode = static_cast<uint32_t>(first.Integer());
        if (index + 1 >= widths.items.size()) {
            break;
        }
        const Object second = document.Resolve(widths.items[index + 1]);
        if (second.IsArray()) {
            for (size_t offset = 0; offset < second.items.size(); ++offset) {
                const Object width = document.Resolve(second.items[offset]);
                if (width.IsNumber()) {
                    m_widths[firstCode + static_cast<uint32_t>(offset)] =
                        width.Number() * 0.001;
                }
            }
            index += 2;
        } else {
            if (index + 2 >= widths.items.size()) {
                break;
            }
            const Object width = document.Resolve(widths.items[index + 2]);
            if (second.IsNumber() && width.IsNumber()) {
                const uint32_t lastCode = static_cast<uint32_t>(second.Integer());
                const double widthEm = width.Number() * 0.001;
                if (lastCode >= firstCode) {
                    // Cap the range at 65536 entries so a corrupt cLast cannot
                    // expand one entry into a multi-gigabyte map.
                    const uint32_t count =
                        std::min<uint32_t>(lastCode - firstCode, 65535u);
                    for (uint32_t offset = 0; offset <= count; ++offset) {
                        m_widths[firstCode + offset] = widthEm;
                    }
                }
            }
            index += 3;
        }
    }
}

void FontDecoder::LoadToUnicode(Document& document, const Object& streamObject) {
    // The CMap is a regular stream (usually Flate compressed), so it goes through
    // the normal decode path; a broken stream leaves the font without a ToUnicode
    // map and the encoding table remains in charge.
    std::vector<uint8_t> bytes;
    if (!document.DecodeStream(streamObject, bytes) || bytes.empty()) {
        return;
    }
    const std::string text(bytes.begin(), bytes.end());
    int codeBytes = m_codeBytes;
    ParseToUnicode(text, m_unicode, codeBytes);
    // Only composite fonts may change their code width: a simple font is always
    // read one byte at a time by the extractor.
    if (m_composite && codeBytes >= 1 && codeBytes <= 4) {
        m_codeBytes = codeBytes;
    }
}

bool FontDecoder::Decode(uint32_t code, std::wstring& text,
                         double& widthEm) const {
    text.clear();

    // Width first and unconditionally: it is what keeps the extraction cursor in
    // the right place even when the character itself is unknown.
    if (m_composite) {
        const auto width = m_widths.find(code);
        widthEm = width != m_widths.end() ? width->second : m_defaultWidthEm;
    } else if (m_hasSimpleWidths && code >= m_firstChar &&
               code - m_firstChar < m_simpleWidths.size()) {
        widthEm = m_simpleWidths[code - m_firstChar];
    } else {
        widthEm = m_missingWidthEm > 0.0 ? m_missingWidthEm : 0.5;
    }

    const auto unicode = m_unicode.find(code);
    if (unicode != m_unicode.end()) {
        // ToUnicode (or a multi-character /Differences name) is authoritative.
        text = unicode->second;
    } else if (!m_composite && code < 256) {
        // No mapping: the /Encoding table is the only remaining source.
        text.assign(1, m_simpleCharacters[code]);
    }
    // Even when there is no Unicode mapping the caller advances the text
    // position using `widthEm`, which keeps later glyphs aligned.
    return true;
}

}  // namespace pdf
