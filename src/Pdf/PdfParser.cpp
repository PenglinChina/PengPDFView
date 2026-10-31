// =============================================================================
//  PdfParser.cpp
//  Document structure parser.
//
//  Responsibilities:
//    * tokenise the PDF syntax (lexer),
//    * read the cross-reference table or cross-reference stream and merge
//      incremental updates,
//    * resolve indirect objects, including objects stored inside object
//      streams,
//    * walk the page tree to obtain a stable page index,
//    * read the document information dictionary.
//
//  The outline (bookmark) tree is handled in PdfOutline.cpp because it is a
//  separate concern that only needs the public parsing primitives.
//
//  Error handling is optimistic: every offset is bounds-checked against the
//  in-memory file image and any inconsistency falls back to the object scanner
//  rather than failing the whole document.  Apart from unreadable or truncated
//  input, the only structural failure is "no document catalog could be found".
// =============================================================================
#include "Pdf/PdfParser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "Core/FileUtil.h"
#include "Core/Log.h"
#include "Core/Utf.h"
#include "Pdf/PdfFilters.h"
#include "Pdf/PdfOutline.h"

namespace pdf {

// Core helpers live in the pengpdf namespace; import the ones used here.
using pengpdf::GetFullPath;
using pengpdf::LogError;
using pengpdf::LogInfo;
using pengpdf::ReadFileBytes;

namespace {

// Sentinel for "no value" used by the stream-length helpers, where 0 is a valid
// length and std::optional would not match the surrounding style.
constexpr size_t kNotSet = static_cast<size_t>(-1);

// PDF whitespace as defined by the specification (ISO 32000, 7.2.2).  NUL is
// included even though it is not printable: producers pad with it and the spec
// explicitly lists it as whitespace.
bool IsPdfWhitespace(uint8_t character) {
    return character == 0 || character == 9 || character == 10 ||
           character == 12 || character == 13 || character == 32;
}

// Delimiters terminate a regular token.  Knowing them is what lets the lexer
// stop "12" before "[0 0 612 792]" without requiring spaces around brackets.
bool IsPdfDelimiter(uint8_t character) {
    switch (character) {
        case '(':
        case ')':
        case '<':
        case '>':
        case '[':
        case ']':
        case '{':
        case '}':
        case '/':
        case '%':
            return true;
        default:
            return false;
    }
}

bool IsRegularCharacter(uint8_t character) {
    return !IsPdfWhitespace(character) && !IsPdfDelimiter(character);
}

bool IsAsciiDigit(uint8_t character) {
    return character >= '0' && character <= '9';
}

bool IsHexDigit(uint8_t character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
}

int HexDigitValue(uint8_t character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    return character - 'A' + 10;
}

// -----------------------------------------------------------------------------
//  Tokenizer
//
//  Parsing works directly on the memory image of the file.  Positions are byte
//  offsets; after a successful parse the lexer stops right behind the last
//  consumed character.
//
//  The lexer references the file image instead of copying it, so it must not
//  outlive the Document.  It never throws: malformed input simply makes the
//  Parse* call being attempted return false.
// -----------------------------------------------------------------------------
class Lexer {
public:
    explicit Lexer(const std::vector<uint8_t>& bytes) : m_bytes(bytes) {}

    size_t Position() const { return m_position; }
    void SetPosition(size_t position) { m_position = position; }
    bool AtEnd() const { return m_position >= m_bytes.size(); }

    void SkipWhitespace() {
        while (m_position < m_bytes.size()) {
            const uint8_t character = m_bytes[m_position];
            if (IsPdfWhitespace(character)) {
                ++m_position;
            } else if (character == '%') {
                // Comments run to the end of the line.
                while (m_position < m_bytes.size() && m_bytes[m_position] != '\n' &&
                       m_bytes[m_position] != '\r') {
                    ++m_position;
                }
            } else {
                break;
            }
        }
    }

    bool Peek(uint8_t& character) {
        SkipWhitespace();
        if (m_position >= m_bytes.size()) {
            return false;
        }
        character = m_bytes[m_position];
        return true;
    }

    // Matches a complete keyword.  The keyword must not be followed by another
    // regular character, otherwise "obj" would match inside "object".
    bool MatchKeyword(std::string_view keyword) {
        SkipWhitespace();
        if (m_bytes.size() - m_position < keyword.size()) {
            return false;
        }
        if (std::memcmp(m_bytes.data() + m_position, keyword.data(), keyword.size()) != 0) {
            return false;
        }
        const size_t end = m_position + keyword.size();
        if (end < m_bytes.size() && IsRegularCharacter(m_bytes[end])) {
            return false;
        }
        m_position = end;
        return true;
    }

    bool ReadToken(std::string& token) {
        token.clear();
        SkipWhitespace();
        while (m_position < m_bytes.size() && IsRegularCharacter(m_bytes[m_position])) {
            token.push_back(static_cast<char>(m_bytes[m_position]));
            ++m_position;
        }
        return !token.empty();
    }

    bool ParseObject(Object& object, int depth) {
        // Hard recursion limit: a crafted file with a million nested "[" would
        // otherwise exhaust the C++ stack before any size check applies.
        if (depth > 32) {
            return false;
        }
        SkipWhitespace();
        if (AtEnd()) {
            return false;
        }

        const uint8_t character = m_bytes[m_position];
        if (character == '/') {
            return ParseName(object);
        }
        if (character == '(') {
            return ParseLiteralString(object);
        }
        if (character == '[') {
            return ParseArray(object, depth);
        }
        if (character == '<') {
            if (m_position + 1 < m_bytes.size() && m_bytes[m_position + 1] == '<') {
                return ParseDictionary(object, depth);
            }
            return ParseHexString(object);
        }
        if (character == '+' || character == '-' || character == '.' ||
            IsAsciiDigit(character)) {
            // A number may be the start of an indirect reference "12 0 R".
            // Parsing is ambiguous until the following token is known, so the
            // position is remembered and rewound when this is a plain integer.
            const size_t saved = m_position;
            int64_t first = 0;
            if (ParseInteger(first)) {
                const size_t afterFirst = m_position;
                int64_t second = 0;
                if (ParseInteger(second) && second >= 0) {
                    SkipWhitespace();
                    if (!AtEnd() && m_bytes[m_position] == 'R' &&
                        (m_position + 1 >= m_bytes.size() ||
                         !IsRegularCharacter(m_bytes[m_position + 1]))) {
                        ++m_position;
                        object = Object::MakeRef(static_cast<int>(first),
                                                 static_cast<int>(second));
                        return true;
                    }
                }
                m_position = afterFirst;
                object = Object::MakeInt(first);
                return true;
            }
            m_position = saved;
            double real = 0.0;
            if (ParseReal(real)) {
                object = Object::MakeReal(real);
                return true;
            }
            return false;
        }

        // The remaining object types are spelled as keywords.
        const size_t saved = m_position;
        std::string token;
        if (ReadToken(token)) {
            if (token == "true") {
                object.type = Object::Type::Bool;
                object.boolean = true;
                return true;
            }
            if (token == "false") {
                object.type = Object::Type::Bool;
                object.boolean = false;
                return true;
            }
            if (token == "null") {
                object = Object();
                return true;
            }
        }
        m_position = saved;
        return false;
    }

