// =============================================================================
//  PageCanvas.h
//  The document view: continuous vertical page layout, zooming, scrolling,
//  text selection, search highlighting and the bitmap cache for rendered pages.
//
//  Rendering itself happens on the RenderService worker thread and text
//  extraction on the TextService worker; this window only requests what is
//  visible and paints whatever has arrived.  A persistent back buffer plus
//  dirty-rectangle painting keeps scrolling and selection smooth.
// =============================================================================
#pragma once

#include <windows.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "App/Settings.h"
#include "Pdf/PdfTextTypes.h"
#include "Render/RenderService.h"
#include "Text/TextSelection.h"
#include "Text/TextService.h"
#include "Ui/ScrollBar.h"
#include "Ui/UiMessages.h"

namespace pengpdf {

class PageCanvas {
public:
    static bool RegisterWindowClass(HINSTANCE instance, const wchar_t* className);

    bool Create(HWND parent, HINSTANCE instance, const wchar_t* className, UINT dpi);
    HWND Handle() const { return m_hwnd; }

    // Recreates fonts and page metrics after a DPI change.
    void SetDpi(UINT dpi);

    // Connects the canvas to the renderer.  `token` identifies the document the
    // renderer currently holds, so late results can be discarded.
    void SetRenderService(RenderService* service, uint64_t token);

    // Connects the canvas to the text service used for selection and search.
    void SetTextService(TextService* service, uint64_t token);
    // Called by the frame when an extracted page arrives; resolves a selection
    // that was waiting for the page and repaints the affected page only.
    void OnTextReady(const TextReadyMessage& message);

    // Text selection --------------------------------------------------------
    bool HasSelection() const { return m_hasSelection; }
    // Selected text in reading order; empty when nothing is selected.
    std::wstring SelectedText() const;
    void ClearSelection();

    // Search highlights -----------------------------------------------------
    // Appends results of another page while a search is running.  Only the
    // pages that gained highlights are invalidated, so long searches do not
    // repaint the whole viewport for every result batch.
    void AppendSearchMatches(const std::vector<pdf::TextMatch>& matches);
    // Makes `index` the current hit, scrolls it into view and repaints the old
    // and new pages.
    void SetCurrentSearchMatch(int index);
    void ClearSearch();

    // Page geometry in device independent pixels, as reported by the PDF engine.
    void SetDocument(const std::vector<PageMetrics>& pages);
    void ClearDocument();

    ZoomMode GetZoomMode() const { return m_zoomMode; }
    void SetZoomMode(ZoomMode mode);
    void SetZoom(double zoom);
    double GetZoom() const { return m_zoom; }
    int ZoomPercent() const;
    void ZoomIn();
    void ZoomOut();

    int PageCount() const { return static_cast<int>(m_pages.size()); }
    int CurrentPage() const;
    void GoToPage(int pageIndex);

    // Zooms by `factor` while keeping the document point under `screenPoint`
    // stationary; used by Ctrl + mouse wheel.
    void ZoomAt(POINT screenPoint, double factor);

    // Called by the frame when a rendered page arrives.
    void OnPageReady(PageReadyMessage& message);

    // Rebuilds the layout after the window DPI changed.
    void RefreshForDpiChange();

    // Recreates the fonts after the interface language changed.  The page
    // bitmaps stay valid because their content does not depend on the UI font.
    void ApplyLanguage();

private:
    // Position and size of one page in content coordinates (before scrolling).
    struct PageLayout {
        int left = 0;
        int top = 0;
        int width = 0;
        int height = 0;
    };

    // A rendered page as a top-down 32bpp DIB payload.
    struct CachedBitmap {
        int pixelWidth = 0;
        int pixelHeight = 0;
        std::vector<uint32_t> pixels;
    };

    // Reading position used to keep the same content under the cursor while
    // zooming or resizing.
    struct Anchor {
        bool valid = false;
        int page = 0;
        double fractionX = 0.0;
        double fractionY = 0.0;
        POINT screen{0, 0};
    };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam);

    RECT ClientRect() const;
    // Computes page rectangles and the content extents for the current zoom.
    void RecomputeLayout();
    void UpdateScrollMetrics();
    void ScrollTo(int scrollX, int scrollY);
    void ScrollBy(int deltaX, int deltaY);
    void NotifyPageChanged();
    void NotifyZoomChanged();

    void CaptureAnchor(Anchor& anchor, POINT screenPoint) const;
    void ApplyAnchor(const Anchor& anchor);

    // Requests bitmaps and text for the pages intersecting the viewport.
    void RequestVisiblePages();
    void ClearRenderCache(bool cancelPendingRequests);
    // Evicts cached bitmaps farthest from the current page.
    void EvictCache();
    // Invalidates the screen rectangle of one page (with shadow padding).
    void InvalidatePage(int pageIndex);

