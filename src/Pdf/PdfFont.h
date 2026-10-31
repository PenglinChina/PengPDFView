// =============================================================================
//  PdfFont.h
//  Character code -> Unicode + advance width decoding for one PDF font.
//
//  PDF fonts are small programs: an encoding maps byte codes to glyph names or
//  CIDs, the optional ToUnicode CMap maps codes to Unicode, and /Widths or /W
//  describes the horizontal advance.  Text extraction needs all three, so this
//  class hides that variability behind one small interface.
//
//  Two families exist and behave differently:
//    * simple fonts (Type1, TrueType, Type3) use one byte per code and a 256
//      entry encoding table;
//    * composite Type0 fonts use two bytes per code in the common Identity
//      encodings, and their codes are CIDs that only ToUnicode can turn into
//      Unicode.
//  Missing pieces degrade to sensible defaults instead of failing: the advance
//  width is always available so text positions stay aligned even when the
//  characters themselves cannot be mapped.
// =============================================================================
#pragma once

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include "Pdf/PdfParser.h"

namespace pdf {

// Decodes codes for one font resource.  The text extractor creates and caches
// one instance per font dictionary, calls Load once, and only reads the decoder
// afterwards, so Decode is effectively const and safe to share.
class FontDecoder {
public:
    FontDecoder();

    // Reads the font dictionary (already resolved) and prepares the decoder.
    // A font that cannot be decoded still returns widths so the caller can keep
    // the text position in sync.  Returns false only when `fontDictionary` is
    // not a dictionary; every other defect is absorbed by the fallbacks.
    bool Load(Document& document, const Object& fontDictionary);

    // Number of bytes that form one character code: 1 for simple fonts and
    // normally 2 for composite Type0 fonts.  A ToUnicode CMap can refine this
    // for composite fonts (its codespace range states the real width).
    int CodeByteCount() const { return m_codeBytes; }

    // True for vertical writing modes (Identity-V / WMode 1).
    bool IsVertical() const { return m_vertical; }

    // Decodes one character code.
    //   text     - Unicode characters (empty when the font has no mapping)
    //   widthEm  - advance width in em units, where 1.0 equals the font size
    // Always returns true: even unmapped codes occupy horizontal space.
    //
    // Precedence: ToUnicode wins; for simple fonts the /Encoding table is the
    // fallback, while composite fonts have no fallback because a CID is not a
    // Unicode code point.
    bool Decode(uint32_t code, std::wstring& text, double& widthEm) const;

private:
    void BuildSimpleFallback();
    void ApplyBaseEncodingName(const std::string& name);
    void ApplyDifferences(Document& document, const Object& differences);
    void ApplyDifference(const std::string& glyphName, uint32_t code);
    void LoadSimpleWidths(Document& document, const Object& fontDictionary);
    void LoadType0(Document& document, const Object& fontDictionary);
    void LoadCidWidths(Document& document, const Object& descendantFont);
    void LoadToUnicode(Document& document, const Object& streamObject);

    bool m_composite = false;   // Type0 font with a CID-keyed descendant
    bool m_vertical = false;
    int m_codeBytes = 1;

    // Width of one width unit in em.  Normal fonts use 1/1000; Type3 fonts
    // provide an arbitrary /FontMatrix whose first entry is used instead.
    double m_widthScale = 0.001;

    double m_missingWidthEm = 0.5;   // used when a code has no width entry
    double m_defaultWidthEm = 0.5;   // composite fonts' /DW, in em

    // Simple fonts: direct lookup table for the 256 possible byte codes.
    std::array<wchar_t, 256> m_simpleCharacters{};
    bool m_hasSimpleWidths = false;
    uint32_t m_firstChar = 0;
    // /Widths entries for codes FirstChar..FirstChar+n-1; codes outside that
    // range use m_missingWidthEm.
    std::vector<double> m_simpleWidths;

    // Code -> Unicode override (ToUnicode), taking precedence over the encoding
    // tables above for simple fonts.
    std::unordered_map<uint32_t, std::wstring> m_unicode;
    // CID -> advance width for composite fonts, only for codes present in /W;
    // anything else uses m_defaultWidthEm.  Identity encodings make the CID equal
    // the character code, which is the case this map assumes.
    std::unordered_map<uint32_t, double> m_widths;
};

}  // namespace pdf