    bool ParseInteger(int64_t& value) {
        SkipWhitespace();
        size_t position = m_position;
        if (position >= m_bytes.size()) {
            return false;
        }

        bool negative = false;
        if (m_bytes[position] == '+' || m_bytes[position] == '-') {
            negative = m_bytes[position] == '-';
            ++position;
        }
        const size_t digitsStart = position;
        int64_t parsed = 0;
        while (position < m_bytes.size() && IsAsciiDigit(m_bytes[position])) {
            if (parsed > 1000000000000000LL) {
                return false;  // absurdly long number, treat as invalid
            }
            parsed = parsed * 10 + (m_bytes[position] - '0');
            ++position;
        }
        if (position == digitsStart) {
            return false;
        }
        // "1.5" or "1e3" is not an integer; leave the token for ParseReal.
        if (position < m_bytes.size() &&
            (m_bytes[position] == '.' || m_bytes[position] == 'e' ||
             m_bytes[position] == 'E')) {
            return false;
        }
        m_position = position;
        value = negative ? -parsed : parsed;
        return true;
    }

    bool ParseReal(double& value) {
        // The token is scanned by hand and only then handed to strtod: strtod
        // alone would also accept "inf", "nan" and locale dependent separators,
        // none of which are legal PDF numbers.
        SkipWhitespace();
        const size_t start = m_position;
        size_t position = start;
        if (position < m_bytes.size() &&
            (m_bytes[position] == '+' || m_bytes[position] == '-')) {
            ++position;
        }
        bool haveDigits = false;
        while (position < m_bytes.size() && IsAsciiDigit(m_bytes[position])) {
            ++position;
            haveDigits = true;
        }
        if (position < m_bytes.size() && m_bytes[position] == '.') {
            ++position;
            while (position < m_bytes.size() && IsAsciiDigit(m_bytes[position])) {
                ++position;
                haveDigits = true;
            }
        }
        if (!haveDigits) {
            return false;
        }
        if (position < m_bytes.size() &&
            (m_bytes[position] == 'e' || m_bytes[position] == 'E')) {
            size_t exponent = position + 1;
            if (exponent < m_bytes.size() &&
                (m_bytes[exponent] == '+' || m_bytes[exponent] == '-')) {
                ++exponent;
            }
            if (exponent < m_bytes.size() && IsAsciiDigit(m_bytes[exponent])) {
                while (exponent < m_bytes.size() && IsAsciiDigit(m_bytes[exponent])) {
                    ++exponent;
                }
                position = exponent;
            }
        }

        const std::string token(
            reinterpret_cast<const char*>(m_bytes.data() + start), position - start);
        value = std::strtod(token.c_str(), nullptr);
        m_position = position;
        return true;
    }

private:
    // Names are stored without the leading '/' and with "#xx" escapes decoded,
    // so /A#20B and /A B compare equal, as the specification requires.
    bool ParseName(Object& object) {
        SkipWhitespace();
        if (AtEnd() || m_bytes[m_position] != '/') {
            return false;
        }
        ++m_position;

        std::string name;
        while (m_position < m_bytes.size() && IsRegularCharacter(m_bytes[m_position])) {
            const uint8_t character = m_bytes[m_position];
            if (character == '#' && m_position + 2 < m_bytes.size() &&
                IsHexDigit(m_bytes[m_position + 1]) && IsHexDigit(m_bytes[m_position + 2])) {
                name.push_back(static_cast<char>(
                    (HexDigitValue(m_bytes[m_position + 1]) << 4) |
                    HexDigitValue(m_bytes[m_position + 2])));
                m_position += 3;
                continue;
            }
            name.push_back(static_cast<char>(character));
            ++m_position;
        }
        object = Object::MakeName(std::move(name));
        return true;
    }

    // Literal strings are balanced parentheses with backslash escapes; the
    // nesting counter is what makes "(a (b) c)" a single string.
    bool ParseLiteralString(Object& object) {
        SkipWhitespace();
        if (AtEnd() || m_bytes[m_position] != '(') {
            return false;
        }
        ++m_position;

        std::string content;
        int nesting = 1;
        while (m_position < m_bytes.size()) {
            const uint8_t character = m_bytes[m_position++];

            if (character == '\\') {
                if (AtEnd()) {
                    break;
                }
                const uint8_t escaped = m_bytes[m_position++];
                switch (escaped) {
                    case 'n': content.push_back('\n'); break;
                    case 'r': content.push_back('\r'); break;
                    case 't': content.push_back('\t'); break;
                    case 'b': content.push_back('\b'); break;
                    case 'f': content.push_back('\f'); break;
                    case '(': content.push_back('('); break;
                    case ')': content.push_back(')'); break;
                    case '\\': content.push_back('\\'); break;
                    case '\r':
                        // Backslash at the end of a line: the line break is
                        // removed and the string continues on the next line.
                        if (!AtEnd() && m_bytes[m_position] == '\n') {
                            ++m_position;
                        }
                        break;
                    case '\n':
                        break;
                    default:
                        if (escaped >= '0' && escaped <= '7') {
                            int octal = escaped - '0';
                            for (int i = 0; i < 2 && !AtEnd() &&
                                            m_bytes[m_position] >= '0' &&
                                            m_bytes[m_position] <= '7';
                                 ++i) {
                                octal = octal * 8 + (m_bytes[m_position] - '0');
                                ++m_position;
                            }
                            content.push_back(static_cast<char>(octal & 0xFF));
                        } else {
                            content.push_back(static_cast<char>(escaped));
                        }
                        break;
                }
                continue;
            }

            if (character == '(') {
                ++nesting;
                content.push_back('(');
                continue;
            }
            if (character == ')') {
                --nesting;
                if (nesting == 0) {
                    break;
                }
                content.push_back(')');
                continue;
            }
            content.push_back(static_cast<char>(character));
        }

        object = Object::MakeString(std::move(content));
        return true;
    }

    // Hex strings ignore embedded whitespace and are padded to a whole byte on
    // an odd digit count, which is what the specification prescribes.
    bool ParseHexString(Object& object) {
        SkipWhitespace();
        if (AtEnd() || m_bytes[m_position] != '<') {
            return false;
        }
        ++m_position;

        std::string content;
        int highNibble = -1;
        while (m_position < m_bytes.size()) {
            const uint8_t character = m_bytes[m_position++];
            if (character == '>') {
                break;
            }
            if (IsPdfWhitespace(character)) {
                continue;
            }
            if (!IsHexDigit(character)) {
                return false;
            }
            const int value = HexDigitValue(character);
            if (highNibble < 0) {
                highNibble = value;
            } else {
                content.push_back(static_cast<char>((highNibble << 4) | value));
                highNibble = -1;
            }
        }
        // An odd number of digits is padded with a trailing zero nibble.
        if (highNibble >= 0) {
            content.push_back(static_cast<char>(highNibble << 4));
        }
        object = Object::MakeString(std::move(content));
        return true;
    }

    bool ParseArray(Object& object, int depth) {
        SkipWhitespace();
        if (AtEnd() || m_bytes[m_position] != '[') {
            return false;
        }
        ++m_position;

        Object array;
        array.type = Object::Type::Array;
        for (;;) {
            SkipWhitespace();
            if (AtEnd()) {
                return false;
            }
            if (m_bytes[m_position] == ']') {
                ++m_position;
                object = std::move(array);
                return true;
            }
            if (array.items.size() > 1000000) {
                return false;  // corrupt length, refuse to allocate without bound
            }
            Object item;
            if (!ParseObject(item, depth + 1)) {
                return false;
            }
            array.items.push_back(std::move(item));
        }
    }