    // Returns the first page whose bottom edge is below `contentY`; because
    // page tops are sorted this is a binary search.  `PageAtContentY` uses it
    // to turn a reading position into a page index in O(log n).
    int FirstPageEndingAfter(int contentY) const;
    int PageAtContentY(int contentY) const;
    // Fills [first, last) with page indices intersecting the given content
    // range; used by painting, rendering and text requests.
    void VisiblePageRange(int contentTop, int contentBottom, int& first,
                          int& last) const;

    // Paints `dirty` (a sub-rectangle of the client) into the back buffer.
    void Paint(HDC dc, const RECT& client, const RECT& dirty);
    void DrawEmptyState(HDC dc, const RECT& client);
    void DrawPage(HDC dc, int pageIndex, const RECT& screenRect);

    // Persistent double buffer.  Recreating a full-window bitmap for every
    // WM_PAINT is measurable while scrolling, so it is kept until the client
    // size changes.  `recreated` tells the caller to repaint the whole client.
    bool EnsureBackBuffer(HDC reference, int width, int height, bool& recreated);
    void ReleaseBackBuffer();

    // Text geometry helpers ------------------------------------------------
    // Returns the page under a screen point and its normalised position.
    int PageAtPoint(POINT point, float& normalizedX, float& normalizedY) const;
    // Cached text page, requesting extraction when it is not ready yet.
    std::shared_ptr<const pdf::TextPage> TextPageFor(int pageIndex);
    void NormalizedPointOnPage(int pageIndex, POINT point, float& normalizedX,
                               float& normalizedY) const;

    // Mouse selection workflow: anchor on press, extend while dragging and
    // finish on release (a plain click clears the selection).
    void BeginSelection(POINT point);
    void ExtendSelection(POINT point);
    void FinishSelection();
    void SelectWordAt(POINT point);
    // Applies an inclusive rank range (reading order) and repaints the page.
    void ApplySelectionRange(size_t anchorRank, size_t currentRank);
    // Completes a selection that was waiting for the text service.
    void ResolvePendingSelection();
    void PaintSelection(HDC dc, const RECT& pageRect, int pageIndex) const;
    void PaintSearch(HDC dc, const RECT& pageRect, int pageIndex) const;
    void RequestVisibleText();
    void ScrollToSearchMatch(int index);

    HWND m_hwnd = nullptr;
    UINT m_dpi = 96;

    RenderService* m_renderer = nullptr;
    uint64_t m_renderToken = 0;

    TextService* m_textService = nullptr;
    uint64_t m_textToken = 0;

    std::vector<PageMetrics> m_pages;
    std::vector<PageLayout> m_layout;
    std::map<int, CachedBitmap> m_cache;
    std::set<int> m_pendingPages;
    std::map<int, int> m_pendingWidths;

    ZoomMode m_zoomMode = ZoomMode::FitWidth;
    double m_zoom = 1.0;

    int m_contentWidth = 0;
    int m_contentHeight = 0;
    int m_scrollX = 0;
    int m_scrollY = 0;

    int m_lastNotifiedPage = -2;
    double m_lastNotifiedZoom = -1.0;

    ScrollBar m_verticalBar;
    ScrollBar m_horizontalBar;
    int m_hoveredBar = 0;  // 0 none, 1 vertical, 2 horizontal
    bool m_trackingMouse = false;

    bool m_panning = false;
    POINT m_panStart{0, 0};
    POINT m_panOrigin{0, 0};

    // Text selection state.  Only one page can be selected at a time, which
    // keeps the model small and matches how a reader is normally used.
    // Positions are *visual ranks* (reading order), not raw glyph indices.
    std::shared_ptr<const pdf::TextPage> m_selectionText;
    std::unique_ptr<VisualOrder> m_selectionOrder;
    int m_selectionPage = -1;
    size_t m_selectionAnchorRank = 0;
    size_t m_selectionFirstRank = 0;
    size_t m_selectionLastRank = 0;
    bool m_hasSelection = false;
    bool m_selecting = false;
    POINT m_selectionDown{0, 0};
    bool m_selectionMoved = false;

    // A selection started before the page text was extracted waits here until
    // the text service delivers the page.
    bool m_pendingSelection = false;
    int m_pendingPage = -1;
    float m_pendingX = 0.0f;
    float m_pendingY = 0.0f;

    // Search overlay state (all matches of the current query).
    std::vector<pdf::TextMatch> m_searchMatches;
    std::map<int, std::vector<int>> m_searchMatchesByPage;
    int m_currentSearchMatch = -1;

    // Persistent back buffer, recreated only when the client size changes.
    HDC m_backDc = nullptr;
    HBITMAP m_backBitmap = nullptr;
    HGDIOBJ m_backPreviousBitmap = nullptr;
    int m_backWidth = 0;
    int m_backHeight = 0;

    HFONT m_placeholderFont = nullptr;
    HFONT m_titleFont = nullptr;
    HFONT m_hintFont = nullptr;
};

}  // namespace pengpdf
