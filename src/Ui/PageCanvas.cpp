// =============================================================================
//  PageCanvas.cpp
//  Continuous page view with zoom, scrolling and an LRU bitmap cache.
// =============================================================================
#include "Ui/PageCanvas.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <utility>

#include "App/Localization.h"
#include "Core/Config.h"
#include "Text/TextSelection.h"
#include "Ui/Theme.h"

namespace pengpdf {
namespace {

constexpr double kZoomStep = 1.1;

// The page shadow is a simple offset rectangle; GDI has no real alpha blur, so
// two subtle layers give a convincing depth cue.
void DrawPageShadow(HDC dc, const RECT& page, UINT dpi, const theme::Palette& colors) {
    const int offset = theme::Scale(3, dpi);
    RECT shadow = page;
    ::OffsetRect(&shadow, offset, offset);
    theme::FillSolid(dc, shadow, colors.pageShadow);
}

}  // namespace

// The canvas needs double clicks for word selection; everything else is
// painted by us, so no background brush is registered.
bool PageCanvas::RegisterWindowClass(HINSTANCE instance, const wchar_t* className) {
    WNDCLASSEXW description = {};
    description.cbSize = sizeof(description);
    description.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    description.lpfnWndProc = &PageCanvas::WindowProc;
    description.hInstance = instance;
    description.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    description.lpszClassName = className;
    return ::RegisterClassExW(&description) != 0;
}

bool PageCanvas::Create(HWND parent, HINSTANCE instance, const wchar_t* className,
                        UINT dpi) {
    m_dpi = dpi;
    m_hwnd = ::CreateWindowExW(0, className, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                               0, 0, 0, 0, parent, nullptr, instance, this);
    if (m_hwnd == nullptr) {
        return false;
    }
    SetDpi(dpi);
    return true;
}

// Recreates fonts and layout metrics for a new monitor DPI.
void PageCanvas::SetDpi(UINT dpi) {
    m_dpi = dpi;
    m_verticalBar.SetDpi(dpi);
    m_horizontalBar.SetDpi(dpi);

    // Fonts are only used for the empty/loading placeholders; page content is
    // painted from bitmaps and is independent of the UI font.
    theme::ReleaseFont(m_placeholderFont);
    theme::ReleaseFont(m_titleFont);
    theme::ReleaseFont(m_hintFont);

    const bool chinese = IsChineseInterface();
    m_placeholderFont = theme::CreateUiFont(dpi, 9.0, FW_NORMAL, chinese);
    m_titleFont = theme::CreateUiFont(dpi, 15.0, FW_SEMIBOLD, chinese);
    m_hintFont = theme::CreateUiFont(dpi, 10.0, FW_NORMAL, chinese);

    if (m_hwnd != nullptr) {
        ClearRenderCache(true);
        RecomputeLayout();
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

// Rebinds the renderer and drops bitmaps that belong to the previous document.
void PageCanvas::SetRenderService(RenderService* service, uint64_t token) {
    m_renderer = service;
    m_renderToken = token;
    // Cached bitmaps belong to the old document/zoom; cancel queued requests so
    // the worker does not waste time on them.
    ClearRenderCache(true);
}

// Rebinds the text service and clears selection/search state tied to the old
// document.
void PageCanvas::SetTextService(TextService* service, uint64_t token) {
    m_textService = service;
    m_textToken = token;
    // Selection and highlights refer to the previous document's text.
    ClearSelection();
    ClearSearch();
    RequestVisibleText();
}

// A page's text was extracted; complete a pending selection and repaint only
// that page.
void PageCanvas::OnTextReady(const TextReadyMessage& message) {
    if (message.documentToken != m_textToken) {
        return;
    }
    if (m_pendingSelection && message.pageIndex == m_pendingPage) {
        ResolvePendingSelection();
    }
    // A late text page only affects its own page; repainting the whole canvas
    // here would make bulk extraction (search) flicker.
    InvalidatePage(message.pageIndex);
}

// Selected text in reading order, with spaces and newlines reconstructed from
// the glyph geometry.
std::wstring PageCanvas::SelectedText() const {
    if (!m_hasSelection || !m_selectionOrder) {
        return std::wstring();
    }
    // VisualOrder inserts spaces/newlines from geometry, so the clipboard text
    // matches what the user sees rather than the raw content stream order.
    return m_selectionOrder->TextInRange(m_selectionFirstRank, m_selectionLastRank);
}

// Clears the selection, repaints and tells the frame to refresh Copy state.
void PageCanvas::ClearSelection() {
    const bool wasVisible = m_hasSelection;
    m_hasSelection = false;
    m_selecting = false;
    m_pendingSelection = false;
    m_selectionText.reset();
    m_selectionOrder.reset();
    m_selectionPage = -1;
    m_selectionFirstRank = 0;
    m_selectionLastRank = 0;
    // The frame enables/disables the Copy command from the selection state, so
    // notify it only when the state actually changed.
    if (wasVisible && m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
        ::PostMessageW(::GetParent(m_hwnd), WM_APP_SELECTION_CHANGED, 0, 0);
    }
}

// Incremental highlight update: results are appended in the same order the
// frame keeps them, so the frame's match index stays valid.
void PageCanvas::AppendSearchMatches(const std::vector<pdf::TextMatch>& matches) {
    if (matches.empty()) {
        return;
    }
    const size_t base = m_searchMatches.size();
    m_searchMatches.insert(m_searchMatches.end(), matches.begin(), matches.end());

    // Results arrive one page at a time, so only that page needs a repaint.
    // InvalidatePage also merges repeated calls for the same rectangle.
    int lastInvalidated = -1;
    for (size_t offset = 0; offset < matches.size(); ++offset) {
        const int pageIndex = matches[offset].pageIndex;
        m_searchMatchesByPage[pageIndex].push_back(
            static_cast<int>(base + offset));
        if (pageIndex != lastInvalidated) {
            InvalidatePage(pageIndex);
            lastInvalidated = pageIndex;
        }
    }
}

// Updates the active hit.  Only the two affected pages are invalidated unless
// scrolling already invalidated the viewport.
void PageCanvas::SetCurrentSearchMatch(int index) {
    const int previous = m_currentSearchMatch;
    if (index < 0 || index >= static_cast<int>(m_searchMatches.size())) {
        m_currentSearchMatch = -1;
    } else {
        m_currentSearchMatch = index;
        ScrollToSearchMatch(index);
    }

    // Repaint the old and new hit so the stronger "current" colour follows the
    // selection.  Scrolling may already have invalidated the whole viewport,
    // in which case these calls simply coalesce with it.
    if (previous >= 0 && previous < static_cast<int>(m_searchMatches.size())) {
        InvalidatePage(m_searchMatches[static_cast<size_t>(previous)].pageIndex);
    }
    if (m_currentSearchMatch >= 0) {
        InvalidatePage(
            m_searchMatches[static_cast<size_t>(m_currentSearchMatch)].pageIndex);
    }
}

// Removes all highlights; a new query starts from an empty overlay.
void PageCanvas::ClearSearch() {
    m_searchMatches.clear();
    m_searchMatchesByPage.clear();
    m_currentSearchMatch = -1;
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

// Scrolls so the match's first rectangle sits about a third down the viewport,
// which leaves context above and below it.
void PageCanvas::ScrollToSearchMatch(int index) {
    if (index < 0 || index >= static_cast<int>(m_searchMatches.size()) ||
        m_layout.empty()) {
        return;
    }
    const pdf::TextMatch& match = m_searchMatches[static_cast<size_t>(index)];
    if (match.pageIndex < 0 ||
        match.pageIndex >= static_cast<int>(m_layout.size())) {
        return;
    }
    const PageLayout& layout = m_layout[static_cast<size_t>(match.pageIndex)];
    const RECT client = ClientRect();
    int contentY = layout.top;
    if (!match.rects.empty()) {
        contentY += static_cast<int>(match.rects.front().top * layout.height);
    }
    // Place the match a third of the way down the viewport.
    ScrollTo(m_scrollX, contentY - static_cast<int>(client.bottom) / 3);
}

int PageCanvas::PageAtPoint(POINT point, float& normalizedX,
                            float& normalizedY) const {
    if (m_layout.empty()) {
        return -1;
    }
    // Pages never overlap, so only the first page whose bottom edge is below
    // the pointer can contain it.
    const int contentY = point.y + m_scrollY;
    const int index = FirstPageEndingAfter(contentY);
    if (index >= static_cast<int>(m_layout.size())) {
        return -1;
    }
    const PageLayout& layout = m_layout[static_cast<size_t>(index)];
    const int left = layout.left - m_scrollX;
    const int top = layout.top - m_scrollY;
    if (point.x < left || point.x >= left + layout.width || point.y < top ||
        point.y >= top + layout.height) {
        return -1;  // the point is in the gap between two pages
    }
    normalizedX = static_cast<float>(point.x - left) /
                  static_cast<float>(std::max(1, layout.width));
    normalizedY = static_cast<float>(point.y - top) /
                  static_cast<float>(std::max(1, layout.height));
    return index;
}

// Returns the cached text page, requesting extraction when necessary.
std::shared_ptr<const pdf::TextPage> PageCanvas::TextPageFor(int pageIndex) {
    if (m_textService == nullptr || pageIndex < 0) {
        return nullptr;
    }
    // Reading from the cache is non-blocking; a miss queues extraction so a
    // click that arrives too early is completed by OnTextReady.
    std::shared_ptr<const pdf::TextPage> page = m_textService->CachedPage(pageIndex);
    if (!page) {
        m_textService->RequestPage(pageIndex);
    }
    return page;
}

// Clamps the pointer into the page rectangle; dragging outside the page keeps
// extending towards the nearest glyph instead of losing the selection.
void PageCanvas::NormalizedPointOnPage(int pageIndex, POINT point, float& normalizedX,
                                       float& normalizedY) const {
    normalizedX = 0.0f;
    normalizedY = 0.0f;
    if (pageIndex < 0 || pageIndex >= static_cast<int>(m_layout.size())) {
        return;
    }
    const PageLayout& layout = m_layout[static_cast<size_t>(pageIndex)];
    const int left = layout.left - m_scrollX;
    const int top = layout.top - m_scrollY;
    normalizedX = std::clamp(
        static_cast<float>(point.x - left) / std::max(1, layout.width), 0.0f, 1.0f);
    normalizedY = std::clamp(
        static_cast<float>(point.y - top) / std::max(1, layout.height), 0.0f, 1.0f);
}

// Starts a drag selection at the glyph under the pointer.  When the page text
// is not extracted yet the anchor is stored and resolved on WM_APP_TEXT_READY.
void PageCanvas::BeginSelection(POINT point) {
    ClearSelection();
    if (m_textService == nullptr || m_pages.empty()) {
        return;
    }

    float x = 0.0f;
    float y = 0.0f;
    const int pageIndex = PageAtPoint(point, x, y);
    if (pageIndex < 0) {
        return;
    }

    m_selectionDown = point;
    m_selectionMoved = false;
    m_selecting = true;

    std::shared_ptr<const pdf::TextPage> text = TextPageFor(pageIndex);
    if (!text) {
        // The page has not been extracted yet; remember where the drag started
        // and finish the selection as soon as the text arrives.
        m_pendingSelection = true;
        m_pendingPage = pageIndex;
        m_pendingX = x;
        m_pendingY = y;
        ::SetCapture(m_hwnd);
        return;
    }

    m_selectionText = text;
    m_selectionOrder = std::make_unique<VisualOrder>(*text);
    m_selectionPage = pageIndex;
    m_selectionAnchorRank = m_selectionOrder->RankForPoint(x, y);
    ApplySelectionRange(m_selectionAnchorRank, m_selectionAnchorRank);
    ::SetCapture(m_hwnd);
}

// Extends the selection to the glyph under the pointer.  Only the page that
// owns the anchor is considered, which keeps the model single-page.
void PageCanvas::ExtendSelection(POINT point) {
    if (!m_selecting) {
        return;
    }
    if (std::abs(point.x - m_selectionDown.x) > 3 ||
        std::abs(point.y - m_selectionDown.y) > 3) {
        m_selectionMoved = true;
    }

    if (m_pendingSelection) {
        float x = 0.0f;
        float y = 0.0f;
        if (PageAtPoint(point, x, y) == m_pendingPage) {
            m_pendingX = x;
            m_pendingY = y;
        }
        return;
    }

    if (!m_selectionOrder || m_selectionPage < 0) {
        return;
    }
    float x = 0.0f;
    float y = 0.0f;
    NormalizedPointOnPage(m_selectionPage, point, x, y);
    ApplySelectionRange(m_selectionAnchorRank, m_selectionOrder->RankForPoint(x, y));
}

// A click without movement clears the selection; a real drag keeps it.
void PageCanvas::FinishSelection() {
    if (!m_selecting) {
        return;
    }
    m_selecting = false;
    m_pendingSelection = false;
    // A plain click clears the selection; only a drag keeps it.
    if (!m_selectionMoved && m_hasSelection) {
        ClearSelection();
    }
}

// Double click selects the word under the pointer using the visual word
// boundaries from VisualOrder.
void PageCanvas::SelectWordAt(POINT point) {
    ClearSelection();
    float x = 0.0f;
    float y = 0.0f;
    const int pageIndex = PageAtPoint(point, x, y);
    if (pageIndex < 0) {
        return;
    }
    std::shared_ptr<const pdf::TextPage> text = TextPageFor(pageIndex);
    if (!text) {
        return;
    }

    m_selectionText = text;
    m_selectionOrder = std::make_unique<VisualOrder>(*text);
    m_selectionPage = pageIndex;
    const size_t rank = m_selectionOrder->RankForPoint(x, y);
    size_t firstRank = 0;
    size_t lastRank = 0;
    m_selectionOrder->WordRangeAt(rank, firstRank, lastRank);
    m_selectionAnchorRank = firstRank;
    m_selecting = false;
    ApplySelectionRange(firstRank, lastRank);
}

// Ranks are inclusive; the anchor is kept so a drag can shrink the selection
// again when the pointer returns towards the start.
void PageCanvas::ApplySelectionRange(size_t anchorRank, size_t currentRank) {
    if (!m_selectionOrder || m_selectionOrder->Count() == 0) {
        return;
    }
    const bool wasSelected = m_hasSelection;
    const size_t limit = m_selectionOrder->Count() - 1;
    m_selectionAnchorRank = std::min(anchorRank, limit);
    m_selectionFirstRank = std::min(anchorRank, currentRank);
    m_selectionLastRank = std::max(anchorRank, currentRank);
    m_selectionFirstRank = std::min(m_selectionFirstRank, limit);
    m_selectionLastRank = std::min(m_selectionLastRank, limit);
    m_hasSelection = true;
    InvalidatePage(m_selectionPage);
    if (!wasSelected && m_hwnd != nullptr) {
        ::PostMessageW(::GetParent(m_hwnd), WM_APP_SELECTION_CHANGED, 0, 0);
    }
}

// The text service finished the page the user pressed on before extraction
// had caught up; turn the stored normalised point into a rank now.
void PageCanvas::ResolvePendingSelection() {
    if (!m_pendingSelection || m_textService == nullptr) {
        return;
    }
    std::shared_ptr<const pdf::TextPage> text = m_textService->CachedPage(m_pendingPage);
    if (!text) {
        return;
    }
    m_selectionText = text;
    m_selectionOrder = std::make_unique<VisualOrder>(*text);
    m_selectionPage = m_pendingPage;
    m_selectionAnchorRank = m_selectionOrder->RankForPoint(m_pendingX, m_pendingY);
    m_pendingSelection = false;
    ApplySelectionRange(m_selectionAnchorRank, m_selectionAnchorRank);
}

// Only visible pages are requested; RequestPage coalesces duplicates.
void PageCanvas::RequestVisibleText() {
    if (m_textService == nullptr || m_pages.empty() ||
        m_layout.size() != m_pages.size() || m_hwnd == nullptr) {
        return;
    }
    const RECT client = ClientRect();
    int first = 0;
    int last = 0;
    VisiblePageRange(m_scrollY, m_scrollY + static_cast<int>(client.bottom), first,
                     last);
    for (int index = first; index < last; ++index) {
        // RequestPage checks the cache and the pending set under one lock, so
        // there is no need to query CachedPage first.
        m_textService->RequestPage(index);
    }
}

// Search highlights are painted after the page bitmap and clipped to the page,
// so they never bleed into the canvas margin.  The current hit uses a stronger
// orange to stand out from the other hits.
void PageCanvas::PaintSearch(HDC dc, const RECT& pageRect, int pageIndex) const {
    const auto found = m_searchMatchesByPage.find(pageIndex);
    if (found == m_searchMatchesByPage.end()) {
        return;
    }
    RECT inner = pageRect;
    ::InflateRect(&inner, -theme::Scale(1, m_dpi), -theme::Scale(1, m_dpi));
    if (inner.right <= inner.left || inner.bottom <= inner.top) {
        return;
    }
    const int width = inner.right - inner.left;
    const int height = inner.bottom - inner.top;

    ::SaveDC(dc);
    ::IntersectClipRect(dc, inner.left, inner.top, inner.right, inner.bottom);
    for (int matchIndex : found->second) {
        if (matchIndex < 0 ||
            matchIndex >= static_cast<int>(m_searchMatches.size())) {
            continue;
        }
        const bool current = matchIndex == m_currentSearchMatch;
        const COLORREF color = current ? RGB(255, 152, 40) : RGB(255, 214, 92);
        const int alpha = current ? 165 : 110;
        for (const pdf::TextRect& rect :
             m_searchMatches[static_cast<size_t>(matchIndex)].rects) {
            RECT screen{inner.left + static_cast<int>(rect.left * width),
                        inner.top + static_cast<int>(rect.top * height),
                        inner.left + static_cast<int>(rect.right * width),
                        inner.top + static_cast<int>(rect.bottom * height)};
            ::InflateRect(&screen, theme::Scale(1, m_dpi), 0);
            theme::FillTranslucent(dc, screen, color, alpha);
        }
    }
    ::RestoreDC(dc, -1);
}

// Selection uses one translucent rectangle per visual line; the line merging
// happens in VisualOrder so the painting code stays trivial.
void PageCanvas::PaintSelection(HDC dc, const RECT& pageRect,
                                int pageIndex) const {
    if (!m_hasSelection || !m_selectionOrder || pageIndex != m_selectionPage) {
        return;
    }
    RECT inner = pageRect;
    ::InflateRect(&inner, -theme::Scale(1, m_dpi), -theme::Scale(1, m_dpi));
    if (inner.right <= inner.left || inner.bottom <= inner.top) {
        return;
    }
    const int width = inner.right - inner.left;
    const int height = inner.bottom - inner.top;

    ::SaveDC(dc);
    ::IntersectClipRect(dc, inner.left, inner.top, inner.right, inner.bottom);
    for (const pdf::TextRect& rect : m_selectionOrder->RectanglesInRange(
             m_selectionFirstRank, m_selectionLastRank)) {
        RECT screen{inner.left + static_cast<int>(rect.left * width),
                    inner.top + static_cast<int>(rect.top * height),
                    inner.left + static_cast<int>(rect.right * width),
                    inner.top + static_cast<int>(rect.bottom * height)};
        ::InflateRect(&screen, theme::Scale(1, m_dpi), 0);
        theme::FillTranslucent(dc, screen, RGB(70, 140, 235), 115);
    }
    ::RestoreDC(dc, -1);
}

// A new document resets scroll position, caches, selection and search; the
// frame has already updated the render/text service tokens.
void PageCanvas::SetDocument(const std::vector<PageMetrics>& pages) {
    m_pages = pages;
    m_scrollX = 0;
    m_scrollY = 0;
    m_lastNotifiedPage = -2;
    ClearSelection();
    ClearSearch();
    ClearRenderCache(true);
    RecomputeLayout();
    RequestVisiblePages();
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    NotifyPageChanged();
    NotifyZoomChanged();
}

// Drops the document geometry and every derived UI state.
void PageCanvas::ClearDocument() {
    m_pages.clear();
    m_layout.clear();
    m_contentWidth = 0;
    m_contentHeight = 0;
    m_scrollX = 0;
    m_scrollY = 0;
    m_lastNotifiedPage = -2;
    // Drop references to text pages and matches before the frame releases the
    // document; the canvas must not keep a stale page alive.
    ClearSelection();
    ClearSearch();
    ClearRenderCache(true);
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

// DPI change: keep the document point at the viewport centre stable while the
// layout is rebuilt, then let the renderer re-render at the new pixel width.
void PageCanvas::RefreshForDpiChange() {
    if (m_hwnd == nullptr) {
        return;
    }
    RECT client = ClientRect();
    Anchor anchor;
    const POINT center{client.right / 2, client.bottom / 2};
    CaptureAnchor(anchor, center);
    ClearRenderCache(true);
    RecomputeLayout();
    ApplyAnchor(anchor);
    RequestVisiblePages();
    NotifyZoomChanged();
    ::InvalidateRect(m_hwnd, nullptr, FALSE);
}

// Rebuilds the placeholder fonts after a language change.
void PageCanvas::ApplyLanguage() {
    const bool chinese = IsChineseInterface();
    // Page bitmaps do not embed UI text, so only the placeholder fonts change.
    theme::ReleaseFont(m_placeholderFont);
    theme::ReleaseFont(m_titleFont);
    theme::ReleaseFont(m_hintFont);
    m_placeholderFont = theme::CreateUiFont(m_dpi, 9.0, FW_NORMAL, chinese);
    m_titleFont = theme::CreateUiFont(m_dpi, 15.0, FW_SEMIBOLD, chinese);
    m_hintFont = theme::CreateUiFont(m_dpi, 10.0, FW_NORMAL, chinese);
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

RECT PageCanvas::ClientRect() const {
    RECT client = {};
    if (m_hwnd != nullptr) {
        ::GetClientRect(m_hwnd, &client);
    }
    return client;
}

// All layout values are in content coordinates, so scrolling only changes the
// viewport offset and never invalidates page rectangles.
void PageCanvas::RecomputeLayout() {
    const RECT client = ClientRect();
    const int margin = theme::Scale(kCanvasMarginDip, m_dpi);
    const int gap = theme::Scale(kPageGapDip, m_dpi);
    const double dpiScale = static_cast<double>(m_dpi) / 96.0;

    // Fit modes use the largest page of the document as reference so that the
    // zoom factor stays stable on mixed page sizes.
    double referenceWidth = 0.0;
    double referenceHeight = 0.0;
    for (const PageMetrics& page : m_pages) {
        referenceWidth = std::max(referenceWidth, page.width);
        referenceHeight = std::max(referenceHeight, page.height);
    }
    if (referenceWidth < 1.0) {
        referenceWidth = 612.0;
    }
    if (referenceHeight < 1.0) {
        referenceHeight = 792.0;
    }

    const double availableWidth = std::max(60.0, client.right - 2.0 * margin);
    const double availableHeight = std::max(60.0, client.bottom - 2.0 * margin);
    switch (m_zoomMode) {
        case ZoomMode::FitWidth:
            m_zoom = availableWidth / (referenceWidth * dpiScale);
            break;
        case ZoomMode::FitPage:
            m_zoom = std::min(availableWidth / (referenceWidth * dpiScale),
                              availableHeight / (referenceHeight * dpiScale));
            break;
        case ZoomMode::Custom:
            break;
    }
    m_zoom = std::clamp(m_zoom, kMinZoom, kMaxZoom);

    m_layout.assign(m_pages.size(), PageLayout{});
    if (m_pages.empty()) {
        m_contentWidth = 0;
        m_contentHeight = 0;
        UpdateScrollMetrics();
        return;
    }

    int maxPageWidth = 0;
    for (const PageMetrics& page : m_pages) {
        maxPageWidth = std::max(
            maxPageWidth, static_cast<int>(std::lround(page.width * m_zoom * dpiScale)));
    }
    const int contentWidth = maxPageWidth + 2 * margin;
    const int offsetX = std::max(0, (static_cast<int>(client.right) - contentWidth) / 2);

    int y = margin;
    for (size_t index = 0; index < m_pages.size(); ++index) {
        const int width = std::max(
            10, static_cast<int>(std::lround(m_pages[index].width * m_zoom * dpiScale)));
        const int height = std::max(
            10, static_cast<int>(std::lround(m_pages[index].height * m_zoom * dpiScale)));

        PageLayout& layout = m_layout[index];
        layout.width = width;
        layout.height = height;
        layout.left = offsetX + margin + (maxPageWidth - width) / 2;
        layout.top = y;
        y += height + gap;
    }

    m_contentWidth = contentWidth;
    m_contentHeight = y - gap + margin;
    UpdateScrollMetrics();
}

// Content extents are measured after every scroll, so the scrollbars clamp the
// current offsets when the window grows or shrinks.
void PageCanvas::UpdateScrollMetrics() {
    const RECT client = ClientRect();
    const int maxX = std::max(0, m_contentWidth - static_cast<int>(client.right));
    const int maxY = std::max(0, m_contentHeight - static_cast<int>(client.bottom));
    m_scrollX = std::clamp(m_scrollX, 0, maxX);
    m_scrollY = std::clamp(m_scrollY, 0, maxY);
}

// Clamps a requested offset, repaints and requests the newly visible pages.
void PageCanvas::ScrollTo(int scrollX, int scrollY) {
    const RECT client = ClientRect();
    const int maxX = std::max(0, m_contentWidth - static_cast<int>(client.right));
    const int maxY = std::max(0, m_contentHeight - static_cast<int>(client.bottom));
    const int newX = std::clamp(scrollX, 0, maxX);
    const int newY = std::clamp(scrollY, 0, maxY);
    if (newX == m_scrollX && newY == m_scrollY) {
        return;  // avoid a repaint when the scroll range is already exhausted
    }
    m_scrollX = newX;
    m_scrollY = newY;
    if (m_hwnd != nullptr) {
        // Scrolling moves every visible page, so the cheap dirty rectangle is
        // the whole client.  The persistent back buffer still avoids a per
        // frame bitmap allocation.
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    NotifyPageChanged();
    RequestVisiblePages();
}

// Convenience wrapper around ScrollTo.
void PageCanvas::ScrollBy(int deltaX, int deltaY) {
    ScrollTo(m_scrollX + deltaX, m_scrollY + deltaY);
}

// Binary search over the sorted page tops; see the header for the contract.
int PageCanvas::FirstPageEndingAfter(int contentY) const {
    // Page tops are strictly increasing, so the first page whose bottom is
    // below `contentY` can be found with a binary search.  A page in the gap
    // after a previous page is therefore selected naturally.
    size_t low = 0;
    size_t high = m_layout.size();
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const PageLayout& layout = m_layout[middle];
        if (layout.top + layout.height <= contentY) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return static_cast<int>(low);
}

void PageCanvas::VisiblePageRange(int contentTop, int contentBottom, int& first,
                                  int& last) const {
    first = 0;
    last = 0;
    if (m_layout.empty()) {
        return;
    }
    const int count = static_cast<int>(m_layout.size());
    first = std::min(FirstPageEndingAfter(contentTop), count);
    last = FirstPageEndingAfter(contentBottom);

    // FirstPageEndingAfter returns the page *after* the viewport when the
    // bottom edge falls into a gap; include the page that starts inside the
    // range so both partially visible pages are covered.
    if (last < count && m_layout[static_cast<size_t>(last)].top <= contentBottom) {
        ++last;
    }
    if (first > last) {
        first = last;
    }
}

// Maps a content-space Y position to the page under it.
int PageCanvas::PageAtContentY(int contentY) const {
    if (m_layout.empty()) {
        return -1;
    }
    const int index = FirstPageEndingAfter(contentY);
    if (index < static_cast<int>(m_layout.size())) {
        return index;
    }
    // Below the last page: keep reporting the last page as the reading
    // position, which matches the previous linear scan.
    return static_cast<int>(m_layout.size()) - 1;
}

// Reports the page whose centre is closest to the viewport centre, used for
// the status bar and the sidebar's current-section highlight.
int PageCanvas::CurrentPage() const {
    if (m_layout.empty()) {
        return -1;
    }
    const RECT client = ClientRect();
    const int centerY = m_scrollY + static_cast<int>(client.bottom) / 2;
    return PageAtContentY(centerY);
}

// Only posts when the reading position actually changed; scrolling within one
// page therefore does not spam the frame.
void PageCanvas::NotifyPageChanged() {
    if (m_hwnd == nullptr) {
        return;
    }
    const int page = CurrentPage();
    if (page == m_lastNotifiedPage) {
        return;
    }
    m_lastNotifiedPage = page;
    ::PostMessageW(::GetParent(m_hwnd), WM_APP_CANVAS_PAGE_CHANGED,
                   static_cast<WPARAM>(page), 0);
}

// Reports the zoom only when it changed; resize events use this to refresh the
// toolbar badge for fit-width/fit-page modes.
void PageCanvas::NotifyZoomChanged() {
    if (m_hwnd == nullptr) {
        return;
    }
    if (std::fabs(m_zoom - m_lastNotifiedZoom) < 0.0005) {
        return;
    }
    m_lastNotifiedZoom = m_zoom;
    ::PostMessageW(::GetParent(m_hwnd), WM_APP_CANVAS_ZOOM_CHANGED, 0, 0);
}

// Keeps the previous reading position under `screenPoint`; used by every zoom
// path so the page does not jump under the cursor.
void PageCanvas::CaptureAnchor(Anchor& anchor, POINT screenPoint) const {
    anchor = Anchor{};
    if (m_layout.empty()) {
        return;
    }
    const int contentX = screenPoint.x + m_scrollX;
    const int contentY = screenPoint.y + m_scrollY;
    const int pageIndex = std::clamp(PageAtContentY(contentY), 0,
                                     static_cast<int>(m_layout.size()) - 1);
    const PageLayout& layout = m_layout[static_cast<size_t>(pageIndex)];

    anchor.valid = true;
    anchor.page = pageIndex;
    anchor.fractionX =
        layout.width > 0
            ? std::clamp((contentX - layout.left) / static_cast<double>(layout.width), 0.0, 1.0)
            : 0.5;
    anchor.fractionY =
        layout.height > 0
            ? std::clamp((contentY - layout.top) / static_cast<double>(layout.height), 0.0, 1.0)
            : 0.5;
    anchor.screen = screenPoint;
}

// Moves the viewport so the captured content point lands on the same screen
// point again.  ScrollTo clamps at the document edges.
void PageCanvas::ApplyAnchor(const Anchor& anchor) {
    if (!anchor.valid || anchor.page < 0 ||
        anchor.page >= static_cast<int>(m_layout.size())) {
        return;
    }
    const PageLayout& layout = m_layout[static_cast<size_t>(anchor.page)];
    const int targetContentX =
        layout.left + static_cast<int>(std::lround(anchor.fractionX * layout.width));
    const int targetContentY =
        layout.top + static_cast<int>(std::lround(anchor.fractionY * layout.height));
    ScrollTo(targetContentX - anchor.screen.x, targetContentY - anchor.screen.y);
}

// Rebuilds the layout for a new zoom mode while keeping the viewport centre
// anchored, then invalidates the bitmap cache because page widths changed.
void PageCanvas::SetZoomMode(ZoomMode mode) {
    if (m_zoomMode == mode) {
        return;
    }
    const RECT client = ClientRect();
    Anchor anchor;
    CaptureAnchor(anchor, POINT{client.right / 2, client.bottom / 2});

    m_zoomMode = mode;
    ClearRenderCache(true);
    RecomputeLayout();
    ApplyAnchor(anchor);
    RequestVisiblePages();
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    NotifyZoomChanged();
}

// Explicit zoom switches to Custom mode so a later resize no longer changes it.
void PageCanvas::SetZoom(double zoom) {
    const RECT client = ClientRect();
    Anchor anchor;
    CaptureAnchor(anchor, POINT{client.right / 2, client.bottom / 2});

    m_zoomMode = ZoomMode::Custom;
    m_zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    ClearRenderCache(true);
    RecomputeLayout();
    ApplyAnchor(anchor);
    RequestVisiblePages();
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    NotifyZoomChanged();
}

// Ctrl + wheel path: the page point under the cursor stays fixed while the
// zoom changes, so the text does not run away from the pointer.
void PageCanvas::ZoomAt(POINT screenPoint, double factor) {
    if (m_layout.empty()) {
        return;
    }
    const RECT client = ClientRect();
    Anchor anchor;
    CaptureAnchor(anchor, screenPoint);

    // When the pointer is outside the client area fall back to the centre.
    if (screenPoint.x < client.left || screenPoint.x >= client.right ||
        screenPoint.y < client.top || screenPoint.y >= client.bottom) {
        CaptureAnchor(anchor, POINT{client.right / 2, client.bottom / 2});
    }

    m_zoomMode = ZoomMode::Custom;
    m_zoom = std::clamp(m_zoom * factor, kMinZoom, kMaxZoom);
    ClearRenderCache(true);
    RecomputeLayout();
    ApplyAnchor(anchor);
    RequestVisiblePages();
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
    NotifyZoomChanged();
}

int PageCanvas::ZoomPercent() const {
    return static_cast<int>(std::lround(m_zoom * 100.0));
}

void PageCanvas::ZoomIn() {
    const RECT client = ClientRect();
    ZoomAt(POINT{client.right / 2, client.bottom / 2}, kZoomStep);
}

void PageCanvas::ZoomOut() {
    const RECT client = ClientRect();
    ZoomAt(POINT{client.right / 2, client.bottom / 2}, 1.0 / kZoomStep);
}

// Scrolls to the top margin of a page.
void PageCanvas::GoToPage(int pageIndex) {
    if (m_layout.empty()) {
        return;
    }
    pageIndex = std::clamp(pageIndex, 0, static_cast<int>(m_layout.size()) - 1);
    const int margin = theme::Scale(kCanvasMarginDip, m_dpi);
    ScrollTo(0, m_layout[static_cast<size_t>(pageIndex)].top - margin);
    if (m_hwnd != nullptr) {
        ::InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

// Requests bitmaps for the visible page range, closest to the reading position
// first.  Text extraction is requested afterwards with the same range.
void PageCanvas::RequestVisiblePages() {
    if (m_renderer == nullptr || m_pages.empty() ||
        m_layout.size() != m_pages.size() || m_hwnd == nullptr) {
        return;
    }

    const RECT client = ClientRect();
    int first = 0;
    int last = 0;
    VisiblePageRange(m_scrollY, m_scrollY + static_cast<int>(client.bottom), first,
                     last);
    if (first >= last) {
        return;
    }

    std::vector<int> visible;
    visible.reserve(static_cast<size_t>(last - first));
    for (int index = first; index < last; ++index) {
        visible.push_back(index);
    }

    // Closest pages first so the document fills in around the reading position.
    const int current = std::max(0, CurrentPage());
    std::sort(visible.begin(), visible.end(), [current](int left, int right) {
        return std::abs(left - current) < std::abs(right - current);
    });

    for (int index : visible) {
        const int desiredWidth =
            std::min(m_layout[static_cast<size_t>(index)].width, kMaxPageRenderWidth);
        if (desiredWidth <= 0) {
            continue;
        }

        const auto cached = m_cache.find(index);
        if (cached != m_cache.end() &&
            std::abs(cached->second.pixelWidth - desiredWidth) <= 2) {
            continue;
        }
        const auto pending = m_pendingWidths.find(index);
        if (pending != m_pendingWidths.end() &&
            std::abs(pending->second - desiredWidth) <= 2) {
            continue;
        }

        if (cached != m_cache.end()) {
            m_cache.erase(cached);
        }
        m_pendingPages.insert(index);
        m_pendingWidths[index] = desiredWidth;
        m_renderer->RequestPage(index, desiredWidth, m_renderToken);
    }

    // Text extraction follows the same visible-page priority, so selection
    // and search stay responsive while scrolling.
    RequestVisibleText();
}

// Drops all rendered pages; cancelling pending renders avoids receiving stale
// bitmaps after a zoom/document change.
void PageCanvas::ClearRenderCache(bool cancelPendingRequests) {
    m_cache.clear();
    m_pendingPages.clear();
    m_pendingWidths.clear();
    if (cancelPendingRequests && m_renderer != nullptr) {
        // Zoom changes make in-flight renders useless; cancelling them frees
        // the worker for the new size sooner.
        m_renderer->CancelPendingPages();
    }
}

// The cache is intentionally small (a handful of pages); scanning it to find
// the farthest page is cheaper than maintaining a second index.  Eviction stops
// at both a page count and a byte budget so a single huge render cannot pin
// gigabytes of memory.
void PageCanvas::EvictCache() {
    auto cacheBytes = [this]() {
        uint64_t total = 0;
        for (const auto& entry : m_cache) {
            total += static_cast<uint64_t>(entry.second.pixelWidth) *
                     static_cast<uint64_t>(entry.second.pixelHeight) * 4ull;
        }
        return total;
    };

    if (static_cast<int>(m_cache.size()) <= kMaxRenderCachePages &&
        cacheBytes() <= kMaxRenderCacheBytes) {
        return;
    }
    const int current = std::max(0, CurrentPage());
    while (m_cache.size() > 1 &&
           (static_cast<int>(m_cache.size()) > kMaxRenderCachePages ||
            cacheBytes() > kMaxRenderCacheBytes)) {
        auto worst = m_cache.end();
        int worstDistance = -1;
        for (auto it = m_cache.begin(); it != m_cache.end(); ++it) {
            const int distance = std::abs(it->first - current);
            if (distance > worstDistance) {
                worstDistance = distance;
                worst = it;
            }
        }
        if (worst == m_cache.end()) {
            break;
        }
        m_cache.erase(worst);
    }
}

// Only the damaged page is invalidated; with the persistent back buffer this
// turns a selection drag into a small, page-sized blit instead of a full frame.
void PageCanvas::InvalidatePage(int pageIndex) {
    if (m_hwnd == nullptr || pageIndex < 0 ||
        pageIndex >= static_cast<int>(m_layout.size())) {
        return;
    }
    const PageLayout& layout = m_layout[static_cast<size_t>(pageIndex)];
    RECT rect{layout.left - m_scrollX, layout.top - m_scrollY,
              layout.left - m_scrollX + layout.width,
              layout.top - m_scrollY + layout.height};
    const int padding = theme::Scale(6, m_dpi);
    ::InflateRect(&rect, padding, padding);
    ::InvalidateRect(m_hwnd, &rect, FALSE);
}

// A rendered page is cached only when it still matches the requested width;
// a stale result (zoom changed meanwhile) is discarded without a repaint.
void PageCanvas::OnPageReady(PageReadyMessage& message) {
    // Clear the bookkeeping even for stale results so a page is not considered
    // pending forever.
    m_pendingPages.erase(message.pageIndex);
    m_pendingWidths.erase(message.pageIndex);

    if (message.token != m_renderToken || message.pageIndex < 0 ||
        message.pageIndex >= static_cast<int>(m_layout.size())) {
        return;
    }
    const int desiredWidth =
        std::min(m_layout[static_cast<size_t>(message.pageIndex)].width, kMaxPageRenderWidth);
    if (std::abs(message.requestedWidth - desiredWidth) > 2) {
        return;  // the zoom changed while the page was being rendered
    }
    if (message.pixels.empty() || message.pixelWidth <= 0 || message.pixelHeight <= 0) {
        return;
    }

    CachedBitmap bitmap;
    bitmap.pixelWidth = message.pixelWidth;
    bitmap.pixelHeight = message.pixelHeight;
    bitmap.pixels = std::move(message.pixels);
    m_cache[message.pageIndex] = std::move(bitmap);

    EvictCache();
    InvalidatePage(message.pageIndex);
    RequestVisiblePages();
}

bool PageCanvas::EnsureBackBuffer(HDC reference, int width, int height,
                                  bool& recreated) {
    recreated = false;
    if (reference == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    if (m_backDc != nullptr && m_backWidth == width && m_backHeight == height) {
        return true;
    }

    ReleaseBackBuffer();
    m_backDc = ::CreateCompatibleDC(reference);
    if (m_backDc == nullptr) {
        return false;
    }
    m_backBitmap = ::CreateCompatibleBitmap(reference, width, height);
    if (m_backBitmap == nullptr) {
        ::DeleteDC(m_backDc);
        m_backDc = nullptr;
        return false;
    }
    m_backPreviousBitmap = ::SelectObject(m_backDc, m_backBitmap);
    m_backWidth = width;
    m_backHeight = height;
    // The new surface has undefined contents; the caller must repaint all of it.
    recreated = true;
    return true;
}

// Destroys the persistent surface; called on WM_DESTROY and before recreating
// a different sized buffer.
void PageCanvas::ReleaseBackBuffer() {
    if (m_backDc != nullptr) {
        if (m_backPreviousBitmap != nullptr) {
            ::SelectObject(m_backDc, m_backPreviousBitmap);
        }
        ::DeleteDC(m_backDc);
        m_backDc = nullptr;
    }
    if (m_backBitmap != nullptr) {
        ::DeleteObject(m_backBitmap);
        m_backBitmap = nullptr;
    }
    m_backPreviousBitmap = nullptr;
    m_backWidth = 0;
    m_backHeight = 0;
}

// Paints only the damaged rectangle into the persistent back buffer.  The
// caller then blits the same rectangle to the window, so page-sized updates
// (selection, search, one rendered page) never redraw the whole viewport.
void PageCanvas::Paint(HDC dc, const RECT& client, const RECT& dirty) {
    const theme::Palette& colors = theme::Colors();
    ::SaveDC(dc);
    ::IntersectClipRect(dc, dirty.left, dirty.top, dirty.right, dirty.bottom);

    // Only the damaged area is cleared; the rest of the back buffer still holds
    // the previous frame.
    theme::FillSolid(dc, dirty, colors.canvasBackground);

    if (m_pages.empty() || m_layout.size() != m_pages.size()) {
        DrawEmptyState(dc, client);
        ::RestoreDC(dc, -1);
        return;
    }

    // Page shadows extend a few pixels outside the page rectangle, so widen
    // the content range before selecting pages.
    const int padding = theme::Scale(8, m_dpi);
    int first = 0;
    int last = 0;
    VisiblePageRange(m_scrollY + static_cast<int>(dirty.top) - padding,
                     m_scrollY + static_cast<int>(dirty.bottom) + padding, first,
                     last);
    for (int index = first; index < last; ++index) {
        const PageLayout& layout = m_layout[static_cast<size_t>(index)];
        RECT pageRect{layout.left - m_scrollX, layout.top - m_scrollY,
                      layout.left - m_scrollX + layout.width,
                      layout.top - m_scrollY + layout.height};
        if (pageRect.bottom + padding < dirty.top ||
            pageRect.top - padding > dirty.bottom) {
            continue;
        }
        DrawPage(dc, index, pageRect);
        PaintSearch(dc, pageRect, index);
        PaintSelection(dc, pageRect, index);
    }

    m_verticalBar.Update(ScrollOrientation::Vertical, client.bottom, m_contentHeight,
                         m_scrollY);
    m_verticalBar.Paint(dc, client, m_hoveredBar == 1);

    m_horizontalBar.Update(ScrollOrientation::Horizontal, client.right, m_contentWidth,
                           m_scrollX);
    m_horizontalBar.Paint(dc, client, m_hoveredBar == 2);
    ::RestoreDC(dc, -1);
}

// The page bitmap is stretched to the layout rectangle; StretchDIBits with
// COLORONCOLOR is fast enough and keeps text crisp at integer-ish zooms.
void PageCanvas::DrawPage(HDC dc, int pageIndex, const RECT& screenRect) {
    const theme::Palette& colors = theme::Colors();
    DrawPageShadow(dc, screenRect, m_dpi, colors);

    // A one pixel border keeps light pages visually separated from the canvas.
    theme::FillSolid(dc, screenRect, colors.pageBorder);
    RECT inner = screenRect;
    ::InflateRect(&inner, -theme::Scale(1, m_dpi), -theme::Scale(1, m_dpi));
    theme::FillSolid(dc, inner, colors.pageBackground);

    const int desiredWidth = std::min(
        m_layout[static_cast<size_t>(pageIndex)].width, kMaxPageRenderWidth);
    const auto cached = m_cache.find(pageIndex);
    const bool cacheValid =
        cached != m_cache.end() &&
        std::abs(cached->second.pixelWidth - desiredWidth) <= 2 &&
        cached->second.pixelHeight > 0;

    if (cacheValid) {
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = cached->second.pixelWidth;
        info.bmiHeader.biHeight = -cached->second.pixelHeight;  // top-down DIB
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        ::SetStretchBltMode(dc, COLORONCOLOR);
        ::StretchDIBits(dc, inner.left, inner.top, inner.right - inner.left,
                        inner.bottom - inner.top, 0, 0, cached->second.pixelWidth,
                        cached->second.pixelHeight, cached->second.pixels.data(), &info,
                        DIB_RGB_COLORS, SRCCOPY);
        return;
    }

    RECT textRect = inner;
    textRect.top = (inner.top + inner.bottom) / 2 - theme::Scale(9, m_dpi);
    textRect.bottom = textRect.top + theme::Scale(18, m_dpi);
    theme::DrawString(dc, Tr(Str::StatusLoading), textRect, colors.textDisabled,
                      m_placeholderFont, DT_CENTER);
}

// Empty document state: a simple page glyph, the localised title and the
// "drag a file here" hint.  Drawn with GDI primitives only.
void PageCanvas::DrawEmptyState(HDC dc, const RECT& client) {
    const theme::Palette& colors = theme::Colors();
    const int centerX = (client.left + client.right) / 2;
    const int centerY = (client.top + client.bottom) / 2;

    const int glyphWidth = theme::Scale(64, m_dpi);
    const int glyphHeight = theme::Scale(82, m_dpi);
    RECT glyph{centerX - glyphWidth / 2,
               centerY - glyphHeight - theme::Scale(46, m_dpi),
               centerX + glyphWidth / 2,
               centerY - theme::Scale(46, m_dpi)};

    theme::FillRound(dc, glyph, theme::Scale(5, m_dpi), colors.windowBackground);
    theme::StrokeRound(dc, glyph, theme::Scale(5, m_dpi), colors.pageBorder, 1);

    // Folded corner.
    const int fold = theme::Scale(18, m_dpi);
    POINT corner[3] = {{glyph.right - fold, glyph.top},
                       {glyph.right - theme::Scale(2, m_dpi), glyph.top + fold},
                       {glyph.right - fold, glyph.top + fold}};
    HPEN pen = ::CreatePen(PS_SOLID, 1, colors.pageBorder);
    HGDIOBJ oldPen = ::SelectObject(dc, pen);
    ::Polyline(dc, corner, 3);
    ::SelectObject(dc, oldPen);
    ::DeleteObject(pen);

    // Three text lines inside the page glyph.
    const int lineTop = glyph.top + theme::Scale(38, m_dpi);
    for (int line = 0; line < 3; ++line) {
        const int y = lineTop + line * theme::Scale(10, m_dpi);
        theme::DrawLine(dc, glyph.left + theme::Scale(14, m_dpi), y,
                        glyph.right - theme::Scale(14, m_dpi), y, colors.separator, 1);
    }

    RECT titleRect{client.left + theme::Scale(40, m_dpi),
                   glyph.bottom + theme::Scale(30, m_dpi),
                   client.right - theme::Scale(40, m_dpi),
                   glyph.bottom + theme::Scale(60, m_dpi)};
    theme::DrawString(dc, Tr(Str::EmptyTitle), titleRect, colors.text, m_titleFont,
                      DT_CENTER);

    RECT hintRect{titleRect.left, titleRect.bottom + theme::Scale(8, m_dpi),
                  titleRect.right, titleRect.bottom + theme::Scale(32, m_dpi)};
    theme::DrawString(dc, Tr(Str::EmptyHint), hintRect, colors.textMuted, m_hintFont,
                      DT_CENTER);
}

// Window procedure: painting is double buffered elsewhere, so WM_PAINT only
// transfers the damaged rectangle.  Input handling is deliberately local to the
// canvas; the frame receives notifications for page/zoom/selection changes.
LRESULT CALLBACK PageCanvas::WindowProc(HWND window, UINT message, WPARAM wParam,
                                        LPARAM lParam) {
    PageCanvas* self =
        reinterpret_cast<PageCanvas*>(::GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_CREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<PageCanvas*>(create->lpCreateParams);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->m_hwnd = window;
        }
        return 0;
    }
    if (self == nullptr) {
        return ::DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
        case WM_ERASEBKGND:
            return 1;

        case WM_SIZE: {
            RECT client = self->ClientRect();
            Anchor anchor;
            const POINT center{client.right / 2, client.bottom / 2};
            self->CaptureAnchor(anchor, center);
            const double previousZoom = self->m_zoom;
            self->RecomputeLayout();
            if (std::fabs(self->m_zoom - previousZoom) >
                std::max(0.0005, previousZoom * 0.002)) {
                self->ClearRenderCache(true);
            }
            self->ApplyAnchor(anchor);
            self->RequestVisiblePages();
            // A fit-width page grows with the window, so the toolbar badge must
            // be told that the zoom changed even though the user did not zoom.
            self->NotifyZoomChanged();
            ::InvalidateRect(window, nullptr, FALSE);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT paint = {};
            HDC dc = ::BeginPaint(window, &paint);
            RECT client = {};
            ::GetClientRect(window, &client);
            const RECT update = paint.rcPaint;
            RECT dirty = update;
            bool fullRepaintNeeded = false;
            if (client.right > 0 && client.bottom > 0 && dirty.right > dirty.left &&
                dirty.bottom > dirty.top) {
                bool recreated = false;
                if (self->EnsureBackBuffer(dc, client.right, client.bottom,
                                           recreated)) {
                    // A freshly created surface has no valid content, so the
                    // first frame after a resize has to cover the whole client.
                    if (recreated) {
                        dirty = client;
                        // BeginPaint's clip still covers only the old update
                        // region; request another pass for the rest so the
                        // restored window cannot show stale pixels.
                        fullRepaintNeeded =
                            update.left > client.left || update.top > client.top ||
                            update.right < client.right ||
                            update.bottom < client.bottom;
                    }
                    self->Paint(self->m_backDc, client, dirty);
                    ::BitBlt(dc, dirty.left, dirty.top, dirty.right - dirty.left,
                             dirty.bottom - dirty.top, self->m_backDc, dirty.left,
                             dirty.top, SRCCOPY);
                } else {
                    // Out of memory: fall back to a temporary surface rather
                    // than leaving the window unpainted.
                    HDC memoryDc = ::CreateCompatibleDC(dc);
                    HBITMAP bitmap = ::CreateCompatibleBitmap(
                        dc, client.right - client.left, client.bottom - client.top);
                    if (memoryDc != nullptr && bitmap != nullptr) {
                        HGDIOBJ oldBitmap = ::SelectObject(memoryDc, bitmap);
                        self->Paint(memoryDc, client, client);
                        ::BitBlt(dc, 0, 0, client.right, client.bottom, memoryDc, 0, 0,
                                 SRCCOPY);
                        ::SelectObject(memoryDc, oldBitmap);
                    }
                    if (bitmap != nullptr) {
                        ::DeleteObject(bitmap);
                    }
                    if (memoryDc != nullptr) {
                        ::DeleteDC(memoryDc);
                    }
                }
            }
            ::EndPaint(window, &paint);
            if (fullRepaintNeeded) {
                ::InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!self->m_trackingMouse) {
                TRACKMOUSEEVENT track = {};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = window;
                ::TrackMouseEvent(&track);
                self->m_trackingMouse = true;
            }

            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const RECT client = self->ClientRect();

            if (self->m_verticalBar.IsDragging() || self->m_horizontalBar.IsDragging()) {
                int newOffset = 0;
                if (self->m_verticalBar.IsDragging()) {
                    if (self->m_verticalBar.DragTo(client, point, newOffset)) {
                        self->ScrollTo(self->m_scrollX, newOffset);
                    }
                } else {
                    if (self->m_horizontalBar.DragTo(client, point, newOffset)) {
                        self->ScrollTo(newOffset, self->m_scrollY);
                    }
                }
                return 0;
            }

            if (self->m_panning) {
                const int deltaX = point.x - self->m_panStart.x;
                const int deltaY = point.y - self->m_panStart.y;
                self->ScrollTo(self->m_panOrigin.x - deltaX,
                               self->m_panOrigin.y - deltaY);
                return 0;
            }

            if (self->m_selecting) {
                self->ExtendSelection(point);
                return 0;
            }

            int hovered = 0;
            if (self->m_verticalBar.HitTestTrack(client, point)) {
                hovered = 1;
            } else if (self->m_horizontalBar.HitTestTrack(client, point)) {
                hovered = 2;
            }
            if (hovered != self->m_hoveredBar) {
                self->m_hoveredBar = hovered;
                ::InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            self->m_trackingMouse = false;
            if (self->m_hoveredBar != 0) {
                self->m_hoveredBar = 0;
                ::InvalidateRect(window, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            ::SetFocus(window);
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const RECT client = self->ClientRect();

            if (self->m_verticalBar.IsVisible() &&
                self->m_verticalBar.HitTestTrack(client, point)) {
                if (!self->m_verticalBar.BeginDrag(client, point)) {
                    const int page = std::max(0, static_cast<int>(client.bottom)) * 8 / 10;
                    const RECT thumb = self->m_verticalBar.ThumbRect(client);
                    self->ScrollBy(0, point.y > (thumb.top + thumb.bottom) / 2 ? page : -page);
                }
                ::SetCapture(window);
                return 0;
            }
            if (self->m_horizontalBar.IsVisible() &&
                self->m_horizontalBar.HitTestTrack(client, point)) {
                if (!self->m_horizontalBar.BeginDrag(client, point)) {
                    const int page = std::max(0, static_cast<int>(client.right)) * 8 / 10;
                    const RECT thumb = self->m_horizontalBar.ThumbRect(client);
                    self->ScrollBy(point.x > (thumb.left + thumb.right) / 2 ? page : -page, 0);
                }
                ::SetCapture(window);
                return 0;
            }

            // Left drag selects text; panning is bound to the middle button.
            self->BeginSelection(point);
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            ::SetFocus(window);
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            self->SelectWordAt(point);
            return 0;
        }

        case WM_MBUTTONDOWN: {
            ::SetFocus(window);
            if (!self->m_pages.empty()) {
                const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                self->m_panning = true;
                self->m_panStart = point;
                self->m_panOrigin = POINT{self->m_scrollX, self->m_scrollY};
                ::SetCapture(window);
                ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));
            }
            return 0;
        }

        case WM_MBUTTONUP:
            if (self->m_panning) {
                self->m_panning = false;
                if (::GetCapture() == window) {
                    ::ReleaseCapture();
                }
            }
            return 0;

        case WM_LBUTTONUP:
            if (self->m_selecting) {
                self->FinishSelection();
            }
            if (::GetCapture() == window) {
                ::ReleaseCapture();
            }
            self->m_panning = false;
            self->m_verticalBar.EndDrag();
            self->m_horizontalBar.EndDrag();
            return 0;

        case WM_CAPTURECHANGED:
            self->m_panning = false;
            // A capture loss (for example Alt+Tab) finishes the drag without
            // discarding the selection that was already made.
            self->m_selecting = false;
            self->m_pendingSelection = false;
            self->m_verticalBar.EndDrag();
            self->m_horizontalBar.EndDrag();
            return 0;

        case WM_MOUSEWHEEL: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ::ScreenToClient(window, &point);
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const int notches = delta / WHEEL_DELTA;
            if (notches == 0) {
                return 0;
            }

            const bool controlDown = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shiftDown = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;

            if (controlDown) {
                self->ZoomAt(point, notches > 0 ? kZoomStep : 1.0 / kZoomStep);
            } else if (shiftDown) {
                self->ScrollBy(-notches * theme::Scale(90, self->m_dpi), 0);
            } else {
                self->ScrollBy(0, -notches * theme::Scale(80, self->m_dpi));
            }
            return 0;
        }

        case WM_KEYDOWN: {
            const int step = theme::Scale(64, self->m_dpi);
            switch (wParam) {
                case VK_UP: self->ScrollBy(0, -step); return 0;
                case VK_DOWN: self->ScrollBy(0, step); return 0;
                case VK_LEFT: self->ScrollBy(-step, 0); return 0;
                case VK_RIGHT: self->ScrollBy(step, 0); return 0;
                case VK_PRIOR: self->ScrollBy(0, -self->ClientRect().bottom * 9 / 10); return 0;
                case VK_NEXT: self->ScrollBy(0, self->ClientRect().bottom * 9 / 10); return 0;
                default: break;
            }
            return 0;
        }

        case WM_SETCURSOR: {
            POINT point = {};
            ::GetCursorPos(&point);
            ::ScreenToClient(window, &point);
            const RECT client = self->ClientRect();
            if (self->m_panning) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            if (self->m_verticalBar.HitTestTrack(client, point) ||
                self->m_horizontalBar.HitTestTrack(client, point)) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            float normalizedX = 0.0f;
            float normalizedY = 0.0f;
            if (self->PageAtPoint(point, normalizedX, normalizedY) >= 0) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_IBEAM));
                return TRUE;
            }
            break;
        }

        case WM_DESTROY:
            self->ReleaseBackBuffer();
            theme::ReleaseFont(self->m_placeholderFont);
            theme::ReleaseFont(self->m_titleFont);
            theme::ReleaseFont(self->m_hintFont);
            self->m_hwnd = nullptr;
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace pengpdf