    bool ParseDictionary(Object& object, int depth) {
        SkipWhitespace();
        if (m_position + 1 >= m_bytes.size() || m_bytes[m_position] != '<' ||
            m_bytes[m_position + 1] != '<') {
            return false;
        }
        m_position += 2;

        auto dictionary = std::make_shared<Dict>();
        // Shared ownership: every resolved reference hands out another copy of
        // this dictionary, and the cache keeps the original alive.
        for (;;) {
            SkipWhitespace();
            if (m_position + 1 >= m_bytes.size()) {
                return false;
            }
            if (m_bytes[m_position] == '>' && m_bytes[m_position + 1] == '>') {
                m_position += 2;
                object.type = Object::Type::Dict;
                object.dict = std::move(dictionary);
                return true;
            }

            Object key;
            if (!ParseName(key)) {
                return false;
            }
            Object value;
            if (!ParseObject(value, depth + 1)) {
                return false;
            }
            // Later duplicates win; malformed files occasionally repeat keys.
            (*dictionary)[key.text] = std::move(value);
        }
    }

    const std::vector<uint8_t>& m_bytes;
    size_t m_position = 0;
};

// One cross-reference entry.  The numbering of `type` mirrors the field layout
// of cross-reference streams (0 = free, 1 = file offset, 2 = compressed object);
// classic tables only ever produce type 1 entries.
struct XrefEntry {
    int type = 0;  // 0 = free, 1 = file offset, 2 = inside an object stream
    uint64_t offset = 0;    // byte offset of "N G obj" when type == 1
    int streamObject = 0;   // object number holding the object stream, type == 2
    int streamIndex = 0;    // index inside that object stream, type == 2
};

}  // namespace

// =============================================================================
//  Document implementation
// =============================================================================
struct Document::Impl {
    std::vector<uint8_t> bytes;   // whole file, kept for random access
    std::wstring path;
    std::wstring lastError;
    bool loaded = false;

    DocumentMetadata metadata;
    Dict trailer;  // merged from every xref section, newest values win

    // Effective cross-reference entries.  Entries from a newer section are
    // inserted first and never overwritten, which implements the "newest
    // revision wins" rule for incremental updates.
    std::unordered_map<int, XrefEntry> xref;
    // Fallback built by scanning for "N G obj" headers.  Used whenever the xref
    // data is absent or points at the wrong object.
    std::unordered_map<int, uint64_t> scanned;  // fallback: object number -> offset
    // Parsed objects by object number.  This is the cache whose shared
    // dictionaries and stream buffers every Object copy aliases.
    std::unordered_map<int, Object> cache;
    // Object numbers currently being parsed.  A second request for the same
    // number means the file contains a reference cycle.
    std::unordered_set<int> loading;

    std::unordered_map<int, int> pageIndexByObject;
    std::vector<PageSize> pageSizes;
    std::vector<PageBox> pageBoxes;   // crop box in PDF points, parallel to pageSizes
    std::vector<int> pageObjects;     // page object number per index, 0 = direct
    std::vector<OutlineEntry> outline;
    std::unordered_set<int> visitedNodes;  // breaks cyclic page trees

    // Guards the lazily filled object cache.  Text extraction runs on a worker
    // thread while the UI may still resolve outline destinations, so every
    // public entry point that touches the cache takes this mutex.
    mutable std::mutex objectMutex;

    // Returned by GetObject for missing objects.  Never written after loading,
    // so handing out const references to it is safe.
    Object nullObject;

    // Loading pipeline, in call order: read the file, scan object headers as a
    // safety net, follow startxref / Prev / XRefStm to merge every xref section,
    // rebuild the trailer when that failed, then walk the page tree and /Info.
    bool Load(const std::wstring& filePath);
    // Clears every piece of per-document state so Load can be called again
    // (for example when the host reuses a Document for another file).
    void ResetForLoad();
    void ScanObjects();
    // Locates the offset stored after the last "startxref" keyword.
    bool FindStartXref(uint64_t& offset) const;
    void ParseXrefChain(uint64_t offset);
    bool ParseXrefSection(uint64_t offset, Dict& sectionTrailer);
    bool ParseXrefTable(Lexer& lexer, Dict& sectionTrailer);
    bool ParseXrefStream(const Object& streamObject);
    void InsertXref(int number, const XrefEntry& entry);
    void MergeTrailer(const Dict& section);
    // Last resort when no usable trailer was found: probe every scanned object
    // for a catalog dictionary.
    void RebuildTrailerFromScan();

    bool ParseObjectAt(size_t offset, int expectedNumber, Object& out);
    const Object& GetObject(int number);
    bool LoadCompressedObject(int number, const XrefEntry& entry, Object& out);
    bool EnsureDecoded(const Object& streamObject);
    Object Resolve(const Object& object);
    size_t DetermineStreamLength(const Object& dictionary);
    size_t FindEndstream(size_t start) const;

    void BuildPageTree();
    void WalkPageNode(const Object& nodeReference, int depth, PageBox inheritedMedia,
                      bool haveMedia, PageBox inheritedCrop, bool haveCrop);
    bool TryReadPageBox(const Object& node, std::string_view key, PageBox& box);
    void ReadMetadata();
    std::wstring ReadInfoString(const Object& info, std::string_view key);

    // Lookup without the "resolve" step; returns a pointer into the dictionary.
    static const Object* FindIn(const Dict& dictionary, std::string_view key) {
        const auto it = dictionary.find(key);
        return it == dictionary.end() ? nullptr : &it->second;
    }

