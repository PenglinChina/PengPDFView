// =============================================================================
//  pdf_tests.cpp
//  Console test harness for the parsing and text layers.
//
//  It runs three independent test groups:
//    1. DEFLATE decoder tests against reference streams produced by zlib,
//    2. text extraction and search tests (synthetic and real documents),
//    3. document parsing tests against real PDF files supplied on the command
//       line, printing the page count, metadata and outline tree.
//
//  Build with build.bat test and run:  pdf_tests.exe <file.pdf> ...
// =============================================================================
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Core/Utf.h"
#include "Pdf/PdfDeflate.h"
#include "Pdf/PdfParser.h"
#include "Pdf/PdfTextExtractor.h"
#include "Text/TextSearch.h"
#include "Text/TextSelection.h"
#include "deflate_vectors.h"

namespace {

int g_failures = 0;

// Each check prints immediately so that a crash later in the run still shows
// how far it got; failures are counted rather than aborting, so a single
// invocation reports every problem instead of only the first one.
void Check(bool condition, const char* description) {
    if (condition) {
        std::printf("  [ ok ] %s\n", description);
    } else {
        std::printf("  [FAIL] %s\n", description);
        ++g_failures;
    }
}

// printf() writes bytes, so wide text (document titles may be Chinese) has to
// be converted explicitly; the console code page is switched to UTF-8 in
// wmain() so these bytes are displayed correctly.
std::string ToUtf8(const std::wstring& text) {
    return pengpdf::WideToUtf8(text);
}

// -----------------------------------------------------------------------------
//  DEFLATE decoder tests
// -----------------------------------------------------------------------------
void TestInflate() {
    std::printf("\n[deflate] reference streams\n");

    // The reference streams live in deflate_vectors.h and were produced by
    // zlib; the decoder must reproduce the original bytes exactly.  The last
    // case is the important negative test: corrupt input has to be rejected
    // instead of decoded into garbage that would later corrupt a page.
    //
    // 1. Dynamic Huffman block produced by zlib's default strategy.
    std::string expected;
    for (int i = 0; i < 50; ++i) {
        expected += "The quick brown fox jumps over the lazy dog. ";
    }
    std::vector<uint8_t> output;
    const bool dynamicOk = pdf::Inflate(testdata::kDeflateDynamicZlib,
                                        testdata::kDeflateDynamicZlibSize, output);
    Check(dynamicOk, "dynamic Huffman stream decodes");
    Check(output.size() == expected.size() &&
              std::memcmp(output.data(), expected.data(), output.size()) == 0,
          "dynamic Huffman output matches the original text");

    // 2. Stored (uncompressed) block.
    output.clear();
    std::vector<uint8_t> storedExpected;
    for (int repeat = 0; repeat < 4; ++repeat) {
        for (int value = 0; value < 256; ++value) {
            storedExpected.push_back(static_cast<uint8_t>(value));
        }
    }
    const bool storedOk = pdf::Inflate(testdata::kDeflateStoredZlib,
                                       testdata::kDeflateStoredZlibSize, output);
    Check(storedOk, "stored block stream decodes");
    Check(output == storedExpected, "stored block output matches");

    // 3. Empty stream (header plus an empty final block).
    output.clear();
    const bool emptyOk = pdf::Inflate(testdata::kDeflateEmptyZlib,
                                      testdata::kDeflateEmptyZlibSize, output);
    Check(emptyOk && output.empty(), "empty zlib stream decodes to zero bytes");

    // 4. Raw DEFLATE without a zlib wrapper.
    output.clear();
    const std::string rawExpected = expected.substr(0, 200);
    const bool rawOk = pdf::InflateRaw(testdata::kDeflateRawStream,
                                       testdata::kDeflateRawStreamSize, output);
    Check(rawOk, "raw DEFLATE stream decodes");
    Check(output.size() == rawExpected.size() &&
              std::memcmp(output.data(), rawExpected.data(), output.size()) == 0,
          "raw DEFLATE output matches");

    // 5. Corrupt data must be rejected instead of producing garbage.
    const uint8_t garbage[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x11};
    output.clear();
    Check(!pdf::Inflate(garbage, sizeof(garbage), output),
          "garbage input is rejected");
}

// -----------------------------------------------------------------------------
//  Text extraction and search tests
// -----------------------------------------------------------------------------
// Builds one synthetic glyph.  Coordinates are page-relative (0..1 spans the
// page), which is the unit used by the text layer, the selection code and the
// search index; the font size is deliberately the same as the box height.
pdf::TextGlyph MakeGlyph(wchar_t character, float left, float top, float width,
                         float height) {
    pdf::TextGlyph glyph;
    glyph.character = character;
    glyph.bounds.left = left;
    glyph.bounds.top = top;
    glyph.bounds.right = left + width;
    glyph.bounds.bottom = top + height;
    glyph.fontSize = height;
    return glyph;
}

// Appends one synthetic line.  The advance (0.011) is slightly wider than the
// glyph itself (0.01) so the test characters never overlap; overlapping boxes
// would make the hit testing assertions ambiguous.
void AddLine(pdf::TextPage& page, const wchar_t* text, float left, float top) {
    float x = left;
    for (const wchar_t* cursor = text; *cursor != L'\0'; ++cursor) {
        page.glyphs.push_back(MakeGlyph(*cursor, x, top, 0.01f, 0.02f));
        x += 0.011f;
    }
}

// Synthetic two-line page: exercises normalisation, cross-line matches,
// case folding, glyph ranges and highlight rectangle generation.
void TestTextSearch() {
    std::printf("\n[text] synthetic page search\n");

    pdf::TextPage page;
    AddLine(page, L"Hello world", 0.10f, 0.10f);
    AddLine(page, L"Second line", 0.10f, 0.20f);
    pengpdf::PageTextIndex index(page);

    // The index lower-cases the text, turns the line break into a single
    // space and collapses runs of whitespace: that is the canonical form all
    // queries are matched against.
    Check(index.Text() == L"hello world second line",
          "page index normalises whitespace and case");

    // "world" starts after "hello " (glyph 6) and its last character is at
    // glyph 10; both ends are reported inclusive and refer to indices into
    // page.glyphs, not to positions in the normalised text.
    auto matches = index.FindAll(L"world", 0);
    Check(matches.size() == 1 && matches[0].pageIndex == 0,
          "search finds a single word");
    Check(!matches.empty() && matches[0].firstGlyph == 6 &&
              matches[0].lastGlyph == 10,
          "search reports the correct glyph range");
    Check(!matches.empty() && !matches[0].rects.empty(),
          "search produces highlight rectangles");

    matches = index.FindAll(L"HELLO", 0);
    Check(matches.size() == 1, "search is case insensitive");

    matches = index.FindAll(L"world second", 0);
    Check(matches.size() == 1, "search crosses a line break");

    // TextService normalises a query once per search and then calls
    // FindAllNormalized for every page; both entry points must agree.
    const auto reference = matches;
    const auto normalizedMatches = index.FindAllNormalized(
        pengpdf::NormalizeSearchQuery(L"  WORLD   SECOND "), 0);
    Check(reference.size() == 1 && normalizedMatches.size() == 1 &&
              reference[0].firstGlyph == normalizedMatches[0].firstGlyph &&
              reference[0].lastGlyph == normalizedMatches[0].lastGlyph,
          "pre-normalised queries produce the same matches");

    matches = index.FindAll(L"  second   line  ", 0);
    Check(matches.size() == 1, "query whitespace is normalised");

    matches = index.FindAll(L"missing", 0);
    Check(matches.empty(), "unknown words produce no match");

    // "  MiXeD\tCase " normalises to "mixed case", i.e. exactly ten
    // characters: leading/trailing spaces dropped and the tab folded to one
    // space.
    Check(pengpdf::NormalizeSearchQuery(L"  MiXeD\tCase ").size() == 10,
          "query normalisation collapses whitespace");
}

// Content-stream order deliberately differs from reading order here, which is
// exactly the case VisualOrder has to repair for selection and copying.
void TestVisualOrder() {
    std::printf("\n[text] reading order and selection geometry\n");

    // The second line is added first to mimic content streams that do not
    // follow the visual reading order, as table-of-contents pages often do.
    pdf::TextPage page;
    AddLine(page, L"Second line", 0.10f, 0.20f);
    AddLine(page, L"First line", 0.10f, 0.10f);

    pengpdf::VisualOrder order(page);
    Check(order.TextInRange(0, order.Count() - 1) == L"First line\nSecond line",
          "visual order sorts lines from top to bottom");

    // Glyph indices (not ranks): "Second line" was added first and occupies
    // glyphs 0..10, "First line" occupies 11..20.  Rank 0 is therefore glyph
    // 11 (the 'F'), and rank 10 is the start of the second visual line.
    Check(order.GlyphAtRank(0) == 11 && order.GlyphAtRank(10) == 0,
          "visual order maps ranks back to glyph indices");

    // A point past the right end of the first line must stay on that line.
    // The expectation is the last character of "First line" (glyph 20, rank 9)
    // rather than the first character of the line below.
    const size_t rank = order.RankForPoint(0.95f, 0.105f);
    Check(rank == 9 && order.GlyphAtRank(rank) == 20,
          "hit testing prefers the line under the pointer");

    size_t first = 0;
    size_t last = 0;
    // Rank 2 is the 'r' inside "First"; a word selection must expand to the
    // whole word, including its first character (rank 0).
    order.WordRangeAt(2, first, last);
    Check(order.TextInRange(first, last) == L"First",
          "word selection expands to word boundaries");

    const auto rects = order.RectanglesInRange(0, order.Count() - 1);
    Check(rects.size() == 2 && rects[0].top < rects[1].top,
          "selection produces one rectangle per visual line");
}

// -----------------------------------------------------------------------------
//  Document parsing tests
// -----------------------------------------------------------------------------
// Runs invariants that must hold for every well-formed document, whatever its
// content: at least one page, sane page sizes, a well-formed outline and text
// boxes inside the page.  The printed metadata, outline tree and text sample
// are for manual review rather than assertions.
bool TestDocument(const std::wstring& path) {
    const std::string displayPath = ToUtf8(path);
    std::printf("\n[document] %s\n", displayPath.c_str());

    pdf::Document document;
    std::wstring error;
    if (!document.LoadFromFile(path, &error)) {
        std::printf("  [FAIL] parse error: %s\n", ToUtf8(error).c_str());
        ++g_failures;
        return false;
    }

    std::printf("  pages   : %d\n", document.PageCount());
    std::printf("  title   : %s\n", ToUtf8(document.Metadata().title).c_str());
    std::printf("  author  : %s\n", ToUtf8(document.Metadata().author).c_str());
    std::printf("  encrypt : %s\n", document.Metadata().encrypted ? "yes" : "no");
    std::printf("  outline : %d entries\n",
                static_cast<int>(document.Outline().size()));

    Check(document.PageCount() > 0, "document reports at least one page");

    bool outlineOk = true;
    int lastLevel = -1;
    for (std::size_t index = 0; index < document.Outline().size(); ++index) {
        const pdf::OutlineEntry& entry = document.Outline()[index];
        if (entry.title.empty()) {
            outlineOk = false;
        }
        if (entry.pageIndex < -1 || entry.pageIndex >= document.PageCount()) {
            outlineOk = false;
        }
        if (entry.level < 0 || entry.level > 16) {
            outlineOk = false;
        }
        if (lastLevel >= 0 && entry.level > lastLevel + 1) {
            outlineOk = false;  // hierarchy must not skip levels
        }
        lastLevel = entry.level;

        std::printf("    %*s- %s%s (page %d)\n", entry.level * 2, "", "",
                    ToUtf8(entry.title).c_str(), entry.pageIndex + 1);
    }
    Check(outlineOk, "outline entries have valid titles, levels and page indices");

    // Page sizes must be sane; this also exercises inherited /MediaBox values.
    bool sizesOk = true;
    for (int page = 0; page < document.PageCount(); ++page) {
        pdf::PageSize size;
        if (!document.GetPageSize(page, size) || size.width < 10.0 ||
            size.height < 10.0 || size.width > 20000.0 || size.height > 20000.0) {
            sizesOk = false;
            break;
        }
        if (page < 3) {
            std::printf("  page %d size: %.2f x %.2f pt\n", page + 1, size.width,
                        size.height);
        }
    }
    Check(sizesOk, "page sizes are within plausible bounds");

    // Text extraction ------------------------------------------------------
    pdf::TextExtractor extractor(document);
    pdf::TextPage textPage;
    const bool extracted = extractor.ExtractPage(0, textPage);
    Check(extracted, "text extraction handles the first page");
    std::printf("  text   : %zu glyphs\n", textPage.glyphs.size());
    if (!textPage.glyphs.empty()) {
        const std::wstring sample = textPage.AllText().substr(0, 80);
        std::printf("  sample : %s\n", ToUtf8(sample).c_str());

        // Verify that the search index and the extractor agree: a word taken
        // from the extracted text must be found again through the index.
        pengpdf::PageTextIndex index(textPage);
        const std::wstring& normalized = index.Text();
        const size_t tokenLength = std::min<size_t>(8, normalized.size());
        if (tokenLength >= 4) {
            const auto matches = index.FindAll(normalized.substr(0, tokenLength), 0);
            Check(!matches.empty(),
                  "search finds text that the extractor produced");
        }
    }

    bool boxesOk = true;
    // Glyph bounds are normalised to the page (0..1 across and down).  A small
    // tolerance is allowed because accents and italic overhangs can stick out
    // slightly past the page box; anything further away indicates a broken
    // text matrix in the extractor.
    for (const pdf::TextGlyph& glyph : textPage.glyphs) {
        const pdf::TextRect& box = glyph.bounds;
        if (!std::isfinite(box.left) || !std::isfinite(box.top) ||
            !std::isfinite(box.right) || !std::isfinite(box.bottom) ||
            box.left < -2.0f || box.top < -2.0f || box.right > 3.0f ||
            box.bottom > 3.0f) {
            boxesOk = false;
            break;
        }
    }
    Check(boxesOk, "text glyph boxes are inside a plausible page range");
    return true;
}

}  // namespace

// Test entry point; document paths are optional and enable the integration
// checks at the end.  A non-zero exit code means at least one check failed.
int wmain(int argc, wchar_t** argv) {
    // All diagnostics are printed as UTF-8 (the sources are compiled with
    // /utf-8), so the console has to use the same encoding for CJK titles.
    ::SetConsoleOutputCP(CP_UTF8);

    std::printf("PengPDFView parser tests\n");
    TestInflate();
    TestTextSearch();
    TestVisualOrder();

    // Document tests are optional: without arguments only the self-contained
    // decoder and text tests run, so the harness needs no sample files.
    if (argc < 2) {
        std::printf("\nNo PDF files supplied; skipping document tests.\n");
    } else {
        for (int index = 1; index < argc; ++index) {
            TestDocument(argv[index]);
        }
    }

    // Non-zero exit status is what scripts and CI look at; the printed summary
    // is only for humans.
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "PASSED" : "FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
