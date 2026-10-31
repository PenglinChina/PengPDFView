// =============================================================================
//  PdfTypes.h
//  The in-memory representation of a PDF object.
//
//  A PDF file is built from a handful of primitive types (numbers, strings,
//  names, arrays and dictionaries) plus indirect references and streams.  This
//  module models those types as a single tagged union so that a dictionary can
//  contain any combination of values without virtual dispatch.
// =============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pdf {

struct Dict;
struct StreamData;

// One PDF object.  The heavier containers (dictionary, stream) are held behind
// shared pointers so copying an Object is cheap - the parser copies objects all
// the time when it resolves indirect references.
//
// This is a tagged union in spirit only: every payload member exists in every
// Object so that the type stays trivially copyable without placement-new or
// manual destruction.  Only the members that match `type` are meaningful.
struct Object {
    enum class Type {
        Null,        // the null object
        Bool,        // true / false
        Int,         // integer number
        Real,        // floating point number
        String,      // literal "(...)" or hexadecimal "<...>" string
        Name,        // "/Name"
        Array,       // "[ ... ]"
        Dict,        // "<< ... >>"
        Stream,      // dictionary followed by "stream ... endstream"
        Reference    // "12 0 R"
    };

    Type type = Type::Null;
    bool boolean = false;
    int64_t integer = 0;
    double real = 0.0;
    std::string text;              // string bytes or the name without its '/'
    std::vector<Object> items;     // array elements
    std::shared_ptr<Dict> dict;    // dictionary entries
    std::shared_ptr<StreamData> streamData;  // stream payload + dictionary
    int refNum = 0;                // object number for Type::Reference
    int refGen = 0;                // generation number for Type::Reference

    // A stream is a dictionary followed by raw bytes, so `dict` is populated
    // for Type::Stream as well and Object::Get can index both kinds.  Both
    // shared pointers alias the parser's object cache: a copied Object is a
    // shallow handle onto the same dictionary and payload, and callers must
    // treat them as read only.

    bool IsNull() const { return type == Type::Null; }
    bool IsBool() const { return type == Type::Bool; }
    bool IsNumber() const { return type == Type::Int || type == Type::Real; }
    bool IsString() const { return type == Type::String; }
    bool IsName() const { return type == Type::Name; }
    bool IsArray() const { return type == Type::Array; }
    bool IsDict() const { return type == Type::Dict; }
    bool IsStream() const { return type == Type::Stream; }
    bool IsRef() const { return type == Type::Reference; }

    // Numeric accessors return 0 when the object is not a number.  Integer()
    // truncates a Real towards zero, matching PDF's implicit conversions.
    double Number() const;
    int64_t Integer() const;

    // Dictionary lookup; returns nullptr when the key is absent or the object
    // is not a dictionary.  The returned pointer is owned by this object (or by
    // the shared dictionary) and becomes dangling if the Object is destroyed.
    const Object* Get(std::string_view key) const;
    // Array element access with bounds checking; nullptr when out of range.
    const Object* GetItem(size_t index) const;
    // Number of array elements, or 0 for any other type.
    size_t ItemCount() const;

    // True when this is the name object "/value".
    bool IsNameValue(std::string_view value) const;

    // Convenience constructors used while parsing.
    static Object MakeName(std::string name);
    static Object MakeString(std::string bytes);
    static Object MakeInt(int64_t value);
    static Object MakeReal(double value);
    // Reference is kept as a number/generation pair and only followed by
    // Document::Resolve, never dereferenced implicitly.
    static Object MakeRef(int number, int generation);
};

// A PDF dictionary keyed by the name without the leading slash.  Inheriting
// from std::map keeps the forward declaration above valid while the dictionary
// still offers the familiar lookup interface.
//
// The transparent comparator allows lookup with a string_view without building
// a temporary std::string.  Iteration order is lexicographic, and assigning an
// existing key overwrites the previous value - which is what malformed files
// with duplicate keys rely on.
struct Dict : std::map<std::string, Object, std::less<>> {};

// A stream is a dictionary plus the raw bytes that follow it in the file.  The
// decoded form is cached because object streams are read repeatedly.
struct StreamData {
    std::vector<uint8_t> raw;      // exact payload, EOL before "endstream" removed
    std::vector<uint8_t> decoded;  // filter chain applied, filled lazily
    bool decodedOk = false;        // decoded is valid; failures are retried
};

// Decodes a PDF text string into UTF-16.  Handles the UTF-16BE byte order mark
// used by nearly every Chinese document as well as the legacy PDFDocEncoding.
//
// This is not a general-purpose decoder: without a byte order mark the bytes
// are interpreted as PDFDocEncoding (Latin-1 with a few substitutions), not as
// UTF-8, because that is what the specification requires.
std::wstring DecodeTextString(const std::string& bytes);

// Converts a byte string that is already known to be UTF-8 (for example a name
// tree key) into a wide string.  Names have already lost their "#xx" escapes in
// the lexer; they are conventionally ASCII but producers may store UTF-8.
std::wstring NameToWide(const std::string& name);

}  // namespace pdf