    // Lookup followed by reference resolution; null when absent.
    Object ResolveIn(const Dict& dictionary, std::string_view key) {
        const Object* entry = FindIn(dictionary, key);
        return entry == nullptr ? Object() : Resolve(*entry);
    }
};

void Document::Impl::ResetForLoad() {
    bytes.clear();
    path.clear();
    lastError.clear();
    loaded = false;

    metadata = DocumentMetadata{};
    trailer.clear();
    xref.clear();
    scanned.clear();
    cache.clear();
    loading.clear();

    pageIndexByObject.clear();
    pageSizes.clear();
    pageBoxes.clear();
    pageObjects.clear();
    outline.clear();
    visitedNodes.clear();
}

bool Document::Impl::Load(const std::wstring& filePath) {
    // Loading another file into the same Document must not leave object
    // numbers or page pointers from the previous one behind.
    ResetForLoad();
    path = GetFullPath(filePath);
    lastError.clear();

    std::wstring readError;
    if (!ReadFileBytes(path, bytes, &readError)) {
        lastError = readError.empty() ? L"Cannot read the document." : readError;
        LogError(L"Failed to read %s: %s", path.c_str(), lastError.c_str());
        return false;
    }
    if (bytes.size() < 32) {
        // Smaller than the smallest legal header plus startxref and %%EOF; do
        // not waste time on it (and avoid underflow in later offset math).
        lastError = L"The file is too small to be a PDF document.";
        return false;
    }

    // Building the fallback offset table first makes the parser tolerant of
    // broken cross-reference data, which is surprisingly common in the wild.
    ScanObjects();

    uint64_t startXref = 0;
    if (FindStartXref(startXref)) {
        ParseXrefChain(startXref);
    } else {
        // Not fatal: the scan below may still recover the catalog.
        LogError(L"No startxref keyword found in %s", path.c_str());
    }

    // Only when the xref machinery failed completely is the catalog searched
    // for by brute force; it is the one object every document must have.
    if (FindIn(trailer, "Root") == nullptr) {
        RebuildTrailerFromScan();
    }
    if (FindIn(trailer, "Root") == nullptr) {
        lastError = L"The document catalog could not be located.";
        LogError(L"%s: no document catalog found", path.c_str());
        return false;
    }

    // The page tree has to exist before metadata because outline destination
    // resolution (performed after Load returns) needs page indices.
    BuildPageTree();
    ReadMetadata();
    loaded = true;

    LogInfo(L"Parsed %s: %zu bytes, %d pages, %d compressed objects, %d scanned objects",
            path.c_str(), bytes.size(), static_cast<int>(pageSizes.size()),
            static_cast<int>(std::count_if(
                xref.begin(), xref.end(),
                [](const auto& entry) { return entry.second.type == 2; })),
            static_cast<int>(scanned.size()));
    return true;
}

// -----------------------------------------------------------------------------
//  Fallback object scan
//  Locates every "N G obj" sequence in the file.  Later occurrences win, which
//  matches the behaviour of incremental updates that append new versions.
// -----------------------------------------------------------------------------
void Document::Impl::ScanObjects() {
    const size_t size = bytes.size();
    if (size < 8) {
        return;
    }

    // The scan is a plain byte compare over the whole file, including the
    // payload of streams, because skipping stream bodies reliably would require
    // knowing every /Length in advance.  False positives are harmless: GetObject
    // re-validates the "N G obj" header at the recorded offset.
    for (size_t position = 0; position + 3 <= size; ++position) {
        if (bytes[position] != 'o' || bytes[position + 1] != 'b' ||
            bytes[position + 2] != 'j') {
            continue;
        }
        const size_t afterKeyword = position + 3;
        if (afterKeyword < size && IsRegularCharacter(bytes[afterKeyword])) {
            continue;
        }

        // Walk backwards over: whitespace, generation number, whitespace,
        // object number.
        size_t cursor = position;
        while (cursor > 0 && IsPdfWhitespace(bytes[cursor - 1])) {
            --cursor;
        }
        if (cursor == position) {
            continue;  // "obj" must be preceded by whitespace
        }

        const size_t generationEnd = cursor;
        while (cursor > 0 && IsAsciiDigit(bytes[cursor - 1])) {
            --cursor;
        }
        if (cursor == generationEnd) {
            continue;
        }

        while (cursor > 0 && IsPdfWhitespace(bytes[cursor - 1])) {
            --cursor;
        }
        const size_t numberEnd = cursor;
        while (cursor > 0 && IsAsciiDigit(bytes[cursor - 1])) {
            --cursor;
        }
        if (cursor == numberEnd) {
            continue;
        }
        if (cursor > 0 && IsRegularCharacter(bytes[cursor - 1])) {
            continue;
        }

        int number = 0;
        for (size_t i = cursor; i < numberEnd; ++i) {
            if (number > 100000000) {
                // Absurdly large object number: almost certainly the digits
                // belong to binary data, not to an object header.
                number = -1;
                break;
            }
            number = number * 10 + (bytes[i] - '0');
        }
        if (number >= 0) {
            scanned[number] = static_cast<uint64_t>(cursor);
            position = afterKeyword;  // skip the rest of this header
        }
    }
}

bool Document::Impl::FindStartXref(uint64_t& offset) const {
    static constexpr char kMarker[] = "startxref";
    constexpr size_t kMarkerLength = sizeof(kMarker) - 1;
    if (bytes.size() < kMarkerLength) {
        return false;
    }

    // The keyword lives near the end of the file, but incremental updates and
    // trailing garbage justify a full scan from the back.
    for (size_t position = bytes.size() - kMarkerLength + 1; position-- > 0;) {
        if (std::memcmp(bytes.data() + position, kMarker, kMarkerLength) != 0) {
            continue;
        }
        Lexer lexer(bytes);
        lexer.SetPosition(position + kMarkerLength);
        int64_t value = 0;
        if (lexer.ParseInteger(value) && value >= 0) {
            offset = static_cast<uint64_t>(value);
            return true;
        }
    }
    return false;
}

void Document::Impl::InsertXref(int number, const XrefEntry& entry) {
    // Earlier sections contain the newest data, so never overwrite an entry.
    if (number < 0 || xref.find(number) != xref.end()) {
        return;
    }
    xref.emplace(number, entry);
}

void Document::Impl::MergeTrailer(const Dict& section) {
    // Sections arrive newest first, so an already present key belongs to a newer
    // revision and must win (incremental updates rewrite /Info and /Root this
    // way).  This is the same precedence rule as InsertXref.
    for (const auto& item : section) {
        if (trailer.find(item.first) == trailer.end()) {
            trailer.emplace(item.first, item.second);
        }
    }
}

void Document::Impl::ParseXrefChain(uint64_t offset) {
    // `visited` breaks circular /Prev chains, `depth` caps a chain that is long
    // but not circular; both are cheaper than relying on the file running out.
    std::unordered_set<uint64_t> visited;
    int depth = 0;

    while (offset < bytes.size() && depth++ < 64 && visited.insert(offset).second) {
        Dict section;
        if (!ParseXrefSection(offset, section)) {
            LogError(L"Invalid cross-reference section at offset %llu",
                     static_cast<unsigned long long>(offset));
            break;
        }
        MergeTrailer(section);

        // Hybrid-reference files store additional entries in a cross-reference
        // stream referenced from the classic table.  The stream is visited here
        // rather than through /Prev because it has no trailer of its own.
        const Object* xrefStm = FindIn(section, "XRefStm");
        if (xrefStm != nullptr && xrefStm->IsNumber()) {
            const uint64_t extraOffset = static_cast<uint64_t>(xrefStm->Integer());
            if (extraOffset < bytes.size() && visited.insert(extraOffset).second) {
                Dict extraSection;
                if (ParseXrefSection(extraOffset, extraSection)) {
                    MergeTrailer(extraSection);
                }
            }
        }

        const Object* previous = FindIn(section, "Prev");
        if (previous == nullptr || !previous->IsNumber()) {
            break;
        }
        const int64_t previousOffset = previous->Integer();
        if (previousOffset <= 0) {
            break;
        }
        offset = static_cast<uint64_t>(previousOffset);
    }
}

bool Document::Impl::ParseXrefSection(uint64_t offset, Dict& sectionTrailer) {
    if (offset >= bytes.size()) {
        return false;
    }

    Lexer lexer(bytes);
    lexer.SetPosition(static_cast<size_t>(offset));
    lexer.SkipWhitespace();

    if (lexer.MatchKeyword("xref")) {
        return ParseXrefTable(lexer, sectionTrailer);
    }

    // Otherwise this offset must point at a cross-reference stream object,
    // introduced in PDF 1.5.  PDF allows both kinds to coexist in one chain.
    Object streamObject;
    if (!ParseObjectAt(static_cast<size_t>(offset), -1, streamObject)) {
        return false;
    }
    if (!streamObject.IsStream() || !streamObject.dict) {
        return false;
    }
    Dict dictionary = *streamObject.dict;
    if (!ParseXrefStream(streamObject)) {
        return false;
    }
    sectionTrailer = std::move(dictionary);
    return true;
}

bool Document::Impl::ParseXrefTable(Lexer& lexer, Dict& sectionTrailer) {
    for (;;) {
        lexer.SkipWhitespace();
        if (lexer.AtEnd()) {
            return false;
        }

        // The table is terminated by a trailer dictionary.
        const size_t saved = lexer.Position();
        uint8_t firstCharacter = 0;
        if (lexer.Peek(firstCharacter) && firstCharacter == 't') {
            if (lexer.MatchKeyword("trailer")) {
                Object trailerObject;
                if (!lexer.ParseObject(trailerObject, 0) || !trailerObject.IsDict()) {
                    return false;
                }
                sectionTrailer = *trailerObject.dict;
                return true;
            }
            lexer.SetPosition(saved);
        }

        // A subsection header is "firstObject count", followed by count entries
        // of "offset generation n|f".  Free entries ("f") are skipped: they only
        // matter for re-using object numbers, not for reading.
        int64_t start = 0;
        int64_t count = 0;
        if (!lexer.ParseInteger(start) || !lexer.ParseInteger(count)) {
            return false;
        }
        if (count < 0 || count > 50000000) {
            return false;  // corrupt subsection, do not loop for billions of rows
        }

        for (int64_t index = 0; index < count; ++index) {
            int64_t entryOffset = 0;
            int64_t generation = 0;
            std::string type;
            if (!lexer.ParseInteger(entryOffset) || !lexer.ParseInteger(generation) ||
                !lexer.ReadToken(type)) {
                return false;
            }
            if (type == "n") {
                XrefEntry entry;
                entry.type = 1;
                entry.offset = static_cast<uint64_t>(entryOffset);
                InsertXref(static_cast<int>(start + index), entry);
            } else if (type != "f") {
                return false;
            }
        }
    }
}

bool Document::Impl::ParseXrefStream(const Object& streamObject) {
    if (!streamObject.IsStream() || !streamObject.dict) {
        return false;
    }
    const Dict& dictionary = *streamObject.dict;

    Object widthsObject = ResolveIn(dictionary, "W");
    if (!widthsObject.IsArray() || widthsObject.items.size() < 3) {
        return false;
    }
    // /W = [typeWidth field2Width field3Width] in bytes.  A zero width means
    // the field is absent and the default value applies (type 1, others 0).
    // Eight bytes is the largest width that fits the uint64 accumulator.
    int widths[3] = {0, 0, 0};
    for (int index = 0; index < 3; ++index) {
        const Object field = Resolve(widthsObject.items[index]);
        if (!field.IsNumber()) {
            return false;
        }
        const int64_t width = field.Integer();
        if (width < 0 || width > 8) {
            return false;
        }
        widths[index] = static_cast<int>(width);
    }

    const int64_t size = ResolveIn(dictionary, "Size").Integer();
    if (size <= 0) {
        return false;
    }

    std::vector<std::pair<int64_t, int64_t>> ranges;
    const Object indexObject = ResolveIn(dictionary, "Index");
    if (indexObject.IsArray() && indexObject.items.size() >= 2) {
        // /Index holds (firstObject, count) pairs: the stream may cover several
        // discontiguous ranges.  Without /Index the range is [0, /Size).
        for (size_t index = 0; index + 1 < indexObject.items.size(); index += 2) {
            const Object first = Resolve(indexObject.items[index]);
            const Object count = Resolve(indexObject.items[index + 1]);
            if (!first.IsNumber() || !count.IsNumber()) {
                return false;
            }
            ranges.emplace_back(first.Integer(), count.Integer());
        }
    } else {
        ranges.emplace_back(0, size);
    }

    if (!EnsureDecoded(streamObject)) {
        LogError(L"Cannot decode a cross-reference stream");
        return false;
    }
    const std::vector<uint8_t>& data = streamObject.streamData->decoded;
    const size_t stride = static_cast<size_t>(widths[0] + widths[1] + widths[2]);
    if (stride == 0) {
        return false;
    }

    size_t position = 0;
    // Fields are stored big-endian.  The lambda assumes `position + width` has
    // already been checked against the data size by the row loop below.
    auto readField = [&](int width) -> uint64_t {
        uint64_t value = 0;
        for (int index = 0; index < width; ++index) {
            value = (value << 8) | data[position + index];
        }
        position += static_cast<size_t>(width);
        return value;
    };

    for (const auto& range : ranges) {
        if (range.second < 0) {
            continue;
        }
        for (int64_t index = 0; index < range.second; ++index) {
            if (position + stride > data.size()) {
                // The stream is shorter than /Index claims.  Returning true
                // keeps the entries read so far: a partially usable xref beats
                // no xref at all, and GetObject still has the scan fallback.
                return true;
            }
            const uint64_t type = widths[0] == 0 ? 1 : readField(widths[0]);
            const uint64_t second = readField(widths[1]);
            const uint64_t third = readField(widths[2]);

            const int number = static_cast<int>(range.first + index);
            if (type == 1) {
                XrefEntry entry;
                entry.type = 1;
                entry.offset = second;
                InsertXref(number, entry);
            } else if (type == 2) {
                XrefEntry entry;
                entry.type = 2;
                entry.streamObject = static_cast<int>(second);
                entry.streamIndex = static_cast<int>(third);
                InsertXref(number, entry);
            }
        }
    }
    return true;
}

void Document::Impl::RebuildTrailerFromScan() {
    // Objects parsed through the broken cross-reference table may be wrong, so
    // drop the cache before probing the file again.
    cache.clear();

    // The map is unordered, so when a damaged file contains several catalog-like
    // dictionaries the winner is arbitrary.  In practice only one matches, and
    // any catalog is better than refusing to open the file.
    for (const auto& item : scanned) {
        Object object;
        if (!ParseObjectAt(static_cast<size_t>(item.second), item.first, object)) {
            continue;
        }
        if (!object.IsDict()) {
            continue;
        }
        const Object* type = object.Get("Type");
        const bool isCatalog = type != nullptr && type->IsNameValue("Catalog");
        // Some scanners drop /Type; a dictionary with /Pages and no /Parent is
        // almost certainly the catalog.
        const bool looksLikeCatalog =
            object.Get("Pages") != nullptr && object.Get("Parent") == nullptr;
        if (isCatalog || looksLikeCatalog) {
            trailer["Root"] = Object::MakeRef(item.first, 0);
            LogInfo(L"Recovered the document catalog: object %d", item.first);
            return;
        }
    }
}

bool Document::Impl::ParseObjectAt(size_t offset, int expectedNumber, Object& out) {
    if (offset >= bytes.size()) {
        return false;
    }

    Lexer lexer(bytes);
    lexer.SetPosition(offset);

    int64_t number = 0;
    int64_t generation = 0;
    if (!lexer.ParseInteger(number) || !lexer.ParseInteger(generation)) {
        return false;
    }
    if (!lexer.MatchKeyword("obj")) {
        return false;
    }
    if (expectedNumber >= 0 && number != expectedNumber) {
        return false;
    }

    Object value;
    if (!lexer.ParseObject(value, 0)) {
        return false;
    }

    if (value.IsDict() && lexer.MatchKeyword("stream")) {
        // The keyword is followed by CRLF or a single LF/CR before the data.
        // Skipping both characters unconditionally would also be accepted by
        // most readers, but this way a lone CR is handled correctly too.
        size_t dataStart = lexer.Position();
        if (dataStart < bytes.size() && bytes[dataStart] == '\r') {
            ++dataStart;
        }
        if (dataStart < bytes.size() && bytes[dataStart] == '\n') {
            ++dataStart;
        }

        size_t dataEnd = kNotSet;
        const size_t declaredLength = DetermineStreamLength(value);
        if (declaredLength != kNotSet && dataStart + declaredLength <= bytes.size()) {
            // Trust /Length only when the endstream keyword follows it.  A
            // wrong /Length is common (especially after an incremental update),
            // and silently using it would corrupt the stream data.
            size_t probe = dataStart + declaredLength;
            size_t probeLimit = probe + 4;
            while (probe < bytes.size() && IsPdfWhitespace(bytes[probe]) && probe < probeLimit) {
                ++probe;
            }
            if (probe + 9 <= bytes.size() &&
                std::memcmp(bytes.data() + probe, "endstream", 9) == 0) {
                dataEnd = dataStart + declaredLength;
            }
        }
        if (dataEnd == kNotSet) {
            // Fall back to searching for the keyword.  This can truncate data
            // that itself contains "endstream", but only broken files get here.
            dataEnd = FindEndstream(dataStart);
        }
        if (dataEnd == kNotSet) {
            return false;
        }

        // The stream dictionary stays in `dict`; the payload is shared through
        // StreamData so that decoding happens at most once per object.
        value.type = Object::Type::Stream;
        value.streamData = std::make_shared<StreamData>();
        value.streamData->raw.assign(bytes.begin() + static_cast<ptrdiff_t>(dataStart),
                                     bytes.begin() + static_cast<ptrdiff_t>(dataEnd));
    }

    out = std::move(value);
    return true;
}

size_t Document::Impl::DetermineStreamLength(const Object& dictionary) {
    // /Length is often an indirect reference (to an object defined after the
    // stream), which is why resolving it is part of reading the entry.
    const Object* lengthEntry = dictionary.Get("Length");
    if (lengthEntry == nullptr) {
        return kNotSet;
    }
    const Object length = Resolve(*lengthEntry);
    if (!length.IsNumber()) {
        return kNotSet;
    }
    const int64_t value = length.Integer();
    if (value < 0 || static_cast<uint64_t>(value) > bytes.size()) {
        return kNotSet;
    }
    return static_cast<size_t>(value);
}

size_t Document::Impl::FindEndstream(size_t start) const {
    // Linear search from the start of the payload; only used when /Length is
    // missing, wrong or points past the end of the file.
    static constexpr char kMarker[] = "endstream";
    constexpr size_t kMarkerLength = sizeof(kMarker) - 1;
    if (start >= bytes.size()) {
        return kNotSet;
    }
    for (size_t position = start; position + kMarkerLength <= bytes.size(); ++position) {
        if (std::memcmp(bytes.data() + position, kMarker, kMarkerLength) == 0) {
            // The EOL before "endstream" is a separator, not stream data.  Strip
            // CRLF or a lone CR/LF, never going below the payload start.
            size_t end = position;
            if (end > start && bytes[end - 1] == '\n') {
                --end;
            }
            if (end > start && bytes[end - 1] == '\r') {
                --end;
            }
            return end;
        }
    }
    return kNotSet;
}

const Object& Document::Impl::GetObject(int number) {
    // The returned reference points into the cache, whose elements are stable
    // across rehashing (unordered_map only invalidates iterators on rehash, not
    // references).  Callers copy the value, which shares the dictionaries.
    const auto cached = cache.find(number);
    if (cached != cache.end()) {
        return cached->second;
    }
    // A recursive reference would otherwise loop forever on corrupt files.
    if (!loading.insert(number).second) {
        return nullObject;
    }

    Object value;
    bool found = false;

    // Trust the cross-reference entry first, whichever kind it is.  Type 2
    // means the object lives inside an object stream and needs that stream to
    // be decoded before it can be parsed.
    const auto entry = xref.find(number);
    if (entry != xref.end()) {
        if (entry->second.type == 1) {
            found = ParseObjectAt(static_cast<size_t>(entry->second.offset), number, value);
        } else if (entry->second.type == 2) {
            found = LoadCompressedObject(number, entry->second, value);
        }
    }
    if (!found) {
        // The cross-reference table lied; fall back to the text scan.
        const auto fallback = scanned.find(number);
        if (fallback != scanned.end()) {
            found = ParseObjectAt(static_cast<size_t>(fallback->second), number, value);
        }
    }

    loading.erase(number);
    if (!found) {
        value = Object();
    }
    const auto inserted = cache.emplace(number, std::move(value));
    return inserted.first->second;
}

bool Document::Impl::LoadCompressedObject(int number, const XrefEntry& entry, Object& out) {
    // The container is an object stream: its decoded data starts with a header
    // of "objectNumber offset" pairs, followed by the objects themselves.
    // Resolving it recursively is safe because object streams cannot themselves
    // be stored compressed (the recursion guard in GetObject also applies).
    const Object& container = GetObject(entry.streamObject);
    if (!container.IsStream() || container.dict == nullptr) {
        return false;
    }
    if (!EnsureDecoded(container)) {
        return false;
    }

    const int64_t objectCount = ResolveIn(*container.dict, "N").Integer();
    const int64_t firstOffset = ResolveIn(*container.dict, "First").Integer();
    const std::vector<uint8_t>& data = container.streamData->decoded;
    // A container that lies about /N or /First is rejected instead of being
    // allowed to trigger a huge allocation or an out-of-bounds read.
    if (objectCount <= 0 || objectCount > 1000000 || firstOffset < 0 ||
        static_cast<size_t>(firstOffset) > data.size()) {
        return false;
    }

    // The header of an object stream is a list of (object number, offset)
    // pairs; the objects themselves start at /First.
    Lexer headerLexer(data);
    std::vector<std::pair<int, size_t>> offsets;
    offsets.reserve(static_cast<size_t>(objectCount));
    for (int64_t index = 0; index < objectCount; ++index) {
        int64_t objectNumber = 0;
        int64_t relativeOffset = 0;
        if (!headerLexer.ParseInteger(objectNumber) || !headerLexer.ParseInteger(relativeOffset)) {
            break;
        }
        offsets.emplace_back(static_cast<int>(objectNumber),
                             static_cast<size_t>(relativeOffset));
    }

    for (const auto& item : offsets) {
        if (item.first != number) {
            continue;
        }
        const size_t position = static_cast<size_t>(firstOffset) + item.second;
        if (position >= data.size()) {
            return false;
        }
        Lexer objectLexer(data);
        objectLexer.SetPosition(position);
        Object value;
        if (!objectLexer.ParseObject(value, 0)) {
            return false;
        }
        out = std::move(value);
        return true;
    }
    return false;
}

bool Document::Impl::EnsureDecoded(const Object& streamObject) {
    if (!streamObject.IsStream() || !streamObject.streamData) {
        return false;
    }
    StreamData& stream = *streamObject.streamData;
    if (stream.decodedOk) {
        return true;
    }

    // Collect the filter chain.  Both /Filter and /DecodeParms may be indirect,
    // and the abbreviated keys /F and /DP are equally valid spellings.
    std::vector<std::string> filters;
    const Object* filterEntry = streamObject.Get("Filter");
    if (filterEntry == nullptr) {
        filterEntry = streamObject.Get("F");
    }
    if (filterEntry != nullptr) {
        const Object filterValue = Resolve(*filterEntry);
        if (filterValue.IsName()) {
            filters.push_back(filterValue.text);
        } else if (filterValue.IsArray()) {
            for (const Object& item : filterValue.items) {
                const Object name = Resolve(item);
                if (name.IsName()) {
                    filters.push_back(name.text);
                }
            }
        }
    }

    std::vector<Object> parameters;
    // /DecodeParms is positional: entry i belongs to filters[i].  A short list
    // is not an error; the missing entries simply get the filter defaults.
    const Object* decodeParmsEntry = streamObject.Get("DecodeParms");
    if (decodeParmsEntry == nullptr) {
        decodeParmsEntry = streamObject.Get("DP");
    }
    if (decodeParmsEntry != nullptr) {
        const Object parms = Resolve(*decodeParmsEntry);
        if (parms.IsDict()) {
            parameters.push_back(parms);
        } else if (parms.IsArray()) {
            for (const Object& item : parms.items) {
                parameters.push_back(Resolve(item));
            }
        }
    }

    // Filters are applied in listed order; each stage writes a fresh buffer so a
    // failure half way leaves the cached stream untouched and retryable.
    std::vector<uint8_t> current = stream.raw;
    for (size_t index = 0; index < filters.size(); ++index) {
        std::vector<uint8_t> next;
        const std::string& name = filters[index];
        const Object& parms =
            (index < parameters.size()) ? parameters[index] : nullObject;

        if (name == "FlateDecode" || name == "Fl") {
            if (!Inflate(current, next)) {
                LogError(L"FlateDecode failed for a stream");
                return false;
            }
            // Predictor parameters default to "no prediction, one component,
            // eight bits, one column" as per the /DecodeParms table; entries
            // missing from the dictionary keep those defaults.
            int predictor = 1;
            int colors = 1;
            int bitsPerComponent = 8;
            int columns = 1;
            if (parms.IsDict()) {
                predictor = static_cast<int>(ResolveIn(*parms.dict, "Predictor").Integer());
                const Object colorsValue = ResolveIn(*parms.dict, "Colors");
                const Object bitsValue = ResolveIn(*parms.dict, "BitsPerComponent");
                const Object columnsValue = ResolveIn(*parms.dict, "Columns");
                if (colorsValue.IsNumber()) colors = static_cast<int>(colorsValue.Integer());
                if (bitsValue.IsNumber()) bitsPerComponent = static_cast<int>(bitsValue.Integer());
                if (columnsValue.IsNumber()) columns = static_cast<int>(columnsValue.Integer());
            }
            if (predictor > 1) {
                std::vector<uint8_t> predicted;
                if (!ApplyPredictor(next, predictor, colors, bitsPerComponent, columns,
                                    predicted)) {
                    LogError(L"Predictor %d failed for a stream", predictor);
                    return false;
                }
                next.swap(predicted);
            }
        } else if (name == "ASCIIHexDecode" || name == "AHx") {
            if (!AsciiHexDecode(current, next)) {
                return false;
            }
        } else if (name == "ASCII85Decode" || name == "A85") {
            if (!Ascii85Decode(current, next)) {
                return false;
            }
        } else if (name == "Crypt") {
            // This reader does not implement decryption.  Copying is correct for
            // the common /Identity crypt filter and is the least harmful choice
            // for encrypted files, which are reported through Metadata().
            next = current;
        } else {
            // Image and font filters are irrelevant for the document
            // structure; report the stream as unusable but do not abort.  The
            // cache stays undecoded, so a later call retries and fails the same
            // way; such streams are rare enough for that to be acceptable.
            return false;
        }
        current.swap(next);
    }

    stream.decoded = std::move(current);
    stream.decodedOk = true;
    return true;
}

Object Document::Impl::Resolve(const Object& object) {
    // Every hop returns a copy of the referenced object, so the loop is cheap
    // for the common one-hop case.  The depth cap turns a reference cycle into
    // the null object instead of an endless loop.
    Object current = object;
    for (int depth = 0; depth < 32 && current.IsRef(); ++depth) {
        current = GetObject(current.refNum);
    }
    if (current.IsRef()) {
        return Object();
    }
    return current;
}

bool Document::Impl::TryReadPageBox(const Object& node, std::string_view key,
                                    PageBox& box) {
    // Page boxes are four numbers in the order left, bottom, right, top.  Extra
    // elements (some old files append the /UserUnit scale) are ignored.
    const Object* entry = node.Get(key);
    if (entry == nullptr) {
        return false;
    }
    const Object array = Resolve(*entry);
    if (!array.IsArray() || array.items.size() < 4) {
        return false;
    }
    double values[4] = {0, 0, 0, 0};
    for (int index = 0; index < 4; ++index) {
        const Object value = Resolve(array.items[index]);
        if (!value.IsNumber()) {
            return false;
        }
        values[index] = value.Number();
    }

    // Some malformed files swap the corners; normalise before using the box.
    const double left = std::min(values[0], values[2]);
    const double right = std::max(values[0], values[2]);
    const double bottom = std::min(values[1], values[3]);
    const double top = std::max(values[1], values[3]);
    if (right - left < 1.0 || top - bottom < 1.0) {
        // Thinner than one point: treat it as absent so the caller keeps the
        // inherited box instead of producing an unusable page.
        return false;
    }
    box.left = left;
    box.bottom = bottom;
    box.right = right;
    box.top = top;
    return true;
}

void Document::Impl::BuildPageTree() {
    // The page tree root is /Root /Pages; its /Kids are either intermediate
    // nodes or page leaves.  The walk starts without inherited boxes so the
    // first /MediaBox found on the way down applies.
    const Object* rootEntry = FindIn(trailer, "Root");
    if (rootEntry == nullptr) {
        return;
    }
    const Object catalog = Resolve(*rootEntry);
    if (!catalog.IsDict()) {
        return;
    }
    const Object* pagesEntry = catalog.Get("Pages");
    if (pagesEntry == nullptr) {
        return;
    }

    // US Letter is the least surprising default when a page inherits nothing.
    const PageBox defaultBox{0.0, 0.0, 612.0, 792.0};
    WalkPageNode(*pagesEntry, 0, defaultBox, false, defaultBox, false);
}

void Document::Impl::WalkPageNode(const Object& nodeReference, int depth,
                                  PageBox inheritedMedia, bool haveMedia,
                                  PageBox inheritedCrop, bool haveCrop) {
    // Depth and page-count caps protect the UI from trees that are cyclic or
    // simply enormous; 200000 pages is far more than any real document.
    if (depth > 64 || pageSizes.size() > 200000) {
        return;
    }

    // Only indirect nodes can form a cycle, and only they get an object number.
    // Registering the number before recursing is what stops self-referencing
    // /Kids arrays from looping.
    const int objectNumber = nodeReference.IsRef() ? nodeReference.refNum : 0;
    if (objectNumber != 0 && !visitedNodes.insert(objectNumber).second) {
        return;  // cyclic page tree
    }

    const Object node = Resolve(nodeReference);
    if (!node.IsDict()) {
        return;
    }

    PageBox mediaBox = inheritedMedia;
    bool haveMediaBox = haveMedia;
    PageBox candidate;
    if (TryReadPageBox(node, "MediaBox", candidate)) {
        mediaBox = candidate;
        haveMediaBox = true;
    }

    PageBox cropBox = inheritedCrop;
    bool haveCropBox = haveCrop;
    if (TryReadPageBox(node, "CropBox", candidate)) {
        cropBox = candidate;
        haveCropBox = true;
    }

    const Object* kidsEntry = node.Get("Kids");
    if (kidsEntry != nullptr) {
        const Object kids = Resolve(*kidsEntry);
        if (kids.IsArray()) {
            // Intermediate node: pass the effective boxes down.  A node with
            // /Kids is not a page even if it also has a /MediaBox.
            for (const Object& kid : kids.items) {
                WalkPageNode(kid, depth + 1, mediaBox, haveMediaBox, cropBox,
                             haveCropBox);
            }
            return;
        }
    }

    // Leaf node: a page.  CropBox wins over MediaBox when present.
    PageBox pageBox;
    if (haveCropBox) {
        pageBox = cropBox;
    } else if (haveMediaBox) {
        pageBox = mediaBox;
    } else {
        pageBox = PageBox{};
    }

    // The specification requires CropBox to lie inside MediaBox; malformed
    // files sometimes disagree, so clamp and fall back when the result is
    // degenerate.
    if (haveMediaBox && haveCropBox) {
        pageBox.left = std::max(cropBox.left, mediaBox.left);
        pageBox.bottom = std::max(cropBox.bottom, mediaBox.bottom);
        pageBox.right = std::min(cropBox.right, mediaBox.right);
        pageBox.top = std::min(cropBox.top, mediaBox.top);
        if (pageBox.right - pageBox.left < 1.0 || pageBox.top - pageBox.bottom < 1.0) {
            pageBox = mediaBox;
        }
    }

    PageSize size;
    size.width = pageBox.right - pageBox.left;
    size.height = pageBox.top - pageBox.bottom;

    // Keep one slot per page even when the page is a direct child (number 0):
    // callers index the parallel arrays by page number.  The object-number map
    // is filled only for indirect pages because only those can be the target of
    // an outline destination.
    const int pageIndex = static_cast<int>(pageSizes.size());
    pageSizes.push_back(size);
    pageBoxes.push_back(pageBox);
    if (objectNumber > 0) {
        pageIndexByObject[objectNumber] = pageIndex;
    }
    pageObjects.push_back(objectNumber);
}

void Document::Impl::ReadMetadata() {
    // /Encrypt only raises a flag: the parser does not implement decryption, so
    // strings and streams of an encrypted file will be garbage.  Reporting the
    // flag lets the UI at least warn the user.
    metadata.encrypted = FindIn(trailer, "Encrypt") != nullptr;

    // /Info is optional; scanned files usually have none.  When it is absent
    // `parsed` stays false and the caller falls back to the file name.
    const Object* infoEntry = FindIn(trailer, "Info");
    if (infoEntry == nullptr) {
        return;
    }
    const Object info = Resolve(*infoEntry);
    if (!info.IsDict()) {
        return;
    }
    metadata.title = ReadInfoString(info, "Title");
    metadata.author = ReadInfoString(info, "Author");
    metadata.subject = ReadInfoString(info, "Subject");
    metadata.creator = ReadInfoString(info, "Creator");
    metadata.parsed = true;
}

std::wstring Document::Impl::ReadInfoString(const Object& info, std::string_view key) {
    // The value is normally a literal or hex string but may be indirect; a name
    // or number in that position is ignored rather than converted.
    const Object* entry = info.Get(key);
    if (entry == nullptr) {
        return std::wstring();
    }
    const Object value = Resolve(*entry);
    if (!value.IsString()) {
        return std::wstring();
    }
    return DecodeTextString(value.text);
}

// =============================================================================
//  Document public interface
// =============================================================================
Document::Document() : m_impl(std::make_unique<Impl>()) {}
Document::~Document() = default;

bool Document::LoadFromFile(const std::wstring& path, std::wstring* errorMessage) {
    if (!m_impl->Load(path)) {
        if (errorMessage != nullptr) {
            *errorMessage = m_impl->lastError.empty()
                                ? L"The document could not be parsed."
                                : m_impl->lastError;
        }
        return false;
    }

    // Outline extraction runs after the page tree so that destinations can be
    // translated into page indices.  It is best effort: a malformed outline
    // produces fewer entries, never a failed load.
    m_impl->outline = BuildOutline(*this);
    return true;
}

bool Document::IsLoaded() const {
    return m_impl->loaded;
}

const std::wstring& Document::Path() const {
    return m_impl->path;
}

const DocumentMetadata& Document::Metadata() const {
    return m_impl->metadata;
}

int Document::PageCount() const {
    return static_cast<int>(m_impl->pageSizes.size());
}

bool Document::GetPageSize(int pageIndex, PageSize& size) const {
    if (pageIndex < 0 || pageIndex >= static_cast<int>(m_impl->pageSizes.size())) {
        return false;
    }
    size = m_impl->pageSizes[static_cast<size_t>(pageIndex)];
    return true;
}

bool Document::GetPageBox(int pageIndex, PageBox& box) const {
    if (pageIndex < 0 || pageIndex >= static_cast<int>(m_impl->pageBoxes.size())) {
        return false;
    }
    box = m_impl->pageBoxes[static_cast<size_t>(pageIndex)];
    return true;
}

bool Document::GetPageDictionary(int pageIndex, Object& pageDictionary) {
    // The mutex is needed because resolving the page may parse it for the first
    // time and touch the shared cache.  The copy handed back shares its
    // dictionary with the cache, so it is only safe to read.
    std::lock_guard<std::mutex> lock(m_impl->objectMutex);
    if (pageIndex < 0 ||
        pageIndex >= static_cast<int>(m_impl->pageObjects.size())) {
        return false;
    }
    const int objectNumber = m_impl->pageObjects[static_cast<size_t>(pageIndex)];
    if (objectNumber <= 0) {
        return false;
    }
    const Object page = m_impl->GetObject(objectNumber);
    if (!page.IsDict()) {
        return false;
    }
    pageDictionary = page;
    return true;
}

bool Document::DecodeStream(const Object& streamOrReference,
                            std::vector<uint8_t>& bytes) {
    // Take the lock: EnsureDecoded fills the shared StreamData cache.  The copy
    // into `bytes` happens while the lock is held so the source cannot move.
    std::lock_guard<std::mutex> lock(m_impl->objectMutex);
    const Object resolved = m_impl->Resolve(streamOrReference);
    if (!resolved.IsStream() || !resolved.streamData) {
        return false;
    }
    if (!m_impl->EnsureDecoded(resolved)) {
        return false;
    }
    bytes = resolved.streamData->decoded;
    return true;
}

const std::vector<OutlineEntry>& Document::Outline() const {
    return m_impl->outline;
}

Object Document::Resolve(const Object& object) {
    // The lock serialises cache access with the text extraction worker thread.
    std::lock_guard<std::mutex> lock(m_impl->objectMutex);
    return m_impl->Resolve(object);
}

int Document::PageIndexForObject(int objectNumber) const {
    // Deliberately lock free: the map is filled by BuildPageTree during Load and
    // never modified afterwards, so concurrent readers see a stable snapshot.
    const auto it = m_impl->pageIndexByObject.find(objectNumber);
    return it == m_impl->pageIndexByObject.end() ? -1 : it->second;
}

const Object* Document::TrailerItem(std::string_view key) const {
    // The trailer is merged while loading and immutable afterwards, which is why
    // returning a raw pointer into it is safe for the document's lifetime.
    return Impl::FindIn(m_impl->trailer, key);
}

}  // namespace pdf
