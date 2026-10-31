// =============================================================================
//  PdfParser.h
//  Reads the document structure of a PDF file.
//
//  The heavy lifting is split into two independent parts:
//
//    * this module parses the cross-reference information, the object graph,
//      the page tree and the document information dictionary;
//    * PdfOutline.cpp walks the outline tree and turns it into a flat list of
//      sidebar entries.
//
//  Rendering is not done here - the Windows PDF engine rasterises pages.
//
//  Robustness strategy: cross-reference data is treated as advisory.  A full
//  scan for "N G obj" headers is always performed while loading and is used
//  whenever the xref table or xref stream is missing, truncated or wrong.  That
//  costs one pass over the file but lets the viewer open the damaged documents
//  that are common on the web.
//
//  Threading: LoadFromFile and the outline walk are single threaded.  After
//  that, Resolve / GetPageDictionary / DecodeStream may be called from a worker
//  thread because they take the object-cache mutex internally.  The cache hands
//  out shallow copies whose dictionaries and stream buffers are shared with the
//  cache, so callers must never modify them.
// =============================================================================
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Pdf/PdfTypes.h"

namespace pdf {

// A rectangular page size in PDF points (1/72 inch).
struct PageSize {
    double width = 0.0;
    double height = 0.0;
};

// The page box in PDF points, using PDF's coordinate origin (bottom left).
// CropBox wins over MediaBox because that is the area the renderer displays.
// The 612 x 792 default is US Letter and only applies when the page tree is
// malformed enough that nothing is inherited at all.
struct PageBox {
    double left = 0.0;
    double bottom = 0.0;
    double right = 612.0;
    double top = 792.0;
};

// One sidebar entry produced from the document outline (its bookmark tree).
struct OutlineEntry {
    std::wstring title;       // visible label
    int pageIndex = -1;       // zero based page index, -1 when unresolved
    int level = 0;            // nesting depth, 0 for top level entries
    bool hasChildren = false;  // /First exists, even if its items were dropped
    bool initiallyOpen = true;  // honour the /Count sign in the outline
};

// Document metadata shown in the About box and the window title.
struct DocumentMetadata {
    std::wstring title;
    std::wstring author;
    std::wstring subject;
    std::wstring creator;
    bool encrypted = false;  // an /Encrypt entry exists; output may be garbage
    bool parsed = false;  // false when only the fallback information is valid
};

// Owns the parsed document.  A Document is heavy and is never copied; the
// implementation keeps the whole file image in memory for random access, so the
// instance (and any Object obtained from it) must outlive its consumers.
class Document {
public:
    Document();
    ~Document();

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    // Loads and parses a file.  Parsing errors never throw; the document simply
    // reports zero pages and the caller falls back to the Windows renderer for
    // page discovery.
    bool LoadFromFile(const std::wstring& path, std::wstring* errorMessage);

    // True once LoadFromFile succeeded.  A failed load leaves an empty
    // document behind, so callers must check this before using PageCount().
    bool IsLoaded() const;
    // Absolute path recorded during loading (useful in error messages).
    const std::wstring& Path() const;
    // Metadata as found in /Info.  When `parsed` is false the caller should
    // fall back to the file name for the window title.
    const DocumentMetadata& Metadata() const;

    // Page count derived from the page tree.  A damaged file can legitimately
    // report zero pages even though the Windows renderer can still show it.
    int PageCount() const;
    // Page size in points, already cropped and normalised.  False for an
    // out-of-range index.
    bool GetPageSize(int pageIndex, PageSize& size) const;
    // Full crop box in PDF coordinates; the text extractor needs it to map
    // glyph positions into normalised page space.
    bool GetPageBox(int pageIndex, PageBox& box) const;

    // Flattened outline in display order.  Empty when the document has no
    // bookmarks, which is very common for scanned PDFs.  The vector is built
    // once during LoadFromFile and never changes afterwards.
    const std::vector<OutlineEntry>& Outline() const;

    // Parsing primitives used by the outline reader -------------------------
    // Follows indirect references until a direct object is reached.  A chain
    // longer than 32 links, or a reference cycle, resolves to null instead of
    // looping forever.
    // All methods below are safe to call from a worker thread: the object
    // cache is guarded by an internal mutex.
    Object Resolve(const Object& object);
    // Maps an indirect page object number to its zero based page index.
    // Returns -1 for objects that are not pages.  The map is immutable after
    // loading, so this deliberately does not take the cache mutex.
    int PageIndexForObject(int objectNumber) const;
    // Access to the document trailer entries (/Root, /Info, /Encrypt ...).
    // The pointer is owned by the document and stays valid until it is
    // destroyed; the trailer is immutable after loading.
    const Object* TrailerItem(std::string_view key) const;

    // Text extraction support -----------------------------------------------
    // Returns the resolved page dictionary (/MediaBox, /CropBox, /Resources,
    // /Contents, /Rotate ...).  The returned object shares its dictionary with
    // the cache and must only be read.
    bool GetPageDictionary(int pageIndex, Object& pageDictionary);
    // Decodes a stream object (or an indirect reference to one) applying its
    // filter chain.  Returns false for unsupported filters or broken data.
    // The decoded bytes are copied out of the cache, which is convenient but
    // costs a memcpy for large content streams.
    bool DecodeStream(const Object& streamOrReference, std::vector<uint8_t>& bytes);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace pdf
