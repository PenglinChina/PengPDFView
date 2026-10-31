// =============================================================================
//  PdfTextExtractor.cpp
//  Content stream interpreter for text extraction.
//
//  Only the parts of the PDF imaging model that influence text are implemented:
//
//      * the transformation matrix and its stack (q / Q / cm),
//      * the graphics state entries used by text (Tf, Tfs, Tc, Tw, Tz, TL, Ts),
//      * text objects and the text/line matrices (BT, ET, Tm, Td, TD, T*),
//      * the showing operators Tj, TJ, ' and ",
//      * form XObjects (text commonly lives inside them) and inline images
//        (which must be skipped so the binary payload is not parsed as text).
//
//  Coordinates are converted to normalised page coordinates at the very end,
//  so the interpreter never needs to know about rendering, DPI or zoom.
//
//  This is not a renderer: paths, colours, patterns and images are skipped
//  because they cannot contribute glyphs.  Ignoring everything the extractor
//  does not need keeps it small and makes it safer to run on untrusted content.
// =============================================================================
#include "Pdf/PdfTextExtractor.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Pdf/PdfFont.h"

namespace pdf {
namespace {

// Page budgets.  Together they bound the worst case per page to a few hundred
// milliseconds even for hostile content; the numbers are far above what a real
// page needs (a dense A4 page of text is well under 10000 glyphs).
constexpr int kMaxFormDepth = 4;
constexpr size_t kMaxGlyphsPerPage = 200000;
constexpr int kMaxInstructions = 4000000;
constexpr size_t kMaxContentBytes = 64ull * 1024ull * 1024ull;
// Form XObjects are often reused as headers/footers on every page.  Their
// decoded bodies are cached per extractor, with a small byte budget so a
// pathological file cannot pin an unbounded amount of memory.
constexpr size_t kMaxFormCacheBytes = 32ull * 1024ull * 1024ull;
constexpr size_t kMaxFormCacheEntryBytes = 4ull * 1024ull * 1024ull;

// -----------------------------------------------------------------------------
//  Tiny 2D affine matrix helpers (PDF uses the [a b c d e f] layout).
//
//  PDF applies matrices to row vectors: [x y 1] * M, which makes the translation
//  live in (e, f) and composition happen left to right.  Multiply(left, right)
//  therefore returns the matrix that applies `left` first.
// -----------------------------------------------------------------------------
struct Matrix {
    double a = 1.0;
    double b = 0.0;
    double c = 0.0;
    double d = 1.0;
    double e = 0.0;
    double f = 0.0;
};

struct Point {
    double x = 0.0;
    double y = 0.0;
};

struct Vector {
    double x = 0.0;
    double y = 0.0;
};

Matrix IdentityMatrix() {
    return Matrix{};
}

Matrix Multiply(const Matrix& left, const Matrix& right) {
    // Applies `left` first and `right` second, which matches PDF's row vector
    // matrix algebra: p' = p x left x right.
    Matrix result;
    result.a = left.a * right.a + left.b * right.c;
    result.b = left.a * right.b + left.b * right.d;
    result.c = left.c * right.a + left.d * right.c;
    result.d = left.c * right.b + left.d * right.d;
    result.e = left.e * right.a + left.f * right.c + right.e;
    result.f = left.e * right.b + left.f * right.d + right.f;
    return result;
}

Matrix Translation(double x, double y) {
    Matrix matrix;
    matrix.e = x;
    matrix.f = y;
    return matrix;
}

// Maps a point through the matrix, including its translation.
Point Apply(const Matrix& matrix, double x, double y) {
    Point point;
    point.x = matrix.a * x + matrix.c * y + matrix.e;
    point.y = matrix.b * x + matrix.d * y + matrix.f;
    return point;
}

// Maps a direction.  The translation part is skipped: it would be meaningless
// for the advance and up vectors computed from it.
Vector TransformVector(const Matrix& matrix, double x, double y) {
    Vector vector;
    vector.x = matrix.a * x + matrix.c * y;
    vector.y = matrix.b * x + matrix.d * y;
    return vector;
}

// -----------------------------------------------------------------------------
//  Byte classification used by the content stream tokenizer.
// -----------------------------------------------------------------------------
bool IsWhiteByte(uint8_t character) {
    return character == 0 || character == 9 || character == 10 ||
           character == 12 || character == 13 || character == 32;
}

bool IsDelimiterByte(uint8_t character) {
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

bool IsRegularByte(uint8_t character) {
    return !IsWhiteByte(character) && !IsDelimiterByte(character);
}

int HexValue(uint8_t character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

// -----------------------------------------------------------------------------
//  Content stream tokenizer
//
//  Unlike the document lexer this one intentionally treats bare words as
//  operators, handles inline image payloads and skips dictionaries because the
//  interpreter never needs their contents.
//
//  Arrays are reported as ArrayBegin/ArrayEnd instead of being parsed here: the
//  interpreter builds the operand array itself, which keeps the tokenizer free
//  of recursion.
// -----------------------------------------------------------------------------
class ContentTokenizer {
public:
    enum class Kind {
        End,
        Number,
        String,
        Name,
        Operator,
        ArrayBegin,
        ArrayEnd,
        Dictionary,
    };

    struct Token {
        Kind kind = Kind::End;
        double number = 0.0;
        std::string text;
    };

    explicit ContentTokenizer(const std::vector<uint8_t>& data) : m_data(data) {}

    bool Next(Token& token) {
        SkipWhitespace();
        if (m_position >= m_data.size()) {
            token.kind = Kind::End;
            return false;
        }

        const uint8_t character = m_data[m_position];
        if (character == '[') {
            ++m_position;
            token.kind = Kind::ArrayBegin;
            return true;
        }
        if (character == ']') {
            ++m_position;
            token.kind = Kind::ArrayEnd;
            return true;
        }
        if (character == '<') {
            if (m_position + 1 < m_data.size() && m_data[m_position + 1] == '<') {
                SkipDictionary();
                token.kind = Kind::Dictionary;
                return true;
            }
            return ReadHexString(token);
        }
        if (character == '(') {
            return ReadLiteralString(token);
        }
        if (character == '/') {
            return ReadName(token);
        }
        if (character == '+' || character == '-' || character == '.' ||
            (character >= '0' && character <= '9')) {
            return ReadNumber(token);
        }

        // Everything else is an operator (Tj, TJ, ', ", T*, Do, BI, ...).
        token.kind = Kind::Operator;
        token.text.clear();
        while (m_position < m_data.size() && IsRegularByte(m_data[m_position])) {
            token.text.push_back(static_cast<char>(m_data[m_position]));
            ++m_position;
        }
        if (token.text.empty()) {
            // A stray delimiter: skip one byte and try again.  Recursion is
            // bounded by the input size because progress is always made.
            ++m_position;
            return Next(token);
        }
        return true;
    }

    // Consumes the binary payload of an inline image.  Called after the BI
    // operator: the dictionary that follows is skipped up to ID, then the data
    // is skipped up to a delimited EI keyword.
    //
    // The image data is arbitrary binary and can contain the bytes "EI", so the
    // keyword only counts when it is surrounded by whitespace or delimiters.
    // That is the heuristic the specification recommends, and getting it wrong
    // would either swallow the rest of the page or emit image bytes as text.
    void SkipInlineImage() {
        size_t position = m_position;
        // Find the ID keyword that starts the binary data.
        while (position + 1 < m_data.size()) {
            if (m_data[position] == 'I' && m_data[position + 1] == 'D' &&
                (position == 0 || IsWhiteByte(m_data[position - 1])) &&
                (position + 2 >= m_data.size() ||
                 IsWhiteByte(m_data[position + 2]) ||
                 IsDelimiterByte(m_data[position + 2]))) {
                position += 2;
                break;
            }
            ++position;
        }
        if (position >= m_data.size()) {
            m_position = m_data.size();
            return;
        }
        // A single whitespace character separates ID from the payload.
        if (position < m_data.size() && IsWhiteByte(m_data[position])) {
            ++position;
        }
        while (position + 1 < m_data.size()) {
            if (m_data[position] == 'E' && m_data[position + 1] == 'I' &&
                (position == 0 || IsWhiteByte(m_data[position - 1])) &&
                (position + 2 >= m_data.size() ||
                 IsWhiteByte(m_data[position + 2]) ||
                 IsDelimiterByte(m_data[position + 2]))) {
                m_position = position + 2;
                return;
            }
            ++position;
        }
        m_position = m_data.size();
    }

private:
    void SkipWhitespace() {
        while (m_position < m_data.size()) {
            const uint8_t character = m_data[m_position];
            if (IsWhiteByte(character)) {
                ++m_position;
            } else if (character == '%') {
                while (m_position < m_data.size() && m_data[m_position] != '\n' &&
                       m_data[m_position] != '\r') {
                    ++m_position;
                }
            } else {
                break;
            }
        }
    }

    bool ReadNumber(Token& token) {
        // Content stream numbers have no exponent form and may start with '.',
        // so the scan is deliberately loose and strtod does the conversion.
        // The caller already guaranteed the first byte is one of "+-.0-9".
        const size_t start = m_position;
        if (m_position < m_data.size() &&
            (m_data[m_position] == '+' || m_data[m_position] == '-')) {
            ++m_position;
        }
        while (m_position < m_data.size() &&
               ((m_data[m_position] >= '0' && m_data[m_position] <= '9') ||
                m_data[m_position] == '.')) {
            ++m_position;
        }
        if (m_position == start) {
            // A lone sign or '.', not part of a number: skip it rather than
            // stalling on the same byte forever.
            ++m_position;
            return Next(token);
        }
        const std::string numberText(
            reinterpret_cast<const char*>(m_data.data() + start), m_position - start);
        token.kind = Kind::Number;
        token.number = std::strtod(numberText.c_str(), nullptr);
        return true;
    }

    bool ReadName(Token& token) {
        ++m_position;  // '/'
        token.kind = Kind::Name;
        token.text.clear();
        // Resource names are matched against the dictionary keys in /Resources,
        // so "#xx" escapes must be decoded here exactly as in the document lexer.
        while (m_position < m_data.size() && IsRegularByte(m_data[m_position])) {
            const uint8_t character = m_data[m_position];
            if (character == '#' && m_position + 2 < m_data.size()) {
                const int high = HexValue(m_data[m_position + 1]);
                const int low = HexValue(m_data[m_position + 2]);
                if (high >= 0 && low >= 0) {
                    token.text.push_back(static_cast<char>((high << 4) | low));
                    m_position += 3;
                    continue;
                }
            }
            token.text.push_back(static_cast<char>(character));
            ++m_position;
        }
        return true;
    }

    bool ReadHexString(Token& token) {
        ++m_position;  // '<'
        token.kind = Kind::String;
        token.text.clear();
        // Hex strings ignore whitespace and pad an odd digit count; the result is
        // the raw bytes that will be fed to the font decoder.
        int highNibble = -1;
        while (m_position < m_data.size() && m_data[m_position] != '>') {
            const int value = HexValue(m_data[m_position]);
            ++m_position;
            if (value < 0) {
                continue;
            }
            if (highNibble < 0) {
                highNibble = value;
            } else {
                token.text.push_back(static_cast<char>((highNibble << 4) | value));
                highNibble = -1;
            }
        }
        if (m_position < m_data.size()) {
            ++m_position;
        }
        if (highNibble >= 0) {
            token.text.push_back(static_cast<char>(highNibble << 4));
        }
        return true;
    }

    bool ReadLiteralString(Token& token) {
        ++m_position;  // '('
        token.kind = Kind::String;
        token.text.clear();
        // Balanced parentheses; the nesting counter keeps "(a (b) c)" whole.
        int nesting = 1;
        while (m_position < m_data.size()) {
            const uint8_t character = m_data[m_position++];
            if (character == '\\') {
                if (m_position >= m_data.size()) {
                    break;
                }
                const uint8_t escaped = m_data[m_position++];
                switch (escaped) {
                    case 'n': token.text.push_back('\n'); break;
                    case 'r': token.text.push_back('\r'); break;
                    case 't': token.text.push_back('\t'); break;
                    case 'b': token.text.push_back('\b'); break;
                    case 'f': token.text.push_back('\f'); break;
                    case '(': token.text.push_back('('); break;
                    case ')': token.text.push_back(')'); break;
                    case '\\': token.text.push_back('\\'); break;
                    case '\r':
                        if (m_position < m_data.size() && m_data[m_position] == '\n') {
                            ++m_position;
                        }
                        break;
                    case '\n':
                        break;
                    default:
                        if (escaped >= '0' && escaped <= '7') {
                            int octal = escaped - '0';
                            for (int index = 0; index < 2 &&
                                                m_position < m_data.size() &&
                                                m_data[m_position] >= '0' &&
                                                m_data[m_position] <= '7';
                                 ++index) {
                                octal = octal * 8 + (m_data[m_position] - '0');
                                ++m_position;
                            }
                            token.text.push_back(static_cast<char>(octal & 0xFF));
                        } else {
                            token.text.push_back(static_cast<char>(escaped));
                        }
                        break;
                }
                continue;
            }
            if (character == '(') {
                ++nesting;
                token.text.push_back('(');
                continue;
            }
            if (character == ')') {
                --nesting;
                if (nesting == 0) {
                    break;
                }
                token.text.push_back(')');
                continue;
            }
            token.text.push_back(static_cast<char>(character));
        }
        return true;
    }

    // Skips a complete "<< ... >>" dictionary, honouring nested dictionaries
    // and strings so that braces inside a string do not terminate it early.
    void SkipDictionary() {
        int depth = 0;
        while (m_position < m_data.size()) {
            const uint8_t character = m_data[m_position];
            if (character == '(') {
                SkipLiteralStringRaw();
                continue;
            }
            if (character == '<' && m_position + 1 < m_data.size() &&
                m_data[m_position + 1] == '<') {
                ++depth;
                m_position += 2;
                continue;
            }
            if (character == '>' && m_position + 1 < m_data.size() &&
                m_data[m_position + 1] == '>') {
                --depth;
                m_position += 2;
                if (depth <= 0) {
                    return;
                }
                continue;
            }
            if (character == '<') {
                ++m_position;
                while (m_position < m_data.size() && m_data[m_position] != '>') {
                    ++m_position;
                }
                if (m_position < m_data.size()) {
                    ++m_position;
                }
                continue;
            }
            ++m_position;
        }
    }

    // Skips a literal string without decoding it.  Used while skipping a
    // dictionary, where only the extent of the string matters, not its bytes.
    void SkipLiteralStringRaw() {
        ++m_position;  // '('
        int nesting = 1;
        while (m_position < m_data.size()) {
            const uint8_t character = m_data[m_position++];
            if (character == '\\') {
                if (m_position < m_data.size()) {
                    ++m_position;
                }
                continue;
            }
            if (character == '(') {
                ++nesting;
            } else if (character == ')') {
                --nesting;
                if (nesting == 0) {
                    return;
                }
            }
        }
    }

    const std::vector<uint8_t>& m_data;
    size_t m_position = 0;
};

// -----------------------------------------------------------------------------
//  Operand stack used by the interpreter.
//
//  Operands are kept as a single tagged struct instead of a variant because the
//  text operators only ever index them positionally.  Dictionaries are pushed as
//  "Other" rather than dropped so that operands after them keep their index.
// -----------------------------------------------------------------------------
struct Operand {
    enum class Kind { Number, String, Name, Array, Other };

    Kind kind = Kind::Other;
    double number = 0.0;
    std::string text;
    std::vector<Operand> items;

    static Operand FromNumber(double value) {
        Operand operand;
        operand.kind = Kind::Number;
        operand.number = value;
        return operand;
    }
    static Operand FromString(std::string value) {
        Operand operand;
        operand.kind = Kind::String;
        operand.text = std::move(value);
        return operand;
    }
    static Operand FromName(std::string value) {
        Operand operand;
        operand.kind = Kind::Name;
        operand.text = std::move(value);
        return operand;
    }
};

// Resolves a dictionary entry, returning a null object when absent.  Streams are
// accepted as well because form XObjects are streams that also carry a
// dictionary (their /Resources, /Matrix and /Subtype live there).
Object ResolveIn(Document& document, const Object& dictionary, const char* key) {
    if (!dictionary.IsDict() && !dictionary.IsStream()) {
        return Object();
    }
    const Object* value = dictionary.Get(key);
    return value == nullptr ? Object() : document.Resolve(*value);
}

// Reads a numeric operand, falling back to the operator's default when the
// operand is missing or has the wrong type (some malformed streams omit it).
double NumberOperand(const std::vector<Operand>& operands, size_t index,
                     double fallback) {
    if (index >= operands.size() || operands[index].kind != Operand::Kind::Number) {
        return fallback;
    }
    return operands[index].number;
}

const Operand* OperandAt(const std::vector<Operand>& operands, size_t index) {
    return index < operands.size() ? &operands[index] : nullptr;
}

// Reads an inheritable page attribute (/Resources, /Rotate ...).
// Inheritance is implemented by walking /Parent: a page that omits /Resources
// uses the one of its nearest ancestor page-tree node.
Object FindInherited(Document& document, const Object& node, const char* key) {
    Object current = node;
    for (int depth = 0; depth < 16; ++depth) {
        if (!current.IsDict()) {
            break;
        }
        const Object* value = current.Get(key);
        if (value != nullptr) {
            return document.Resolve(*value);
        }
        const Object* parent = current.Get("Parent");
        if (parent == nullptr) {
            break;
        }
        current = document.Resolve(*parent);
    }
    return Object();
}

Matrix ReadMatrix(Document& document, const Object* entry, const Matrix& fallback) {
    // A missing or malformed matrix falls back to the identity: a broken form
    // XObject should still contribute its text at the default position.
    if (entry == nullptr) {
        return fallback;
    }
    const Object array = document.Resolve(*entry);
    if (!array.IsArray() || array.items.size() < 6) {
        return fallback;
    }
    double values[6] = {1.0, 0.0, 0.0, 1.0, 0.0, 0.0};
    for (int index = 0; index < 6; ++index) {
        const Object value = document.Resolve(array.items[static_cast<size_t>(index)]);
        if (!value.IsNumber()) {
            return fallback;
        }
        values[index] = value.Number();
    }
    Matrix matrix;
    matrix.a = values[0];
    matrix.b = values[1];
    matrix.c = values[2];
    matrix.d = values[3];
    matrix.e = values[4];
    matrix.f = values[5];
    return matrix;
}

}  // namespace

// =============================================================================
//  Extractor implementation
// =============================================================================
struct TextExtractor::Impl {
    explicit Impl(Document& source) : document(source) {}

    Document& document;

    // Fonts are reused across pages; the dictionary pointer is stable because
    // the document keeps every parsed object alive in its cache and a
    // dictionary is owned by the object that contains it.  The cache is per
    // extractor, so it is also a convenient place to keep decoded CMaps warm
    // while paging through a document.
    std::unordered_map<const Dict*, std::shared_ptr<FontDecoder>> fontCache;

    // Decoded Form XObject content, keyed by the stream dictionary pointer.
    // DecodeStream caches the decoded bytes internally but returns a copy, so
    // this cache avoids both the copy and repeated flate decoding when a form
    // (typically a header or footer) is used by many pages.
    std::unordered_map<const Dict*, std::shared_ptr<const std::vector<uint8_t>>>
        formContentCache;
    size_t formContentBytes = 0;

    // Per-page state, reset by ExtractPage.  `aborted` is sticky for the rest of
    // the page once a budget runs out.
    PageBox pageBox;
    int rotation = 0;
    int instructionBudget = 0;
    size_t contentBudget = 0;
    bool aborted = false;

    // Resolves a font resource name to a decoder, creating it on first use.
    std::shared_ptr<FontDecoder> ResolveFont(const Object& resources,
                                             const std::string& name) {
        const Object fonts = ResolveIn(document, resources, "Font");
        if (!fonts.IsDict()) {
            return nullptr;
        }
        const Object font = ResolveIn(document, fonts, name.c_str());
        if (!font.IsDict()) {
            return nullptr;
        }
        const Dict* key = font.dict.get();
        const auto found = fontCache.find(key);
        if (found != fontCache.end()) {
            return found->second;
        }
        auto decoder = std::make_shared<FontDecoder>();
        decoder->Load(document, font);
        fontCache.emplace(key, decoder);
        return decoder;
    }

    // Converts PDF page coordinates to normalised display coordinates.  The
    // page rotation is applied here so every consumer sees an upright page.
    //
    // u/v are page fractions with the origin at the bottom left; the unrotated
    // mapping flips v because display coordinates grow downwards.  The rotated
    // cases simply permute u/v, which is what a quarter turn does to the axes.
    void MapToDisplay(double x, double y, float& outX, float& outY) const {
        const double width = pageBox.right - pageBox.left;
        const double height = pageBox.top - pageBox.bottom;
        const double u = width > 0.0 ? (x - pageBox.left) / width : 0.0;
        const double v = height > 0.0 ? (y - pageBox.bottom) / height : 0.0;

        double displayX = u;
        double displayY = 1.0 - v;
        switch (rotation) {
            case 90:
                displayX = v;
                displayY = u;
                break;
            case 180:
                displayX = 1.0 - u;
                displayY = v;
                break;
            case 270:
                displayX = 1.0 - v;
                displayY = 1.0 - u;
                break;
            default:
                break;
        }
        // Clamp to a generous margin around the page: glyphs that stick out are
        // still selectable, but a broken matrix cannot produce huge coordinates
        // that would overflow float math in the UI.
        outX = static_cast<float>(std::clamp(displayX, -4.0, 5.0));
        outY = static_cast<float>(std::clamp(displayY, -4.0, 5.0));
    }
};

// The interpreter walks one content stream.  It owns the graphics/text state
// and writes glyphs into the page being extracted.
class TextInterpreter {
public:
    TextInterpreter(TextExtractor::Impl& impl, TextPage& page)
        : m_impl(impl), m_page(page) {}

    void Run(const std::vector<uint8_t>& content, const Object& resources,
             const Matrix& ctm, int depth) {
        // Depth is the form XObject nesting level; when it is exhausted the
        // remaining text is silently dropped rather than interpreted incorrectly.
        if (depth > kMaxFormDepth || m_impl.aborted) {
            return;
        }

        // Forms inherit the graphics state; save it so the parent stream is
        // unaffected when this call returns.  The text and line matrices are
        // saved separately because they are interpreter state, not PDF state.
        const State savedState = m_state;
        const std::vector<State> savedStack = m_stack;
        const Object savedResources = m_resources;
        const Matrix savedText = m_textMatrix;
        const Matrix savedLine = m_lineMatrix;
        const bool savedInText = m_inText;
        const int savedDepth = m_depth;

        m_state.ctm = ctm;
        m_resources = resources;
        m_inText = false;
        m_depth = depth;

        ContentTokenizer tokenizer(content);
        std::vector<Operand> operands;
        std::vector<std::vector<Operand>> stacks;
        ContentTokenizer::Token token;

        while (tokenizer.Next(token)) {
            // One token, one instruction: the budget therefore limits total work
            // across the page including nested form XObjects.
            if (--m_impl.instructionBudget <= 0) {
                m_impl.aborted = true;
                break;
            }
            switch (token.kind) {
                case ContentTokenizer::Kind::Number:
                    operands.push_back(Operand::FromNumber(token.number));
                    break;
                case ContentTokenizer::Kind::String:
                    operands.push_back(Operand::FromString(std::move(token.text)));
                    break;
                case ContentTokenizer::Kind::Name:
                    operands.push_back(Operand::FromName(std::move(token.text)));
                    break;
                case ContentTokenizer::Kind::ArrayBegin:
                    // Arrays can nest (a TJ array contains strings and numbers
                    // only, but the grammar allows more): keep the outer operand
                    // list on a stack of its own.
                    stacks.push_back(std::move(operands));
                    operands.clear();
                    break;
                case ContentTokenizer::Kind::ArrayEnd:
                    if (!stacks.empty()) {
                        Operand array;
                        array.kind = Operand::Kind::Array;
                        array.items = std::move(operands);
                        operands = std::move(stacks.back());
                        stacks.pop_back();
                        operands.push_back(std::move(array));
                    }
                    break;
                case ContentTokenizer::Kind::Dictionary:
                    // No text operator needs dictionary contents, but the operand
                    // slot is kept so positional operands stay in place.
                    operands.push_back(Operand());
                    break;
                case ContentTokenizer::Kind::Operator:
                    Execute(token.text, operands, tokenizer);
                    operands.clear();
                    break;
                case ContentTokenizer::Kind::End:
                    break;
            }
        }

        m_state = savedState;
        m_stack = savedStack;
        m_resources = savedResources;
        m_textMatrix = savedText;
        m_lineMatrix = savedLine;
        m_inText = savedInText;
        m_depth = savedDepth;
    }

private:
    // The subset of the PDF graphics state that affects text placement.  The
    // values are exactly those saved by "q" and restored by "Q".
    struct State {
        Matrix ctm;
        std::shared_ptr<FontDecoder> font;
        double fontSize = 0.0;
        double charSpacing = 0.0;
        double wordSpacing = 0.0;
        double horizontalScale = 100.0;
        double leading = 0.0;
        double rise = 0.0;
        int renderMode = 0;
    };

    void Execute(const std::string& op, const std::vector<Operand>& operands,
                 ContentTokenizer& tokenizer) {
        // Text placement operators are handled as a flat chain of comparisons:
        // the set is small and a map lookup would be slower than these compares.
        if (op == "q") {
            // An unmatched Q is ignored rather than popping an empty stack, so a
            // malformed stream cannot unbalance the interpreter state.
            m_stack.push_back(m_state);
        } else if (op == "Q") {
            if (!m_stack.empty()) {
                m_state = m_stack.back();
                m_stack.pop_back();
            }
        } else if (op == "cm") {
            if (operands.size() >= 6) {
                Matrix matrix;
                matrix.a = NumberOperand(operands, 0, 1.0);
                matrix.b = NumberOperand(operands, 1, 0.0);
                matrix.c = NumberOperand(operands, 2, 0.0);
                matrix.d = NumberOperand(operands, 3, 1.0);
                matrix.e = NumberOperand(operands, 4, 0.0);
                matrix.f = NumberOperand(operands, 5, 0.0);
                // New CTM = cm x CTM: the new transform is applied before the
                // existing one, matching the row-vector convention.
                m_state.ctm = Multiply(matrix, m_state.ctm);
            }
        } else if (op == "BT") {
            // BT resets both text matrices, so the text starts at the origin of
            // the current coordinate system.
            m_textMatrix = IdentityMatrix();
            m_lineMatrix = IdentityMatrix();
            m_inText = true;
        } else if (op == "ET") {
            m_inText = false;
        } else if (op == "Tf") {
            // The size is stored as given; all later advances multiply it by the
            // text matrices, so a missing size simply collapses the glyphs.
            const Operand* name = OperandAt(operands, 0);
            if (name != nullptr && name->kind == Operand::Kind::Name) {
                m_state.font = m_impl.ResolveFont(m_resources, name->text);
                m_state.fontSize = NumberOperand(operands, 1, 0.0);
            }
        } else if (op == "Tm") {
            Matrix matrix;
            matrix.a = NumberOperand(operands, 0, 1.0);
            matrix.b = NumberOperand(operands, 1, 0.0);
            matrix.c = NumberOperand(operands, 2, 0.0);
            matrix.d = NumberOperand(operands, 3, 1.0);
            matrix.e = NumberOperand(operands, 4, 0.0);
            matrix.f = NumberOperand(operands, 5, 0.0);
            m_textMatrix = matrix;
            m_lineMatrix = matrix;
        } else if (op == "Td") {
            MoveText(NumberOperand(operands, 0, 0.0),
                     NumberOperand(operands, 1, 0.0));
        } else if (op == "TD") {
            const double ty = NumberOperand(operands, 1, 0.0);
            m_state.leading = -ty;
            MoveText(NumberOperand(operands, 0, 0.0), ty);
        } else if (op == "T*") {
            MoveText(0.0, -m_state.leading);
        } else if (op == "TL") {
            m_state.leading = NumberOperand(operands, 0, 0.0);
        } else if (op == "Tc") {
            m_state.charSpacing = NumberOperand(operands, 0, 0.0);
        } else if (op == "Tw") {
            m_state.wordSpacing = NumberOperand(operands, 0, 0.0);
        } else if (op == "Tz") {
            m_state.horizontalScale = NumberOperand(operands, 0, 100.0);
        } else if (op == "Ts") {
            m_state.rise = NumberOperand(operands, 0, 0.0);
        } else if (op == "Tr") {
            m_state.renderMode = static_cast<int>(NumberOperand(operands, 0, 0.0));
        } else if (op == "Tj") {
            const Operand* text = OperandAt(operands, 0);
            if (text != nullptr && text->kind == Operand::Kind::String) {
                ShowBytes(text->text);
            }
        } else if (op == "TJ") {
            const Operand* array = OperandAt(operands, 0);
            if (array != nullptr && array->kind == Operand::Kind::Array) {
                for (const Operand& item : array->items) {
                    if (item.kind == Operand::Kind::String) {
                        ShowBytes(item.text);
                    } else if (item.kind == Operand::Kind::Number) {
                        AdjustText(item.number);
                    }
                }
            }
        } else if (op == "'") {
            MoveText(0.0, -m_state.leading);
            const Operand* text = OperandAt(operands, 0);
            if (text != nullptr && text->kind == Operand::Kind::String) {
                ShowBytes(text->text);
            }
        } else if (op == "\"") {
            m_state.wordSpacing = NumberOperand(operands, 0, m_state.wordSpacing);
            m_state.charSpacing = NumberOperand(operands, 1, m_state.charSpacing);
            MoveText(0.0, -m_state.leading);
            const Operand* text = OperandAt(operands, 2);
            if (text != nullptr && text->kind == Operand::Kind::String) {
                ShowBytes(text->text);
            }
        } else if (op == "Do") {
            const Operand* name = OperandAt(operands, 0);
            if (name != nullptr && name->kind == Operand::Kind::Name) {
                DoXObject(name->text);
            }
        } else if (op == "BI") {
            // Inline image: the payload is binary and must never be tokenised.
            tokenizer.SkipInlineImage();
        }
        // All remaining operators do not influence text extraction.
    }

    // Td moves the line matrix and makes the text matrix follow it; the two are
    // kept separate because TJ/Tj only change the text matrix.
    void MoveText(double tx, double ty) {
        m_lineMatrix = Multiply(Translation(tx, ty), m_lineMatrix);
        m_textMatrix = m_lineMatrix;
    }

    // TJ kerning values shift the text position without producing a glyph.
    void AdjustText(double thousandths) {
        if (m_state.font != nullptr && m_state.font->IsVertical()) {
            const double dy = -thousandths / 1000.0 * m_state.fontSize;
            m_textMatrix = Multiply(Translation(0.0, dy), m_textMatrix);
        } else {
            const double dx = -thousandths / 1000.0 * m_state.fontSize *
                              (m_state.horizontalScale / 100.0);
            m_textMatrix = Multiply(Translation(dx, 0.0), m_textMatrix);
        }
    }

    void ShowBytes(const std::string& bytes) {
        if (m_state.font == nullptr || bytes.empty()) {
            return;
        }
        const int codeBytes = std::max(1, m_state.font->CodeByteCount());
        size_t index = 0;
        while (index + static_cast<size_t>(codeBytes) <= bytes.size()) {
            uint32_t code = 0;
            for (int part = 0; part < codeBytes; ++part) {
                code = (code << 8) |
                       static_cast<unsigned char>(bytes[index + static_cast<size_t>(part)]);
            }
            index += static_cast<size_t>(codeBytes);

            std::wstring unicode;
            double widthEm = 0.5;
            m_state.font->Decode(code, unicode, widthEm);

            if (!unicode.empty()) {
                EmitGlyph(unicode, widthEm);
            }
            AdvanceText(widthEm, codeBytes == 1 && code == 32);
        }
    }

    void EmitGlyph(const std::wstring& unicode, double widthEm) {
        // Hard cap: a page of micro-text can otherwise produce millions of glyph
        // entries and make selection unusable.  Exceeding it aborts the page.
        if (m_page.glyphs.size() >= kMaxGlyphsPerPage) {
            m_impl.aborted = true;
            return;
        }

        // Text rendering matrix: font size, horizontal scaling and rise applied
        // on top of the text matrix and the current transformation matrix.
        const double horizontalScale = m_state.horizontalScale / 100.0;
        const Matrix textToDevice = Multiply(m_textMatrix, m_state.ctm);
        const Matrix rendering =
            Multiply(Matrix{m_state.fontSize * horizontalScale, 0.0, 0.0,
                            m_state.fontSize, 0.0, m_state.rise},
                     textToDevice);

        const Point origin = Apply(rendering, 0.0, 0.0);
        Vector advance =
            TransformVector(textToDevice, widthEm * m_state.fontSize * horizontalScale, 0.0);
        Vector up = TransformVector(textToDevice, 0.0, m_state.fontSize);

        if (m_state.font != nullptr && m_state.font->IsVertical()) {
            advance = TransformVector(textToDevice, 0.0, -widthEm * m_state.fontSize);
            up = TransformVector(textToDevice, m_state.fontSize, 0.0);
        }

        // A degenerate font size would collapse the selection rectangles.
        if (std::fabs(advance.x) + std::fabs(advance.y) < 1.0e-9) {
            advance.x = 1.0e-6;
        }
        if (std::fabs(up.x) + std::fabs(up.y) < 1.0e-9) {
            up.y = 1.0e-6;
        }

        // A single code may map to several UTF-16 units (a supplementary
        // character or a ligature).  The advance is split evenly: without the
        // real font metrics that is the best estimate, and it keeps every unit
        // individually selectable.
        const size_t count = std::max<size_t>(1, unicode.size());
        for (size_t index = 0; index < count; ++index) {
            const double startFraction = static_cast<double>(index) / count;
            const double endFraction = static_cast<double>(index + 1) / count;

            const Point start{origin.x + advance.x * startFraction,
                              origin.y + advance.y * startFraction};
            const Point end{origin.x + advance.x * endFraction,
                            origin.y + advance.y * endFraction};
            const Point corners[4] = {
                start,
                end,
                Point{end.x + up.x, end.y + up.y},
                Point{start.x + up.x, start.y + up.y},
            };

            // Rotation and skew turn the glyph quad into a rotated rectangle, so
            // the axis aligned bounding box of its four corners is the closest
            // representation the UI can use for hit testing.
            float minX = 1.0e9f;
            float minY = 1.0e9f;
            float maxX = -1.0e9f;
            float maxY = -1.0e9f;
            for (const Point& corner : corners) {
                float x = 0.0f;
                float y = 0.0f;
                m_impl.MapToDisplay(corner.x, corner.y, x, y);
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }

            TextGlyph glyph;
            glyph.character = unicode[std::min(index, unicode.size() - 1)];
            glyph.bounds.left = minX;
            glyph.bounds.top = minY;
            glyph.bounds.right = maxX;
            glyph.bounds.bottom = maxY;
            glyph.fontSize = std::max(0.0005f, maxY - minY);
            m_page.glyphs.push_back(glyph);
        }
    }

    void AdvanceText(double widthEm, bool isWordSpace) {
        // The advance is (glyph width + character spacing [+ word spacing when
        // the code is a single-byte space]) scaled by the font size and then by
        // Th.  Word spacing only applies to single byte code 32 by definition.
        const double spacing =
            m_state.charSpacing + (isWordSpace ? m_state.wordSpacing : 0.0);
        if (m_state.font != nullptr && m_state.font->IsVertical()) {
            const double dy = -(widthEm * m_state.fontSize + spacing);
            m_textMatrix = Multiply(Translation(0.0, dy), m_textMatrix);
        } else {
            const double dx = (widthEm * m_state.fontSize + spacing) *
                              (m_state.horizontalScale / 100.0);
            m_textMatrix = Multiply(Translation(dx, 0.0), m_textMatrix);
        }
    }

    // Executes /XObject /Form resources, which often contain headers, footers
    // and table cells.
    void DoXObject(const std::string& name) {
        if (m_depth >= kMaxFormDepth || m_impl.aborted) {
            return;
        }
        const Object xobjects = ResolveIn(m_impl.document, m_resources, "XObject");
        if (!xobjects.IsDict()) {
            return;
        }
        const Object xobject = ResolveIn(m_impl.document, xobjects, name.c_str());
        if (!xobject.IsStream() || !xobject.dict) {
            return;
        }
        const Object subtype = ResolveIn(m_impl.document, xobject, "Subtype");
        if (!subtype.IsName() || subtype.text != "Form") {
            return;  // image XObjects never contain text
        }

        // /Matrix maps the form coordinate system into the current one, so it is
        // applied before the existing CTM (left multiplication).
        const Object* matrixEntry = xobject.Get("Matrix");
        const Matrix formMatrix = ReadMatrix(m_impl.document, matrixEntry,
                                             IdentityMatrix());

        // A form may carry its own resources; otherwise it uses the resources of
        // the stream that invoked it.
        Object formResources = ResolveIn(m_impl.document, xobject, "Resources");
        if (!formResources.IsDict()) {
            formResources = m_resources;
        }

        // Decode the body once; the cache is shared across pages because the
        // extractor itself lives for the whole document.  A form invoked many
        // times is still charged against the page budget on every use so a
        // self-referential file cannot run forever.
        const Dict* formKey = xobject.dict.get();
        std::shared_ptr<const std::vector<uint8_t>> content;
        const auto cached = m_impl.formContentCache.find(formKey);
        if (cached != m_impl.formContentCache.end()) {
            content = cached->second;
        } else {
            auto decoded = std::make_shared<std::vector<uint8_t>>();
            if (!m_impl.document.DecodeStream(xobject, *decoded) || decoded->empty()) {
                return;
            }
            content = decoded;
            if (decoded->size() <= kMaxFormCacheEntryBytes &&
                m_impl.formContentBytes + decoded->size() <= kMaxFormCacheBytes) {
                m_impl.formContentBytes += decoded->size();
                m_impl.formContentCache.emplace(formKey, decoded);
            }
        }

        if (content->size() > m_impl.contentBudget) {
            m_impl.aborted = true;
            return;
        }
        m_impl.contentBudget -= content->size();

        Run(*content, formResources, Multiply(formMatrix, m_state.ctm), m_depth + 1);
    }

    TextExtractor::Impl& m_impl;
    TextPage& m_page;
    State m_state;
    std::vector<State> m_stack;
    Object m_resources;
    Matrix m_textMatrix = IdentityMatrix();
    Matrix m_lineMatrix = IdentityMatrix();
    bool m_inText = false;
    int m_depth = 0;
};

// =============================================================================
//  Page extraction
// =============================================================================
TextExtractor::TextExtractor(Document& document)
    : m_impl(std::make_unique<Impl>(document)) {}

TextExtractor::~TextExtractor() = default;

bool TextExtractor::ExtractPage(int pageIndex, TextPage& page) {
    page.Clear();
    m_impl->aborted = false;

    Object pageDictionary;
    if (!m_impl->document.GetPageDictionary(pageIndex, pageDictionary)) {
        return false;
    }
    if (!m_impl->document.GetPageBox(pageIndex, m_impl->pageBox)) {
        return false;
    }

    // Page rotation is inherited from the page tree.  /Rotate must be a multiple
    // of 90; anything else is ignored rather than applied as an arbitrary angle,
    // because the UI only understands the four upright orientations.
    int rotation = 0;
    const Object rotate = FindInherited(m_impl->document, pageDictionary, "Rotate");
    if (rotate.IsNumber()) {
        rotation = static_cast<int>(rotate.Integer()) % 360;
        if (rotation < 0) {
            rotation += 360;
        }
        if (rotation % 90 != 0) {
            rotation = 0;
        }
    }
    m_impl->rotation = rotation;

    // Collect all content streams of the page (an array is legal).  Parts are
    // decoded separately and joined with a newline so that a token cannot be
    // glued to the next part's first token.
    std::vector<uint8_t> content;
    const Object contents =
        FindInherited(m_impl->document, pageDictionary, "Contents");
    if (!contents.IsNull()) {
        const Object resolved = m_impl->document.Resolve(contents);
        if (resolved.IsArray()) {
            content.reserve(4096);
            for (const Object& part : resolved.items) {
                std::vector<uint8_t> bytes;
                if (!m_impl->document.DecodeStream(part, bytes)) {
                    continue;
                }
                if (content.size() + bytes.size() > kMaxContentBytes) {
                    // Truncate instead of failing: the text read so far is still
                    // useful, and the rest of the page is probably pathological.
                    break;
                }
                content.insert(content.end(), bytes.begin(), bytes.end());
                content.push_back('\n');
            }
        } else if (resolved.IsStream()) {
            std::vector<uint8_t> bytes;
            if (m_impl->document.DecodeStream(contents, bytes) &&
                bytes.size() <= kMaxContentBytes) {
                content = std::move(bytes);
            }
        }
    }

    // /Resources is inheritable; a page without one uses an ancestor's.
    const Object resources =
        FindInherited(m_impl->document, pageDictionary, "Resources");

    // Budgets are per page and reset here, not in the constructor: the extractor
    // is reused for many pages.
    m_impl->instructionBudget = kMaxInstructions;
    m_impl->contentBudget = kMaxContentBytes;
    // The identity CTM is correct: page content is already authored in page
    // coordinates, and MapToDisplay subtracts the crop box origin afterwards.
    TextInterpreter interpreter(*m_impl, page);
    interpreter.Run(content, resources, IdentityMatrix(), 0);
    return true;
}

}  // namespace pdf
