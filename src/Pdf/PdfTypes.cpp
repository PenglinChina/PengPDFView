// =============================================================================
//  PdfTypes.cpp
//  Small helpers on the PDF object model plus text-string decoding.
//
//  The helpers are deliberately forgiving: a lookup on the wrong type returns
//  zero or nullptr instead of throwing.  Real documents contain entries with a
//  surprising type (a string where a name is expected, an indirect array that
//  points at null) and a viewer must keep going rather than reject the file.
// =============================================================================
#include "Pdf/PdfTypes.h"

#include <cmath>

#include "Core/Utf.h"

namespace pdf {

using pengpdf::Utf8ToWide;

double Object::Number() const {
    if (type == Type::Int) {
        return static_cast<double>(integer);
    }
    if (type == Type::Real) {
        return real;
    }
    return 0.0;
}

int64_t Object::Integer() const {
    if (type == Type::Int) {
        return integer;
    }
    if (type == Type::Real) {
        return static_cast<int64_t>(real);
    }
    return 0;
}

const Object* Object::Get(std::string_view key) const {
    // Both dictionaries and streams carry their entries in `dict`; a stream is
    // simply a dictionary with data attached, so the same lookup serves both.
    const Dict* dictionary = nullptr;
    if (type == Type::Dict) {
        dictionary = dict.get();
    } else if (type == Type::Stream) {
        dictionary = dict.get();
    }
    if (dictionary == nullptr) {
        return nullptr;
    }
    const auto it = dictionary->find(key);
    return it == dictionary->end() ? nullptr : &it->second;
}

const Object* Object::GetItem(size_t index) const {
    if (type != Type::Array || index >= items.size()) {
        return nullptr;
    }
    return &items[index];
}

size_t Object::ItemCount() const {
    return type == Type::Array ? items.size() : 0;
}

bool Object::IsNameValue(std::string_view value) const {
    return type == Type::Name && text == value;
}

Object Object::MakeName(std::string name) {
    Object object;
    object.type = Type::Name;
    object.text = std::move(name);
    return object;
}

Object Object::MakeString(std::string bytes) {
    Object object;
    object.type = Type::String;
    object.text = std::move(bytes);
    return object;
}

Object Object::MakeInt(int64_t value) {
    Object object;
    object.type = Type::Int;
    object.integer = value;
    return object;
}

Object Object::MakeReal(double value) {
    Object object;
    object.type = Type::Real;
    object.real = value;
    return object;
}

Object Object::MakeRef(int number, int generation) {
    Object object;
    object.type = Type::Reference;
    object.refNum = number;
    object.refGen = generation;
    return object;
}

namespace {

// PDFDocEncoding differs from Latin-1 in these positions.  The table is
// straight from the PDF specification (Annex D.2) and matters for documents
// written before Unicode became the norm.
struct EncodingDifference {
    uint8_t code;
    wchar_t unicode;
};

constexpr EncodingDifference kEncodingDifferences[] = {
    {0x18, 0x02D8}, {0x19, 0x02C7}, {0x1A, 0x02C6}, {0x1B, 0x02D9},
    {0x1C, 0x02DD}, {0x1D, 0x02DB}, {0x1E, 0x02DA}, {0x1F, 0x02DC},
    {0x80, 0x2022}, {0x81, 0x2020}, {0x82, 0x2021}, {0x83, 0x2026},
    {0x84, 0x2014}, {0x85, 0x2013}, {0x86, 0x0192}, {0x87, 0x2044},
    {0x88, 0x2039}, {0x89, 0x203A}, {0x8A, 0x2212}, {0x8B, 0x2030},
    {0x8C, 0x201E}, {0x8D, 0x201C}, {0x8E, 0x201D}, {0x8F, 0x2018},
    {0x90, 0x2019}, {0x91, 0x201A}, {0x92, 0x2122}, {0x93, 0xFB01},
    {0x94, 0xFB02}, {0x95, 0x0141}, {0x96, 0x0152}, {0x97, 0x0160},
    {0x98, 0x0178}, {0x99, 0x017D}, {0x9A, 0x0131}, {0x9B, 0x0142},
    {0x9C, 0x0153}, {0x9D, 0x0161}, {0x9E, 0x017E}, {0x9F, 0xFFFD},
    {0xA0, 0x20AC}, {0xAD, 0x002D},
};

wchar_t MapPdfDocByte(uint8_t code) {
    // Bytes missing from the table keep their Latin-1 code point.  The spec
    // leaves the control range undefined, but passing the byte through is more
    // useful for broken files than dropping the character.
    for (const EncodingDifference& difference : kEncodingDifferences) {
        if (difference.code == code) {
            return difference.unicode;
        }
    }
    return static_cast<wchar_t>(code);
}

}  // namespace

std::wstring DecodeTextString(const std::string& bytes) {
    const size_t count = bytes.size();
    if (count == 0) {
        return std::wstring();
    }

    // UTF-16BE with a byte order mark - the normal representation for titles
    // written by Chinese and Japanese producers.  Surrogate pairs are copied
    // through untouched (wchar_t is UTF-16 on Windows); an odd trailing byte is
    // ignored because a truncated string is preferred over a parse failure.
    if (count >= 2 && static_cast<uint8_t>(bytes[0]) == 0xFE &&
        static_cast<uint8_t>(bytes[1]) == 0xFF) {
        std::wstring result;
        result.reserve((count - 2) / 2);
        for (size_t i = 2; i + 1 < count; i += 2) {
            const uint16_t unit = static_cast<uint16_t>(
                (static_cast<uint8_t>(bytes[i]) << 8) | static_cast<uint8_t>(bytes[i + 1]));
            result.push_back(static_cast<wchar_t>(unit));
        }
        return result;
    }

    // UTF-16LE with a byte order mark (rare, but some producers emit it).
    if (count >= 2 && static_cast<uint8_t>(bytes[0]) == 0xFF &&
        static_cast<uint8_t>(bytes[1]) == 0xFE) {
        std::wstring result;
        result.reserve((count - 2) / 2);
        for (size_t i = 2; i + 1 < count; i += 2) {
            const uint16_t unit = static_cast<uint16_t>(
                (static_cast<uint8_t>(bytes[i + 1]) << 8) | static_cast<uint8_t>(bytes[i]));
            result.push_back(static_cast<wchar_t>(unit));
        }
        return result;
    }

    // UTF-8 with a byte order mark (allowed by PDF 2.0).
    if (count >= 3 && static_cast<uint8_t>(bytes[0]) == 0xEF &&
        static_cast<uint8_t>(bytes[1]) == 0xBB && static_cast<uint8_t>(bytes[2]) == 0xBF) {
        return Utf8ToWide(std::string_view(bytes).substr(3));
    }

    // No byte order mark means PDFDocEncoding by definition.  UTF-8 without a
    // BOM is therefore mis-decoded, but guessing UTF-8 here would break the
    // Latin-1 documents that are still common in older archives.
    std::wstring result;
    result.reserve(count);
    for (char character : bytes) {
        result.push_back(MapPdfDocByte(static_cast<uint8_t>(character)));
    }
    return result;
}

std::wstring NameToWide(const std::string& name) {
    // Names follow the text-string encoding rules after the lexer has removed
    // the "#xx" escapes, so the same decoder applies.
    return DecodeTextString(name);
}

}  // namespace pdf
